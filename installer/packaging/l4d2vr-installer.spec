# -*- mode: python ; coding: utf-8 -*-
# PyInstaller spec for the L4D2VR installer.
#
#   pyinstaller --clean --noconfirm installer/packaging/l4d2vr-installer.spec
#
# Produces a single file in dist/:
#   Windows: l4d2vr-installer-windows.exe
#   Linux:   l4d2vr-installer-linux
# The build Python must include tkinter (python.org and uv builds do).
import os
import sys

ROOT = os.path.abspath(os.path.join(SPECPATH, ".."))

NAME = "l4d2vr-installer-windows" if sys.platform.startswith("win") else "l4d2vr-installer-linux"

a = Analysis(
    [os.path.join(ROOT, "l4d2vr-installer.py")],
    pathex=[ROOT],
    binaries=[],
    datas=[],
    hiddenimports=["l4d2vr_installer.gui", "tkinter", "tkinter.ttk", "tkinter.filedialog",
                   "tkinter.messagebox", "tkinter.scrolledtext"],
    excludes=["tests"],
    noarchive=False,
)
pyz = PYZ(a.pure)

exe = EXE(
    pyz,
    a.scripts,
    a.binaries,
    a.datas,
    [],
    name=NAME,
    debug=False,
    strip=False,
    upx=False,
    # A console program so the command line interface can print; the GUI
    # hides its own console window when started by double-click.
    console=True,
    version=os.path.join(SPECPATH, "version_info.txt") if sys.platform.startswith("win") else None,
)
