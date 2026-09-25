# INKademic v1.5.4-rc.2

Release candidate dated 5 September 2026.

## Included firmware

| Device | File | Size | SHA-256 |
| --- | --- | ---: | --- |
| X3 / X4 | `firmware-x3-x4-v1.5.4-rc.2.bin` | 6,209,968 bytes | `35415ad151d60cde8069a72edd76c479352610c6ab8064d4cfdd25af1d07fe6a` |
| X4 Pro | `firmware-x4-pro-v1.5.4-rc.2.bin` | 6,116,816 bytes | `9fb9bd96f172840216682ff629067ee7e04edc1658239f88415fc8676850f030` |
| Sticky | `firmware-sticky-v1.5.4-rc.2.bin` | 6,008,048 bytes | `f7daca20b3751d6574a4768fbbf0a74b4495e78dfb0439ea865b5fe090d3ee7b` |

## Included improvements

- Academic notes, highlights, clippings, bookmarks, and annotation tags are enabled in every device build.
- The X4 Pro uses the same INKademic academic workflow as X3/X4 and Sticky, including touch navigation, persistence, frontlight behavior, and the USB identity.
- Added date-aware academic filters and Markdown, JSON, and CSV exports with document identity, chapter, quote, stable layout metadata, and note context.
- Preserved touch navigation for footnotes and academic references, including table clippings and return-to-origin behavior.
- Avoided repeated recent-book and automatic statistics writes when their data is unchanged.
- Kept note writes durable and rejected oversized or newer unreadable note data instead of replacing it with an empty file.
- Added Notes Connect from the reader, with the current book preselected in the academic web workspace.
- Added streaming atomic JSON persistence, no-op write detection, watchdog recovery, reset-cause diagnostics, and bounded JSON handling to reduce SD wear and OOM risk.
- Added OTA signature metadata and X4 Pro Ed25519 verification over the SHA-256 digest, with signed-image enforcement and A/B rollback support.
- Preserved quick resume, sleep power savings, reading-time/WPM indicators, configurable shortcuts, and safe shutdown across all builds.
- Separated pioarduino SDK cache decisions by chip and SDK configuration.
- Stopped the SD backend before deep sleep, preserved the X4 Pro frontlight policy across wake and Quick Lock, and hardened compressed EPUB reads against invalid or oversized storage results.
- Applied the new INKademic logo in the web companion pages and firmware web portal.
- Exposed the X4 Pro “Restore light on wake” setting, persisted the live frontlight state before sleep, and parked/released the PWM driver safely around deep sleep.
- Hardened SD firmware installation on X4 Pro: the installer now uses short 16 KiB erase windows, temporarily allows a 60-second task-watchdog window during the flash transaction, feeds the watchdog between erase/write operations, and limits progress redraws to 5% steps. Persistent log capture remains paused while the OTA partition is being erased and written.
- Corrected the X4 Pro portrait logo bitmap orientation. All RC2 hardware profiles disable serial and persistent SD log capture to keep firmware installation free of auxiliary I/O.

## Forks and references

The integration was based on CrossPoint Reader, the CrossInk academic work, and the FreeInk SDK. Compatible ideas were reviewed from [CrossXT](https://github.com/yazdipour/CrossXT), [CrossInk-Bookorbit](https://github.com/agosez/CrossInk-Bookorbit), [CrossNotes](https://github.com/HilbergK/CrossNotes), and [YACP](https://github.com/Sichroteph/YACP/f). The complete attribution and compatibility notes are in [fork lineage and references](./fork-lineage.md).

These projects are references for selected features and compatibility decisions; they are not wholesale runtime dependencies of INKademic.

## Validation

- X3/X4, Sticky, and X4 Pro production builds completed successfully with OTA-size checks.
- Simulator builds for X3/X4 and X4 Pro completed successfully and reported `INKademic version 1.5.4-rc.2`.
- The refreshed X4 Pro production image uses 93.3% of the OTA application partition, leaving 436,784 bytes free.
- The default image uses 94.8% of its OTA application partition, leaving 343,632 bytes free; Sticky uses 91.7%, leaving 545,552 bytes free.
- X4 Pro and X3/X4 simulator builds completed successfully; the X4 Pro smoke test passed with three page turns. Sticky production compilation was resumed for this refresh.
- Host-side syntax, signing-script help, and whitespace validation passed for the build and release changes.

The X4 Pro OTA path supports a 64-byte Ed25519 signature over the firmware SHA-256 digest. This refresh intentionally removes the previous `.sig` asset because it was for the older image and the GitHub Actions secret `INKADEMIC_OTA_ED25519_PRIVATE_KEY` is not currently configured. Until that secret is configured and the signature is regenerated, use the refreshed image through the SD/recovery installer; do not use the old signature with this binary.

This is a release candidate. Physical X4 Pro validation should be completed using [the validation checklist](x4-pro-validation.md) before promoting it to a stable release.
