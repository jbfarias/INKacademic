# INKademic v1.5.4-rc.1

Release candidate dated 23 August 2026.

## Included firmware

| Device | File | SHA-256 |
| --- | --- | --- |
| X3 / X4 | `firmware-x3-x4-v1.5.4-rc.1.bin` | `d5c74d70ed67812eafd06228f8610eabef2a8c05e189d85b2d1f5faeeeb249ae` |
| X4 Pro | `firmware-x4-pro-v1.5.4-rc.1.bin` | `a1e349a5e23fdef8ca19218ad29fad1d9d96a67f13ca1c99bdfa56dce76f7fce` |
| Sticky | `firmware-sticky-v1.5.4-rc.1.bin` | `7ec75e849d057120ce5190df53ee819c2f2652e300acdff730315665d5373ec2` |

## What changed

- Academic notes, highlights, clippings, bookmarks, and annotation tags remain enabled in every device build.
- X4 Pro receives the same INKademic feature set as X3/X4, including touch-aware navigation and annotations.
- Added a physical X4 Pro validation matrix covering touch, page turns, frontlight, menus, notes, highlights, tags, and persistence.
- Added host-side memory-budget regression tests for EPUB image decoding, JPEG buffers, and SD-card font lifecycle handling.
- Kept BookOrbit synchronization out of the firmware core for this release candidate; the existing KOReader Sync path remains unchanged.

## Validation

- X3/X4, Sticky, and X4 Pro firmware builds completed successfully with OTA-size checks.
- The release binaries embed the same `1.5.4-rc.1` version string across all three device profiles.
- X4 Pro simulator smoke test passed with touch page turns, frontlight controls, Home, touch-disabled mode, and menu navigation.
- X3/X4 simulator smoke test passed with three page turns.
- Memory-budget test suite passed 5/5 tests.
- A complete legacy CMake test run still has an unrelated pre-existing link failure in `DifferentialRoundingTest` for missing `EpdFontFamily` symbols; the new memory-budget suite passes independently.

This is a release candidate. Physical X4 Pro validation should be completed using [the validation checklist](x4-pro-validation.md) before promoting it to a stable release.
