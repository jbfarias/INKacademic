"""Keep the pinned FreeInk SDK's X4 Pro frontlight configuration usable.

The SDK profile requests 25 kHz at 10-bit resolution from the sleep-retained
RC_FAST LEDC clock. That combination exceeds the clock and leaves both light
channels disabled. Apply the small compatibility patch before PlatformIO
compiles the symlinked SDK libraries so clean CI builds use the verified
10 kHz configuration and reject future invalid combinations at compile time.
"""

from pathlib import Path


Import("env")  # noqa: F821 - SCons injects this at build time


project_dir = Path(env.subst("$PROJECT_DIR"))
board_config = project_dir / "freeink-sdk/libs/hardware/BoardConfig/include/BoardConfig.h"
frontlight_manager = project_dir / "freeink-sdk/libs/hardware/FrontlightManager/src/FrontlightManager.cpp"

board_text = board_config.read_text()
old_board = """    // The original bring-up dump used 10 kHz; stock 7.0.8 passes 25 kHz / 10-bit to
    // the frontlight initializer on the same pins. Use that directly recovered value.
"""
new_board = """    // The original bring-up dump used 10 kHz. Stock 7.0.8 passes 25 kHz / 10-bit,
    // but FREEINK_FRONTLIGHT_LS clocks LEDC from the S3 RC_FAST source. That
    // source cannot produce 25 kHz at 10-bit resolution, so LEDC rejects the
    // timer configuration and both channels remain dark. Keep the verified
    // 10 kHz value, which preserves all 10 brightness bits and survives sleep.
"""
if old_board in board_text:
    board_text = board_text.replace(old_board, new_board).replace(
        "    {8, 25000, 10, true, 9},", "    {8, 10000, 10, true, 9},"
    )
    board_config.write_text(board_text)
    print(f"Patched X4 Pro frontlight PWM compatibility: {board_config}")
elif new_board not in board_text or "    {8, 10000, 10, true, 9}," not in board_text:
    raise RuntimeError("Unrecognized X4 Pro frontlight profile in pinned FreeInk SDK")

manager_text = frontlight_manager.read_text()
guard_marker = "constexpr uint32_t RC_FAST_NOMINAL_HZ = 17500000;"
if guard_marker not in manager_text:
    anchor = "#ifdef FREEINK_FRONTLIGHT_LS\n"
    guard = """#ifdef FREEINK_FRONTLIGHT_LS
constexpr uint32_t RC_FAST_NOMINAL_HZ = 17500000;
constexpr auto& SLEEP_FRONTLIGHT = BoardConfig::DEFAULT_DEVICE.frontlight;
static_assert(SLEEP_FRONTLIGHT.pwmResolutionBits < 31, "Frontlight PWM resolution must fit the LEDC duty calculation");
static_assert(static_cast<uint64_t>(SLEEP_FRONTLIGHT.pwmFrequency) *
                      (uint64_t{1} << SLEEP_FRONTLIGHT.pwmResolutionBits) <=
                  RC_FAST_NOMINAL_HZ,
              "FREEINK_FRONTLIGHT_LS PWM frequency/resolution exceeds the S3 RC_FAST clock");

"""
    if anchor not in manager_text:
        raise RuntimeError("FREEINK_FRONTLIGHT_LS block not found in pinned FreeInk SDK")
    frontlight_manager.write_text(manager_text.replace(anchor, guard, 1))
    print(f"Added X4 Pro frontlight clock guard: {frontlight_manager}")
