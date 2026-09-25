#include "CrossPointWebServer.h"

#include <ArduinoJson.h>
#include <BoardConfig.h>
#ifdef SIMULATOR
#include <ArduinoJsonStringCompat.h>
#endif
#include <Epub.h>
#include <FsHelpers.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <WiFi.h>
#include <esp_efuse.h>
#include <esp_efuse_table.h>
#ifndef SIMULATOR
#include <esp_ota_ops.h>
#endif

#include <algorithm>
#include <cctype>
#include <cstring>
#include <iterator>
#include <string_view>

#include "AnnotationTagStore.h"
#include "AppCapabilities.h"
#include "AppVersion.h"
#include "ClippingStore.h"
#include "CrossPointSettings.h"
#include "FirmwareIdentity.h"
#include "FontInstaller.h"
#include "NoteStore.h"
#include "OpdsServerStore.h"
#include "QuickActions.h"
#include "SdCardFontSystem.h"
#include "SettingsList.h"
#include "WebDAVHandler.h"
#include "WifiCredentialStore.h"
#include "activities/boot_sleep/SleepImageIndex.h"
#include "html/FilesPageHtml.generated.h"
#include "html/FirmwarePageHtml.generated.h"
#include "html/FontsPageHtml.generated.h"
#include "html/HighlightsPageHtml.generated.h"
#include "html/HomePageHtml.generated.h"
#include "html/LogoPng.generated.h"
#include "html/SettingsPageHtml.generated.h"
#include "html/StyleCss.generated.h"
#include "html/js/jszip_minJs.generated.h"
#include "network/FirmwareFlasher.h"
#include "network/OtaUpdater.h"
#include "util/BookCacheUtils.h"
#include "util/FontFamilyLabel.h"
#include "util/StringUtils.h"

namespace {
// Folders/files to hide from the web interface file browser.
// Dot-prefixed items are hidden unless showHiddenFiles is enabled.
constexpr const char* HIDDEN_ITEMS[] = {"System Volume Information", "XTCache"};
constexpr uint16_t UDP_PORTS[] = {54982, 48123, 39001, 44044, 59678};
constexpr uint16_t LOCAL_UDP_PORT = 8134;
constexpr size_t MIN_FIRMWARE_UPDATE_FREE_HEAP = 96 * 1024;
constexpr size_t MIN_FIRMWARE_UPDATE_MAX_ALLOC = 32 * 1024;
constexpr size_t FIRMWARE_SIGNATURE_SIZE = 64;

// Static pointer for WebSocket callback (WebSocketsServer requires C-style callback)
CrossPointWebServer* wsInstance = nullptr;

size_t parseFirmwareSizeArg(WebServer* server, const char* name, const size_t fallback = 0) {
  if (server == nullptr || name == nullptr || !server->hasArg(name)) return fallback;
  const String value = server->arg(name);
  size_t parsed = 0;
  // Invalid supplied values must reject START, never silently become offset 0
  // and erase an existing partial upload.
  return firmware_request::parseSize(value.c_str(), parsed) ? parsed : std::numeric_limits<size_t>::max();
}

String firmwareStateField(const String& marker, const char* field) {
  if (field == nullptr) return "";
  const String prefix = String(field) + "=";
  const int start = marker.indexOf(prefix);
  if (start < 0) return "";
  const int valueStart = start + prefix.length();
  int valueEnd = marker.indexOf(';', valueStart);
  if (valueEnd < 0) valueEnd = marker.length();
  return marker.substring(valueStart, valueEnd);
}

uint8_t enumDisplayIndexForRawValue(const SettingInfo& setting, uint8_t rawValue) {
  if (setting.enumRawValues.empty()) {
    return rawValue;
  }

  auto it = std::find(setting.enumRawValues.begin(), setting.enumRawValues.end(), rawValue);
  if (it == setting.enumRawValues.end()) {
    return 0;
  }
  return static_cast<uint8_t>(std::distance(setting.enumRawValues.begin(), it));
}

uint8_t enumRawValueForDisplayIndex(const SettingInfo& setting, uint8_t displayIndex) {
  if (setting.enumRawValues.empty()) {
    return displayIndex;
  }
  if (displayIndex >= setting.enumRawValues.size()) {
    return setting.enumRawValues.front();
  }
  return setting.enumRawValues[displayIndex];
}

bool isWebSettingAvailable(const SettingInfo& setting) {
  if (setting.nameId == StrId::STR_PAGE_TURN_GESTURE && !gpio.hasTouch()) {
    return false;
  }

#if !INKADEMIC_APP_CAP_TOUCH
  if (setting.nameId == StrId::STR_TOUCH_READER_CONTROLS || setting.nameId == StrId::STR_DISABLE_TOUCHSCREEN) {
    return false;
  }
#endif

#if !FREEINK_CAP_FRONTLIGHT
  if (setting.nameId == StrId::STR_BRIGHTNESS || setting.nameId == StrId::STR_WARMTH ||
      setting.nameId == StrId::STR_FRONTLIGHT) {
    return false;
  }
#endif

  if (!halClock.isAvailable()) {
    switch (setting.nameId) {
      case StrId::STR_HIDE_CLOCK:
      case StrId::STR_AUTO_BACKUP_STATS:
      case StrId::STR_CLOCK_UTC_OFFSET:
      case StrId::STR_CLOCK_FORMAT:
      case StrId::STR_DATE_FORMAT:
      case StrId::STR_DATE_SEPARATOR:
      case StrId::STR_CLOCK_SYNCED:
        return false;
      default:
        break;
    }
  }

  return true;
}

// Streams a font-catalog JSON response in bounded pieces. This avoids holding
// both an ArduinoJson document and its serialized String in the fragmented
// network heap, and gives WiFi a chance to drain each piece before the next.
class FontListJsonWriter {
 public:
  explicit FontListJsonWriter(WebServer& server) : server_(server) {}

  void append(const char* text) { append(text, strlen(text)); }

  void append(const char* text, size_t textLength) {
    while (textLength > 0) {
      if (length_ == sizeof(buffer_)) flush();
      const size_t copyLength = std::min(textLength, sizeof(buffer_) - length_);
      memcpy(buffer_ + length_, text, copyLength);
      length_ += copyLength;
      text += copyLength;
      textLength -= copyLength;
    }
  }

  void appendUnsigned(unsigned long value) {
    char number[16];
    const int length = snprintf(number, sizeof(number), "%lu", value);
    append(number, static_cast<size_t>(length));
  }

  void appendJsonString(const char* value) {
    append("\"");
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(value); *p; ++p) {
      switch (*p) {
        case '\"':
          append("\\\"");
          break;
        case '\\':
          append("\\\\");
          break;
        case '\b':
          append("\\b");
          break;
        case '\f':
          append("\\f");
          break;
        case '\n':
          append("\\n");
          break;
        case '\r':
          append("\\r");
          break;
        case '\t':
          append("\\t");
          break;
        default:
          if (*p < 0x20) {
            static constexpr char kHexDigits[] = "0123456789ABCDEF";
            const char escaped[] = {'\\', 'u', '0', '0', kHexDigits[*p >> 4], kHexDigits[*p & 0x0F]};
            append(escaped, sizeof(escaped));
          } else {
            append(reinterpret_cast<const char*>(p), 1);
          }
          break;
      }
    }
    append("\"");
  }

  void flush() {
    if (length_ == 0) return;
    server_.sendContent(buffer_, length_);
    length_ = 0;
  }

 private:
  static constexpr size_t BUFFER_SIZE = 192;
  WebServer& server_;
  char buffer_[BUFFER_SIZE];
  size_t length_ = 0;
};

// WebSocket upload state
HalFile wsUploadFile;
String wsUploadFileName;
String wsUploadPath;
size_t wsUploadSize = 0;
size_t wsUploadReceived = 0;
unsigned long wsUploadStartTime = 0;
bool wsUploadInProgress = false;
uint8_t wsUploadClientNum = 255;  // 255 = no active upload client
size_t wsLastProgressSent = 0;
String wsLastCompleteName;
size_t wsLastCompleteSize = 0;
unsigned long wsLastCompleteAt = 0;

String normalizeWebPath(const String& inputPath) {
  if (inputPath.isEmpty() || inputPath == "/") {
    return "/";
  }
  std::string normalized = FsHelpers::normalisePath(inputPath.c_str());
  String result = normalized.c_str();
  if (result.isEmpty()) {
    return "/";
  }
  if (!result.startsWith("/")) {
    result = "/" + result;
  }
  if (result.length() > 1 && result.endsWith("/")) {
    result = result.substring(0, result.length() - 1);
  }
  return result;
}

bool isProtectedPath(const String& path) {
  // Hidden/system items stay out of the default file-manager view. Enabling
  // Show Hidden Files intentionally makes them fully manageable.
  if (SETTINGS.showHiddenFiles) return false;

  // Check every segment so a hidden parent also protects its descendants.
  int start = 0;
  while (start < (int)path.length()) {
    if (path.charAt(start) == '/') {
      start++;
      continue;
    }
    int end = path.indexOf('/', start);
    if (end == -1) end = path.length();

    String segment = path.substring(start, end);

    if (segment.startsWith(".")) return true;

    for (const auto* item : HIDDEN_ITEMS) {
      if (segment.equals(item)) return true;
    }

    start = end + 1;
  }

  return false;
}
}  // namespace

static void sendStaticContent(WebServer* server, const char* data, size_t len, const char* etag,
                              const char* contentType, bool gzip = true, const char* cacheControl = "no-cache");

// File listing page template - now using generated headers:
// - HomePageHtml (from html/HomePage.html)
// - FilesPageHeaderHtml (from html/FilesPageHeader.html)
// - FilesPageFooterHtml (from html/FilesPageFooter.html)
CrossPointWebServer::CrossPointWebServer() {}

CrossPointWebServer::~CrossPointWebServer() { stop(); }

void CrossPointWebServer::begin() {
  if (running) {
    LOG_DBG("WEB", "Web server already running");
    return;
  }

  // Check if we have a valid network connection (either STA connected or AP mode)
  const wifi_mode_t wifiMode = WiFi.getMode();
  const bool isStaConnected = (wifiMode & WIFI_MODE_STA) && (WiFi.status() == WL_CONNECTED);
  const bool isInApMode = (wifiMode & WIFI_MODE_AP) && (WiFi.softAPgetStationNum() >= 0);  // AP is running

  if (!isStaConnected && !isInApMode) {
    LOG_DBG("WEB", "Cannot start webserver - no valid network (mode=%d, status=%d)", wifiMode, WiFi.status());
    return;
  }

  // Store AP mode flag for later use (e.g., in handleStatus)
  apMode = isInApMode;

  LOG_DBG("WEB", "[MEM] Free heap before begin: %d bytes", ESP.getFreeHeap());
  LOG_DBG("WEB", "Network mode: %s", apMode ? "AP" : "STA");

  LOG_DBG("WEB", "Creating web server on port %d...", port);
  server.reset(new WebServer(port));

  // Disable WiFi sleep to improve responsiveness and prevent 'unreachable' errors.
  // This is critical for reliable web server operation on ESP32.
  WiFi.setSleep(false);
  // Default varies by ESP32 core version. The activity's loss-recovery loop
  // relies on driver retries during transient disconnects.
  WiFi.setAutoReconnect(true);

  // Note: WebServer class doesn't have setNoDelay() in the standard ESP32 library.
  // We rely on disabling WiFi sleep for responsiveness.

  if (!server) {
    LOG_ERR("WEB", "Failed to create WebServer!");
    return;
  }

  // Add Access-Control-Allow-* headers to every response so web-based clients
  // and PWAs on other origins can use the HTTP API. Preflight OPTIONS requests
  // are answered in handleNotFound().
  server->enableCORS(true);

  // Setup routes
  server->on("/", HTTP_GET, [this] { handleRoot(); });
  server->on("/files", HTTP_GET, [this] { handleFileList(); });
  server->on("/js/jszip.min.js", HTTP_GET, [this] { handleJszip(); });
  server->on("/style.css", HTTP_GET, [this] { handleStyleCss(); });
  server->on("/logo.png", HTTP_GET, [this] { handleLogo(); });

  server->on("/api/status", HTTP_GET, [this] { handleStatus(); });
  server->on("/firmware", HTTP_GET, [this] { handleFirmwarePage(); });
  server->on("/api/firmware/status", HTTP_GET, [this] { handleFirmwareStatus(); });
  server->on(
      "/api/firmware/upload", HTTP_POST, [this] { handleFirmwareUploadPost(); }, [this] { handleFirmwareUpload(); });
  server->on(
      "/api/firmware/signature", HTTP_POST, [this] { handleFirmwareSignatureUploadPost(); },
      [this] { handleFirmwareSignatureUpload(); });
  server->on("/api/firmware/catalog", HTTP_GET, [this] { handleFirmwareCatalog(); });
  server->on("/api/firmware/download", HTTP_POST, [this] { handleFirmwareOfficialDownload(); });
  server->on("/api/firmware/manual-download", HTTP_POST, [this] { handleFirmwareManualDownload(); });
  server->on("/api/firmware/source", HTTP_GET, [this] { handleFirmwareSource(); });
  server->on("/api/firmware/source", HTTP_POST, [this] { handleFirmwareSource(); });
  server->on("/api/firmware/install", HTTP_POST, [this] { handleFirmwareInstall(); });
  server->on("/api/firmware/cancel", HTTP_POST, [this] { handleFirmwareCancel(); });
  server->on("/api/files", HTTP_GET, [this] { handleFileListData(); });
  server->on("/download", HTTP_GET, [this] { handleDownload(); });

  // Upload endpoint with special handling for multipart form data
  server->on("/upload", HTTP_POST, [this] { handleUploadPost(upload); }, [this] { handleUpload(upload); });

  // Create folder endpoint
  server->on("/mkdir", HTTP_POST, [this] { handleCreateFolder(); });

  // Rename file endpoint
  server->on("/rename", HTTP_POST, [this] { handleRename(); });

  // Move file endpoint
  server->on("/move", HTTP_POST, [this] { handleMove(); });

  // Delete file/folder endpoint
  server->on("/delete", HTTP_POST, [this] { handleDelete(); });

  // Settings endpoints
  server->on("/settings", HTTP_GET, [this] { handleSettingsPage(); });
  server->on("/api/settings", HTTP_GET, [this] { handleGetSettings(); });
  server->on("/api/settings", HTTP_POST, [this] { handlePostSettings(); });

  // Highlights and written notes. These use the academic ClippingStore v4
  // unchanged and keep note records in their own store.
  server->on("/highlights", HTTP_GET, [this] {
    sendStaticContent(server.get(), HighlightsPageHtml, HighlightsPageHtmlCompressedSize, HighlightsPageHtmlETag,
                      "text/html");
  });
  server->on("/api/highlights", HTTP_GET, [this] { handleGetHighlights(); });
  server->on("/api/notes", HTTP_POST, [this] { handlePostNote(); });

  // Font management endpoints
  server->on("/fonts", HTTP_GET, [this] { handleFontsPage(); });
  server->on("/api/fonts", HTTP_GET, [this] { handleFontList(); });
  server->on("/api/fonts/upload", HTTP_POST, [this] { handleFontUpload(); }, [this] { handleFontUploadData(); });
  server->on("/api/fonts/delete", HTTP_POST, [this] { handleFontDelete(); });

  // OPDS server endpoints
  server->on("/api/opds", HTTP_GET, [this] { handleGetOpdsServers(); });
  server->on("/api/opds", HTTP_POST, [this] { handlePostOpdsServer(); });
  server->on("/api/opds/delete", HTTP_POST, [this] { handleDeleteOpdsServer(); });

  // Wi-Fi credential endpoints
  server->on("/api/wifi", HTTP_GET, [this] { handleGetWifiNetworks(); });
  server->on("/api/wifi", HTTP_POST, [this] { handlePostWifiNetwork(); });
  server->on("/api/wifi/delete", HTTP_POST, [this] { handleDeleteWifiNetwork(); });

  server->onNotFound([this] { handleNotFound(); });

  // Collect WebDAV headers and register handler
  const char* davHeaders[] = {"Depth",   "Destination", "Overwrite",     "If",   "Lock-Token",
                              "Timeout", "Origin",      "If-None-Match", "Host", "X-Inkademic-Firmware"};
  server->collectHeaders(davHeaders, 10);
  server->addHandler(new WebDAVHandler());  // Note: WebDAVHandler will be deleted by WebServer when server is stopped

  server->begin();
  restoreFirmwareState();

  // Start WebSocket server for fast binary uploads
  wsServer.reset(new WebSocketsServer(wsPort));
  wsInstance = const_cast<CrossPointWebServer*>(this);
  wsServer->begin();
  wsServer->onEvent(wsEventCallback);

  udpActive = udp.begin(LOCAL_UDP_PORT);
  LOG_DBG("WEB", "Discovery UDP %s on port %d", udpActive ? "enabled" : "failed", LOCAL_UDP_PORT);

  // The main Arduino task remains subscribed to its watchdog. The activity
  // processes one request per outer loop pass so that task is fed between slow
  // browser connections.

  running = true;

  // Show the correct IP based on network mode
  const String ipAddr = apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
  LOG_DBG("WEB", "[MEM] Free heap after server.begin(): %d bytes", ESP.getFreeHeap());
}

