# INKademic v1.5.4

Stable promotion of the 1.5.4 release-candidate work for X3, X4, X4 Pro, and
Sticky, with the academic notes and annotation features enabled in every
device build.

## Highlights

- Academic notes, highlights, clippings, bookmarks, and annotation tags across
  all supported device builds.
- X4 Pro frontlight persistence, side-key navigation, USB recovery, wake
  optimization, and the INKademic boot logo.
- Quick resume, idle power saving, atomic settings writes, reduced SD-card
  writes, watchdog protection, post-failure diagnostics, and bootloader
  rollback support.
- Notes Connect for opening the current book's academic notes from the device.
- Browser-based firmware staging through the existing file-transfer interface,
  with image structure, chip, bounds, checksum, size, and SHA-256 validation.
- Nearby EPUB transfers now invalidate derived chapter and image caches after a
  replacement, so an existing book with annotations cannot leave incompatible
  cached data attached to the received ZIP.
- Image decoding keeps the temporary pixel-cache band in X4 Pro PSRAM when
  available and writes cache rows in larger batches, reducing low-memory
  fallbacks and slow repeated decodes on image-heavy EPUBs.
- OTA checks and downloads now use bounded network setup time and service the
  task watchdog; EPUB parsing and large file-index builds also yield regularly,
  preventing the X4 Pro reset seen while checking updates or indexing dense
  books.
- The X4 Pro application target now uses the factory-compatible 16 MB A/B
  partition map, avoiding the smaller X3/X4 app-slot assumptions.
- Corrected bitmap rendering so the INKademic boot/sleep logo and other image
  assets are no longer rotated 90 degrees on the portrait X4 Pro display.
- OTA manifest correction to the canonical repository endpoint:
  `https://api.github.com/repos/jbfarias/INKademic/releases/latest`.

## Firmware files

| Device | File | Size | SHA-256 |
|---|---|---:|---|
| X3 / X4 | `firmware-x3-x4-v1.5.4.bin` | 6,221,136 | `ef472cc7531da5d1da5224f36229f931cdf46eaac05445e4f0eb2ce259473a86` |
| X4 Pro | `firmware-x4-pro-v1.5.4.bin` | 6,125,824 | `c4c474d2beaa7f0fb3f40a40fa4b24aff51305419ca3a98aae8687b30d8b8273` |
| Sticky | `firmware-sticky-v1.5.4.bin` | 6,017,824 | `4cca6b50c756f192fa546d2356d7f884274580f5c16ed2b44427651fdf6f0789` |
| X4 Pro recovery | `firmware-recovery-x4-pro-v1.5.4.bin` | 386,384 | `b4413945b6376f84b3da51069d08f0ca0f26c61dbe63edcc887f1118e0715edf` |

The recovery image is only for the documented X4 Pro factory-compatible
recovery path. Verify the published SHA-256 values before flashing.

## Important X4 Pro note

The simulator complements, but does not replace, physical validation. For an
X4 Pro installation, keep the recovery procedure available and do not power
off the reader during an image write. The A/B bootloader rollback remains
responsible for returning to the previous application slot if the new image
does not complete its first boot.

## Fork lineage

INKademic remains an independent academic fork based on CrossPoint Reader and
incorporates selected ideas and compatibility work from CrossInk, YACP,
CrossNotes, and BookOrbit. The complete attribution and integration notes are
in [Fork lineage and references](./fork-lineage.md).
