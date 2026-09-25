#pragma once

#include <atomic>
#include <string>

class OtaUpdater {
  bool updateAvailable = false;
  bool manualSource = false;
  std::string latestVersion;
  std::string otaUrl;
  std::string otaSha256;
  std::string otaSignatureUrl;
  size_t otaSize = 0;
  size_t otaSignatureSize = 0;
  size_t processedSize = 0;
  size_t totalSize = 0;

 public:
  using ProgressCallback = void (*)(void* ctx);

  enum OtaUpdaterError {
    OK = 0,
    NO_UPDATE,
    HTTP_ERROR,
    JSON_PARSE_ERROR,
    UPDATE_OLDER_ERROR,
    INTERNAL_UPDATE_ERROR,
    OOM_ERROR,
    CANCELLED_ERROR,
    HASH_MISMATCH_ERROR,
    WRONG_DEVICE_ERROR,
    SIGNATURE_MISSING_ERROR,
    SIGNATURE_INVALID_ERROR,
  };

  size_t getOtaSize() const { return otaSize; }

  size_t getProcessedSize() const { return processedSize; }

  size_t getTotalSize() const { return totalSize; }

  const std::string& getLatestUrl() const { return otaUrl; }
  const std::string& getLatestSha256() const { return otaSha256; }
  const std::string& getLatestSignatureUrl() const { return otaSignatureUrl; }
  size_t getLatestSignatureSize() const { return otaSignatureSize; }

  static constexpr const char* MANUAL_SOURCE_PATH = "/.inkademic-ota-source";
  OtaUpdater() = default;
  bool isManualSource() const { return manualSource; }
  bool setManualSource(const std::string& url);
  // Only the on-device menu opts into the user's saved manual source. The
  // official browser catalog constructs a default updater and remains signed.
  OtaUpdaterError loadSavedSource();
  OtaUpdaterError downloadManualToFile(const char* path, ProgressCallback onProgress = nullptr, void* ctx = nullptr,
                                       std::atomic<bool>* cancelRequested = nullptr);
  bool isUpdateNewer() const;
  const std::string& getLatestVersion() const;
  OtaUpdaterError checkForUpdate();
  // Download the already checked official release to SD-card files. The
  // caller still performs the same local image, target, and signature
  // validation before promoting either file to the install queue.
  OtaUpdaterError downloadLatestToFiles(const char* imagePath, const char* signaturePath,
                                        ProgressCallback onProgress = nullptr, void* ctx = nullptr);
  OtaUpdaterError installUpdate(ProgressCallback onProgress = nullptr, void* ctx = nullptr,
                                std::atomic<bool>* cancelRequested = nullptr);
  // Explicit manual source only; never used by the official catalog.
  OtaUpdaterError installManualUpdate(ProgressCallback onProgress, void* ctx, std::atomic<bool>* cancelRequested);
};