void CrossPointWebServer::abortWsUpload(const char* tag) {
  // Explicit close() required: file-scope global persists beyond function scope
  wsUploadFile.close();
  String filePath = wsUploadPath;
  if (!filePath.endsWith("/")) filePath += "/";
  filePath += wsUploadFileName;
  if (Storage.remove(filePath.c_str())) {
    LOG_DBG(tag, "Deleted incomplete upload: %s", filePath.c_str());
  } else {
    LOG_DBG(tag, "Failed to delete incomplete upload: %s", filePath.c_str());
  }
  wsUploadInProgress = false;
  wsUploadClientNum = 255;
  wsLastProgressSent = 0;
}

void CrossPointWebServer::stop() {
  if (!running || !server) {
    LOG_DBG("WEB", "stop() called but already stopped (running=%d, server=%p)", running, server.get());
    return;
  }

  running = false;  // Set this FIRST to prevent handleClient from using server

  LOG_DBG("WEB", "[MEM] Free heap before stop: %d bytes", ESP.getFreeHeap());

  // Close any in-progress WebSocket upload and remove partial file
  if (wsUploadInProgress && wsUploadFile) {
    abortWsUpload("WEB");
  }

  if (firmwareUpload.active) {
    firmwareUpload.file.close();
    firmwareUpload.active = false;
  }
  if (firmwareSignatureActive) {
    firmwareSignatureFile.close();
    firmwareSignatureActive = false;
  }

  // Stop WebSocket server
  if (wsServer) {
    wsServer->close();
    wsServer.reset();
    wsInstance = nullptr;
  }

  if (udpActive) {
    udp.stop();
    udpActive = false;
  }

  // Brief delay to allow any in-flight handleClient() calls to complete
  delay(20);

  server->stop();

  // Brief delay before deletion
  delay(10);

  server.reset();

  // Note: Static upload variables (uploadFileName, uploadPath, uploadError) are declared
  // later in the file and will be cleared when they go out of scope or on next upload
  LOG_DBG("WEB", "[MEM] Free heap final: %d bytes", ESP.getFreeHeap());
}

void CrossPointWebServer::handleClient() {
  static unsigned long lastDebugPrint = 0;

  // Check running flag FIRST before accessing server
  if (!running) {
    return;
  }

  // Double-check server pointer is valid
  if (!server) {
    LOG_DBG("WEB", "WARNING: handleClient called with null server!");
    return;
  }

  // Print debug every 10 seconds to confirm handleClient is being called
  if (millis() - lastDebugPrint > 10000) {
    LOG_DBG("WEB", "handleClient active, server running on port %d", port);
    lastDebugPrint = millis();
  }

  server->handleClient();

  // Handle WebSocket events
  if (wsServer) {
    wsServer->loop();
  }

  // The HTTP handler only queues installation. Run the flash transaction from
  // this activity's main task after the 202 response has been handed to the
  // browser; never erase/write flash inside a WebServer callback.
  processPendingFirmwareDownload();
  processPendingFirmwareInstall();

  // Respond to discovery broadcasts
  if (udpActive) {
    int packetSize = udp.parsePacket();
    if (packetSize > 0) {
      char buffer[16];
      int len = udp.read(buffer, sizeof(buffer) - 1);
      if (len > 0) {
        buffer[len] = '\0';
        if (strcmp(buffer, "hello") == 0) {
          String hostname = WiFi.getHostname();
          if (hostname.isEmpty()) {
            hostname = "crosspoint";
          }
          String message = "crosspoint (on " + hostname + ");" + String(wsPort);
          udp.beginPacket(udp.remoteIP(), udp.remotePort());
          udp.write(reinterpret_cast<const uint8_t*>(message.c_str()), message.length());
          udp.endPacket();
        }
      }
    }
  }
}

CrossPointWebServer::WsUploadStatus CrossPointWebServer::getWsUploadStatus() const {
  WsUploadStatus status;
  status.inProgress = wsUploadInProgress;
  status.received = wsUploadReceived;
  status.total = wsUploadSize;
  status.filename = wsUploadFileName.c_str();
  status.lastCompleteName = wsLastCompleteName.c_str();
  status.lastCompleteSize = wsLastCompleteSize;
  status.lastCompleteAt = wsLastCompleteAt;
  return status;
}

static void sendStaticContent(WebServer* server, const char* data, const size_t len, const char* etag,
                              const char* contentType, const bool gzip, const char* cacheControl) {
  // Static portal content lives in flash for the lifetime of this firmware.
  // Revalidate by ETag so repeat navigation returns a small 304 response while
  // a newly installed firmware immediately supplies its new page.
  if (server->header("If-None-Match") == etag) {
    server->sendHeader("ETag", etag);
    server->sendHeader("Cache-Control", cacheControl);
    server->send(304, contentType, "");
    return;
  }
  if (gzip) server->sendHeader("Content-Encoding", "gzip");
  server->sendHeader("ETag", etag);
  server->sendHeader("Cache-Control", cacheControl);
  server->send_P(200, contentType, data, len);
}

void CrossPointWebServer::handleRoot() const {
  sendStaticContent(server.get(), HomePageHtml, HomePageHtmlCompressedSize, HomePageHtmlETag, "text/html");
}

const char* CrossPointWebServer::firmwareStateName(const FirmwareState state) {
  switch (state) {
    case FirmwareState::IDLE:
      return "idle";
    case FirmwareState::UPLOADING:
      return "uploading";
    case FirmwareState::READY:
      return "ready";
    case FirmwareState::INSTALL_REQUESTED:
      return "install_requested";
    case FirmwareState::INSTALLING:
      return "installing";
    case FirmwareState::REBOOTING:
      return "rebooting";
    case FirmwareState::COMPLETED:
      return "completed";
    case FirmwareState::FAILED:
      return "failed";
    case FirmwareState::INTERRUPTED:
      return "interrupted";
    case FirmwareState::AWAITING_SIGNATURE:
      return "awaiting_signature";
  }
  return "unknown";
}

void CrossPointWebServer::restoreFirmwareState() {
#ifdef SIMULATOR
  firmwareState = FirmwareState::IDLE;
  return;
#else
  const String marker = Storage.readFile(FIRMWARE_STATE_PATH);
  firmwareManual = firmwareStateField(marker, "mode") == "manual";
  firmwareCandidateDevice = firmwareStateField(marker, "device");
  firmwareCandidateVersion = firmwareStateField(marker, "version");
  firmwareCandidateSha256 = firmwareStateField(marker, "sha256");
  if (marker.startsWith("rebooting")) {
    firmwareState = FirmwareState::COMPLETED;
    firmwareError = "Firmware booted after the web update.";
  } else if (marker.startsWith("installing")) {
    firmwareState = FirmwareState::INTERRUPTED;
    firmwareError = "The previous installation was interrupted; the active firmware was retained.";
  } else if (marker.startsWith("failed:")) {
    firmwareState = FirmwareState::FAILED;
    firmwareError = marker.substring(7);
  } else if (marker.startsWith("awaiting_signature") && Storage.exists(FIRMWARE_TEMP_PATH)) {
    firmwareState = FirmwareState::AWAITING_SIGNATURE;
    firmwareUpload.session = firmwareStateField(marker, "session");
    firmwareUpload.fileName = firmwareStateField(marker, "name");
    firmwareUpload.total = firmwareStateField(marker, "total").toInt();
    HalFile file;
    if (Storage.openFileForRead("WEB", FIRMWARE_TEMP_PATH, file) && file) {
      firmwareSize = file.fileSize();
      firmwareUpload.received = firmwareSize;
      firmwareUpload.total = firmwareSize;
      file.close();
    }
  } else if (marker.startsWith("uploading") && Storage.exists(FIRMWARE_TEMP_PATH)) {
    firmwareState = FirmwareState::UPLOADING;
    firmwareUpload.session = firmwareStateField(marker, "session");
    firmwareUpload.fileName = firmwareStateField(marker, "name");
    firmwareUpload.total = firmwareStateField(marker, "total").toInt();
    HalFile file;
    if (Storage.openFileForRead("WEB", FIRMWARE_TEMP_PATH, file) && file) {
      firmwareUpload.received = file.fileSize();
      file.close();
    }
  } else if (marker.startsWith("manual_downloading")) {
    firmwareState = FirmwareState::INTERRUPTED;
    firmwareError = "The manual download was interrupted; download the image again.";
  } else if (marker.startsWith("official_downloading") && Storage.exists(FIRMWARE_TEMP_PATH)) {
    firmwareState = FirmwareState::UPLOADING;
    firmwareUpload.session = "official";
    firmwareUpload.fileName = firmwareStateField(marker, "name");
    HalFile file;
    if (Storage.openFileForRead("WEB", FIRMWARE_TEMP_PATH, file) && file) {
      firmwareUpload.received = file.fileSize();
      firmwareUpload.total = firmwareStateField(marker, "total").toInt();
      firmwareUploadTotal = firmwareUpload.total;
      file.close();
    }
  } else if (marker.startsWith("ready") && Storage.exists(FIRMWARE_PATH) &&
             (firmwareManual || Storage.exists(FIRMWARE_SIGNATURE_PATH))) {
    firmwareState = FirmwareState::READY;
    HalFile file;
    if (Storage.openFileForRead("WEB", FIRMWARE_PATH, file) && file) {
      firmwareSize = file.fileSize();
      file.close();
    }
  }
#endif
}

void CrossPointWebServer::writeFirmwareState(const char* state, const char* detail) const {
#ifndef SIMULATOR
  String marker = state == nullptr ? "idle" : state;
  if (detail != nullptr && detail[0] != '\0') {
    marker += ":";
    marker += detail;
  }
  Storage.writeFile(FIRMWARE_STATE_PATH, marker);
  Storage.writeFile(FIRMWARE_DIAGNOSTIC_PATH, marker);
#else
  (void)state;
  (void)detail;
#endif
}

void CrossPointWebServer::resetFirmwareStaging(const bool removeReadyImage) {
#ifndef SIMULATOR
  firmwareUpload.file.close();
  Storage.remove(FIRMWARE_TEMP_PATH);
  firmwareSignatureFile.close();
  Storage.remove(FIRMWARE_SIGNATURE_TEMP_PATH);
  Storage.remove(FIRMWARE_BACKUP_PATH);
  Storage.remove("/.inkademic-firmware.sig.bak");
  if (removeReadyImage) {
    Storage.remove(FIRMWARE_PATH);
    Storage.remove(FIRMWARE_SIGNATURE_PATH);
  }
#else
  (void)removeReadyImage;
#endif
  firmwareUpload.active = false;
  firmwareUpload.received = 0;
  firmwareUpload.partitionLimit = 0;
  firmwareUpload.total = 0;
  firmwareUpload.session = "";
  firmwareUpload.fileName = "";
  firmwareUpload.error = "";
  firmwareInstallPending = false;
  firmwareDownloadPending = false;
  firmwareManual = false;
  manualUpdater.reset();
  firmwareSize = 0;
  firmwareWritten = 0;
  firmwareTotal = 0;
  firmwareUploadTotal = 0;
  firmwareCandidateDevice = "";
  firmwareCandidateVersion = "";
  firmwareCandidateSha256 = "";
  firmwareSignatureReceived = 0;
  firmwareSignatureActive = false;
}

bool CrossPointWebServer::hasEnoughHeapForFirmwareUpdate(String& reason) const {
#ifdef SIMULATOR
  reason = "";
  return true;
#else
  const size_t freeHeap = ESP.getFreeHeap();
  const size_t maxAlloc = ESP.getMaxAllocHeap();
  if (freeHeap < MIN_FIRMWARE_UPDATE_FREE_HEAP || maxAlloc < MIN_FIRMWARE_UPDATE_MAX_ALLOC) {
    reason = "Not enough free memory for a safe firmware update. Close the reader and retry.";
    LOG_ERR("WEBFW", "Refusing update for low heap: free=%u maxAlloc=%u", static_cast<unsigned>(freeHeap),
            static_cast<unsigned>(maxAlloc));
    return false;
  }
  return true;
#endif
}

void CrossPointWebServer::handleFirmwarePage() const {
  sendStaticContent(server.get(), FirmwarePageHtml, FirmwarePageHtmlCompressedSize, FirmwarePageHtmlETag, "text/html");
}

void CrossPointWebServer::handleFirmwareStatus() const {
  JsonDocument doc;
  doc["state"] = firmwareStateName(firmwareState);
  doc["version"] = INKADEMIC_VERSION;
  doc["device"] = firmware_identity::deviceType();
  doc["identity"] = firmware_identity::marker();
  doc["received"] = (firmwareState == FirmwareState::UPLOADING || firmwareState == FirmwareState::AWAITING_SIGNATURE)
                        ? firmwareUpload.received
                        : firmwareWritten;
  doc["mode"] = firmwareManual ? "manual" : "signed";
  doc["size"] = firmwareSize;
  doc["total"] = firmwareUpload.total > 0 ? firmwareUpload.total : firmwareUploadTotal;
  doc["partitionLimit"] = firmwareUpload.partitionLimit;
  doc["session"] = firmwareUpload.session;
  doc["filename"] = firmwareUpload.fileName;
  doc["signaturePresent"] = Storage.exists(FIRMWARE_SIGNATURE_PATH) || Storage.exists(FIRMWARE_SIGNATURE_TEMP_PATH);
  doc["signatureVerified"] = !firmwareManual && firmwareState == FirmwareState::READY;
  doc["candidateDevice"] = firmwareCandidateDevice;
  doc["candidateVersion"] = firmwareCandidateVersion;
  doc["candidateSha256"] = firmwareCandidateSha256;
  doc["freeHeap"] = ESP.getFreeHeap();
  doc["maxAllocHeap"] = ESP.getMaxAllocHeap();
  doc["installSupported"] =
#ifdef SIMULATOR
      false;
#else
      true;
#endif
  if (!firmwareError.isEmpty()) doc["error"] = firmwareError;

  String response;
  serializeJson(doc, response);
  server->send(200, "application/json", response);
}

bool CrossPointWebServer::firmwareRequestAllowed() const {
  const String origin = server->header("Origin");
  const String intent = server->header("X-Inkademic-Firmware");
  const String host = server->header("Host");
  return firmware_request::allowed(origin.c_str(), host.c_str(), intent.c_str());
}

bool CrossPointWebServer::requireFirmwareRequest() const {
  if (firmwareRequestAllowed()) return true;
  server->send(403, "application/json", "{\"error\":\"Open firmware controls on the reader's own web page.\"}");
  return false;
}

