#pragma once

#include <esp_partition.h>

#include <cstddef>
#include <cstdint>

// Flash a firmware image from an SD-card path into the next OTA app
// partition, then switch otadata so the X3/X4 stock bootloader picks it up
// on next boot. Mirrors the web flasher: raw esp_partition_erase_range +
// esp_partition_write + ota_boot::switchTo (no Arduino Update class, no
// esp_image_verify — those reject our patched image on X4 silicon).
//
// SD and browser updates land here. The on-device OTA menu uses the SDK
// streaming updater with sector-bounded writes in OtaSectorWriter.h.

namespace firmware_flash {

enum class Result {
  OK,
  OPEN_FAIL,
  TOO_SMALL,
  TOO_LARGE,
  BAD_MAGIC,
  BAD_SEGMENTS,  // segment table malformed or runs past EOF
  BAD_CHECKSUM,  // ESP image XOR checksum mismatch
  BAD_SHA,       // SHA256 trailer mismatch (hash_appended images)
  BAD_CHIP,      // image chip_id doesn't match the running MCU family
  BAD_SIZE,      // body+pad+sha length doesn't match file size
  BAD_TARGET,    // embedded INKademic device identity does not match this device
  BAD_VERSION,   // candidate is not newer than the running firmware
  SIGNATURE_MISSING,
  SIGNATURE_INVALID,
  NO_PARTITION,
  OOM,
  READ_FAIL,
  ERASE_FAIL,
  WRITE_FAIL,
  OTADATA_FAIL,
};

// Progress callback: called after every chunk write. `written`/`total` are bytes.
using ProgressCb = void (*)(size_t written, size_t total, void* ctx);

// Open `sdPath`, validate it looks like an ESP32 image, then stream it into the
// next OTA app partition with interleaved 4 KiB erase + sector writes. On
// success switches otadata via ota_boot::switchTo. Caller is responsible for
// ESP.restart() afterwards.
//
// A caller may skip the integrity pass only by providing both alreadyValidated
// and the 32-byte full-file digest returned by validation. The digest is checked
// against actual flash before boot selection. A boolean alone never bypasses
// validation after reopening SD. Manual SD updates do not require an INKademic
// identity or signature, so compatible firmware from other projects stays usable.
Result flashFromSdPath(const char* sdPath, ProgressCb onProgress, void* ctx, bool alreadyValidated = false,
                       const uint8_t* authenticatedDigest = nullptr);

// Full-image integrity check that mirrors the bootloader's verification:
// header magic, segment table walk, XOR checksum, and SHA256 trailer (when
// hash_appended == 1). Run this before flashing a candidate firmware so a
// truncated/corrupted .bin never reaches otadata.
//
// `partitionSize` is the size of the destination OTA partition; pass 0 to
// skip the size-fits-partition check (e.g. when validating ahead of partition
// lookup). Streams the file in CHUNK-sized reads and closes it on return.
// If supplied, fullDigest receives SHA-256 of the entire file, including trailer.
Result validateImageFile(const char* sdPath, size_t partitionSize, uint8_t* fullDigest = nullptr);

// Manual files may come from other projects. SignedRelease is the default and
// must remain in effect for the official catalog and unattended update checks.
enum class ValidationPolicy { SignedRelease, Manual };

// Validate an image for the browser updater. In addition to the ESP image
// checks above, this requires the identity marker embedded by the build,
// rejects cross-device and downgrade images, and verifies the raw Ed25519
// signature over the file's raw 32-byte SHA-256 digest (the signature is
// exactly 64 bytes).
Result validateBrowserImageFile(const char* sdPath, size_t partitionSize, const char* expectedDevice,
                                const char* currentVersion, const char* signaturePath, char* imageDevice,
                                size_t imageDeviceCapacity, char* imageVersion, size_t imageVersionCapacity,
                                uint8_t* authenticatedDigest = nullptr,
                                ValidationPolicy policy = ValidationPolicy::SignedRelease);

// Verify actual flash contents before changing the boot partition.
bool verifyPartitionDigest(const esp_partition_t* partition, size_t size, const uint8_t expected[32]);

const char* resultName(Result r);

// Returns the chip_id at byte 12 of the running app image, or 0xFFFF when it
// cannot be read. The running image booted successfully, so its ID is the
// authoritative compatibility value for candidate firmware.
uint16_t runningPartitionChipId();

}  // namespace firmware_flash
