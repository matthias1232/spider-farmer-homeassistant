#!/usr/bin/env python3
"""Build and run the firmware host tests.

  improv_host_test      main/improv_serial.c   (USB protocol: Improv + SpiderBridge commands)
  supervisor_host_test  main/supervisor_logic.c (when a restart counts as a failed run, safe mode,
                                                 stuck-task rule)

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

TESTS = [
    ("improv_host_test", ["improv_host_test.c", str(MAIN / "improv_serial.c")]),
    ("supervisor_host_test", ["supervisor_host_test.c", str(MAIN / "supervisor_logic.c")]),
]


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
    for name, sources in TESTS:
        exe = HERE / (name + (".exe" if sys.platform == "win32" else ""))
        cmd = compiler() + [
            "-std=gnu11", "-Wall", "-Wextra", "-Wno-unused-parameter", "-O0", "-g",
            "-I", str(HERE / "stubs"), "-I", str(MAIN),
        ] + [s if Path(s).is_absolute() else str(HERE / s) for s in sources] + ["-o", str(exe)]
        print("$", " ".join(cmd))
        if subprocess.run(cmd).returncode:
            return 1
        if subprocess.run([str(exe)], cwd=HERE).returncode:
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())