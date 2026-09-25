#include "OtaUpdater.h"

#include "FirmwareRequestPolicy.h"

#ifdef SIMULATOR
#include "OtaUpdater.h"

bool OtaUpdater::isUpdateNewer() const { return false; }
const std::string& OtaUpdater::getLatestVersion() const { return latestVersion; }
OtaUpdater::OtaUpdaterError OtaUpdater::checkForUpdate() { return NO_UPDATE; }
OtaUpdater::OtaUpdaterError OtaUpdater::loadSavedSource() { return NO_UPDATE; }
OtaUpdater::OtaUpdaterError OtaUpdater::downloadManualToFile(const char*, ProgressCallback, void*, std::atomic<bool>*) {
  return NO_UPDATE;
}
OtaUpdater::OtaUpdaterError OtaUpdater::downloadLatestToFiles(const char*, const char*, ProgressCallback, void*) {
  return NO_UPDATE;
}
OtaUpdater::OtaUpdaterError OtaUpdater::installUpdate(ProgressCallback, void*, std::atomic<bool>*) { return NO_UPDATE; }
#else
#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>
#include <ReleaseJsonParser.h>
#include <strings.h>

#include <algorithm>
#include <cstring>
#include <utility>

#include "AppVersion.h"
#include "FirmwareFlasher.h"
#include "FirmwareIdentity.h"
#include "FirmwareIdentityScanner.h"
#include "FirmwareVersion.h"
#include "OtaSectorWriter.h"
#include "OtaSignature.h"
#include "OtaUpdater.h"
#include "WiFi.h"
#include "esp_http_client.h"
#include "esp_ota_ops.h"
#include "esp_task_wdt.h"
#include "mbedtls/sha256.h"
#include "network/HttpDownloader.h"
#include "network/WifiPowerSaveGuard.h"

