# INKademic v1.5.4-rc.4

This release candidate adds a browser-based firmware update flow to the
existing device web interface. It is intended to make recovery and upgrades
possible through the same local connection used to send books, while retaining
the watchdog-safe A/B flash path introduced in `v1.5.4-rc.3`.

## Browser firmware updates

- Open **File Transfer**, browse to `/firmware`, select the `.bin` matching the
  device, and upload it.
- The device stages the image on the SD card and validates its ESP image
  structure, segment boundaries, running chip ID, size, XOR checksum, and
  appended SHA-256 trailer before exposing the install action.
- Installation is a separate explicit action. The HTTP response returns first;
  the main application task then flashes the inactive A/B partition using the
  sector-sized, watchdog-safe writer and reboots.
- The page persists state across reconnects and reports interrupted or failed
  installations rather than silently retrying them.
- `POST /upload` remains the book/file endpoint; it does not install firmware.

## Security and limitations

The web server has no authentication. Use the page only on a private network
or a device hotspot that you control. This route performs image integrity and
device-compatibility checks, but this build does not enforce Ed25519 signatures
because the private release-signing key is not present in the repository. Do
not treat a browser upload as proof of publisher authenticity.

The feature is present only after this release is installed. It cannot repair a
device that cannot boot far enough to start the web server; use the documented
X4 Pro recovery/bootstrap path for that situation.

## Published artifacts

| Variant | File | Size | SHA-256 |
| --- | --- | ---: | --- |
| X3 / X4 | `firmware-x3-x4-v1.5.4-rc.4.bin` | 6,220,944 bytes | `d7d792bddecc7d8c359da5be8a482585371bc9f3a3234a215aa31c3512bac383` |
| X4 Pro | `firmware-x4-pro-v1.5.4-rc.4.bin` | 6,125,728 bytes | `c682399e2145780229c2ae84122b3828616c86f542c64c641b1673573bd84f03` |
| Sticky | `firmware-sticky-v1.5.4-rc.4.bin` | 6,017,728 bytes | `fc1d2dd13393cf3f692866380ea92bb9050e4942d0f90b7ab4f517d15b7c75ac` |
| X4 Pro recovery | `firmware-recovery-x4-pro-v1.5.4-rc.4.bin` | 386,384 bytes | `7001812f555efc35a080713e1de903e060b68d5a92abe1e6fef2535f122f9c5a` |

The recovery loader is not a replacement for the normal X4 Pro application
image. It is intended only for the factory-compatible recovery layout described
in [X4 Pro reliability](./x4-pro-reliability.md).
