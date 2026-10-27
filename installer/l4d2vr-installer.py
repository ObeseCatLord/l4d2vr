#!/usr/bin/env python3
"""Entry point for running from source and for PyInstaller builds.

    python3 l4d2vr-installer.py            # GUI
    python3 l4d2vr-installer.py --help     # command line
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from l4d2vr_installer.cli import main  # noqa: E402

if __name__ == "__main__":
    sys.exit(main())
