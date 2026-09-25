# INKademic v1.5.5-rc.2 plan

This release candidate consolidates the CrossPoint and CrossInk improvements
that are useful to the academic fork, while keeping the X4 Pro update path
conservative after the previous watchdog failures.

## Goals

1. Keep academic features available on every target: notes, highlights,
   markings, tags, EPUB export, reading statistics, and the browser companion.
2. Preserve the X4 Pro-specific hardware behavior: native USB, frontlight,
   side buttons, RTC, and the SD-card firmware path.
3. Make the signed OTA/browser updater understand the exact RC identifier
   `1.5.5-rc.2` and never mistake it for an older `1.5.5-rc.1` image.
4. Avoid enabling unvalidated power-saving or logging behavior that can starve
   the X4 Pro task watchdog during installation or dense EPUB indexing.

## Included in this RC

### Update safety

- The version parser accepts both `-rc.2` and `-rc-2` spellings and compares
  their numeric RC suffix.
- The release catalog marks `1.5.5-rc.2` as `rc`, not `stable`.
- The existing signed A/B flow remains mandatory: Ed25519 signature, device
  identity, chip/size checks, heap guard, inactive-partition write, and
  bootloader rollback.

### Frontlight and wake

- New X4 Pro installations restore the saved frontlight state after wake by
  default.
- Every existing saved off value remains respected, whether it came from a
  previous default or an explicit choice. There is no migration that distinguishes
  those cases. Brightness and warmth continue to be stored independently.
- The existing X4 Pro USB Drive lifecycle keeps storage exclusive and returns
  to Home after eject, cable removal, startup failure, or I/O failure.

### Memory and EPUB baseline

- The RC keeps the existing cache invalidation, atomic annotation/settings
  stores, font-cache release points, image-size checks, and low-heap guards.
- No new continuous log capture, aggressive light sleep, or large image buffer
  is enabled in this candidate. These changes require physical X4 Pro testing
  because they can change watchdog and battery behavior.

## Validation gates

Before promoting the RC, validate each target in the simulator and on hardware:

- X3/X4, X4 Pro, and Sticky boot and report `1.5.5-rc.2`.
- X4 Pro: toggle **Restore Light on Wake**, sleep, wake, and verify the saved
  state after both a normal wake and a restart.
- X4 Pro: enter USB Drive, copy a file, eject it from the host, and confirm the
  reader returns to Home without a reset loop.
- Transfer an EPUB containing highlights/notes between devices and open it on
  the receiver.
- Index an image-heavy EPUB and monitor the free heap; no task watchdog reset
  or permanent low-memory screen is acceptable.
- Upload the matching signed image through the browser and confirm the device
  rejects an unsigned image and an image for another model.

## Upstream references

The implementation continues to use the CrossPoint reader architecture and
adapts relevant ideas from [CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader/releases),
[CrossInk](https://github.com/uxjulia/CrossInk), and the fork references listed
in [fork lineage](./fork-lineage.md). Their reader, cache, USB, and power
changes are treated as upstream input; academic annotation formats and signed
OTA remain INKademic-owned integration points.

