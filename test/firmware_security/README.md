# Firmware update regressions

Standalone suite, also run by the X4 Pro CI build after PlatformIO installs
Arduino-wolfSSL 5.7.2. Requires CMake, C/C++ compiler, Python, Node and OpenSSL.

```sh
cmake -S test/firmware_security -B /tmp/firmware-security
cmake --build /tmp/firmware-security -j2
ctest --test-dir /tmp/firmware-security --output-on-failure
```

Set `WOLFSSL_ROOT` to the `src` directory of Arduino-wolfSSL 5.7.2 if its
PlatformIO installation is elsewhere. On macOS, CMake may also need
`-DOPENSSL_ROOT_DIR=/opt/homebrew/opt/openssl@3`.

The suite compiles production `FirmwareFlasher.cpp` and `OtaManualUpdater.cpp`.
Storage, HTTP and ESP flash calls are replaced by deterministic test backends;
SHA-256 and Ed25519 are real cryptographic implementations. Ed25519 uses the
same wolfSSL sources as hardware. SHA-256 uses OpenSSL as the host adapter.
No private signing key is needed or included.

Coverage includes:

- Three public signed release digests, modified digest/signature and wrong length.
- Official browser rejection of unsigned, wrong-model and older images.
- Explicit unsigned manual installation through SD, browser and OTA, with no
  INKademic identity marker; official policy remains the default.
- Partial reads/changed SD bytes, bad flash readback, erase/write/read failures,
  chip mismatch, corrupt images, excessive download size and cancellation.
- Manual source loading, URL limits, return to official source, verified HTTPS
  transport selection and retention of the current boot slot on failure.
- Bounded identity/version parsing, request origin/header and multipart ownership.
- Sector boundaries, 14-byte first header and watchdog servicing.
- Actual production bitmap functions: four orientations, odd widths and clipping.
- Real browser JavaScript with a fake DOM/transport: signed and manual uploads,
  OTA URL/source selection and failure responses that must not report success.

ASan and UBSan are enabled. wolfSSL's ref10 field implementation uses signed
shifts that UBSan reports on the host: only `shift-base` is suppressed on that
third-party library. All UBSan checks remain enabled on the application code;
ASan and other UBSan checks remain enabled on wolfSSL. These tests do not
certify radio behavior, flash timing, bootloader behavior or hardware watchdogs.

`ReleaseSignatures.h` contains only SHA-256 digests and public signatures from
[the v1.8.0-rc-2 release](https://github.com/jbfarias/INKademic/releases/tag/v1.8.0-rc-2).
For a full signed-image browser/flash test, download the X4 Pro `.bin`/`.bin.sig`
to a directory as `rc2-x4-pro.bin` and `rc2-x4-pro.bin.sig`, then run:

```sh
UBSAN_OPTIONS=halt_on_error=1 /tmp/firmware-security/firmware_security_test /path/to/assets
```

The default suite keeps only 96 bytes per release vector, not multi-megabyte
firmware artifacts. It does not download anything during test execution.