void CrossPointWebServer::handleFirmwareUpload() {
#ifdef SIMULATOR
  return;
#else
  HTTPUpload& upload = server->upload();

  if (upload.status == UPLOAD_FILE_START) {
    if (!firmwareRequest.start()) return;
    if (!firmwareRequestAllowed()) {
      firmwareRequest.reject("Firmware request origin or header rejected.");
      return;
    }
    if (firmwareUpload.active || firmwareInstallPending || firmwareState == FirmwareState::INSTALLING ||
        firmwareDownloadPending) {
      firmwareRequest.reject("A firmware transaction is already in progress.");
      return;
    }

    String lowerName = upload.filename;
    lowerName.toLowerCase();
    if (!lowerName.endsWith(".bin") || !firmware_request::validStateValue(upload.filename.c_str())) {
      firmwareRequest.reject("Select an ESP firmware .bin file.");
      return;
    }

    String memoryError;
    if (!hasEnoughHeapForFirmwareUpdate(memoryError)) {
      firmwareRequest.reject("Not enough memory for a firmware upload.");
      return;
    }

    const esp_partition_t* destination = esp_ota_get_next_update_partition(nullptr);
    if (!destination) {
      firmwareRequest.reject("No OTA application partition is available.");
      return;
    }

    const String mode = server->arg("mode");
    if (!mode.isEmpty() && mode != "manual" && mode != "signed") {
      firmwareRequest.reject("Unknown firmware installation mode.");
      return;
    }
    const bool manual = mode == "manual";
    const size_t offset = parseFirmwareSizeArg(server.get(), "offset", 0);
    const size_t total = parseFirmwareSizeArg(server.get(), "total", upload.totalSize);
    String session = server->arg("session");
    if (session.isEmpty()) session = "legacy";
    if (!firmware_request::validStateValue(session.c_str())) {
      firmwareRequest.reject("Invalid upload session identifier.");
      return;
    }

    if (total > destination->size || offset > destination->size || (total > 0 && offset > total)) {
      firmwareRequest.reject("The upload size or offset exceeds the firmware limits.");
      return;
    }
    if (offset == 0) {
      resetFirmwareStaging(true);
      firmwareManual = manual;
      firmwareUpload.session = session;
      firmwareUpload.fileName = upload.filename;
      firmwareUpload.total = total;
      firmwareUploadTotal = total;
      if (!Storage.openFileForWrite("WEBFW", FIRMWARE_TEMP_PATH, firmwareUpload.file)) {
        firmwareRequest.reject("Could not create the temporary firmware file on the SD card.");
        return;
      }
    } else {
      if (firmwareState != FirmwareState::UPLOADING || firmwareUpload.session != session ||
          firmwareUpload.received != offset || firmwareManual != manual || !Storage.exists(FIRMWARE_TEMP_PATH)) {
        firmwareRequest.reject("The upload offset does not match the saved partial image.");
        return;
      }
      if (total > 0) firmwareUpload.total = total;
      firmwareUpload.file = Storage.open(FIRMWARE_TEMP_PATH, O_WRONLY | O_APPEND);
      if (!firmwareUpload.file) {
        firmwareRequest.reject("Could not reopen the partial firmware image.");
        return;
      }
    }
    firmwareUpload.partitionLimit = destination->size;
    if (offset == 0) firmwareUpload.received = 0;
    firmwareUpload.error = "";
    firmwareUpload.active = true;
    firmwareRequest.accept();
    firmwareState = FirmwareState::UPLOADING;
    firmwareError = "";
    String stateDetail = "session=" + firmwareUpload.session + ";name=" + firmwareUpload.fileName +
                         ";received=" + String(firmwareUpload.received) + ";total=" + String(firmwareUpload.total) +
                         ";mode=" + (firmwareManual ? "manual" : "signed");
    writeFirmwareState("uploading", stateDetail.c_str());
    return;
  }

  if (!firmwareRequest.accepted()) {
    if (upload.status == UPLOAD_FILE_ABORTED) firmwareRequest.finish();
    return;
  }

  if (upload.status == UPLOAD_FILE_WRITE) {
    if (!firmwareUpload.active || !firmwareUpload.error.isEmpty()) return;
    const size_t nextSize = firmwareUpload.received + upload.currentSize;
    if (nextSize < firmwareUpload.received || nextSize > firmwareUpload.partitionLimit ||
        (firmwareUpload.total > 0 && nextSize > firmwareUpload.total)) {
      firmwareUpload.error = "The image is larger than the next OTA partition.";
      firmwareUpload.file.close();
      Storage.remove(FIRMWARE_TEMP_PATH);
      firmwareUpload.active = false;
      firmwareState = FirmwareState::FAILED;
      firmwareError = firmwareUpload.error;
      writeFirmwareState("failed", firmwareError.c_str());
      return;
    }
    const size_t written = firmwareUpload.file.write(upload.buf, upload.currentSize);
    if (written != upload.currentSize) {
      firmwareUpload.error = "The firmware upload could not be written completely to the SD card.";
      firmwareUpload.file.close();
      Storage.remove(FIRMWARE_TEMP_PATH);
      firmwareUpload.active = false;
      firmwareState = FirmwareState::FAILED;
      firmwareError = firmwareUpload.error;
      writeFirmwareState("failed", firmwareError.c_str());
      return;
    }
    firmwareUpload.received = nextSize;
    return;
  }

  if (upload.status != UPLOAD_FILE_END && upload.status != UPLOAD_FILE_ABORTED) return;

  if (!firmwareUpload.active) {
    if (upload.status == UPLOAD_FILE_ABORTED) firmwareRequest.finish();
    return;
  }

  if (upload.status == UPLOAD_FILE_ABORTED) {
    firmwareRequest.finish();
    firmwareUpload.file.sync();
    firmwareUpload.file.close();
    firmwareUpload.active = false;
    firmwareState = FirmwareState::UPLOADING;
    firmwareError = "The connection stopped; the partial image was kept so the browser can resume it.";
    String stateDetail = "session=" + firmwareUpload.session + ";name=" + firmwareUpload.fileName +
                         ";received=" + String(firmwareUpload.received) + ";total=" + String(firmwareUpload.total) +
                         ";mode=" + (firmwareManual ? "manual" : "signed");
    writeFirmwareState("uploading", stateDetail.c_str());
    return;
  }

  const bool synced = firmwareUpload.file.sync();
  firmwareUpload.file.close();
  firmwareUpload.active = false;
  if (!synced || !firmwareUpload.error.isEmpty()) {
    firmwareError =
        firmwareUpload.error.isEmpty() ? "The temporary firmware file could not be finalized." : firmwareUpload.error;
    firmwareState = FirmwareState::FAILED;
    Storage.remove(FIRMWARE_TEMP_PATH);
    writeFirmwareState("failed", firmwareError.c_str());
    return;
  }

  if (firmwareUpload.total > 0 && firmwareUpload.received != firmwareUpload.total) {
    firmwareState = FirmwareState::UPLOADING;
    String stateDetail = "session=" + firmwareUpload.session + ";name=" + firmwareUpload.fileName +
                         ";received=" + String(firmwareUpload.received) + ";total=" + String(firmwareUpload.total) +
                         ";mode=" + (firmwareManual ? "manual" : "signed");
    writeFirmwareState("uploading", stateDetail.c_str());
    return;
  }

  const auto validation = firmware_flash::validateImageFile(FIRMWARE_TEMP_PATH, firmwareUpload.partitionLimit);
  if (validation != firmware_flash::Result::OK) {
    firmwareError = String("Firmware validation failed: ") + firmware_flash::resultName(validation);
    firmwareState = FirmwareState::FAILED;
    Storage.remove(FIRMWARE_TEMP_PATH);
    writeFirmwareState("failed", firmwareError.c_str());
    return;
  }

  firmwareSize = firmwareUpload.received;
  if (firmwareManual) {
    finalizeFirmwareCandidate(FIRMWARE_TEMP_PATH, FIRMWARE_SIGNATURE_TEMP_PATH, firmwareSize, false);
    return;
  }
  firmwareError = "";
  firmwareState = FirmwareState::AWAITING_SIGNATURE;
  const String stateDetail = String("session=") + firmwareUpload.session + ";name=" + firmwareUpload.fileName +
                             ";received=" + String(firmwareUpload.received) + ";total=" + String(firmwareUpload.total) +
                             ";mode=" + (firmwareManual ? "manual" : "signed");
  writeFirmwareState("awaiting_signature", stateDetail.c_str());
#endif
}

void CrossPointWebServer::handleFirmwareUploadPost() {
#ifdef SIMULATOR
  server->send(501, "application/json", "{\"error\":\"Firmware updates are not available in the simulator.\"}");
#else
  const auto request = firmwareRequest;
  firmwareRequest.finish();
  if (!requireFirmwareRequest()) return;
  if (request.error()) {
    JsonDocument error;
    error["error"] = request.error();
    String response;
    serializeJson(error, response);
    server->send(409, "application/json", response);
    return;
  }
  if (!request.accepted()) {
    server->send(400, "application/json", "{\"error\":\"No firmware file received in this request.\"}");
    return;
  }
  JsonDocument doc;
  doc["state"] = firmwareStateName(firmwareState);
  doc["size"] = firmwareSize;
  doc["received"] = firmwareUpload.received;
  doc["total"] = firmwareUpload.total;
  if (!firmwareError.isEmpty()) doc["error"] = firmwareError;
  String response;
  serializeJson(doc, response);
  const bool accepted = firmwareState == FirmwareState::UPLOADING ||
                        firmwareState == FirmwareState::AWAITING_SIGNATURE || firmwareState == FirmwareState::READY;
  server->send(accepted ? 200 : 400, "application/json", response);
#endif
}

bool CrossPointWebServer::finalizeFirmwareCandidate(const char* imagePath, const char* signaturePath,
                                                    const size_t imageSize, const bool fromOfficial) {
#ifdef SIMULATOR
  (void)imagePath;
  (void)signaturePath;
  (void)imageSize;
  (void)fromOfficial;
  return false;
#else
  const esp_partition_t* destination = esp_ota_get_next_update_partition(nullptr);
  if (destination == nullptr) {
    firmwareError = "No OTA application partition is available.";
    firmwareState = FirmwareState::FAILED;
    writeFirmwareState("failed", firmwareError.c_str());
    return false;
  }

  char candidateDevice[32] = {};
  char candidateVersion[64] = {};
  uint8_t candidateDigest[32];
  const auto validation = firmware_flash::validateBrowserImageFile(
      imagePath, destination->size, firmware_identity::deviceType(), INKADEMIC_VERSION, signaturePath, candidateDevice,
      sizeof(candidateDevice), candidateVersion, sizeof(candidateVersion), candidateDigest,
      firmwareManual ? firmware_flash::ValidationPolicy::Manual : firmware_flash::ValidationPolicy::SignedRelease);
  if (validation != firmware_flash::Result::OK) {
    firmwareError = String("Firmware validation failed: ") + firmware_flash::resultName(validation);
    firmwareState = FirmwareState::FAILED;
    writeFirmwareState("failed", firmwareError.c_str());
    Storage.remove(imagePath);
    Storage.remove(signaturePath);
    return false;
  }

  const char* signatureBackupPath = "/.inkademic-firmware.sig.bak";
  Storage.remove(FIRMWARE_BACKUP_PATH);
  Storage.remove(signatureBackupPath);
  const bool hadReadyImage = Storage.exists(FIRMWARE_PATH);
  const bool hadReadySignature = Storage.exists(FIRMWARE_SIGNATURE_PATH);
  if (hadReadyImage && !Storage.rename(FIRMWARE_PATH, FIRMWARE_BACKUP_PATH)) {
    firmwareError = "Could not preserve the previously staged firmware.";
    firmwareState = FirmwareState::FAILED;
    writeFirmwareState("failed", firmwareError.c_str());
    return false;
  }
  if (hadReadySignature && !Storage.rename(FIRMWARE_SIGNATURE_PATH, signatureBackupPath)) {
    if (hadReadyImage) Storage.rename(FIRMWARE_BACKUP_PATH, FIRMWARE_PATH);
    firmwareError = "Could not preserve the previous firmware signature.";
    firmwareState = FirmwareState::FAILED;
    writeFirmwareState("failed", firmwareError.c_str());
    return false;
  }
  if (!Storage.rename(imagePath, FIRMWARE_PATH) ||
      (!firmwareManual && !Storage.rename(signaturePath, FIRMWARE_SIGNATURE_PATH))) {
    Storage.remove(FIRMWARE_PATH);
    Storage.remove(FIRMWARE_SIGNATURE_PATH);
    if (hadReadyImage) Storage.rename(FIRMWARE_BACKUP_PATH, FIRMWARE_PATH);
    if (hadReadySignature) Storage.rename(signatureBackupPath, FIRMWARE_SIGNATURE_PATH);
    firmwareError = "Could not atomically promote the validated firmware and signature.";
    firmwareState = FirmwareState::FAILED;
    writeFirmwareState("failed", firmwareError.c_str());
    return false;
  }
  Storage.remove(FIRMWARE_BACKUP_PATH);
  Storage.remove(signatureBackupPath);

  firmwareSize = imageSize;
  firmwareUpload.received = imageSize;
  firmwareUpload.total = imageSize;
  firmwareWritten = 0;
  firmwareTotal = imageSize;
  firmwareCandidateDevice = candidateDevice;
  firmwareCandidateVersion = candidateVersion;
  char digestHex[65];
  for (size_t i = 0; i < 32; ++i) snprintf(digestHex + i * 2, 3, "%02x", candidateDigest[i]);
  firmwareCandidateSha256 = digestHex;
  firmwareError = "";
  firmwareState = FirmwareState::READY;
  const String detail = String("version=") + candidateVersion + ";device=" + candidateDevice +
                        ";source=" + (fromOfficial ? "official" : "browser") +
                        ";mode=" + (firmwareManual ? "manual" : "signed") + ";sha256=" + firmwareCandidateSha256;
  writeFirmwareState("ready", detail.c_str());
  return true;
#endif
}

