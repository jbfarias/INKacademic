"""Keep the native simulator shim aligned with the firmware flash API.

The published simulator currently names a ``WRONG_BOARD`` result that was
removed from INKademic's firmware flasher in favor of ``BAD_CHIP``.  The shim
is generated under ``.pio/libdeps`` by PlatformIO, so patch it at build time
instead of editing generated dependency files in the repository.
"""

from pathlib import Path


Import("env")  # noqa: F821 - SCons injects this at build time


def patch_simulator_firmware(source, target, env):
    simulator_source = Path(env.subst("$PROJECT_LIBDEPS_DIR")) / env.subst(
        "$PIOENV"
    ) / "simulator" / "src" / "simulator_firmware.cpp"
    if not simulator_source.exists():
        return

    source_text = simulator_source.read_text()
    stale_case = '  case Result::WRONG_BOARD:\n    return "WRONG_BOARD";\n'
    patched_text = source_text.replace(stale_case, "")
    patched_text = patched_text.replace(
        "Result flashFromSdPath(const char *, ProgressCb onProgress, void *ctx) {",
        "Result flashFromSdPath(const char *, ProgressCb onProgress, void *ctx, "
        "bool, const uint8_t *) {",
    ).replace(
        "Result flashFromSdPath(const char *, ProgressCb onProgress, void *ctx, bool) {",
        "Result flashFromSdPath(const char *, ProgressCb onProgress, void *ctx, "
        "bool, const uint8_t *) {",
    )
    patched_text = patched_text.replace(
        "Result validateImageFile(const char *, size_t) {",
        "Result validateImageFile(const char *, size_t, uint8_t *) {",
    )
    if patched_text != source_text:
        simulator_source.write_text(patched_text)
        print(f"Patched simulator flash API compatibility: {simulator_source}")
    patch_simulator_ota(env)


def patch_simulator_ota(env):
    simulator_ota_source = Path(env.subst("$PROJECT_LIBDEPS_DIR")) / env.subst(
        "$PIOENV"
    ) / "simulator" / "src" / "simulator_ota.cpp"
    if not simulator_ota_source.exists():
        return

    ota_text = simulator_ota_source.read_text()
    patched_ota_text = ota_text.replace("#ifdef CROSSINK_VERSION", "#ifdef INKADEMIC_VERSION")
    if "OtaUpdater::loadSavedSource()" not in patched_ota_text:
        marker = "OtaUpdater::OtaUpdaterError OtaUpdater::checkForUpdate() {"
        saved_source_stub = (
            "OtaUpdater::OtaUpdaterError OtaUpdater::loadSavedSource() {\n"
            "  return NO_UPDATE;\n"
            "}\n\n"
        )
        patched_ota_text = patched_ota_text.replace(marker, saved_source_stub + marker)
    if patched_ota_text != ota_text:
        simulator_ota_source.write_text(patched_ota_text)
        print(f"Patched simulator OTA version guard: {simulator_ota_source}")


patch_simulator_firmware(None, None, env)