namespace {
#ifndef INKADEMIC_OTA_RELEASE_URL
#define INKADEMIC_OTA_RELEASE_URL "https://api.github.com/repos/jbfarias/INKademic/releases/latest"
#endif

constexpr char latestReleaseUrl[] = INKADEMIC_OTA_RELEASE_URL;

#ifdef INKADEMIC_FIRMWARE_DEVICE_TYPE
constexpr char firmwareAssetStem[] = "firmware-" INKADEMIC_FIRMWARE_DEVICE_TYPE;
constexpr char firmwareAssetName[] = "firmware-" INKADEMIC_FIRMWARE_DEVICE_TYPE ".bin";
#else
constexpr char firmwareAssetStem[] = "firmware";
constexpr char firmwareAssetName[] = "firmware.bin";
#endif

constexpr char binSuffix[] = ".bin";
constexpr size_t OTA_PROGRESS_UPDATE_BYTES = 64 * 1024;
constexpr size_t OTA_SIGNATURE_SIZE = 64;

bool isGitHubReleaseEndpoint(const char* url) {
  return url != nullptr && strncmp(url, "https://api.github.com/", sizeof("https://api.github.com/") - 1) == 0;
}

bool isUnlockerNetwork() {
#if defined(INKADEMIC_ENABLE_UNLOCKER_OTA_COMPAT) && INKADEMIC_ENABLE_UNLOCKER_OTA_COMPAT
  const String ssid = WiFi.SSID();
  return ssid.equalsIgnoreCase("crosspoint") || ssid.equalsIgnoreCase("Xteink Unlocker");
#else
  return false;
#endif
}

bool shouldRetryWithUnlockerCertificate() {
#if defined(INKADEMIC_ENABLE_UNLOCKER_OTA_COMPAT) && INKADEMIC_ENABLE_UNLOCKER_OTA_COMPAT
  // The Unlocker deliberately answers api.github.com with a trusted bridge
  // certificate. Keep ordinary Internet OTA strict; only permit the fallback
  // on the Unlocker SSID and only for that exact API endpoint.
  return isGitHubReleaseEndpoint(latestReleaseUrl) && isUnlockerNetwork();
#else
  return false;
#endif
}

bool startsWith(const char* value, const char* prefix) {
  if (value == nullptr || prefix == nullptr) return false;
  const size_t prefixLength = strlen(prefix);
  return strncmp(value, prefix, prefixLength) == 0;
}

char lowerHex(const uint8_t value) {
  return value < 10 ? static_cast<char>('0' + value) : static_cast<char>('a' + value - 10);
}

char asciiLower(const char c) { return (c >= 'A' && c <= 'F') ? static_cast<char>(c - 'A' + 'a') : c; }

bool isHexChar(const char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

bool isSha256Hex(const char* value) {
  if (value == nullptr || strlen(value) != 64) return false;
  for (size_t i = 0; i < 64; ++i) {
    if (!isHexChar(value[i])) return false;
  }
  return true;
}

bool sha256Matches(const uint8_t digest[32], const char* expectedHex) {
  if (!isSha256Hex(expectedHex)) return false;

  for (size_t i = 0; i < 32; ++i) {
    const char high = lowerHex((digest[i] >> 4) & 0x0F);
    const char low = lowerHex(digest[i] & 0x0F);
    if (high != asciiLower(expectedHex[i * 2]) || low != asciiLower(expectedHex[i * 2 + 1])) return false;
  }
  return true;
}

void formatSha256(const uint8_t digest[32], char output[65]) {
  for (size_t i = 0; i < 32; ++i) {
    output[i * 2] = lowerHex((digest[i] >> 4) & 0x0F);
    output[i * 2 + 1] = lowerHex(digest[i] & 0x0F);
  }
  output[64] = '\0';
}

bool isHttpUrl(const std::string& url) { return url.rfind("http://", 0) == 0; }

bool endsWith(const char* value, const char* suffix) {
  if (value == nullptr || suffix == nullptr) return false;
  const size_t valueLength = strlen(value);
  const size_t suffixLength = strlen(suffix);
  if (suffixLength > valueLength) return false;
  return strcmp(value + valueLength - suffixLength, suffix) == 0;
}

bool isMatchingFirmwareAssetName(const char* assetName) {
  if (assetName == nullptr) return false;
  if (strcmp(assetName, firmwareAssetName) == 0) return true;
  if (!startsWith(assetName, firmwareAssetStem)) return false;
  if (assetName[strlen(firmwareAssetStem)] != '-') return false;
  return endsWith(assetName, binSuffix);
}

bool isMatchingSignatureAssetName(const char* assetName) {
  if (assetName == nullptr) return false;
  const std::string expectedName = std::string(firmwareAssetName) + ".sig";
  if (strcmp(assetName, expectedName.c_str()) == 0) return true;

  // Release workflows publish immutable, versioned names such as
  // firmware-x4-pro-v1.8.0.bin.sig. Accept only that exact device stem,
  // with a separating hyphen and the complete .bin.sig suffix.
  if (!startsWith(assetName, firmwareAssetStem)) return false;
  if (assetName[strlen(firmwareAssetStem)] != '-') return false;
  return endsWith(assetName, ".bin.sig");
}

/*
 * When esp_crt_bundle.h included, it is pointing wrong header file
 * which is something under WifiClientSecure because of our framework based on arduno platform.
 * To manage this obstacle, don't include anything, just extern and it will point correct one.
 */
extern "C" {
extern esp_err_t esp_crt_bundle_attach(void* conf);
}

size_t totalBytesReceived = 0;

struct OtaInstallContext {
  size_t* processedSize = nullptr;
  size_t totalSize = 0;
  size_t lastProgressBytes = 0;
  int lastReportedPct = -1;
  OtaUpdater::ProgressCallback onProgress = nullptr;
  void* progressCtx = nullptr;
};

esp_err_t release_manifest_event_handler(esp_http_client_event_t* event) {
  esp_task_wdt_reset();
  if (event->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
  if (event->data_len <= 0) return ESP_OK;

  auto* parser = static_cast<ReleaseJsonParser*>(event->user_data);
  if (parser == nullptr) {
    LOG_ERR("OTA", "HTTP client parser missing");
    return ESP_ERR_INVALID_ARG;
  }

  totalBytesReceived += static_cast<size_t>(event->data_len);
  parser->feed(static_cast<const char*>(event->data), event->data_len);
  return ESP_OK;
}

void notifyOtaProgress(OtaInstallContext* ctx, const bool force) {
  if (ctx == nullptr || ctx->onProgress == nullptr || ctx->processedSize == nullptr || ctx->totalSize == 0) return;

  const size_t processed = *ctx->processedSize;
  const int pct = static_cast<int>(static_cast<uint64_t>(processed) * 100 / ctx->totalSize);
  if (force || pct != ctx->lastReportedPct || processed - ctx->lastProgressBytes >= OTA_PROGRESS_UPDATE_BYTES) {
    ctx->lastReportedPct = pct;
    ctx->lastProgressBytes = processed;
    ctx->onProgress(ctx->progressCtx);
  }
}

}  // namespace

OtaUpdater::OtaUpdaterError OtaUpdater::checkForUpdate() {
  if (manualSource) return firmware_request::validManualUrl(otaUrl) ? OK : JSON_PARSE_ERROR;
  WifiPowerSaveGuard wifiPowerSaveGuard;

  updateAvailable = false;
  latestVersion.clear();
  otaUrl.clear();
  otaSha256.clear();
  otaSignatureUrl.clear();
  otaSize = 0;
  otaSignatureSize = 0;
  processedSize = 0;
  totalSize = 0;

  esp_err_t esp_err;
  ReleaseJsonParser releaseParser(isMatchingFirmwareAssetName, isMatchingSignatureAssetName);

  const auto fetchManifest = [&](const bool allowUnlockerCertificate) -> esp_err_t {
    releaseParser.reset();
    totalBytesReceived = 0;

    esp_http_client_config_t client_config = {
        .url = latestReleaseUrl,
        .event_handler = release_manifest_event_handler,
        // 4096 holds the API response headers; the body streams through the
        // parser in chunks so RX needn't be larger. Both buffers are released
        // before installUpdate to reduce fragmentation.
        .buffer_size = 4096,
        .buffer_size_tx = 1024,
        .user_data = &releaseParser,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = true,
    };
    client_config.timeout_ms = 10000;
    // Keep CA validation enabled. This only relaxes the certificate-name check
    // for the local Unlocker bridge, whose trusted certificate is for
    // unlocker.crosspointreader.com while DNS maps api.github.com locally.
    client_config.skip_cert_common_name_check = allowUnlockerCertificate;

    esp_http_client_handle_t client_handle = esp_http_client_init(&client_config);
    if (!client_handle) {
      LOG_ERR("OTA", "HTTP Client Handle Failed");
      return ESP_ERR_NO_MEM;
    }

    esp_err_t result = esp_http_client_set_header(client_handle, "User-Agent", "INKademic-ESP32-" INKADEMIC_VERSION);
    if (result == ESP_OK) {
      esp_task_wdt_reset();
      result = esp_http_client_perform(client_handle);
      esp_task_wdt_reset();
    }
    if (result != ESP_OK) LOG_ERR("OTA", "manifest request failed: %s", esp_err_to_name(result));

    const esp_err_t cleanupResult = esp_http_client_cleanup(client_handle);
    if (result == ESP_OK && cleanupResult != ESP_OK) {
      LOG_ERR("OTA", "manifest cleanup failed: %s", esp_err_to_name(cleanupResult));
      return cleanupResult;
    }
    return result;
  };

  LOG_DBG("OTA", "Checking for update (current: %s)", INKADEMIC_VERSION);
  esp_err = fetchManifest(false);
  if (esp_err != ESP_OK && shouldRetryWithUnlockerCertificate()) {
    LOG_INF("OTA", "Retrying manifest through trusted Xteink Unlocker bridge");
    esp_err = fetchManifest(true);
  }
  if (esp_err != ESP_OK) return HTTP_ERROR;

  LOG_DBG("OTA", "Response received: %zu bytes total", totalBytesReceived);
  LOG_DBG("OTA", "Parser results: tag=%s firmware=%s signature=%s", releaseParser.foundTag() ? "yes" : "no",
          releaseParser.foundFirmware() ? "yes" : "no", releaseParser.foundSignature() ? "yes" : "no");

  if (!releaseParser.foundTag()) {
    LOG_ERR("OTA", "No tag_name in release JSON");
    return JSON_PARSE_ERROR;
  }

  latestVersion = releaseParser.getTagName();

  if (!releaseParser.foundFirmware()) {
    LOG_ERR("OTA", "No matching %s asset found for release %s", firmwareAssetStem, latestVersion.c_str());
    return NO_UPDATE;
  }

  otaUrl = releaseParser.getFirmwareUrl();
  otaSha256 = releaseParser.getFirmwareSha256();
  otaSignatureUrl = releaseParser.getSignatureUrl();
  otaSize = releaseParser.getFirmwareSize();
  otaSignatureSize = releaseParser.getSignatureSize();
  totalSize = otaSize;
  updateAvailable = true;

  LOG_DBG("OTA", "Found update: tag=%s size=%zu sha256=%s", latestVersion.c_str(), otaSize,
          otaSha256.empty() ? "missing" : "present");
  LOG_DBG("OTA", "Firmware URL: %s", otaUrl.c_str());
  LOG_DBG("OTA", "Signature URL: %s", otaSignatureUrl.empty() ? "missing" : otaSignatureUrl.c_str());
  return OK;
}

bool OtaUpdater::isUpdateNewer() const {
  if (manualSource) return updateAvailable;
  if (!updateAvailable || latestVersion.empty() || latestVersion == INKADEMIC_VERSION) {
    return false;
  }

  const int comparison = firmware_version::compareForUpdate(latestVersion.c_str(), INKADEMIC_VERSION);
  LOG_DBG("OTA", "Version comparison latest=%s current=%s result=%d", latestVersion.c_str(), INKADEMIC_VERSION,
          comparison);
  return comparison > 0;
}

const std::string& OtaUpdater::getLatestVersion() const { return latestVersion; }

OtaUpdater::OtaUpdaterError OtaUpdater::downloadLatestToFiles(const char* imagePath, const char* signaturePath,
                                                              ProgressCallback onProgress, void* ctx) {
  if (!isUpdateNewer() || imagePath == nullptr || signaturePath == nullptr) return UPDATE_OLDER_ERROR;
  if (otaUrl.empty() || otaSignatureUrl.empty() || otaSize == 0) return SIGNATURE_MISSING_ERROR;

  processedSize = 0;
  totalSize = otaSize;
  HttpDownloader::DownloadOptions imageOptions(true, true);
  if (!otaSha256.empty()) imageOptions.transport = HttpDownloader::Transport::WOLFSSL;
  const auto imageResult = HttpDownloader::downloadToFile(
      otaUrl, imagePath,
      [&](const size_t downloaded, const size_t total) {
        processedSize = downloaded;
        totalSize = total > 0 ? total : otaSize;
        if (onProgress) onProgress(ctx);
      },
      nullptr, "", "", std::move(imageOptions));
  if (imageResult != HttpDownloader::OK) {
    return imageResult == HttpDownloader::ABORTED ? CANCELLED_ERROR : HTTP_ERROR;
  }

  const auto signatureResult = HttpDownloader::downloadToFile(otaSignatureUrl, signaturePath, nullptr, nullptr);
  if (signatureResult != HttpDownloader::OK) {
    return signatureResult == HttpDownloader::ABORTED ? CANCELLED_ERROR : SIGNATURE_MISSING_ERROR;
  }
  processedSize = totalSize;
  if (onProgress) onProgress(ctx);
  return OK;
}

OtaUpdater::OtaUpdaterError OtaUpdater::installUpdate(ProgressCallback onProgress, void* ctx,
                                                      std::atomic<bool>* cancelRequested) {
  const auto isCancellationRequested = [cancelRequested]() -> bool {
    return cancelRequested != nullptr && cancelRequested->load(std::memory_order_relaxed);
  };

  if (!isUpdateNewer()) {
    return UPDATE_OLDER_ERROR;
  }

  if (isCancellationRequested()) {
    return CANCELLED_ERROR;
  }
  if (manualSource) return installManualUpdate(onProgress, ctx, cancelRequested);

#if defined(INKADEMIC_REQUIRE_SIGNED_OTA) && INKADEMIC_REQUIRE_SIGNED_OTA
  if (otaSignatureUrl.empty()) {
    LOG_ERR("OTA", "Refusing unsigned firmware on this device");
    return SIGNATURE_MISSING_ERROR;
  }
#endif
  const bool hasManifestSha256 = isSha256Hex(otaSha256.c_str());
  if (!otaSha256.empty() && !hasManifestSha256) {
    LOG_ERR("OTA", "Refusing firmware with invalid manifest sha256");
    return JSON_PARSE_ERROR;
  }
  if (isHttpUrl(otaUrl) && !hasManifestSha256) {
    LOG_ERR("OTA", "Refusing HTTP firmware URL without manifest sha256");
    return JSON_PARSE_ERROR;
  }

  processedSize = 0;

  const esp_partition_t* updatePartition = esp_ota_get_next_update_partition(nullptr);
  if (updatePartition == nullptr) {
    LOG_ERR("OTA", "No OTA update partition found");
    return INTERNAL_UPDATE_ERROR;
  }

  if (otaSize > 0 && otaSize > updatePartition->size) {
    LOG_ERR("OTA", "Firmware too large: %zu > %zu", otaSize, updatePartition->size);
    return INTERNAL_UPDATE_ERROR;
  }

  esp_ota_handle_t otaHandle = 0;
  OtaInstallContext installCtx;
  installCtx.processedSize = &processedSize;
  installCtx.totalSize = totalSize;
  installCtx.onProgress = onProgress;
  installCtx.progressCtx = ctx;

  WifiPowerSaveGuard wifiPowerSaveGuard;

  LOG_INF("OTA", "Starting firmware download: url=%s heap=%u maxAlloc=%u", otaUrl.c_str(), ESP.getFreeHeap(),
          ESP.getMaxAllocHeap());

  mbedtls_sha256_context shaCtx;
  mbedtls_sha256_init(&shaCtx);
  mbedtls_sha256_starts(&shaCtx, /*is224=*/0);
  bool otaStarted = false;
  esp_err_t otaBeginError = ESP_OK;
  esp_err_t otaWriteError = ESP_OK;
  uint8_t imageHeader[14] = {};
  size_t imageHeaderLength = 0;
  bool wrongChip = false;
  firmware_identity::Scanner identity;

  HttpDownloader::DownloadOptions downloadOptions;
  downloadOptions.shouldCancel = isCancellationRequested;
  // wolfSSL currently has no CA bundle, so only use it when the trusted release
  // manifest supplied a digest that pins the firmware bytes. Older HTTPS
  // releases without a digest retain the verified esp_http_client path.
  if (hasManifestSha256) downloadOptions.transport = HttpDownloader::Transport::WOLFSSL;
  const auto transferResult = HttpDownloader::streamUrl(
      otaUrl,
      [&](const uint8_t* data, const size_t len) {
        if (len == 0) return true;
        const auto writeChunk = [&](const uint8_t* chunk, const size_t chunkLength) {
          if (chunkLength == 0) return true;
          if (!otaStarted) {
            const size_t firmwareSize = otaSize > 0 ? otaSize : OTA_SIZE_UNKNOWN;
            LOG_INF("OTA", "Writing firmware to %s @0x%x size=%zu heap=%u maxAlloc=%u", updatePartition->label,
                    static_cast<unsigned>(updatePartition->address), firmwareSize, ESP.getFreeHeap(),
                    ESP.getMaxAllocHeap());
            otaBeginError = ota_sector::begin(updatePartition, &otaHandle);
            if (otaBeginError != ESP_OK) {
              LOG_ERR("OTA", "esp_ota_begin failed: %s (heap=%u maxAlloc=%u)", esp_err_to_name(otaBeginError),
                      ESP.getFreeHeap(), ESP.getMaxAllocHeap());
              return false;
            }
            otaStarted = true;
          }

          otaWriteError = ota_sector::write(otaHandle, chunk, chunkLength, processedSize);
          if (otaWriteError != ESP_OK) {
            LOG_ERR("OTA", "esp_ota_write failed after %zu bytes: %s", processedSize, esp_err_to_name(otaWriteError));
            return false;
          }

          mbedtls_sha256_update(&shaCtx, chunk, chunkLength);
          for (size_t i = 0; i < chunkLength; ++i) identity.feed(static_cast<char>(chunk[i]));
          notifyOtaProgress(&installCtx, false);
          return true;
        };

        size_t dataOffset = 0;
        if (imageHeaderLength < sizeof(imageHeader)) {
          const size_t take = std::min(len, sizeof(imageHeader) - imageHeaderLength);
          std::memcpy(imageHeader + imageHeaderLength, data, take);
          imageHeaderLength += take;
          dataOffset = take;
          if (imageHeaderLength < sizeof(imageHeader)) return true;

          uint16_t imageChipId;
          std::memcpy(&imageChipId, imageHeader + 12, sizeof(imageChipId));
          const uint16_t runningChipId = firmware_flash::runningPartitionChipId();
          if (runningChipId != 0xFFFF && imageChipId != runningChipId) {
            LOG_ERR("OTA", "wrong chip: image=0x%04X device=0x%04X", imageChipId, runningChipId);
            wrongChip = true;
            return false;
          }

          if (!writeChunk(imageHeader, sizeof(imageHeader))) return false;
        }

        return writeChunk(data + dataOffset, len - dataOffset);
      },
      nullptr, "", "", std::move(downloadOptions));

  if (transferResult != HttpDownloader::OK) {
    mbedtls_sha256_free(&shaCtx);
    if (otaStarted) esp_ota_abort(otaHandle);
    if (wrongChip) return WRONG_DEVICE_ERROR;
    if (transferResult == HttpDownloader::ABORTED || isCancellationRequested()) {
      LOG_INF("OTA", "Update cancelled");
      return CANCELLED_ERROR;
    }
    if (otaBeginError != ESP_OK) return otaBeginError == ESP_ERR_NO_MEM ? OOM_ERROR : INTERNAL_UPDATE_ERROR;
    if (otaWriteError != ESP_OK) return INTERNAL_UPDATE_ERROR;
    LOG_ERR("OTA", "Firmware download failed after %zu/%zu bytes", processedSize, totalSize);
    return HTTP_ERROR;
  }

  if (!otaStarted || processedSize == 0) {
    LOG_ERR("OTA", "Firmware download returned no data");
    mbedtls_sha256_free(&shaCtx);
    if (otaStarted) esp_ota_abort(otaHandle);
    return HTTP_ERROR;
  }
  if (otaSize > 0 && processedSize != otaSize) {
    LOG_ERR("OTA", "Firmware size mismatch: got %zu, expected %zu", processedSize, otaSize);
    mbedtls_sha256_free(&shaCtx);
    esp_ota_abort(otaHandle);
    return INTERNAL_UPDATE_ERROR;
  }

  notifyOtaProgress(&installCtx, true);

  uint8_t computedSha256[32];
  mbedtls_sha256_finish(&shaCtx, computedSha256);
  mbedtls_sha256_free(&shaCtx);
  if (hasManifestSha256) {
    if (!sha256Matches(computedSha256, otaSha256.c_str())) {
      char computedSha256Hex[65];
      formatSha256(computedSha256, computedSha256Hex);
      LOG_ERR("OTA", "Firmware sha256 mismatch: expected=%s actual=%s", otaSha256.c_str(), computedSha256Hex);
      esp_ota_abort(otaHandle);
      return HASH_MISMATCH_ERROR;
    }
    LOG_INF("OTA", "Firmware sha256 verified");
  }

  uint8_t otaSignature[OTA_SIGNATURE_SIZE] = {};
  size_t otaSignatureBytes = 0;
  if (!otaSignatureUrl.empty()) {
    const auto signatureResult = HttpDownloader::streamUrl(
        otaSignatureUrl,
        [&](const uint8_t* data, const size_t len) {
          if (len == 0) return true;
          if (otaSignatureBytes + len > sizeof(otaSignature)) {
            LOG_ERR("OTA", "Ed25519 signature asset is too large");
            return false;
          }
          memcpy(otaSignature + otaSignatureBytes, data, len);
          otaSignatureBytes += len;
          return true;
        },
        nullptr, "", "", HttpDownloader::DownloadOptions());
    if (signatureResult != HttpDownloader::OK || otaSignatureBytes != sizeof(otaSignature)) {
      LOG_ERR("OTA", "Ed25519 signature download failed (%zu bytes, result=%d)", otaSignatureBytes,
              static_cast<int>(signatureResult));
      esp_ota_abort(otaHandle);
#if defined(INKADEMIC_REQUIRE_SIGNED_OTA) && INKADEMIC_REQUIRE_SIGNED_OTA
      return signatureResult == HttpDownloader::ABORTED ? CANCELLED_ERROR : SIGNATURE_MISSING_ERROR;
#else
      return SIGNATURE_INVALID_ERROR;
#endif
    }
#if defined(HAVE_ED25519) && defined(HAVE_ED25519_VERIFY) && defined(HAVE_ED25519_KEY_IMPORT)
    if (!ota_signature::verify(computedSha256, otaSignature, otaSignatureBytes)) {
      LOG_ERR("OTA", "Ed25519 signature verification failed");
      esp_ota_abort(otaHandle);
      return SIGNATURE_INVALID_ERROR;
    }
    LOG_INF("OTA", "Ed25519 signature verified");
#else
    LOG_ERR("OTA", "Signature received but Ed25519 support is unavailable");
    esp_ota_abort(otaHandle);
    return SIGNATURE_INVALID_ERROR;
#endif
  }

  if (!identity.found() || std::strcmp(identity.device(), firmware_identity::deviceType()) != 0) {
    esp_ota_abort(otaHandle);
    return WRONG_DEVICE_ERROR;
  }
  if (firmware_version::compareForUpdate(identity.version(), INKADEMIC_VERSION) <= 0) {
    esp_ota_abort(otaHandle);
    return UPDATE_OLDER_ERROR;
  }
  esp_err_t esp_err = esp_ota_end(otaHandle);
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_ota_end failed: %s", esp_err_to_name(esp_err));
    return INTERNAL_UPDATE_ERROR;
  }

  // esp_ota_end flushes any buffered bytes before the readback. It does not
  // select the next boot partition; reject mismatches before that final step.
  if (!firmware_flash::verifyPartitionDigest(updatePartition, processedSize, computedSha256)) {
    return HASH_MISMATCH_ERROR;
  }

  esp_err = esp_ota_set_boot_partition(updatePartition);
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_ota_set_boot_partition failed: %s", esp_err_to_name(esp_err));
    return INTERNAL_UPDATE_ERROR;
  }

  LOG_INF("OTA", "Update completed: %zu bytes", processedSize);
  return OK;
}
#endif