void CrossPointWebServer::handleFirmwareSignatureUpload() {
#ifdef SIMULATOR
  return;
#else
  HTTPUpload& upload = server->upload();
  if (upload.status == UPLOAD_FILE_START) {
    if (!signatureRequest.start()) return;
    if (!firmwareRequestAllowed()) {
      signatureRequest.reject("Firmware request origin or header rejected.");
      return;
    }
    if (firmwareManual || firmwareState != FirmwareState::AWAITING_SIGNATURE || firmwareInstallPending ||
        firmwareDownloadPending) {
      signatureRequest.reject("Upload the complete firmware image before its signature.");
      return;
    }
    firmwareSignatureFile.close();
    Storage.remove(FIRMWARE_SIGNATURE_TEMP_PATH);
    if (!Storage.openFileForWrite("WEBFWSIG", FIRMWARE_SIGNATURE_TEMP_PATH, firmwareSignatureFile)) {
      signatureRequest.reject("Could not create the temporary signature file on the SD card.");
      firmwareError = "Could not create the temporary signature file on the SD card.";
      firmwareState = FirmwareState::AWAITING_SIGNATURE;
      writeFirmwareState("awaiting_signature", "signature=retry");
      return;
    }
    firmwareSignatureReceived = 0;
    firmwareSignatureActive = true;
    signatureRequest.accept();
    return;
  }
  if (!signatureRequest.accepted() || !firmwareSignatureActive) {
    if (upload.status == UPLOAD_FILE_ABORTED) signatureRequest.finish();
    return;
  }

  if (upload.status == UPLOAD_FILE_WRITE) {
    if (!firmwareSignatureActive) return;
    if (firmwareSignatureReceived + upload.currentSize > FIRMWARE_SIGNATURE_SIZE) {
      firmwareSignatureFile.close();
      firmwareSignatureActive = false;
      Storage.remove(FIRMWARE_SIGNATURE_TEMP_PATH);
      firmwareError = "The Ed25519 signature must contain exactly 64 bytes.";
      firmwareState = FirmwareState::AWAITING_SIGNATURE;
      writeFirmwareState("awaiting_signature", "signature=retry");
      return;
    }
    if (firmwareSignatureFile.write(upload.buf, upload.currentSize) != upload.currentSize) {
      firmwareSignatureFile.close();
      firmwareSignatureActive = false;
      firmwareError = "The signature could not be written to the SD card.";
      firmwareState = FirmwareState::AWAITING_SIGNATURE;
      writeFirmwareState("awaiting_signature", "signature=retry");
      return;
    }
    firmwareSignatureReceived += upload.currentSize;
    return;
  }
  if (upload.status == UPLOAD_FILE_ABORTED) {
    signatureRequest.finish();
    firmwareSignatureFile.close();
    firmwareSignatureActive = false;
    Storage.remove(FIRMWARE_SIGNATURE_TEMP_PATH);
    firmwareError = "The signature upload was cancelled.";
    firmwareState = FirmwareState::AWAITING_SIGNATURE;
    writeFirmwareState("awaiting_signature", "signature=retry");
    return;
  }
  if (upload.status != UPLOAD_FILE_END) return;

  const bool synced = firmwareSignatureFile.sync();
  firmwareSignatureFile.close();
  firmwareSignatureActive = false;
  if (!synced || firmwareSignatureReceived != FIRMWARE_SIGNATURE_SIZE) {
    firmwareError = "The Ed25519 signature must contain exactly 64 bytes.";
    firmwareState = FirmwareState::AWAITING_SIGNATURE;
    writeFirmwareState("awaiting_signature", "signature=retry");
    return;
  }
  finalizeFirmwareCandidate(FIRMWARE_TEMP_PATH, FIRMWARE_SIGNATURE_TEMP_PATH, firmwareSize, false);
#endif
}

void CrossPointWebServer::handleFirmwareSignatureUploadPost() {
#ifdef SIMULATOR
  server->send(501, "application/json", "{\"error\":\"Firmware updates are not available in the simulator.\"}");
#else
  const auto request = signatureRequest;
  signatureRequest.finish();
  if (!requireFirmwareRequest()) return;
  if (request.error()) {
    server->send(409, "application/json", "{\"error\":\"Signature upload rejected; staged image retained.\"}");
    return;
  }
  if (!request.accepted()) {
    server->send(400, "application/json", "{\"error\":\"No firmware file received in this request.\"}");
    return;
  }
  JsonDocument doc;
  doc["state"] = firmwareStateName(firmwareState);
  if (!firmwareError.isEmpty()) doc["error"] = firmwareError;
  String response;
  serializeJson(doc, response);
  server->send(firmwareState == FirmwareState::READY ? 200 : 400, "application/json", response);
#endif
}

void CrossPointWebServer::handleFirmwareInstall() {
  if (!requireFirmwareRequest()) return;
#ifdef SIMULATOR
  server->send(501, "application/json", "{\"error\":\"Firmware updates are not available in the simulator.\"}");
#else
  if (firmwareState != FirmwareState::READY || !Storage.exists(FIRMWARE_PATH) ||
      (!firmwareManual && !Storage.exists(FIRMWARE_SIGNATURE_PATH))) {
    server->send(409, "application/json", "{\"error\":\"Upload and validate a firmware image first.\"}");
    return;
  }
  if (firmwareCandidateSha256.length() != 64 || server->arg("sha256") != firmwareCandidateSha256 ||
      server->arg("mode") != (firmwareManual ? "manual" : "signed")) {
    server->send(409, "application/json",
                 "{\"error\":\"The selected image changed or needs validation. Upload or download it again before "
                 "installing.\"}");
    return;
  }
  if (firmwareInstallPending || firmwareState == FirmwareState::INSTALLING) {
    server->send(409, "application/json", "{\"error\":\"A firmware installation is already in progress.\"}");
    return;
  }
  String memoryError;
  if (!hasEnoughHeapForFirmwareUpdate(memoryError)) {
    firmwareError = memoryError;
    firmwareState = FirmwareState::FAILED;
    writeFirmwareState("failed", firmwareError.c_str());
    server->send(503, "application/json", String("{\"error\":\"") + firmwareError + "\"}");
    return;
  }
  firmwareInstallPending = true;
  firmwareState = FirmwareState::INSTALL_REQUESTED;
  firmwareError = "";
  const String detail = String("version=") + firmwareCandidateVersion + ";device=" + firmwareCandidateDevice;
  writeFirmwareState("queued", detail.c_str());
  server->send(
      202, "application/json",
      "{\"state\":\"install_requested\",\"message\":\"The device will install the validated image and reboot.\"}");
#endif
}

void CrossPointWebServer::handleFirmwareCatalog() {
  if (!requireFirmwareRequest()) return;
#ifdef SIMULATOR
  server->send(501, "application/json", "{\"error\":\"The official catalog is unavailable in the simulator.\"}");
#else
  String memoryError;
  if (!hasEnoughHeapForFirmwareUpdate(memoryError)) {
    server->send(503, "application/json", String("{\"error\":\"") + memoryError + "\"}");
    return;
  }
  if (!officialUpdater) officialUpdater.reset(new (std::nothrow) OtaUpdater());
  if (!officialUpdater) {
    server->send(503, "application/json", "{\"error\":\"Not enough memory for the official catalog.\"}");
    return;
  }
  const auto result = officialUpdater->checkForUpdate();
  JsonDocument doc;
  doc["device"] = firmware_identity::deviceType();
  doc["currentVersion"] = INKADEMIC_VERSION;
  doc["state"] = firmwareStateName(firmwareState);
  doc["available"] = result == OtaUpdater::OK && officialUpdater->isUpdateNewer();
  if (result == OtaUpdater::OK) {
    doc["version"] = officialUpdater->getLatestVersion();
    doc["size"] = officialUpdater->getOtaSize();
    doc["sha256"] = officialUpdater->getLatestSha256();
    doc["signatureSize"] = officialUpdater->getLatestSignatureSize();
    doc["downloadUrl"] = officialUpdater->getLatestUrl();
    doc["signatureUrl"] = officialUpdater->getLatestSignatureUrl();
  } else {
    doc["error"] = "Could not read the official signed INKademic release catalog.";
    doc["code"] = static_cast<int>(result);
  }
  String response;
  serializeJson(doc, response);
  server->send(result == OtaUpdater::OK ? 200 : 502, "application/json", response);
#endif
}

void CrossPointWebServer::handleFirmwareOfficialDownload() {
  if (!requireFirmwareRequest()) return;
#ifdef SIMULATOR
  server->send(501, "application/json", "{\"error\":\"Firmware updates are unavailable in the simulator.\"}");
#else
  if (firmwareDownloadPending || firmwareInstallPending || firmwareState == FirmwareState::INSTALLING) {
    server->send(409, "application/json", "{\"error\":\"A firmware transaction is already in progress.\"}");
    return;
  }
  String memoryError;
  if (!hasEnoughHeapForFirmwareUpdate(memoryError)) {
    server->send(503, "application/json", String("{\"error\":\"") + memoryError + "\"}");
    return;
  }
  if (!officialUpdater) officialUpdater.reset(new (std::nothrow) OtaUpdater());
  if (!officialUpdater) {
    server->send(503, "application/json", "{\"error\":\"Not enough memory for the official catalog.\"}");
    return;
  }
  if (!officialUpdater->isUpdateNewer()) {
    const auto checkResult = officialUpdater->checkForUpdate();
    if (checkResult != OtaUpdater::OK || !officialUpdater->isUpdateNewer()) {
      server->send(409, "application/json", "{\"error\":\"No newer signed firmware is available for this device.\"}");
      return;
    }
  }
  resetFirmwareStaging(true);
  firmwareUpload.fileName = String("official-") + officialUpdater->getLatestVersion().c_str() + ".bin";
  firmwareUpload.total = officialUpdater->getOtaSize();
  firmwareUploadTotal = firmwareUpload.total;
  firmwareDownloadPending = true;
  firmwareState = FirmwareState::UPLOADING;
  firmwareError = "";
  const String stateDetail = String("name=") + firmwareUpload.fileName + ";total=" + String(firmwareUpload.total) +
                             ";mode=" + (firmwareManual ? "manual" : "signed");
  writeFirmwareState("official_downloading", stateDetail.c_str());
  server->send(202, "application/json", "{\"state\":\"uploading\",\"source\":\"official\"}");
#endif
}

void CrossPointWebServer::handleFirmwareManualDownload() {
  if (!requireFirmwareRequest()) return;
#ifdef SIMULATOR
  server->send(501, "application/json", "{\"error\":\"Firmware updates are unavailable in the simulator.\"}");
#else
  if (firmwareDownloadPending || firmwareInstallPending || firmwareUpload.active || firmwareSignatureActive ||
      firmwareState == FirmwareState::INSTALLING) {
    server->send(409, "application/json", "{\"error\":\"A firmware transaction is already in progress.\"}");
    return;
  }
  const String url = server->arg("url");
  if (server->arg("mode") != "manual" || !firmware_request::validManualUrl(url.c_str())) {
    server->send(400, "application/json", "{\"error\":\"Choose manual mode and a direct HTTP(S) firmware URL.\"}");
    return;
  }
  String memoryError;
  if (!hasEnoughHeapForFirmwareUpdate(memoryError)) {
    server->send(503, "application/json", "{\"error\":\"Close the reader and retry with more free memory.\"}");
    return;
  }
  // One updater (bounded URL <= 1024 bytes), retained until download completes;
  // it cannot live on this request's stack because work runs after the response.
  auto updater = makeUniqueNoThrow<OtaUpdater>();
  if (!updater) {
    LOG_ERR("WEBFW", "OOM creating manual OTA updater");
    server->send(503, "application/json", "{\"error\":\"Not enough memory for firmware download.\"}");
    return;
  }
  if (!updater->setManualSource(url.c_str())) {
    server->send(400, "application/json", "{\"error\":\"Invalid firmware URL.\"}");
    return;
  }
  resetFirmwareStaging(true);
  manualUpdater = std::move(updater);
  firmwareManual = true;
  firmwareUpload.fileName = "manual-ota.bin";
  firmwareDownloadPending = true;
  firmwareState = FirmwareState::UPLOADING;
  firmwareError = "";
  writeFirmwareState("manual_downloading", "mode=manual");
  server->send(202, "application/json", "{\"state\":\"uploading\",\"mode\":\"manual\"}");
#endif
}

void CrossPointWebServer::handleFirmwareSource() {
  if (!requireFirmwareRequest()) return;
#ifdef SIMULATOR
  server->send(501, "application/json", "{\"error\":\"OTA sources are unavailable in the simulator.\"}");
#else
  if (server->method() == HTTP_POST) {
    const String mode = server->arg("mode");
    const String url = server->arg("url");
    if (mode == "official") {
      if (Storage.exists(OtaUpdater::MANUAL_SOURCE_PATH) && !Storage.remove(OtaUpdater::MANUAL_SOURCE_PATH)) {
        server->send(500, "application/json", "{\"error\":\"Could not restore the official OTA source.\"}");
        return;
      }
    } else if (mode == "manual" && firmware_request::validManualUrl(url.c_str())) {
      if (!Storage.writeFile(OtaUpdater::MANUAL_SOURCE_PATH, url)) {
        server->send(500, "application/json", "{\"error\":\"Could not save the OTA source.\"}");
        return;
      }
    } else {
      server->send(400, "application/json",
                   "{\"error\":\"Choose the official source or a direct HTTP(S) firmware URL.\"}");
      return;
    }
  }
  OtaUpdater source;
  if (source.loadSavedSource() != OtaUpdater::OK) {
    server->send(400, "application/json",
                 "{\"error\":\"Saved OTA source is invalid. Save a new URL or restore the official source.\"}");
    return;
  }
  JsonDocument doc;
  doc["mode"] = source.isManualSource() ? "manual" : "official";
  doc["url"] = source.getLatestUrl();
  String response;
  serializeJson(doc, response);
  server->send(200, "application/json", response);
#endif
}

void CrossPointWebServer::handleFirmwareCancel() {
  if (!requireFirmwareRequest()) return;
  if (firmwareState == FirmwareState::INSTALLING || firmwareState == FirmwareState::REBOOTING) {
    server->send(409, "application/json", "{\"error\":\"The firmware is already being installed.\"}");
    return;
  }
  resetFirmwareStaging(true);
  firmwareState = FirmwareState::IDLE;
  firmwareError = "";
  writeFirmwareState("idle");
  server->send(200, "application/json", "{\"state\":\"idle\"}");
}

void CrossPointWebServer::firmwareProgress(const size_t written, const size_t total, void* context) {
  auto* self = static_cast<CrossPointWebServer*>(context);
  if (self == nullptr) return;
  self->firmwareWritten = written;
  self->firmwareTotal = total;
}

void CrossPointWebServer::officialFirmwareProgress(void* context) {
  auto* self = static_cast<CrossPointWebServer*>(context);
  if (self == nullptr) return;
  auto* updater = self->firmwareManual ? self->manualUpdater.get() : self->officialUpdater.get();
  if (!updater) return;
  self->firmwareWritten = updater->getProcessedSize();
  self->firmwareTotal = updater->getTotalSize();
  self->firmwareUpload.received = self->firmwareWritten;
  self->firmwareUpload.total = self->firmwareTotal;
}

void CrossPointWebServer::processPendingFirmwareDownload() {
#ifdef SIMULATOR
  return;
#else
  if (!firmwareDownloadPending) return;
  firmwareDownloadPending = false;
  auto* updater = firmwareManual ? manualUpdater.get() : officialUpdater.get();
  if (!updater) {
    firmwareState = FirmwareState::FAILED;
    firmwareError = "The download session was lost; choose the firmware source again.";
    writeFirmwareState("failed", firmwareError.c_str());
    return;
  }

  firmwareWritten = 0;
  firmwareTotal = firmwareUpload.total;
  delay(100);
  const auto result = firmwareManual ? updater->downloadManualToFile(FIRMWARE_TEMP_PATH, officialFirmwareProgress, this)
                                     : updater->downloadLatestToFiles(FIRMWARE_TEMP_PATH, FIRMWARE_SIGNATURE_TEMP_PATH,
                                                                      officialFirmwareProgress, this);
  if (result != OtaUpdater::OK) {
    firmwareState = FirmwareState::FAILED;
    firmwareError = String("Firmware download failed: code=") + String(static_cast<int>(result));
    writeFirmwareState("failed", firmwareError.c_str());
    return;
  }

  HalFile image;
  if (!Storage.openFileForRead("WEBFW", FIRMWARE_TEMP_PATH, image) || !image) {
    firmwareState = FirmwareState::FAILED;
    firmwareError = "The downloaded firmware file could not be reopened.";
    writeFirmwareState("failed", firmwareError.c_str());
    return;
  }
  firmwareSize = image.fileSize();
  image.close();
  finalizeFirmwareCandidate(FIRMWARE_TEMP_PATH, FIRMWARE_SIGNATURE_TEMP_PATH, firmwareSize, !firmwareManual);
  manualUpdater.reset();
#endif
}

