# INKademic v1.5.5-rc.2

This is the next release candidate after `v1.5.5-rc.1`. It is built for X3, X4,
X4 Pro, and Sticky, with the same academic reading and annotation features on
all supported models.

## Main changes

- Fixed OTA and browser-update version ordering for the exact version
  `1.5.5-rc.2`.
- Classified the release catalog entry as `rc` so stable clients do not select
  it as a production release.
- Set frontlight restoration on wake as the default when the setting is absent.
  Existing saved `off` values remain off, including values saved by an older
  default. This does not migrate all existing users to restoration enabled.
- Retained the A/B update path and model/heap checks. A later audit found
  Ed25519 verification compiled out of these published hardware images; see
  [the audit](review-rc2-external-audit.md). Do not treat the presence of `.sig`
  release assets as proof that this version verifies them on the device.
- Retained academic notes, highlights, markings, tags, EPUB export, statistics,
  and the browser file/firmware interface in every target build.

## Important upgrade note

The firmware image must match the device:

| Device | Artifact |
| --- | --- |
| X3 / X4 | `firmware-x3-x4-v1.5.5-rc.2.bin` |
| X4 Pro | `firmware-x4-pro-v1.5.5-rc.2.bin` |
| Sticky | `firmware-sticky-v1.5.5-rc.2.bin` |

Each binary is accompanied by a `.sig` file. The private signing key is not
stored in this repository. These signature files belong to the official release
flow. Manual SD installation does not require an INKademic signature; later
local corrections also provide explicit manual browser and OTA modes.

## X4 Pro safety scope

This candidate deliberately does not enable continuous diagnostic logging or
experimental aggressive sleep/downclock changes. Those options previously
increased the risk of a task-watchdog reset during SD/OTA work. The remaining
validation focus is the frontlight wake state, USB Drive exit path, dense
EPUB indexing, and signed updates.

## Documentation

The implementation plan and hardware validation gates are in
[plan-1.5.5-rc.2.md](./plan-1.5.5-rc.2.md). The upstream projects and adapted
areas are recorded in [fork-lineage.md](./fork-lineage.md).

