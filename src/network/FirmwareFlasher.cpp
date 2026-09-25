#include "FirmwareFlasher.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_task_wdt.h>
#include <mbedtls/sha256.h>
#include <spi_flash_mmap.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>

#include "FirmwareIdentityScanner.h"
#include "FirmwareVersion.h"
#include "OtaBootSwitch.h"
#include "OtaSignature.h"

namespace firmware_flash {

namespace {
constexpr uint8_t ESP_IMAGE_MAGIC = 0xE9;
constexpr size_t MIN_FIRMWARE_SIZE = 64 * 1024;
constexpr size_t SEC = SPI_FLASH_SEC_SIZE;  // 4 KiB
// Keep each erase call to one sector. The previous 16 KiB window was already
// safer than the old up-front erase, but an X4 Pro can still spend long enough
// inside a multi-sector flash call for the task watchdog to fire. Sector-sized
// writes cost more calls but leave a bounded interval between every erase and
// watchdog service point.
constexpr size_t BLK = SEC;
constexpr size_t CHUNK = 4096;
constexpr size_t SHA_TRAILER = 32;
constexpr uint8_t CHECKSUM_SEED = 0xEF;
constexpr size_t HEADER_SIZE = 24;
constexpr size_t SEG_HEADER_SIZE = 8;
}  // namespace

const char* resultName(Result r) {
  switch (r) {
    case Result::OK:
      return "OK";
    case Result::OPEN_FAIL:
      return "OPEN_FAIL";
    case Result::TOO_SMALL:
      return "TOO_SMALL";
    case Result::TOO_LARGE:
      return "TOO_LARGE";
    case Result::BAD_MAGIC:
      return "BAD_MAGIC";
    case Result::BAD_SEGMENTS:
      return "BAD_SEGMENTS";
    case Result::BAD_CHECKSUM:
      return "BAD_CHECKSUM";
    case Result::BAD_SHA:
      return "BAD_SHA";
    case Result::BAD_CHIP:
      return "BAD_CHIP";
    case Result::BAD_SIZE:
      return "BAD_SIZE";
    case Result::BAD_TARGET:
      return "BAD_TARGET";
    case Result::BAD_VERSION:
      return "BAD_VERSION";
    case Result::SIGNATURE_MISSING:
      return "SIGNATURE_MISSING";
    case Result::SIGNATURE_INVALID:
      return "SIGNATURE_INVALID";
    case Result::NO_PARTITION:
      return "NO_PARTITION";
    case Result::OOM:
      return "OOM";
    case Result::READ_FAIL:
      return "READ_FAIL";
    case Result::ERASE_FAIL:
      return "ERASE_FAIL";
    case Result::WRITE_FAIL:
      return "WRITE_FAIL";
    case Result::OTADATA_FAIL:
      return "OTADATA_FAIL";
  }
  return "?";
}

uint16_t runningPartitionChipId() {
  // Reading SPI flash is relatively expensive; the running image is immutable,
  // so cache its chip ID once per boot.
  static const uint16_t cached = [] {
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (running == nullptr) return static_cast<uint16_t>(0xFFFF);

    uint16_t chipId = 0xFFFF;
    if (esp_partition_read(running, 12, &chipId, sizeof(chipId)) != ESP_OK) {
      return static_cast<uint16_t>(0xFFFF);
    }
    return chipId;
  }();
  return cached;
}

namespace {
class FlashWatchdogGuard {
 public:
  FlashWatchdogGuard() {
#ifndef SIMULATOR
    // Arduino may initialize TWDT before the application reaches setup(), so
    // accepting ESP_ERR_INVALID_STATE from esp_task_wdt_init() does not tell us
    // which timeout is active. Give the long SD->OTA transaction an explicit
    // bounded window and restore the normal application timeout afterwards.
    const esp_task_wdt_config_t flashConfig = {
        60U * 1000U,
        0,
        true,
    };
    reconfigured = esp_task_wdt_reconfigure(&flashConfig) == ESP_OK;
    if (!reconfigured) {
      LOG_DBG("FLASH", "TWDT reconfigure unavailable; continuing with active watchdog");
    }
#endif
    esp_task_wdt_reset();
  }