void CrossPointWebServer::processPendingFirmwareInstall() {
#ifdef SIMULATOR
  return;
#else
  if (!firmwareInstallPending) return;
  firmwareInstallPending = false;
  firmwareState = FirmwareState::INSTALLING;
  firmwareWritten = 0;
  firmwareTotal = firmwareSize;
  const String detail = String("version=") + firmwareCandidateVersion + ";device=" + firmwareCandidateDevice;
  writeFirmwareState("installing", detail.c_str());

  String memoryError;
  if (!hasEnoughHeapForFirmwareUpdate(memoryError)) {
    firmwareState = FirmwareState::FAILED;
    firmwareError = memoryError;
    writeFirmwareState("failed", firmwareError.c_str());
    return;
  }

  const esp_partition_t* destination = esp_ota_get_next_update_partition(nullptr);
  if (destination == nullptr) {
    firmwareState = FirmwareState::FAILED;
    firmwareError = "No OTA application partition is available.";
    writeFirmwareState("failed", firmwareError.c_str());
    return;
  }
  uint8_t authenticatedDigest[32];
  char candidateDevice[32] = {};
  char candidateVersion[64] = {};
  const auto finalValidation = firmware_flash::validateBrowserImageFile(
      FIRMWARE_PATH, destination->size, firmware_identity::deviceType(), INKADEMIC_VERSION, FIRMWARE_SIGNATURE_PATH,
      candidateDevice, sizeof(candidateDevice), candidateVersion, sizeof(candidateVersion), authenticatedDigest,
      firmwareManual ? firmware_flash::ValidationPolicy::Manual : firmware_flash::ValidationPolicy::SignedRelease);
  if (finalValidation != firmware_flash::Result::OK) {
    firmwareState = FirmwareState::FAILED;
    firmwareError = String("Final firmware validation failed: ") + firmware_flash::resultName(finalValidation);
    writeFirmwareState("failed", firmwareError.c_str());
    return;
  }

  char digestHex[65];
  for (size_t i = 0; i < 32; ++i) snprintf(digestHex + i * 2, 3, "%02x", authenticatedDigest[i]);
  if (firmwareManual && firmwareCandidateSha256 != digestHex) {
    firmwareState = FirmwareState::FAILED;
    firmwareError = "The staged file changed; upload or download it again before installing.";
    writeFirmwareState("failed", firmwareError.c_str());
    return;
  }

  // Let the HTTP response leave the socket before the long flash transaction
  // blocks this activity's request loop.
  delay(150);
  const auto result = firmware_flash::flashFromSdPath(FIRMWARE_PATH, firmwareProgress, this, true, authenticatedDigest);
  if (result != firmware_flash::Result::OK) {
    firmwareState = FirmwareState::FAILED;
    firmwareError = String("Firmware installation failed: ") + firmware_flash::resultName(result);
    writeFirmwareState("failed", firmwareError.c_str());
    return;
  }

  firmwareState = FirmwareState::REBOOTING;
  writeFirmwareState("rebooting");
  delay(250);
  ESP.restart();
#endif
}

void CrossPointWebServer::handleJszip() const {
  sendStaticContent(server.get(), jszip_minJs, jszip_minJsCompressedSize, jszip_minJsETag, "application/javascript");
}

// Shared stylesheet and logo are referenced with a content-hashed ?v= query,
// so they can be cached aggressively: a new build changes the URL.
void CrossPointWebServer::handleStyleCss() const {
  sendStaticContent(server.get(), StyleCss, StyleCssCompressedSize, StyleCssETag, "text/css", true,
                    "public, max-age=31536000, immutable");
}

void CrossPointWebServer::handleLogo() const {
  sendStaticContent(server.get(), LogoPng, LogoPngSize, LogoPngETag, "image/png", false,
                    "public, max-age=31536000, immutable");
}

void CrossPointWebServer::handleNotFound() const {
  // in AP mode, redirect unmatched browser/captive-portal requests to "/" so the OS auto-opens the browser
  // API requests (/api/*) still return 404 so XHR errors surface correctly
  // see https://en.wikipedia.org/wiki/Captive_portal#Detection
  if (apMode && !server->uri().startsWith("/api/")) {
    server->sendHeader("Location", "/", true);
    server->send(302, "text/plain", "");
    return;
  }

  String message = "404 Not Found\n\n";
  message += "URI: " + server->uri() + "\n";
  server->send(404, "text/plain", message);
}

void CrossPointWebServer::handleStatus() const {
  // Get correct IP based on AP vs STA mode
  const String ipAddr = apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();

  JsonDocument doc;
  doc["version"] = INKADEMIC_VERSION;
  doc["ip"] = ipAddr;
  doc["mode"] = apMode ? "AP" : "STA";
  doc["rssi"] = apMode ? 0 : WiFi.RSSI();
  doc["freeHeap"] = ESP.getFreeHeap();
  doc["uptime"] = millis() / 1000;
#if FREEINK_DEVICE_X4 || FREEINK_DEVICE_X3
  doc["device"] = gpio.deviceIsX3() ? "X3" : "X4";
#else
#ifdef SIMULATOR
  doc["device"] = "Simulator";
#else
  doc["device"] = BoardConfig::ACTIVE.name;
#endif
#endif

  char snBuf[33] = {0};
  bool valid = false;
#if !CONFIG_IDF_TARGET_ESP32
  // Classic ESP32's efuse table has no USER_DATA block (C3/S3 only)
  if (esp_efuse_read_field_blob(ESP_EFUSE_USER_DATA, snBuf, 256) == ESP_OK) {
    valid = snBuf[0] != '\0' && snBuf[0] != (char)0xFF;
    for (int i = 0; i < 32 && snBuf[i] != '\0'; i++) {
      if (!std::isprint(static_cast<unsigned char>(snBuf[i]))) {
        valid = false;
        break;
      }
    }
  }
#endif

  if (valid) {
    doc["serial"] = snBuf;
  } else {
    doc["serial"] = "Not found";
  }

  String response;
  serializeJson(doc, response);
  server->send(200, "application/json", response);
}

void CrossPointWebServer::scanFiles(const char* path, const FileVisitor visitor, void* context) const {
  HalFile root = Storage.open(path);
  if (!root) {
    LOG_DBG("WEB", "Failed to open directory: %s", path);
    return;
  }

  if (!root.isDirectory()) {
    LOG_DBG("WEB", "Not a directory: %s", path);
    root.close();
    return;
  }

  HalFile file = root.openNextFile();
  char name[500];
  while (file) {
    file.getName(name, sizeof(name));
    auto fileName = String(name);

    // Skip hidden items (starting with ".")
    bool shouldHide = !SETTINGS.showHiddenFiles && fileName.startsWith(".");

    // Treat OS/device metadata like other hidden items: keep it out of the
    // default view, but let users manage it when Show Hidden Files is enabled.
    if (!shouldHide && !SETTINGS.showHiddenFiles) {
      for (const auto* item : HIDDEN_ITEMS) {
        if (fileName.equals(item)) {
          shouldHide = true;
          break;
        }
      }
    }

    if (!shouldHide) {
      FileInfo info;
      info.name = fileName;
      info.isDirectory = file.isDirectory();

      if (info.isDirectory) {
        info.size = 0;
        info.isEpub = false;
      } else {
        info.size = file.size();
        info.isEpub = isEpubFile(info.name);
      }

      visitor(info, context);
    }

    file.close();
    yield();  // Yield to allow WiFi and other tasks to process during long scans
    file = root.openNextFile();
  }
  root.close();
}

bool CrossPointWebServer::isEpubFile(const String& filename) const { return FsHelpers::hasEpubExtension(filename); }

void CrossPointWebServer::handleFileList() const {
  sendStaticContent(server.get(), FilesPageHtml, FilesPageHtmlCompressedSize, FilesPageHtmlETag, "text/html");
}

void CrossPointWebServer::handleFileListData() const {
  // Get current path from query string (default to root)
  String currentPath = "/";
  if (server->hasArg("path")) {
    currentPath = normalizeWebPath(server->arg("path"));
  }

  if (isProtectedPath(currentPath)) {
    server->send(403, "application/json", "[]");
    return;
  }

  server->setContentLength(CONTENT_LENGTH_UNKNOWN);
  server->send(200, "application/json", "");

  // This response runs on the web-server task, so a TCP-sized heap buffer is
  // safer than adding 1.4KB to its stack. Allocation is fallible and retains
  // the old per-entry path as a low-memory fallback.
  constexpr size_t BATCH_CAPACITY = 1400;
  auto batch = makeUniqueNoThrow<char[]>(BATCH_CAPACITY);
  char output[512];
  constexpr size_t outputSize = sizeof(output);
  JsonDocument doc;

  struct FileListContext {
    WebServer* server;
    char* batch;
    size_t batchLen;
    char* output;
    JsonDocument* doc;
    bool seenFirst;
  } context{server.get(), batch.get(), 0, output, &doc, false};

  if (batch) {
    batch[context.batchLen++] = '[';
  } else {
    LOG_ERR("WEB", "OOM: file list batch buffer; falling back to per-entry sends");
    server->sendContent("[");
  }

  scanFiles(
      currentPath.c_str(),
      [](const FileInfo& info, void* rawContext) {
        auto& context = *static_cast<FileListContext*>(rawContext);
        context.doc->clear();
        (*context.doc)["name"] = info.name;
        (*context.doc)["size"] = info.size;
        (*context.doc)["isDirectory"] = info.isDirectory;
        (*context.doc)["isEpub"] = info.isEpub;

        const size_t written = serializeJson(*context.doc, context.output, outputSize);
        if (written >= outputSize) {
          // JSON output truncated; skip this entry to avoid sending malformed JSON
          LOG_DBG("WEB", "Skipping file entry with oversized JSON for name: %s", info.name.c_str());
          return;
        }

        const size_t required = written + (context.seenFirst ? 1 : 0);
        if (context.batch) {
          if (context.batchLen + required > BATCH_CAPACITY) {
            context.server->sendContent(context.batch, context.batchLen);
            context.batchLen = 0;
          }
          if (context.seenFirst) context.batch[context.batchLen++] = ',';
          memcpy(context.batch + context.batchLen, context.output, written);
          context.batchLen += written;
        } else {
          if (context.seenFirst) context.server->sendContent(",");
          context.server->sendContent(context.output);
        }
        context.seenFirst = true;
      },
      &context);

  if (batch) {
    if (context.batchLen + 1 > BATCH_CAPACITY) {
      server->sendContent(batch.get(), context.batchLen);
      context.batchLen = 0;
    }
    batch[context.batchLen++] = ']';
    server->sendContent(batch.get(), context.batchLen);
  } else {
    server->sendContent("]");
  }
  // End of streamed response, empty chunk to signal client
  server->sendContent("");
}

void CrossPointWebServer::handleDownload() const {
  if (!server->hasArg("path")) {
    server->send(400, "text/plain", "Missing path");
    return;
  }

  String itemPath = normalizeWebPath(server->arg("path"));
  if (itemPath.isEmpty() || itemPath == "/") {
    server->send(400, "text/plain", "Invalid path");
    return;
  }

  if (isProtectedPath(itemPath)) {
    server->send(403, "text/plain", "Access denied to protected path");
    return;
  }

  if (!Storage.exists(itemPath.c_str())) {
    server->send(404, "text/plain", "Item not found");
    return;
  }

  HalFile file = Storage.open(itemPath.c_str());
  if (!file) {
    server->send(500, "text/plain", "Failed to open file");
    return;
  }
  if (file.isDirectory()) {
    file.close();
    server->send(400, "text/plain", "Path is a directory");
    return;
  }

  String contentType = "application/octet-stream";
  if (isEpubFile(itemPath)) {
    contentType = "application/epub+zip";
  }

  char nameBuf[128] = {0};
  String filename = "download";
  if (file.getName(nameBuf, sizeof(nameBuf))) {
    filename = nameBuf;
  }

  server->setContentLength(file.size());
  server->sendHeader("Content-Disposition", "attachment; filename=\"" + filename + "\"");
  server->send(200, contentType.c_str(), "");

  NetworkClient client = server->client();
  const size_t chunkSize = 4096;
  uint8_t buffer[chunkSize];

  bool downloadOk = true;
  while (downloadOk && file.available()) {
    int result = file.read(buffer, chunkSize);
    if (result <= 0) break;
    size_t bytesRead = static_cast<size_t>(result);
    size_t totalWritten = 0;
    while (totalWritten < bytesRead) {
      size_t wrote = client.write(buffer + totalWritten, bytesRead - totalWritten);
      if (wrote == 0) {
        downloadOk = false;
        break;
      }
      totalWritten += wrote;
    }
  }
#ifndef SIMULATOR
  client.clear();
#endif
  file.close();
}

// Upload start time is used for the completion throughput summary.
static unsigned long uploadStartTime = 0;

static bool flushUploadBuffer(CrossPointWebServer::UploadState& state) {
  if (state.bufferPos > 0 && state.file) {
    const size_t written = state.file.write(state.buffer.data(), state.bufferPos);

    if (written != state.bufferPos) {
      LOG_DBG("WEB", "[UPLOAD] Buffer flush failed: expected %d, wrote %d", state.bufferPos, written);
      state.bufferPos = 0;
      return false;
    }
    state.bufferPos = 0;
  }
  return true;
}

