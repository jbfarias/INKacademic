"""Fail every hardware application build if the OTA verifier was compiled out.

This structural gate supplements signature behavior tests; key presence alone
does not prove verification. Runs in both local PlatformIO builds and release CI.
"""
import re
import subprocess
from pathlib import Path


def check_image(image, key_header, symbols):
    key = bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})", key_header))
    if len(key) != 32 or key not in image:
        raise RuntimeError("OTA public key is absent from firmware")
    for symbol in ("wc_ed25519_verify_msg", "wc_ed25519_import_public"):
        if not re.search(r"\b[Tt]\s+" + symbol + r"$", symbols, re.MULTILINE):
            raise RuntimeError(f"OTA verifier symbol missing: {symbol}")


def verify_build(source, target, env):
    build = Path(env.subst("$BUILD_DIR"))
    root = Path(env.subst("$PROJECT_DIR"))
    # PlatformIO's ESP builders do not consistently define $NM. Resolve it
    # alongside the actual cross-compiler, using SCons' toolchain search path.
    compiler = env.WhereIs(env.subst("$CC"))
    if not compiler or not compiler.endswith("gcc"):
        raise RuntimeError("Could not resolve ESP cross-compiler for OTA symbol audit")
    nm = compiler[:-3] + "nm"
    result = subprocess.run([nm, str(build / "firmware.elf")],
                            check=True, capture_output=True, text=True)
    check_image((build / "firmware.bin").read_bytes(),
                (root / "include/OtaUpdatePublicKey.h").read_text(), result.stdout)
    print("OTA security gate: embedded key and linked Ed25519 verifier confirmed")


try:
    Import("env")
except NameError:
    pass
else:
    env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", verify_build)
