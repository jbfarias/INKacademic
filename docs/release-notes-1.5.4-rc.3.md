# INKademic v1.5.4-rc.3

This release candidate replaces `v1.5.4-rc.2` after the X4 Pro installation
test exposed two independent problems: the old SD updater could reset in the
erase path, and the current OTA client closed the Unlocker TLS handshake before
receiving its release manifest.

## Safety and recovery changes

- SD firmware installation erases one 4 KiB sector at a time and services the
  task watchdog before and after every erase/write operation.
- The minimal X4 Pro recovery loader uses the same sector-sized flash windows.
- Full image validation remains enabled before any erase: ESP image layout,
  chip ID, segment boundaries, XOR checksum, size and appended SHA-256.
- The A/B slot is selected only after the complete image has been written.
- The normal OTA path still uses the system CA bundle and image digest checks.

## Published artifacts

| Variant | File | Size | SHA-256 |
| --- | --- | ---: | --- |
| X3 / X4 | `firmware-x3-x4-v1.5.4-rc.3.bin` | 6,211,536 bytes | `1a41b0a9640e1a59c773e6d8749028562f0511ff920fa0e864d6d719d72d8fd0` |
| X4 Pro | `firmware-x4-pro-v1.5.4-rc.3.bin` | 6,117,184 bytes | `8d1d05321efb88dfecd657e4b756cc8c290dbd973627eb3b9ad590269259d292` |
| Sticky | `firmware-sticky-v1.5.4-rc.3.bin` | 6,009,312 bytes | `dac9327f50769234c5fbd1d7de9bbfc3cef54d6b09422370b4d3eae1bd6762e3` |
| X4 Pro recovery | `firmware-recovery-x4-pro-v1.5.4-rc.3.bin` | 386,384 bytes | `9038d6f42ea7882df26ee15756ade344f117f2ef9a4670dca8adcbf33238d5e3` |

The recovery loader is not a replacement for the normal X4 Pro application image.
It is intended for a factory-compatible partition layout and must only be used
with the recovery procedure described in [X4 Pro reliability](./x4-pro-reliability.md).

## Xteink Unlocker compatibility

The X4 Pro build first uses strict HTTPS validation. If that request fails while
connected to the Unlocker hotspot (`crosspoint` or `Xteink Unlocker`), it retries
the `api.github.com` manifest through the Unlocker bridge. The retry does not
disable CA validation and does not weaken image validation; it only accommodates
the bridge certificate name. Firmware downloads continue to use the manifest
digest and the normal A/B OTA path.

## Academic firmware

Notes, highlights, page tags, annotation tags, Notes Connect, reading-time/WPM,
quick resume, configurable shortcuts, safe shutdown and the INKademic identity
remain enabled for X3/X4, Sticky and X4 Pro. The X4 Pro build continues to use
the factory-compatible two-slot partition layout.

## Important installation note

This release contains the fix for the watchdog failure; the older image already
installed on a device cannot acquire this fix through the failed SD path. Use a
working OTA/bootstrap path or a lower-level recovery method to install this
image initially. Do not reuse the old RC2 `.sig` file with these binaries.

This repository does not contain the private Ed25519 signing key. Therefore this
RC is published with SHA-256 checksums; no fabricated signature is included.