void CrossPointWebServer::handleUpload(UploadState& state) const {
  // Safety check: ensure server is still valid
  if (!running || !server) {
    LOG_DBG("WEB", "[UPLOAD] ERROR: handleUpload called but server not running!");
    return;
  }

  const HTTPUpload& upload = server->upload();

  if (upload.status == UPLOAD_FILE_START) {
    state.fileName = StringUtils::sanitizeFilename(upload.filename.c_str()).c_str();
    state.size = 0;
    state.success = false;
    state.error = "";
    uploadStartTime = millis();
    state.bufferPos = 0;

    // Get upload path from query parameter (defaults to root if not specified)
    // Note: We use query parameter instead of form data because multipart form
    // fields aren't available until after file upload completes
    if (server->hasArg("path")) {
      state.path = normalizeWebPath(server->arg("path"));
    } else {
      state.path = "/";
    }

    LOG_DBG("WEB", "[UPLOAD] START: %s to path: %s", state.fileName.c_str(), state.path.c_str());
    LOG_DBG("WEB", "[UPLOAD] Free heap: %d bytes", ESP.getFreeHeap());

    String filePath = state.path;
    if (!filePath.endsWith("/")) filePath += "/";
    filePath += state.fileName;

    if (isProtectedPath(filePath)) {
      state.error = "Access denied to protected path";
      LOG_DBG("WEB", "[UPLOAD] FAILED: Access denied to protected path: %s", filePath.c_str());
      return;
    }

    // Check if file already exists.
    if (Storage.exists(filePath.c_str())) {
      state.error = "File already exists: " + state.fileName;
      LOG_DBG("WEB", "[UPLOAD] Collision: %s", filePath.c_str());
      return;
    }

    // Open file for writing - this can be slow due to FAT cluster allocation
    if (!Storage.openFileForWrite("WEB", filePath, state.file)) {
      state.error = "Failed to create file on SD card";
      LOG_DBG("WEB", "[UPLOAD] FAILED to create file: %s", filePath.c_str());
      return;
    }

  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (state.file && state.error.isEmpty()) {
      // Buffer incoming data and flush when buffer is full
      // This reduces SD card write operations and improves throughput
      const uint8_t* data = upload.buf;
      size_t remaining = upload.currentSize;

      while (remaining > 0) {
        const size_t space = UploadState::UPLOAD_BUFFER_SIZE - state.bufferPos;
        const size_t toCopy = (remaining < space) ? remaining : space;

        memcpy(state.buffer.data() + state.bufferPos, data, toCopy);
        state.bufferPos += toCopy;
        data += toCopy;
        remaining -= toCopy;

        // Flush buffer when full
        if (state.bufferPos >= UploadState::UPLOAD_BUFFER_SIZE) {
          if (!flushUploadBuffer(state)) {
            state.error = "Failed to write to SD card - disk may be full";
            state.file.close();
            return;
          }
        }
      }

      state.size += upload.currentSize;
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (state.file) {
      // Flush any remaining buffered data
      if (!flushUploadBuffer(state)) {
        state.error = "Failed to write final data to SD card";
      }
      state.file.close();

      if (state.error.isEmpty()) {
        state.success = true;
        const unsigned long elapsed = millis() - uploadStartTime;
        const float avgKbps = (elapsed > 0) ? (state.size / 1024.0) / (elapsed / 1000.0) : 0;
        LOG_DBG("WEB", "[UPLOAD] Complete: %s (%d bytes in %lu ms, avg %.1f KB/s)", state.fileName.c_str(), state.size,
                elapsed, avgKbps);

        // Clear epub cache after uploading the file
        String filePath = state.path;
        if (!filePath.endsWith("/")) filePath += "/";
        filePath += state.fileName;
        clearBookCachePreservingUserState(filePath.c_str());
        SleepImageIndex::invalidateForPath(filePath.c_str());
      }
    }
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    state.bufferPos = 0;  // Discard buffered data
    if (state.file) {
      state.file.close();
      // Try to delete the incomplete file
      String filePath = state.path;
      if (!filePath.endsWith("/")) filePath += "/";
      filePath += state.fileName;
      Storage.remove(filePath.c_str());
    }
    state.error = "Upload aborted";
    LOG_DBG("WEB", "Upload aborted");
  }
}

void CrossPointWebServer::handleUploadPost(UploadState& state) const {
  if (state.success) {
    server->send(200, "text/plain", "File uploaded successfully: " + state.fileName);
  } else {
    const String error = state.error.isEmpty() ? "Unknown error during upload" : state.error;
    server->send(400, "text/plain", error);
  }
}

void CrossPointWebServer::handleCreateFolder() const {
  // Get folder name from form data
  if (!server->hasArg("name")) {
    server->send(400, "text/plain", "Missing folder name");
    return;
  }

  const String folderName = StringUtils::sanitizeFilename(server->arg("name").c_str()).c_str();

  // Validate folder name
  if (folderName.isEmpty() || folderName == "book") {
    server->send(400, "text/plain", "Invalid folder name");
    return;
  }

  // Get parent path
  String parentPath = "/";
  if (server->hasArg("path")) {
    parentPath = normalizeWebPath(server->arg("path"));
  }

  // Build full folder path
  String folderPath = parentPath;
  if (!folderPath.endsWith("/")) folderPath += "/";
  folderPath += folderName;

  if (isProtectedPath(folderPath)) {
    server->send(403, "text/plain", "Access denied to protected path");
    return;
  }

  // Check if already exists
  if (Storage.exists(folderPath.c_str())) {
    server->send(400, "text/plain", "Folder already exists");
    return;
  }

  // Create the folder
  if (Storage.mkdir(folderPath.c_str())) {
    SleepImageIndex::invalidateForPath(folderPath.c_str());
    server->send(200, "text/plain", "Folder created: " + folderName);
  } else {
    LOG_DBG("WEB", "Failed to create folder: %s", folderPath.c_str());
    server->send(500, "text/plain", "Failed to create folder");
  }
}

void CrossPointWebServer::handleRename() const {
  if (!server->hasArg("path") || !server->hasArg("name")) {
    server->send(400, "text/plain", "Missing path or new name");
    return;
  }

  String itemPath = normalizeWebPath(server->arg("path"));
  String newName = StringUtils::sanitizeFilename(server->arg("name").c_str()).c_str();

  if (itemPath.isEmpty() || itemPath == "/") {
    server->send(400, "text/plain", "Invalid path");
    return;
  }
  if (newName.isEmpty()) {
    server->send(400, "text/plain", "New name cannot be empty");
    return;
  }
  if (isProtectedPath(itemPath)) {
    server->send(403, "text/plain", "Cannot rename protected item");
    return;
  }

  // Calculate new path to check if it's protected
  String parentPath = itemPath.substring(0, itemPath.lastIndexOf('/'));
  if (parentPath.isEmpty()) {
    parentPath = "/";
  }
  String newPath = parentPath;
  if (!newPath.endsWith("/")) {
    newPath += "/";
  }
  newPath += newName;

  if (isProtectedPath(newPath)) {
    server->send(403, "text/plain", "Cannot rename to protected path");
    return;
  }

  const String itemName = itemPath.substring(itemPath.lastIndexOf('/') + 1);
  if (newName == itemName) {
    server->send(200, "text/plain", "Name unchanged");
    return;
  }

  if (!Storage.exists(itemPath.c_str())) {
    server->send(404, "text/plain", "Item not found");
    return;
  }

  HalFile file = Storage.open(itemPath.c_str());
  if (!file) {
    server->send(500, "text/plain", "Failed to open file");
    return;
  }
  if (file.isDirectory()) {
    file.close();
    server->send(400, "text/plain", "Only files can be renamed");
    return;
  }

  if (Storage.exists(newPath.c_str())) {
    file.close();
    server->send(409, "text/plain", "Target already exists");
    return;
  }

  clearBookCache(itemPath.c_str());
  const bool success = file.rename(newPath.c_str());
  file.close();

  if (success) {
    LOG_DBG("WEB", "Renamed file: %s -> %s", itemPath.c_str(), newPath.c_str());
    SleepImageIndex::invalidateForPath(itemPath.c_str());
    SleepImageIndex::invalidateForPath(newPath.c_str());
    server->send(200, "text/plain", "Renamed successfully");
  } else {
    LOG_ERR("WEB", "Failed to rename file: %s -> %s", itemPath.c_str(), newPath.c_str());
    server->send(500, "text/plain", "Failed to rename file");
  }
}

void CrossPointWebServer::handleMove() const {
  if (!server->hasArg("path") || !server->hasArg("dest")) {
    server->send(400, "text/plain", "Missing path or destination");
    return;
  }

  String itemPath = normalizeWebPath(server->arg("path"));
  String destPath = normalizeWebPath(server->arg("dest"));

  if (itemPath.isEmpty() || itemPath == "/") {
    server->send(400, "text/plain", "Invalid path");
    return;
  }
  if (destPath.isEmpty()) {
    server->send(400, "text/plain", "Invalid destination");
    return;
  }

  if (isProtectedPath(itemPath)) {
    server->send(403, "text/plain", "Cannot move protected item");
    return;
  }
  if (isProtectedPath(destPath)) {
    server->send(403, "text/plain", "Cannot move into protected folder");
    return;
  }

  const String itemName = itemPath.substring(itemPath.lastIndexOf('/') + 1);

  if (!Storage.exists(itemPath.c_str())) {
    server->send(404, "text/plain", "Item not found");
    return;
  }

  HalFile file = Storage.open(itemPath.c_str());
  if (!file) {
    server->send(500, "text/plain", "Failed to open file");
    return;
  }
  if (file.isDirectory()) {
    file.close();
    server->send(400, "text/plain", "Only files can be moved");
    return;
  }

  if (!Storage.exists(destPath.c_str())) {
    file.close();
    server->send(404, "text/plain", "Destination not found");
    return;
  }
  HalFile destDir = Storage.open(destPath.c_str());
  if (!destDir || !destDir.isDirectory()) {
    if (destDir) {
      destDir.close();
    }
    file.close();
    server->send(400, "text/plain", "Destination is not a folder");
    return;
  }
  destDir.close();

  String newPath = destPath;
  if (!newPath.endsWith("/")) {
    newPath += "/";
  }
  newPath += itemName;

  if (newPath == itemPath) {
    file.close();
    server->send(200, "text/plain", "Already in destination");
    return;
  }
  if (Storage.exists(newPath.c_str())) {
    file.close();
    server->send(409, "text/plain", "Target already exists");
    return;
  }

  clearBookCache(itemPath.c_str());
  const bool success = file.rename(newPath.c_str());
  file.close();

  if (success) {
    LOG_DBG("WEB", "Moved file: %s -> %s", itemPath.c_str(), newPath.c_str());
    SleepImageIndex::invalidateForPath(itemPath.c_str());
    SleepImageIndex::invalidateForPath(newPath.c_str());
    server->send(200, "text/plain", "Moved successfully");
  } else {
    LOG_ERR("WEB", "Failed to move file: %s -> %s", itemPath.c_str(), newPath.c_str());
    server->send(500, "text/plain", "Failed to move file");
  }
}

void CrossPointWebServer::handleDelete() const {
  // To ensure backwards compatibility, plain `path` is mapped
  // to a single element JSON array.
  bool hasPathArg = server->hasArg("path");
  bool hasPathsArg = server->hasArg("paths");
  // Check 'paths' or `path` argument is provided
  if (!(hasPathArg || hasPathsArg)) {
    server->send(400, "text/plain", "Missing `path` or `paths` argument");
    return;
  }
  if (hasPathArg && hasPathsArg) {
    server->send(400, "text/plain", "Provide either 'path' or 'paths', not both");
    return;
  }

  // Parse paths
  String pathsArg;
  JsonDocument doc;
  DeserializationError error = DeserializationError(DeserializationError::Code::Ok);
  if (hasPathsArg) {
    pathsArg = server->arg("paths");
    error = deserializeJson(doc, pathsArg);
  } else {
    pathsArg = server->arg("path");
    doc.add(pathsArg);
  }
  if (error) {
    server->send(400, "text/plain", "Invalid paths format");
    return;
  }

  auto paths = doc.as<JsonArray>();
  if (paths.isNull() || paths.size() == 0) {
    server->send(400, "text/plain", "No paths provided");
    return;
  }

  // Iterate over paths and delete each item
  bool allSuccess = true;
  String failedItems;

  for (const auto& p : paths) {
    auto itemPath = normalizeWebPath(p.as<String>());

    // Validate path
    if (itemPath.isEmpty() || itemPath == "/") {
      failedItems += itemPath + " (cannot delete root); ";
      allSuccess = false;
      continue;
    }

    // Check if item exists
    if (!Storage.exists(itemPath.c_str())) {
      failedItems += itemPath + " (not found); ";
      allSuccess = false;
      continue;
    }

    // Decide whether it's a directory or file by opening it
    bool success = false;
    HalFile f = Storage.open(itemPath.c_str());
    if (f && f.isDirectory()) {
      f.close();
      success = Storage.removeDir(itemPath.c_str());
    } else {
      // It's a file (or couldn't open as dir) — remove file
      if (f) f.close();
      success = Storage.remove(itemPath.c_str());
      clearBookCache(itemPath.c_str());
    }

    if (!success) {
      LOG_ERR("WEB", "Failed to delete item: %s", itemPath.c_str());
      failedItems += itemPath + " (deletion failed); ";
      allSuccess = false;
    } else {
      SleepImageIndex::invalidateForPath(itemPath.c_str());
    }
  }

  if (allSuccess) {
    server->send(200, "text/plain", "All items deleted successfully");
  } else {
    server->send(500, "text/plain", "Failed to delete some items: " + failedItems);
  }
}

void CrossPointWebServer::handleSettingsPage() const {
  sendStaticContent(server.get(), SettingsPageHtml, SettingsPageHtmlCompressedSize, SettingsPageHtmlETag, "text/html");
}

void CrossPointWebServer::handleGetSettings() const {
  // The device settings UI needs an owned, mutable copy of the settings list.
  // The web API only reads it, so iterate the static base list directly rather
  // than copying its nested vectors and callbacks while WiFi is using the heap.
  sdFontSystem.refreshIfDirty();
  const auto& settings = getBaseSettingsList();
  const auto& fontFamilies = sdFontSystem.registry().getFamilies();
  const SdCardFontFamilyInfo* selectedSdFamily =
      SETTINGS.sdFontFamilyName[0] == '\0' ? nullptr : sdFontSystem.registry().findFamily(SETTINGS.sdFontFamilyName);

  server->setContentLength(CONTENT_LENGTH_UNKNOWN);
  server->send(200, "application/json", "");
  server->sendContent("[");

  char output[512];
  constexpr size_t outputSize = sizeof(output);
  bool seenFirst = false;
  JsonDocument doc;

  for (const auto& s : settings) {
    if (!s.key || !isWebSettingAvailable(s)) continue;  // Skip ACTION-only and unavailable entries.

    doc.clear();
    doc["key"] = s.key;
    doc["name"] = I18N.get(s.nameId);
    doc["category"] = I18N.get(s.category);

    switch (s.type) {
      case SettingType::TOGGLE: {
        doc["type"] = "toggle";
        if (s.valuePtr) {
          doc["value"] = static_cast<int>(SETTINGS.*(s.valuePtr));
        }
        break;
      }
      case SettingType::ENUM: {
        doc["type"] = "enum";
        if (s.nameId == StrId::STR_FONT_FAMILY && !fontFamilies.empty()) {
          uint8_t selected = SETTINGS.fontFamily < CrossPointSettings::BUILTIN_FONT_COUNT ? SETTINGS.fontFamily : 0;
          if (selectedSdFamily) {
            const auto it = std::find_if(
                fontFamilies.begin(), fontFamilies.end(),
                [](const SdCardFontFamilyInfo& family) { return family.name == SETTINGS.sdFontFamilyName; });
            if (it != fontFamilies.end()) {
              selected = static_cast<uint8_t>(CrossPointSettings::BUILTIN_FONT_COUNT +
                                              std::distance(fontFamilies.begin(), it));
            }
          }
          doc["value"] = selected;
        } else if (s.nameId == StrId::STR_FONT_SIZE && selectedSdFamily && s.valuePtr) {
          const auto sizes = selectedSdFamily->availableSizes();
          const uint8_t selectedPointSize = SETTINGS.*(s.valuePtr);
          const auto it = std::find(sizes.begin(), sizes.end(), selectedPointSize);
          doc["value"] = static_cast<int>(it == sizes.end() ? 0 : std::distance(sizes.begin(), it));
        } else if (s.valuePtr) {
          doc["value"] = static_cast<int>(enumDisplayIndexForRawValue(s, SETTINGS.*(s.valuePtr)));
        } else if (s.valueGetter) {
          doc["value"] = static_cast<int>(s.valueGetter());
        }
        JsonArray options = doc["options"].to<JsonArray>();
        if (s.nameId == StrId::STR_FONT_FAMILY && !fontFamilies.empty()) {
          constexpr FontFamilyPointSizeRange builtinRange{10, 16};
          options.add(fontFamilyLabel(I18N.get(StrId::STR_LEXEND_DECA), builtinRange));
          options.add(fontFamilyLabel(I18N.get(StrId::STR_BITTER), builtinRange));
          for (const auto& family : fontFamilies) {
            options.add(fontFamilyLabel(family.name, fontFamilyPointSizeRange(family)));
          }
        } else if (s.nameId == StrId::STR_FONT_SIZE && selectedSdFamily) {
          const auto sizes = selectedSdFamily->availableSizes();
          for (const uint8_t pointSize : sizes) {
            char label[8];
            snprintf(label, sizeof(label), "%u pt", pointSize);
            options.add(label);
          }
        } else if (!s.enumStringValues.empty()) {
          for (const auto& opt : s.enumStringValues) {
            options.add(opt);
          }
        } else {
          for (const auto& opt : s.enumValues) {
            options.add(I18N.get(opt));
          }
        }
        break;
      }
      case SettingType::VALUE: {
        doc["type"] = "value";
        if (s.valuePtr) {
          doc["value"] = static_cast<int>(SETTINGS.*(s.valuePtr));
        }
        doc["min"] = s.valueRange.min;
        doc["max"] = s.valueRange.max;
        doc["step"] = s.valueRange.step;
        break;
      }
      case SettingType::STRING: {
        doc["type"] = "string";
        if (s.stringGetter) {
          doc["value"] = s.stringGetter();
        } else if (s.stringMaxLen > 0) {
          doc["value"] = reinterpret_cast<const char*>(&SETTINGS) + s.stringOffset;
        }
        break;
      }
      default:
        continue;
    }

    const size_t prefixSize = seenFirst ? 1 : 0;
    if (prefixSize != 0) output[0] = ',';
    const size_t written = serializeJson(doc, output + prefixSize, outputSize - prefixSize);
    if (written >= outputSize - prefixSize) {
      LOG_DBG("WEB", "Skipping oversized setting JSON for: %s", s.key);
      continue;
    }

    seenFirst = true;
    server->sendContent(output, written + prefixSize);
    yield();  // Allow WiFi and other tasks to run during a slow send.
  }

  server->sendContent("]");
  server->sendContent("");
  sdFontSystem.releaseRegistry();
}

void CrossPointWebServer::handlePostSettings() {
  if (!server->hasArg("plain")) {
    server->send(400, "text/plain", "Missing JSON body");
    return;
  }

  const String body = server->arg("plain");
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, body);
  if (err) {
    server->send(400, "text/plain", String("Invalid JSON: ") + err.c_str());
    return;
  }

  sdFontSystem.refreshIfDirty();
  const auto& settings = getSettingsList(&sdFontSystem.registry());
  int applied = 0;

  for (const auto& s : settings) {
    if (!s.key || !isWebSettingAvailable(s)) continue;
    if (!doc[s.key].is<JsonVariant>()) continue;

    switch (s.type) {
      case SettingType::TOGGLE: {
        const int val = doc[s.key].as<int>() ? 1 : 0;
        if (s.valuePtr) {
          SETTINGS.*(s.valuePtr) = val;
        }
        applied++;
        break;
      }
      case SettingType::ENUM: {
        const int val = doc[s.key].as<int>();
        const int maxVal = s.enumStringValues.empty() ? static_cast<int>(s.enumValues.size())
                                                      : static_cast<int>(s.enumStringValues.size());
        if (val >= 0 && val < maxVal) {
          if (s.valuePtr) {
            SETTINGS.*(s.valuePtr) = enumRawValueForDisplayIndex(s, static_cast<uint8_t>(val));
            QuickActions::settingChanged(SETTINGS, s.valuePtr);
          } else if (s.valueSetter) {
            s.valueSetter(static_cast<uint8_t>(val));
          }
          applied++;
        }
        break;
      }
      case SettingType::VALUE: {
        const int val = doc[s.key].as<int>();
        if (val >= s.valueRange.min && val <= s.valueRange.max) {
          if (s.valuePtr) {
            SETTINGS.*(s.valuePtr) = static_cast<uint8_t>(val);
          }
          applied++;
        }
        break;
      }
      case SettingType::STRING: {
        const std::string val = doc[s.key].as<std::string>();
        if (std::strcmp(s.key, "deviceName") == 0 && (val.length() < CrossPointSettings::MIN_DEVICE_NAME_LENGTH ||
                                                      val.length() > CrossPointSettings::MAX_DEVICE_NAME_LENGTH)) {
          break;
        }
        if (s.stringSetter) {
          s.stringSetter(val);
        } else if (s.stringMaxLen > 0) {
          char* ptr = reinterpret_cast<char*>(&SETTINGS) + s.stringOffset;
          strncpy(ptr, val.c_str(), s.stringMaxLen - 1);
          ptr[s.stringMaxLen - 1] = '\0';
        }
        applied++;
        break;
      }
      default:
        break;
    }
  }

  SETTINGS.saveToFile();

  LOG_DBG("WEB", "Applied %d setting(s)", applied);
  server->send(200, "text/plain", String("Applied ") + String(applied) + " setting(s)");
  sdFontSystem.releaseRegistry();
}