  ~FlashWatchdogGuard() {
    esp_task_wdt_reset();
#ifndef SIMULATOR
    if (reconfigured) {
      const esp_task_wdt_config_t appConfig = {
          15U * 1000U,
          0,
          true,
      };
      (void)esp_task_wdt_reconfigure(&appConfig);
    }
#endif
  }

  FlashWatchdogGuard(const FlashWatchdogGuard&) = delete;
  FlashWatchdogGuard& operator=(const FlashWatchdogGuard&) = delete;

 private:
  bool reconfigured = false;
};

// Stream `length` bytes from `file` starting at the current read offset, feeding them through
// both the XOR-checksum and SHA256 accumulators. Used by validateImageFile so the whole image
// is verified end-to-end without holding it in RAM (ESP32-C3 only has ~380 KB).
Result feedHashAndChecksum(HalFile& file, size_t length, uint8_t* xorAccum, mbedtls_sha256_context* sha, uint8_t* buf) {
  size_t remaining = length;
  while (remaining > 0) {
    const size_t want = std::min<size_t>(CHUNK, remaining);
    const int got = file.read(buf, want);
    if (got <= 0 || static_cast<size_t>(got) != want) return Result::READ_FAIL;
    if (sha) mbedtls_sha256_update(sha, buf, want);
    if (xorAccum) {
      uint8_t acc = *xorAccum;
      for (size_t i = 0; i < want; i++) acc ^= buf[i];
      *xorAccum = acc;
    }
    remaining -= want;
    esp_task_wdt_reset();
    yield();
  }
  return Result::OK;
}
}  // namespace

Result validateImageFile(const char* sdPath, size_t partitionSize, uint8_t* fullDigest) {
  HalFile file;
  if (!Storage.openFileForRead("FLASH", sdPath, file) || !file) {
    LOG_ERR("FLASH", "validate: open failed: %s", sdPath);
    return Result::OPEN_FAIL;
  }

  const size_t fileSize = file.fileSize();
  if (fileSize < MIN_FIRMWARE_SIZE) {
    LOG_ERR("FLASH", "validate: too small: %u", static_cast<unsigned>(fileSize));
    file.close();
    return Result::TOO_SMALL;
  }
  if (partitionSize > 0 && fileSize > partitionSize) {
    LOG_ERR("FLASH", "validate: too large: %u > %u", static_cast<unsigned>(fileSize),
            static_cast<unsigned>(partitionSize));
    file.close();
    return Result::TOO_LARGE;
  }

  uint8_t header[HEADER_SIZE];
  if (file.read(header, HEADER_SIZE) != static_cast<int>(HEADER_SIZE)) {
    LOG_ERR("FLASH", "validate: header read failed");
    file.close();
    return Result::READ_FAIL;
  }
  if (header[0] != ESP_IMAGE_MAGIC) {
    LOG_ERR("FLASH", "validate: bad magic 0x%02X", header[0]);
    file.close();
    return Result::BAD_MAGIC;
  }
  uint16_t imageChipId;
  std::memcpy(&imageChipId, header + 12, sizeof(imageChipId));
  const uint16_t runningChipId = runningPartitionChipId();
  if (runningChipId != 0xFFFF && imageChipId != runningChipId) {
    LOG_ERR("FLASH", "validate: wrong chip: image=0x%04X device=0x%04X", imageChipId, runningChipId);
    file.close();
    return Result::BAD_CHIP;
  }
  const uint8_t segCount = header[1];
  const bool hashAppended = header[23] != 0;

  // Reuse 4 KiB off the small task stack for the entire validation pass.
  auto buf = makeUniqueNoThrow<uint8_t[]>(CHUNK);
  if (!buf) {
    LOG_ERR("FLASH", "OOM allocating 4 KiB validation buffer");
    file.close();
    return Result::OOM;
  }

  mbedtls_sha256_context shaCtx;
  mbedtls_sha256_init(&shaCtx);
  mbedtls_sha256_starts(&shaCtx, /*is224=*/0);
  mbedtls_sha256_update(&shaCtx, header, HEADER_SIZE);

  uint8_t xorAccum = CHECKSUM_SEED;
  size_t pos = HEADER_SIZE;

  for (uint8_t i = 0; i < segCount; i++) {
    if (pos + SEG_HEADER_SIZE > fileSize) {
      LOG_ERR("FLASH", "validate: seg %u header overruns EOF at %u", i, static_cast<unsigned>(pos));
      mbedtls_sha256_free(&shaCtx);
      file.close();
      return Result::BAD_SEGMENTS;
    }
    uint8_t segHdr[SEG_HEADER_SIZE];
    if (file.read(segHdr, SEG_HEADER_SIZE) != static_cast<int>(SEG_HEADER_SIZE)) {
      mbedtls_sha256_free(&shaCtx);
      file.close();
      return Result::READ_FAIL;
    }
    mbedtls_sha256_update(&shaCtx, segHdr, SEG_HEADER_SIZE);
    pos += SEG_HEADER_SIZE;

    uint32_t dataLen;
    std::memcpy(&dataLen, segHdr + 4, sizeof(dataLen));
    if (pos + dataLen > fileSize) {
      LOG_ERR("FLASH", "validate: seg %u data overruns EOF (%u + %u > %u)", i, static_cast<unsigned>(pos),
              static_cast<unsigned>(dataLen), static_cast<unsigned>(fileSize));
      mbedtls_sha256_free(&shaCtx);
      file.close();
      return Result::BAD_SEGMENTS;
    }

    const Result feedRes = feedHashAndChecksum(file, dataLen, &xorAccum, &shaCtx, buf.get());
    if (feedRes != Result::OK) {
      mbedtls_sha256_free(&shaCtx);
      file.close();
      return feedRes;
    }
    pos += dataLen;
  }

  // pad_end is the 16-byte aligned offset at which the checksum byte sits at pad_end - 1.
  const size_t padEnd = (pos + 16) & ~static_cast<size_t>(15);
  const size_t expectedTotal = padEnd + (hashAppended ? SHA_TRAILER : 0);
  if (expectedTotal != fileSize) {
    LOG_ERR("FLASH", "validate: size mismatch body+pad=%u sha=%u expected=%u actual=%u", static_cast<unsigned>(padEnd),
            static_cast<unsigned>(hashAppended ? SHA_TRAILER : 0), static_cast<unsigned>(expectedTotal),
            static_cast<unsigned>(fileSize));
    mbedtls_sha256_free(&shaCtx);
    file.close();
    return Result::BAD_SIZE;
  }

  // Read the padding bytes (which include the stored checksum at the last byte) into the SHA stream.
  const size_t padLen = padEnd - pos;
  uint8_t padBuf[16];
  if (padLen > sizeof(padBuf)) {
    mbedtls_sha256_free(&shaCtx);
    file.close();
    return Result::BAD_SIZE;
  }
  if (padLen > 0 && file.read(padBuf, padLen) != static_cast<int>(padLen)) {
    mbedtls_sha256_free(&shaCtx);
    file.close();
    return Result::READ_FAIL;
  }
  mbedtls_sha256_update(&shaCtx, padBuf, padLen);

  const uint8_t storedChecksum = padBuf[padLen - 1];
  if ((xorAccum & 0xFF) != storedChecksum) {
    LOG_ERR("FLASH", "validate: checksum mismatch computed=0x%02X stored=0x%02X", xorAccum, storedChecksum);
    mbedtls_sha256_free(&shaCtx);
    file.close();
    return Result::BAD_CHECKSUM;
  }

  if (hashAppended) {
    uint8_t computed[SHA_TRAILER];
    mbedtls_sha256_context copy;
    mbedtls_sha256_init(&copy);
    mbedtls_sha256_clone(&copy, &shaCtx);
    mbedtls_sha256_finish(&copy, computed);
    mbedtls_sha256_free(&copy);
    uint8_t stored[SHA_TRAILER];
    if (file.read(stored, SHA_TRAILER) != static_cast<int>(SHA_TRAILER)) {
      mbedtls_sha256_free(&shaCtx);
      file.close();
      return Result::READ_FAIL;
    }
    if (std::memcmp(computed, stored, SHA_TRAILER) != 0) {
      LOG_ERR("FLASH", "validate: SHA256 mismatch");
      mbedtls_sha256_free(&shaCtx);
      file.close();
      return Result::BAD_SHA;
    }
    mbedtls_sha256_update(&shaCtx, stored, SHA_TRAILER);
  }

  if (fullDigest) mbedtls_sha256_finish(&shaCtx, fullDigest);
  mbedtls_sha256_free(&shaCtx);
  file.close();
  return Result::OK;
}

namespace {
bool computeFileSha256(const char* sdPath, uint8_t digest[32], firmware_identity::Scanner* identity = nullptr) {
  HalFile file;
  if (!Storage.openFileForRead("FLASH", sdPath, file) || !file) return false;
  // One 4 KiB buffer per scan, not per chunk; too large for the task stack.
  auto buffer = makeUniqueNoThrow<uint8_t[]>(CHUNK);
  if (!buffer) {
    LOG_ERR("FLASH", "OOM allocating 4 KiB hash buffer");
    file.close();
    return false;
  }
  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts(&sha, 0);
  size_t remaining = file.fileSize();
  while (remaining > 0) {
    const size_t want = std::min<size_t>(CHUNK, remaining);
    const int got = file.read(buffer.get(), want);
    if (got <= 0 || static_cast<size_t>(got) != want) {
      mbedtls_sha256_free(&sha);
      file.close();
      return false;
    }
    mbedtls_sha256_update(&sha, buffer.get(), want);
    if (identity)
      for (size_t i = 0; i < want; ++i) identity->feed(static_cast<char>(buffer[i]));
    remaining -= want;
    esp_task_wdt_reset();
    yield();
  }
  mbedtls_sha256_finish(&sha, digest);
  mbedtls_sha256_free(&sha);
  file.close();
  return true;
}

bool verifyDigestSignature(const uint8_t digest[32], const char* signaturePath) {
  HalFile signature;
  if (!Storage.openFileForRead("FLASH", signaturePath, signature) || !signature) return false;
  uint8_t rawSignature[64] = {};
  const bool ok = signature.fileSize() == sizeof(rawSignature) &&
                  signature.read(rawSignature, sizeof(rawSignature)) == static_cast<int>(sizeof(rawSignature));
  signature.close();
  return ok && ota_signature::verify(digest, rawSignature, sizeof(rawSignature));
}
}  // namespace

Result validateBrowserImageFile(const char* sdPath, size_t partitionSize, const char* expectedDevice,
                                const char* currentVersion, const char* signaturePath, char* imageDevice,
                                size_t imageDeviceCapacity, char* imageVersion, size_t imageVersionCapacity,
                                uint8_t* authenticatedDigest, ValidationPolicy policy) {
  if (imageDevice && imageDeviceCapacity) imageDevice[0] = '\0';
  if (imageVersion && imageVersionCapacity) imageVersion[0] = '\0';
  if (policy == ValidationPolicy::Manual) {
    // Explicit user choice: integrity/chip/size checks apply, but another
    // project does not need our identity, version scheme, or signing key.
    return validateImageFile(sdPath, partitionSize, authenticatedDigest);
  }
  uint8_t structuralDigest[32];
  const Result baseResult = validateImageFile(sdPath, partitionSize, structuralDigest);
  if (baseResult != Result::OK) return baseResult;

  uint8_t digest[32];
  firmware_identity::Scanner identity;
  // Identity and signed digest come from the same read, not two independently
  // reopenable files. The caller carries this digest into the flash operation.
  if (!computeFileSha256(sdPath, digest, &identity)) return Result::READ_FAIL;
  if (std::memcmp(digest, structuralDigest, sizeof(digest)) != 0) return Result::BAD_SHA;
  if (!identity.found() || !imageDevice || !imageVersion || std::strlen(identity.device()) >= imageDeviceCapacity ||
      std::strlen(identity.version()) >= imageVersionCapacity)
    return Result::BAD_TARGET;
  std::strcpy(imageDevice, identity.device());
  std::strcpy(imageVersion, identity.version());
  if (expectedDevice == nullptr || std::strcmp(imageDevice, expectedDevice) != 0) {
    LOG_ERR("FLASH", "browser validation: target=%s expected=%s", imageDevice,
            expectedDevice ? expectedDevice : "none");
    return Result::BAD_TARGET;
  }
  if (currentVersion == nullptr || firmware_version::compareForUpdate(imageVersion, currentVersion) <= 0) {
    LOG_ERR("FLASH", "browser validation: candidate=%s current=%s", imageVersion,
            currentVersion ? currentVersion : "none");
    return Result::BAD_VERSION;
  }
  if (signaturePath == nullptr || !Storage.exists(signaturePath)) return Result::SIGNATURE_MISSING;
  if (!verifyDigestSignature(digest, signaturePath)) {
    LOG_ERR("FLASH", "browser validation: Ed25519 signature rejected");
    return Result::SIGNATURE_INVALID;
  }
  if (authenticatedDigest) std::memcpy(authenticatedDigest, digest, 32);
  return Result::OK;
}

bool verifyPartitionDigest(const esp_partition_t* partition, size_t size, const uint8_t expected[32]) {
  if (!partition || !expected || size > partition->size) return false;
  // One reusable 4 KiB buffer: too large for the task stack, released on return.
  auto buffer = makeUniqueNoThrow<uint8_t[]>(CHUNK);
  if (!buffer) {
    LOG_ERR("FLASH", "OOM during flash readback");
    return false;
  }
  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts(&sha, 0);
  for (size_t offset = 0; offset < size;) {
    const size_t count = std::min(CHUNK, size - offset);
    esp_task_wdt_reset();
    if (esp_partition_read(partition, offset, buffer.get(), count) != ESP_OK) {
      mbedtls_sha256_free(&sha);
      LOG_ERR("FLASH", "Flash readback failed");
      return false;
    }
    mbedtls_sha256_update(&sha, buffer.get(), count);
    offset += count;
    esp_task_wdt_reset();
    delay(1);
  }
  uint8_t actual[32];
  mbedtls_sha256_finish(&sha, actual);
  mbedtls_sha256_free(&sha);
  const bool matches = std::memcmp(actual, expected, 32) == 0;
  if (!matches) LOG_ERR("FLASH", "Written firmware differs from validated image");
  return matches;
}

Result flashFromSdPath(const char* sdPath, ProgressCb onProgress, void* ctx, bool alreadyValidated,
                       const uint8_t* authenticatedDigest) {
  // Resolve destination first so we can size-check during validation. The full image-integrity
  // pass below verifies header, segment table, XOR checksum and SHA256 trailer end-to-end before
  // we touch otadata, so a truncated/corrupted .bin can never become the next boot target.
  const esp_partition_t* dest = esp_ota_get_next_update_partition(nullptr);
  if (!dest) {
    LOG_ERR("FLASH", "no next-update partition");
    return Result::NO_PARTITION;
  }

  // A prior validation is reusable only with its full-file digest. The final
  // readback binds the flashed bytes to that validation, even if SD changes.
  uint8_t expectedDigest[32];
  if (!alreadyValidated || !authenticatedDigest) {
    const Result validateRes = validateImageFile(sdPath, dest->size, expectedDigest);
    if (validateRes != Result::OK) {
      LOG_ERR("FLASH", "image validation failed: %s", resultName(validateRes));
      return validateRes;
    }
  }

  if (authenticatedDigest) std::memcpy(expectedDigest, authenticatedDigest, sizeof(expectedDigest));

  HalFile file;
  if (!Storage.openFileForRead("FLASH", sdPath, file) || !file) {
    LOG_ERR("FLASH", "open failed: %s", sdPath);
    return Result::OPEN_FAIL;
  }

  const size_t firmwareSize = file.fileSize();
  // Recheck even when a browser caller validated before reopening the file.
  if (firmwareSize < MIN_FIRMWARE_SIZE || firmwareSize > dest->size) {
    file.close();
    return Result::BAD_SIZE;
  }
  LOG_INF("FLASH", "src=%s size=%u dest=%s @0x%x partsize=%u", sdPath, static_cast<unsigned>(firmwareSize), dest->label,
          static_cast<unsigned>(dest->address), static_cast<unsigned>(dest->size));

  auto buffer = makeUniqueNoThrow<uint8_t[]>(CHUNK);
  if (!buffer) {
    LOG_ERR("FLASH", "OOM");
    file.close();
    return Result::OOM;
  }

  // The SD-to-flash transaction can take longer than the main loop watchdog
  // interval. Keep the RTC crash ring active, but do not let persistent log
  // capture compete for the storage path while the firmware image is being
  // streamed and the OTA partition is being erased/written.
  struct CapturePauseGuard {
    CapturePauseGuard() { pauseCapturedLogs(); }
    ~CapturePauseGuard() { resumeCapturedLogs(); }
  } capturePauseGuard;
  FlashWatchdogGuard flashWatchdogGuard;

  // Interleave erase + write so the progress bar advances 0→100% smoothly
  // rather than stalling for several seconds during a single up-front erase.
  size_t streamPos = 0;
  size_t erasedUpto = 0;
  while (streamPos < firmwareSize) {
    if (streamPos >= erasedUpto) {
      size_t eraseLen = std::min<size_t>(BLK, dest->size - streamPos);
      eraseLen = (eraseLen + SEC - 1) & ~(SEC - 1);
      eraseLen = std::min<size_t>(eraseLen, dest->size - streamPos);
      esp_task_wdt_reset();
      yield();
      if (esp_partition_erase_range(dest, streamPos, eraseLen) != ESP_OK) {
        LOG_ERR("FLASH", "erase @%u (len=%u) failed", static_cast<unsigned>(streamPos),
                static_cast<unsigned>(eraseLen));
        file.close();
        return Result::ERASE_FAIL;
      }
      erasedUpto = streamPos + eraseLen;
      esp_task_wdt_reset();
      yield();
      delay(1);
    }

    const size_t want = std::min<size_t>(CHUNK, firmwareSize - streamPos);
    const int read = file.read(buffer.get(), want);
    if (read <= 0 || static_cast<size_t>(read) != want) {
      LOG_ERR("FLASH", "read @%u: got=%d want=%u", static_cast<unsigned>(streamPos), read, static_cast<unsigned>(want));
      file.close();
      return Result::READ_FAIL;
    }
    if (esp_partition_write(dest, streamPos, buffer.get(), want) != ESP_OK) {
      LOG_ERR("FLASH", "write @%u failed", static_cast<unsigned>(streamPos));
      file.close();
      return Result::WRITE_FAIL;
    }
    streamPos += want;
    if (onProgress) onProgress(streamPos, firmwareSize, ctx);
    esp_task_wdt_reset();
    yield();
    delay(1);
  }
  file.close();

  buffer.reset();  // Release the write buffer before allocating the readback buffer.

  // Check actual flash contents against the authenticated bytes before changing
  // boot selection. This also catches SD changes during validation/write.
  if (!verifyPartitionDigest(dest, firmwareSize, expectedDigest)) return Result::BAD_SHA;

  if (!ota_boot::switchTo(dest)) {
    LOG_ERR("FLASH", "otadata switch failed");
    return Result::OTADATA_FAIL;
  }
  return Result::OK;
}

}  // namespace firmware_flash
