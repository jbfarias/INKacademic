#include "FirmwareRequestPolicy.h"
#include "OtaUpdater.h"

bool OtaUpdater::setManualSource(const std::string& url) {
  if (!firmware_request::validManualUrl(url)) return false;
  otaUrl = url;
  manualSource = true;
  updateAvailable = true;
  otaSha256.clear();
  otaSignatureUrl.clear();
  otaSize = totalSize = processedSize = 0;
  latestVersion.clear();
  return true;
}

#ifndef SIMULATOR
#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>
#include <esp_ota_ops.h>
#include <esp_task_wdt.h>

#include "FirmwareFlasher.h"
#include "HttpDownloader.h"
#include "WifiPowerSaveGuard.h"

OtaUpdater::OtaUpdaterError OtaUpdater::loadSavedSource() {
  if (!Storage.exists(MANUAL_SOURCE_PATH)) {
    manualSource = updateAvailable = false;
    otaUrl.clear();
    return OK;
  }
  HalFile file;
  if (!Storage.openFileForRead("OTA", MANUAL_SOURCE_PATH, file) || !file) return HTTP_ERROR;
  const size_t size = file.fileSize();
  if (size == 0 || size > 1024) {
    file.close();
    return JSON_PARSE_ERROR;
  }
  // Saved URL only: a bounded cold-path string, never a whole firmware buffer.
  std::string url(size, '\0');
  const int count = file.read(reinterpret_cast<uint8_t*>(url.data()), size);
  file.close();
  if (count != static_cast<int>(size) || !setManualSource(url)) return JSON_PARSE_ERROR;
  return OK;
}

OtaUpdater::OtaUpdaterError OtaUpdater::downloadManualToFile(const char* path, ProgressCallback onProgress, void* ctx,
                                                             std::atomic<bool>* cancelRequested) {
  if (!manualSource || !firmware_request::validManualUrl(otaUrl) || !path) return JSON_PARSE_ERROR;
  const esp_partition_t* destination = esp_ota_get_next_update_partition(nullptr);
  if (!destination) return INTERNAL_UPDATE_ERROR;
  HalFile file;
  if (!Storage.openFileForWrite("OTA", path, file) || !file) return INTERNAL_UPDATE_ERROR;
  processedSize = totalSize = 0;
  bool writeFailed = false;
  HttpDownloader::DownloadOptions options;
  options.shouldCancel = [cancelRequested] { return cancelRequested && cancelRequested->load(); };
  // Default ESP_HTTP validates HTTPS certificates. Manual sources have no
  // trusted release digest to authorize the wolfSSL transport without CAs.
  WifiPowerSaveGuard wifiPowerSaveGuard;
  const auto result = HttpDownloader::streamUrl(
      otaUrl,
      [&](const uint8_t* bytes, size_t count) {
        if (count > destination->size - processedSize || file.write(bytes, count) != count) {
          writeFailed = true;
          return false;
        }
        processedSize += count;
        esp_task_wdt_reset();
        if (onProgress) onProgress(ctx);
        return true;
      },
      [&](size_t, size_t total) { totalSize = total; }, "", "", std::move(options));
  const bool synced = file.sync();
  file.close();
  if (result != HttpDownloader::OK || writeFailed || !synced) {
    Storage.remove(path);
    LOG_ERR("OTA", "Manual firmware download failed (result=%d, write=%d)", static_cast<int>(result), writeFailed);
    return result == HttpDownloader::ABORTED && !writeFailed ? CANCELLED_ERROR : HTTP_ERROR;
  }
  otaSize = totalSize = processedSize;
  return OK;
}

OtaUpdater::OtaUpdaterError OtaUpdater::installManualUpdate(ProgressCallback onProgress, void* ctx,
                                                            std::atomic<bool>* cancelRequested) {
  constexpr const char* path = "/.inkademic-ota-manual.bin";
  const auto downloaded = downloadManualToFile(path, onProgress, ctx, cancelRequested);
  if (downloaded != OK) return downloaded;
  if ((cancelRequested && cancelRequested->load())) {
    Storage.remove(path);
    return CANCELLED_ERROR;
  }
  processedSize = 0;
  struct Progress {
    OtaUpdater* updater;
    ProgressCallback callback;
    void* context;
  } progress{this, onProgress, ctx};
  const auto result = firmware_flash::flashFromSdPath(
      path,
      [](size_t written, size_t total, void* context) {
        auto* p = static_cast<Progress*>(context);
        p->updater->processedSize = written;
        p->updater->totalSize = total;
        if (p->callback) p->callback(p->context);
      },
      &progress);
  Storage.remove(path);
  if (result == firmware_flash::Result::BAD_CHIP) return WRONG_DEVICE_ERROR;
  if (result == firmware_flash::Result::BAD_SHA) return HASH_MISMATCH_ERROR;
  if (result != firmware_flash::Result::OK) return INTERNAL_UPDATE_ERROR;
  return OK;
}

#endif