// ---- OPDS Server API ----

void CrossPointWebServer::handleGetOpdsServers() const {
  const auto& servers = OPDS_STORE.getServers();

  // Stream JSON array incrementally to avoid allocating the full response in memory
  server->setContentLength(CONTENT_LENGTH_UNKNOWN);
  server->send(200, "application/json", "");
  server->sendContent("[");

  char output[512];
  constexpr size_t outputSize = sizeof(output);
  JsonDocument doc;

  for (size_t i = 0; i < servers.size(); i++) {
    doc.clear();
    doc["index"] = i;
    doc["name"] = servers[i].name;
    doc["url"] = servers[i].url;
    doc["username"] = servers[i].username;
    doc["filenameFormat"] = opdsFilenameFormatToJson(servers[i].filenameFormat);
    // Never expose passwords over the API — only indicate whether one is set
    doc["hasPassword"] = !servers[i].password.empty();

    const size_t written = serializeJson(doc, output, outputSize);
    if (written >= outputSize) continue;

    if (i > 0) server->sendContent(",");
    server->sendContent(output);
    yield();  // Allow WiFi and other tasks to run during a slow send.
  }

  server->sendContent("]");
  server->sendContent("");
}

void CrossPointWebServer::handlePostOpdsServer() {
  if (!server->hasArg("plain")) {
    server->send(400, "text/plain", "Missing JSON body");
    return;
  }

  const String body = server->arg("plain");
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, body);
  if (err) {
    server->send(400, "text/plain", String("Invalid JSON: ") + err.c_str());
    return;
  }

  OpdsServer opdsServer;
  opdsServer.name = doc["name"] | std::string("");
  opdsServer.url = doc["url"] | std::string("");
  opdsServer.username = doc["username"] | std::string("");
  opdsServer.filenameFormat = opdsFilenameFormatFromJson(doc["filenameFormat"] | "");

  // The password field is optional in the JSON payload. When absent (vs. present but empty),
  // we preserve the existing password — the web UI omits it when the user hasn't changed it.
  bool hasPasswordField = doc["password"].is<const char*>() || doc["password"].is<std::string>();
  std::string password = doc["password"] | std::string("");
  const bool hasFilenameFormatField =
      doc["filenameFormat"].is<const char*>() || doc["filenameFormat"].is<std::string>();

  if (doc["index"].is<int>()) {
    int idx = doc["index"].as<int>();
    if (idx < 0 || idx >= static_cast<int>(OPDS_STORE.getCount())) {
      server->send(400, "text/plain", "Invalid server index");
      return;
    }
    const auto* existing = OPDS_STORE.getServer(static_cast<size_t>(idx));
    // Preserve existing values for fields older clients do not know how to send.
    if (existing && !hasPasswordField) {
      password = existing->password;
    }
    if (existing && !hasFilenameFormatField) {
      opdsServer.filenameFormat = existing->filenameFormat;
    }
    opdsServer.password = password;
    OPDS_STORE.updateServer(static_cast<size_t>(idx), opdsServer);
    LOG_DBG("WEB", "Updated OPDS server at index %d", idx);
  } else {
    opdsServer.password = password;
    if (!OPDS_STORE.addServer(opdsServer)) {
      server->send(400, "text/plain", "Cannot add server (limit reached)");
      return;
    }
    LOG_DBG("WEB", "Added new OPDS server: %s", opdsServer.name.c_str());
  }

  server->send(200, "text/plain", "OK");
}

// Uses POST (not HTTP DELETE) because ESP32 WebServer doesn't support DELETE with body.
void CrossPointWebServer::handleDeleteOpdsServer() {
  if (!server->hasArg("plain")) {
    server->send(400, "text/plain", "Missing JSON body");
    return;
  }

  const String body = server->arg("plain");
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, body);
  if (err) {
    server->send(400, "text/plain", String("Invalid JSON: ") + err.c_str());
    return;
  }

  if (!doc["index"].is<int>()) {
    server->send(400, "text/plain", "Missing index");
    return;
  }

  int idx = doc["index"].as<int>();
  if (idx < 0 || idx >= static_cast<int>(OPDS_STORE.getCount())) {
    server->send(400, "text/plain", "Invalid server index");
    return;
  }

  OPDS_STORE.removeServer(static_cast<size_t>(idx));
  LOG_DBG("WEB", "Deleted OPDS server at index %d", idx);
  server->send(200, "text/plain", "OK");
}

// ---- Wi-Fi Credentials API ----

void CrossPointWebServer::handleGetWifiNetworks() const {
  const auto credentials = WIFI_STORE.getCredentialSummaries();

  // Stream JSON array incrementally to avoid allocating the full response in memory
  server->setContentLength(CONTENT_LENGTH_UNKNOWN);
  server->send(200, "application/json", "");
  server->sendContent("[");

  char output[320];
  constexpr size_t outputSize = sizeof(output);
  JsonDocument doc;

  for (size_t i = 0; i < credentials.size(); i++) {
    doc.clear();
    doc["index"] = i;
    doc["ssid"] = credentials[i].ssid;
    // Never expose Wi-Fi passwords over the API — only indicate whether one is set
    doc["hasPassword"] = credentials[i].hasPassword;
    doc["isLastConnected"] = credentials[i].isLastConnected;

    const size_t written = serializeJson(doc, output, outputSize);
    if (written >= outputSize) continue;

    if (i > 0) server->sendContent(",");
    server->sendContent(output);
    yield();  // Allow WiFi and other tasks to run during a slow send.
  }

  server->sendContent("]");
  server->sendContent("");
}

void CrossPointWebServer::handlePostWifiNetwork() {
  if (!server->hasArg("plain")) {
    server->send(400, "text/plain", "Missing JSON body");
    return;
  }

  const String body = server->arg("plain");
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, body);
  if (err) {
    server->send(400, "text/plain", String("Invalid JSON: ") + err.c_str());
    return;
  }

  std::string ssid = doc["ssid"] | std::string("");
  if (ssid.empty()) {
    server->send(400, "text/plain", "SSID is required");
    return;
  }

  // The password field is optional in the JSON payload. When absent (vs. present but empty),
  // preserve the existing password for updates. Empty passwords are valid for open networks.
  bool hasPasswordField = doc["password"].is<const char*>() || doc["password"].is<std::string>();
  std::string password = doc["password"] | std::string("");

  if (doc["index"].is<int>()) {
    int idx = doc["index"].as<int>();
    if (idx < 0) {
      server->send(400, "text/plain", "Invalid network index");
      return;
    }
    const auto credential = WIFI_STORE.getCredentialAt(static_cast<size_t>(idx));
    if (!credential) {
      server->send(400, "text/plain", "Invalid network index");
      return;
    }

    const std::string oldSsid = credential->ssid;
    if (!hasPasswordField) {
      password = credential->password;
    }

    bool ok = true;
    if (oldSsid != ssid) {
      ok = WIFI_STORE.removeCredential(oldSsid) && WIFI_STORE.addCredential(ssid, password);
    } else {
      ok = WIFI_STORE.addCredential(ssid, password);
    }

    if (!ok) {
      server->send(400, "text/plain", "Failed to update Wi-Fi network");
      return;
    }

    LOG_DBG("WEB", "Updated Wi-Fi network at index %d (SSID: %s)", idx, ssid.c_str());
  } else {
    if (!WIFI_STORE.addCredential(ssid, password)) {
      server->send(400, "text/plain", "Cannot add network (limit reached)");
      return;
    }
    LOG_DBG("WEB", "Added Wi-Fi network: %s", ssid.c_str());
  }

  server->send(200, "text/plain", "OK");
}

// Uses POST (not HTTP DELETE) because ESP32 WebServer doesn't support DELETE with body.
void CrossPointWebServer::handleDeleteWifiNetwork() {
  if (!server->hasArg("plain")) {
    server->send(400, "text/plain", "Missing JSON body");
    return;
  }

  const String body = server->arg("plain");
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, body);
  if (err) {
    server->send(400, "text/plain", String("Invalid JSON: ") + err.c_str());
    return;
  }

  if (!doc["index"].is<int>()) {
    server->send(400, "text/plain", "Missing index");
    return;
  }

  int idx = doc["index"].as<int>();
  if (idx < 0) {
    server->send(400, "text/plain", "Invalid network index");
    return;
  }
  const auto ssid = WIFI_STORE.getSsidAt(static_cast<size_t>(idx));
  if (!ssid) {
    server->send(400, "text/plain", "Invalid network index");
    return;
  }

  if (!WIFI_STORE.removeCredential(*ssid)) {
    server->send(400, "text/plain", "Failed to delete Wi-Fi network");
    return;
  }

  LOG_DBG("WEB", "Deleted Wi-Fi network at index %d (SSID: %s)", idx, ssid->c_str());
  server->send(200, "text/plain", "OK");
}

// WebSocket callback trampoline
void CrossPointWebServer::wsEventCallback(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
  if (wsInstance) {
    wsInstance->onWebSocketEvent(num, type, payload, length);
  }
}

// WebSocket event handler for fast binary uploads
// Protocol:
//   1. Client sends TEXT message: "START:<filename>:<size>:<path>"
//   2. Client sends BINARY messages with file data chunks
//   3. Server sends TEXT "PROGRESS:<received>:<total>" after each chunk
//   4. Server sends TEXT "DONE" or "ERROR:<message>" when complete
void CrossPointWebServer::onWebSocketEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_DISCONNECTED:
      LOG_DBG("WS", "Client %u disconnected", num);
      // Only clean up if this is the client that owns the active upload.
      // A new client may have already started a fresh upload before this
      // DISCONNECTED event fires (race condition on quick cancel + retry).
      if (num == wsUploadClientNum && wsUploadInProgress && wsUploadFile) {
        abortWsUpload("WS");
      }
      break;

    case WStype_CONNECTED: {
      LOG_DBG("WS", "Client %u connected", num);
      break;
    }

    case WStype_TEXT: {
      // Parse control messages
      String msg = String((char*)payload);

      if (msg.startsWith("START:")) {
        // Reject any START while an upload is already active to prevent
        // leaking the open wsUploadFile handle (owning client re-START included)
        if (wsUploadInProgress) {
          wsServer->sendTXT(num, "ERROR:Upload already in progress");
          break;
        }

        // Parse: START:<filename>:<size>:<path>
        int firstColon = msg.indexOf(':', 6);
        int secondColon = msg.indexOf(':', firstColon + 1);

        if (firstColon > 0 && secondColon > 0) {
          wsUploadFileName = StringUtils::sanitizeFilename(msg.substring(6, firstColon).c_str()).c_str();
          String sizeToken = msg.substring(firstColon + 1, secondColon);
          bool sizeValid = sizeToken.length() > 0;
          int digitStart = (sizeValid && sizeToken[0] == '+') ? 1 : 0;
          if (digitStart > 0 && sizeToken.length() < 2) sizeValid = false;
          for (int i = digitStart; i < (int)sizeToken.length() && sizeValid; i++) {
            if (!isdigit((unsigned char)sizeToken[i])) sizeValid = false;
          }
          if (!sizeValid) {
            LOG_DBG("WS", "START rejected: invalid size token '%s'", sizeToken.c_str());
            wsServer->sendTXT(num, "ERROR:Invalid START format");
            return;
          }
          wsUploadSize = sizeToken.toInt();
          wsUploadPath = normalizeWebPath(msg.substring(secondColon + 1));
          wsUploadReceived = 0;
          wsLastProgressSent = 0;
          wsUploadStartTime = millis();

          String filePath = wsUploadPath;
          if (!filePath.endsWith("/")) filePath += "/";
          filePath += wsUploadFileName;

          if (isProtectedPath(filePath)) {
            wsServer->sendTXT(num, "ERROR:Access denied to protected path");
            wsUploadInProgress = false;
            wsUploadClientNum = 255;
            return;
          }

          if (Storage.exists(filePath.c_str())) {
            LOG_DBG("WS", "Upload collision: %s", filePath.c_str());
            wsServer->sendTXT(num, "ERROR:File already exists: " + wsUploadFileName);
            return;
          }

          LOG_DBG("WS", "Starting upload: %s (%d bytes) to %s", wsUploadFileName.c_str(), wsUploadSize,
                  filePath.c_str());

          // Open file for writing
          if (!Storage.openFileForWrite("WS", filePath, wsUploadFile)) {
            wsServer->sendTXT(num, "ERROR:Failed to create file");
            wsUploadInProgress = false;
            wsUploadClientNum = 255;
            return;
          }

          // Zero-byte upload: complete immediately without waiting for BIN frames
          if (wsUploadSize == 0) {
            // Explicit close() required: file-scope global persists beyond function scope
            wsUploadFile.close();
            wsLastCompleteName = wsUploadFileName;
            wsLastCompleteSize = 0;
            wsLastCompleteAt = millis();
            LOG_DBG("WS", "Zero-byte upload complete: %s", filePath.c_str());
            clearBookCachePreservingUserState(filePath.c_str());
            SleepImageIndex::invalidateForPath(filePath.c_str());
            wsServer->sendTXT(num, "DONE");
            wsLastProgressSent = 0;
            break;
          }

          wsUploadClientNum = num;
          wsUploadInProgress = true;
          wsServer->sendTXT(num, "READY");
        } else {
          wsServer->sendTXT(num, "ERROR:Invalid START format");
        }
      }
      break;
    }

    case WStype_BIN: {
      if (!wsUploadInProgress || !wsUploadFile || num != wsUploadClientNum) {
        wsServer->sendTXT(num, "ERROR:No upload in progress");
        return;
      }

      // Write binary data directly to file
      size_t remaining = wsUploadSize - wsUploadReceived;
      if (length > remaining) {
        abortWsUpload("WS");
        wsServer->sendTXT(num, "ERROR:Upload overflow");
        return;
      }
      size_t written = wsUploadFile.write(payload, length);

      if (written != length) {
        abortWsUpload("WS");
        wsServer->sendTXT(num, "ERROR:Write failed - disk full?");
        return;
      }

      wsUploadReceived += written;

      // Send progress update (every 64KB or at end)
      if (wsUploadReceived - wsLastProgressSent >= 65536 || wsUploadReceived >= wsUploadSize) {
        String progress = "PROGRESS:" + String(wsUploadReceived) + ":" + String(wsUploadSize);
        wsServer->sendTXT(num, progress);
        wsLastProgressSent = wsUploadReceived;
      }

      // Check if upload complete
      if (wsUploadReceived >= wsUploadSize) {
        // Explicit close() required: file-scope global persists beyond function scope
        wsUploadFile.close();
        wsUploadInProgress = false;
        wsUploadClientNum = 255;

        wsLastCompleteName = wsUploadFileName;
        wsLastCompleteSize = wsUploadSize;
        wsLastCompleteAt = millis();

        unsigned long elapsed = millis() - wsUploadStartTime;
        float kbps = (elapsed > 0) ? (wsUploadSize / 1024.0) / (elapsed / 1000.0) : 0;

        LOG_DBG("WS", "Upload complete: %s (%d bytes in %lu ms, %.1f KB/s)", wsUploadFileName.c_str(), wsUploadSize,
                elapsed, kbps);

        // Clear epub cache after uploading the file
        String filePath = wsUploadPath;
        if (!filePath.endsWith("/")) filePath += "/";
        filePath += wsUploadFileName;
        clearBookCachePreservingUserState(filePath.c_str());
        SleepImageIndex::invalidateForPath(filePath.c_str());

        wsServer->sendTXT(num, "DONE");
        wsLastProgressSent = 0;
      }
      break;
    }

    default:
      break;
  }
}

