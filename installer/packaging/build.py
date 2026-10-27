#!/usr/bin/env python3
"""Build the installer binary for the current OS with PyInstaller.

    python installer/packaging/build.py            # creates installer/dist/<name>[.exe] + .sha256
    python installer/packaging/build.py --test     # run the unit tests first

Requires: Python 3.10+ with tkinter, and network access the first time to
install PyInstaller into a private virtual environment (installer/.build-venv).
Windows binaries must be built on Windows, Linux binaries on Linux. For a Linux
binary that runs on older distributions, build on the oldest glibc you want to
support (for example inside a Debian 11 / Ubuntu 20.04 container).
"""

import argparse
import hashlib
import os
import subprocess
import sys
import venv
from pathlib import Path

PYINSTALLER = "pyinstaller==6.11.1"
HERE = Path(__file__).resolve().parent
ROOT = HERE.parent


def run(command, **kwargs):
    print("+", " ".join(str(part) for part in command), flush=True)
    subprocess.run([str(part) for part in command], check=True, **kwargs)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--test", action="store_true", help="run the unit tests before building")
    parser.add_argument("--venv", default=str(ROOT / ".build-venv"))
    args = parser.parse_args()

    try:
        import tkinter  # noqa: F401
    except ImportError:
        print("This Python has no tkinter; the GUI would be missing. Use a Python with Tk.", file=sys.stderr)
        return 1
    if args.test:
        run([sys.executable, "-m", "unittest"], cwd=ROOT)

    env_dir = Path(args.venv)
    python = env_dir / ("Scripts/python.exe" if os.name == "nt" else "bin/python")
    if not python.exists():
        venv.EnvBuilder(with_pip=True).create(env_dir)
    run([python, "-m", "pip", "install", "--disable-pip-version-check", "-q", PYINSTALLER])
    run([python, "-m", "PyInstaller", "--clean", "--noconfirm", "--distpath", ROOT / "dist",
         "--workpath", ROOT / "build", HERE / "l4d2vr-installer.spec"], cwd=ROOT)

    name = "l4d2vr-installer-windows.exe" if os.name == "nt" else "l4d2vr-installer-linux"
    binary = ROOT / "dist" / name
    digest = hashlib.sha256(binary.read_bytes()).hexdigest()
    (ROOT / "dist" / f"{name}.sha256").write_text(f"{digest}  {name}\n")
    run([binary, "--version"])
    print(f"Built {binary}\nSHA-256 {digest}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
