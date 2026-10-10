#!/usr/bin/env python3
"""Build and run the host test for main/improv_serial.c.

Needs a C compiler on the PATH ("cc", "gcc", "clang") or the `ziglang`
pip package (python -m pip install ziglang). Writes fixtures.json (the raw
bytes of real firmware replies) next to this script for the installer's
JavaScript test.

    python firmware/ggs/host_test/run.py
"""
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
MAIN = HERE.parent / "main"


def compiler():
    for name in ("cc", "gcc", "clang"):
        if shutil.which(name):
            return [name]
    try:
        import ziglang  # noqa: F401
        return [sys.executable, "-m", "ziglang", "cc"]
    except ImportError:
        sys.exit("No C compiler found. Install gcc/clang or: python -m pip install ziglang")


def main() -> int:
    exe = HERE / ("improv_host_test.exe" if sys.platform == "win32" else "improv_host_test")
    cmd = compiler() + [
        "-std=gnu11", "-Wall", "-Wextra", "-Wno-unused-parameter", "-O0", "-g",
        "-I", str(HERE / "stubs"), "-I", str(MAIN),
        str(HERE / "improv_host_test.c"), str(MAIN / "improv_serial.c"),
        "-o", str(exe),
    ]
    print("$", " ".join(cmd))
    if subprocess.run(cmd).returncode:
        return 1
    return subprocess.run([str(exe)], cwd=HERE).returncode


if __name__ == "__main__":
    sys.exit(main())