// --- Font management handlers ---

void CrossPointWebServer::handleFontsPage() const {
  sendStaticContent(server.get(), FontsPageHtml, FontsPageHtmlCompressedSize, FontsPageHtmlETag, "text/html");
}

void CrossPointWebServer::handleFontList() const {
  // Pick up any uploads/deletes that happened since the last reader load.
  sdFontSystem.ensureRegistry();
  const auto& families = sdFontSystem.registry().getFamilies();

  // Send the catalog as it is enumerated. Building an ArduinoJson document and
  // then serializing it to a String keeps two complete copies in RAM, which
  // can exhaust the fragmented network heap with larger font collections.
  server->setContentLength(CONTENT_LENGTH_UNKNOWN);
  server->send(200, "application/json", "");
  FontListJsonWriter json(*server);
  json.append("{\"families\":[");

  bool firstFamily = true;
  for (const auto& family : families) {
    if (!firstFamily) json.append(",");
    firstFamily = false;

    json.append("{\"name\":");
    json.appendJsonString(family.name.c_str());
    json.append(",\"sizes\":[");

    bool firstSize = true;
    for (uint8_t s : family.availableSizes()) {
      if (!firstSize) json.append(",");
      firstSize = false;
      json.appendUnsigned(s);
    }
    json.append("],\"files\":[");

    bool firstFile = true;
    for (const auto& file : family.files) {
      if (!firstFile) json.append(",");
      firstFile = false;

      // Extract filename from full path
      const char* name = strrchr(file.path.c_str(), '/');
      json.append("{\"name\":");
      json.appendJsonString(name ? name + 1 : file.path.c_str());

      // Stat the file for size
      HalFile f;
      unsigned long fileSize = 0;
      if (Storage.openFileForRead("WEB", file.path.c_str(), f)) {
        fileSize = static_cast<unsigned long>(f.size());
        f.close();
      }

      json.append(",\"size\":");
      json.appendUnsigned(fileSize);
      json.append("}");
      json.flush();
      yield();
    }
    json.append("]}");
    json.flush();
    yield();
  }

  json.append("],\"maxFamilies\":");
  json.appendUnsigned(SdCardFontRegistry::MAX_SD_FAMILIES);
  json.append("}");
  json.flush();
  server->sendContent("");
}

void CrossPointWebServer::handleFontUploadData() {
  HTTPUpload& upload = server->upload();

  switch (upload.status) {
    case UPLOAD_FILE_START: {
      String family = server->arg("family");
      fontUpload.file = HalFile();
      fontUpload.familyName.clear();
      fontUpload.filePath.clear();
      fontUpload.valid = false;
      fontUpload.magicChecked = false;
      fontUpload.bytesWritten = 0;
      fontUpload.bufferPos = 0;

      if (!FontInstaller::isValidFamilyName(family.c_str())) {
        LOG_ERR("WEB", "Invalid font family name: %s", family.c_str());
        break;
      }

      String filename = upload.filename;
      filename.replace(' ', '_');
      // Validate filename: rejects path traversal (../, /, \) and enforces
      // a .cpfont basename of alphanumeric + hyphen + underscore. Without
      // this an attacker could supply "../../.crosspoint/settings.json" as
      // a "filename" and have it written outside the fonts directory.
      if (!FontInstaller::isValidCpfontFilename(filename.c_str())) {
        LOG_ERR("WEB", "Invalid font filename: %s", filename.c_str());
        break;
      }

      fontUpload.familyName = family.c_str();

      // Create a temporary FontInstaller for directory creation
      FontInstaller installer(sdFontSystem.registry());
      if (!installer.ensureFamilyDir(family.c_str())) {
        LOG_ERR("WEB", "Failed to create font family dir");
        break;
      }

      char path[192];
      FontInstaller::buildFontPath(family.c_str(), filename.c_str(), path, sizeof(path));
      fontUpload.filePath = path;

      if (!Storage.openFileForWrite("WEB", path, fontUpload.file)) {
        LOG_ERR("WEB", "Failed to open font file for write: %s", path);
        break;
      }

      fontUpload.valid = true;
      LOG_DBG("WEB", "Font upload started: %s -> %s", filename.c_str(), path);
      break;
    }

    case UPLOAD_FILE_WRITE: {
      if (!fontUpload.valid) break;

      // Validate magic bytes on first chunk only
      if (!fontUpload.magicChecked && upload.currentSize >= 8) {
        if (memcmp(upload.buf, "CPFONT\0\0", 8) != 0) {
          LOG_ERR("WEB", "Invalid .cpfont magic bytes");
          fontUpload.valid = false;
          break;
        }
        fontUpload.magicChecked = true;
      }

      // Buffer writes for efficiency
      size_t remaining = upload.currentSize;
      const uint8_t* src = upload.buf;
      while (remaining > 0) {
        size_t space = FontUploadState::BUFFER_SIZE - fontUpload.bufferPos;
        size_t chunk = (remaining < space) ? remaining : space;
        memcpy(fontUpload.buffer.data() + fontUpload.bufferPos, src, chunk);
        fontUpload.bufferPos += chunk;
        src += chunk;
        remaining -= chunk;

        if (fontUpload.bufferPos >= FontUploadState::BUFFER_SIZE) {
          fontUpload.file.write(fontUpload.buffer.data(), fontUpload.bufferPos);
          fontUpload.bytesWritten += fontUpload.bufferPos;
          fontUpload.bufferPos = 0;
        }
      }
      break;
    }

    case UPLOAD_FILE_END: {
      // Flush remaining buffer
      if (fontUpload.valid && fontUpload.bufferPos > 0) {
        fontUpload.file.write(fontUpload.buffer.data(), fontUpload.bufferPos);
        fontUpload.bytesWritten += fontUpload.bufferPos;
        fontUpload.bufferPos = 0;
      }
      if (fontUpload.file.isOpen()) {
        fontUpload.file.close();
      }

      if (!fontUpload.valid && !fontUpload.filePath.empty()) {
        Storage.remove(fontUpload.filePath.c_str());
      }

      LOG_DBG("WEB", "Font upload end: valid=%d, %zu bytes", fontUpload.valid, fontUpload.bytesWritten);
      break;
    }

    case UPLOAD_FILE_ABORTED: {
      if (fontUpload.file) {
        fontUpload.file.close();
      }
      if (!fontUpload.filePath.empty()) {
        Storage.remove(fontUpload.filePath.c_str());
      }
      fontUpload.valid = false;
      LOG_DBG("WEB", "Font upload aborted");
      break;
    }
  }
}

void CrossPointWebServer::handleFontUpload() {
  if (fontUpload.valid) {
    sdFontSystem.markRegistryDirty();
    server->send(200, "application/json", "{\"ok\":true}");
    LOG_DBG("WEB", "Font upload complete: %s", fontUpload.filePath.c_str());
  } else {
    server->send(400, "application/json", "{\"error\":\"Invalid .cpfont file\"}");
  }
}

void CrossPointWebServer::handleFontDelete() {
  String body = server->arg("plain");
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);

  if (err || !doc["family"].is<const char*>()) {
    server->send(400, "application/json", "{\"error\":\"Invalid request\"}");
    return;
  }

  const char* familyName = doc["family"];
  FontInstaller installer(sdFontSystem.registry());
  auto result = installer.deleteFamily(familyName);

  if (result == FontInstaller::Error::OK) {
    sdFontSystem.markRegistryDirty();
    server->send(200, "application/json", "{\"ok\":true}");
    LOG_DBG("WEB", "Deleted font family: %s", familyName);
  } else {
    server->send(500, "application/json", "{\"error\":\"Delete failed\"}");
    LOG_ERR("WEB", "Failed to delete font family: %s", familyName);
  }
}

void CrossPointWebServer::handleGetHighlights() const {
  if (!server->hasArg("path")) {
    server->send(400, "text/plain", "Missing path parameter");
    return;
  }
  const String path = server->arg("path");
  if (path.isEmpty() || !FsHelpers::hasEpubExtension(path)) {
    server->send(400, "text/plain", "Invalid EPUB path");
    return;
  }

  ANNOTATION_TAGS.load();
  if (!CLIPPINGS.loadForBook(path.c_str(), "", "", "epub")) {
    server->send(500, "text/plain", "Could not read highlights; existing data preserved");
    return;
  }
  NOTES.loadForBook(path.c_str(), "epub");
  const auto& clippings = CLIPPINGS.getClippings();
  server->setContentLength(CONTENT_LENGTH_UNKNOWN);
  server->send(200, "application/json", "");
  server->sendContent("[");

  bool first = true;
  JsonDocument doc;
  for (size_t i = 0; i < clippings.size(); ++i) {
    const Clipping& clipping = clippings[i];
    const Note* note =
        NOTES.getNoteForClipping(clipping.spineIndex, clipping.startPage, clipping.startWordIndex, clipping.timestamp);
    std::string text;
    CLIPPINGS.readClippingText(i, text);
    doc.clear();
    doc["spineIndex"] = clipping.spineIndex;
    doc["startPage"] = clipping.startPage;
    doc["startWordIndex"] = clipping.startWordIndex;
    doc["timestamp"] = clipping.timestamp;
    doc["chapterTitle"] = clipping.chapterTitle;
    doc["paragraphIndex"] = clipping.paragraphIndex;
    doc["endWordIndex"] = clipping.endWordIndex;
    doc["layoutSignature"] = clipping.layoutSignature;
    doc["text"] = text;
    if (i == 0) {
      doc["bookTitle"] = CLIPPINGS.getBookTitle();
      doc["bookAuthor"] = CLIPPINGS.getBookAuthor();
      doc["documentId"] = CLIPPINGS.getDocumentId();
      JsonArray tags = doc["availableTags"].to<JsonArray>();
      for (uint8_t tagIndex = 0; tagIndex < ANNOTATION_TAGS.count(); ++tagIndex) {
        const AnnotationTag* tag = ANNOTATION_TAGS.at(tagIndex);
        if (!tag) continue;
        JsonObject item = tags.add<JsonObject>();
        item["id"] = tag->id;
        item["name"] = tag->name;
      }
    }
    if (note) {
      JsonObject noteObject = doc["note"].to<JsonObject>();
      noteObject["text"] = note->text;
      noteObject["tagId"] = note->tagId;
      noteObject["modifiedUnixTime"] = note->modifiedUnixTime;
      if (note->legacyTag != 0) noteObject["legacyTag"] = std::string(1, note->legacyTag);
      const char* tagName = ANNOTATION_TAGS.nameForId(note->tagId);
      if (tagName) noteObject["tagName"] = tagName;
    } else {
      doc["note"] = nullptr;
    }
    String output;
    serializeJson(doc, output);
    if (!first) server->sendContent(",");
    first = false;
    server->sendContent(output);
  }
  server->sendContent("]");
  server->sendContent("");
  CLIPPINGS.unload();
  NOTES.unload();
}

void CrossPointWebServer::handlePostNote() {
  if (!server->hasArg("plain")) {
    server->send(400, "text/plain", "Missing JSON body");
    return;
  }
  const String body = server->arg("plain");
  constexpr size_t MAX_NOTE_BODY_BYTES = NoteStore::kNoteTextMax + 1024;
  if (body.length() > MAX_NOTE_BODY_BYTES) {
    server->send(413, "text/plain", "Note too long");
    return;
  }
  JsonDocument doc;
  if (const DeserializationError err = deserializeJson(doc, body)) {
    server->send(400, "text/plain", String("Invalid JSON: ") + err.c_str());
    return;
  }

  const std::string path = doc["path"] | std::string{};
  const uint16_t spineIndex = doc["spineIndex"] | uint16_t(0);
  const uint16_t startPage = doc["startPage"] | uint16_t(0);
  const uint16_t startWordIndex = doc["startWordIndex"] | uint16_t(0);
  const uint32_t clippingTimestamp = doc["timestamp"] | uint32_t(0);
  const uint16_t tagId = doc["tagId"] | uint16_t(0);
  const std::string text = doc["text"] | std::string{};

  if (path.empty() || !FsHelpers::hasEpubExtension(std::string_view(path.data(), path.size()))) {
    server->send(400, "text/plain", "Missing or invalid book path");
    return;
  }
  ANNOTATION_TAGS.load();
  if (tagId != 0 && ANNOTATION_TAGS.nameForId(tagId) == nullptr) {
    server->send(400, "text/plain", "Unknown annotation tag");
    return;
  }

  NOTES.loadForBook(path.c_str(), "epub");
  const bool hasTag = doc["tagId"].is<JsonVariant>();
  const bool ok = (text.empty() && (!hasTag || tagId == 0))
                      ? NOTES.deleteNote(path.c_str(), spineIndex, startPage, startWordIndex, clippingTimestamp)
                      : NOTES.saveNoteAndTag(path.c_str(), spineIndex, startPage, startWordIndex, clippingTimestamp,
                                             text.c_str(), tagId, hasTag);
  NOTES.unload();

  if (!ok) {
    server->send(500, "text/plain", "Could not save note");
    return;
  }
  server->send(200, "text/plain", "OK");
}
