"""Desktop shortcuts: a freedesktop ``.desktop`` file on Linux and an
Internet-shortcut ``.url`` file on Windows (no script execution needed)."""

from __future__ import annotations

import os
import sys
from pathlib import Path
from typing import List, Optional

from . import APP_ID
from .fsutil import atomic_write_text

SHORTCUT_NAME = "Left 4 Dead 2 VR"


def installer_command() -> List[str]:
    """argv that starts this installer (frozen binary or source checkout)."""
    if getattr(sys, "frozen", False):
        return [sys.executable]
    entry = Path(__file__).resolve().parents[1] / "l4d2vr-installer.py"
    return [sys.executable, str(entry)]


def _desktop_quote(arg: str) -> str:
    """Quote one Exec argument per the Desktop Entry specification."""
    needs = any(ch in arg for ch in ' \t\n"\'\\><~|&;$*?#()`') or not arg
    escaped = arg.replace("%", "%%")
    if not needs:
        return escaped
    for ch in ('\\', '"', '`', '$'):
        escaped = escaped.replace(ch, "\\" + ch)
    # The spec applies a second level of backslash unescaping for string values.
    escaped = escaped.replace("\\", "\\\\")
    return f'"{escaped}"'


def desktop_entry(argv: List[str], name: str = SHORTCUT_NAME, icon: Optional[str] = None,
                  comment: str = "Play Left 4 Dead 2 in VR") -> str:
    lines = [
        "[Desktop Entry]",
        "Type=Application",
        f"Name={name}",
        f"Comment={comment}",
        "Exec=" + " ".join(_desktop_quote(arg) for arg in argv),
        f"Icon={icon or 'steam_icon_' + APP_ID}",
        "Terminal=false",
        "Categories=Game;",
    ]
    return "\n".join(lines) + "\n"


def install_linux_shortcut(argv: List[str], *, desktop: bool = True, data_home: Optional[Path] = None,
                           desktop_dir: Optional[Path] = None, dry_run: bool = False,
                           name: str = SHORTCUT_NAME) -> List[Path]:
    data_home = Path(data_home or os.environ.get("XDG_DATA_HOME") or Path.home() / ".local" / "share")
    text = desktop_entry(argv, name=name)
    targets = [data_home / "applications" / "l4d2vr.desktop"]
    if desktop:
        targets.append(Path(desktop_dir) if desktop_dir else linux_desktop_dir())
        targets[-1] = targets[-1] / "l4d2vr.desktop"
    if not dry_run:
        for target in targets:
            atomic_write_text(target, text)
            os.chmod(target, 0o755)
    return targets


def linux_desktop_dir() -> Path:
    """The user's Desktop folder (honours localized XDG user dirs)."""
    config = Path(os.environ.get("XDG_CONFIG_HOME") or Path.home() / ".config") / "user-dirs.dirs"
    try:
        for line in config.read_text().splitlines():
            if line.startswith("XDG_DESKTOP_DIR="):
                value = line.split("=", 1)[1].strip().strip('"')
                return Path(value.replace("$HOME", str(Path.home())))
    except OSError:
        pass
    return Path.home() / "Desktop"


def windows_desktop_dir() -> Path:
    try:
        import winreg  # type: ignore
        key = r"Software\Microsoft\Windows\CurrentVersion\Explorer\User Shell Folders"
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, key) as handle:
            value, _ = winreg.QueryValueEx(handle, "Desktop")
            return Path(os.path.expandvars(value))
    except (ImportError, OSError):
        return Path(os.environ.get("USERPROFILE", str(Path.home()))) / "Desktop"


def url_shortcut(url: str, icon: Optional[Path] = None) -> str:
    lines = ["[InternetShortcut]", f"URL={url}"]
    if icon:
        lines += [f"IconFile={icon}", "IconIndex=0"]
    return "\r\n".join(lines) + "\r\n"


def install_windows_shortcut(url: str, game_dir: Optional[Path] = None, desktop_dir: Optional[Path] = None,
                             dry_run: bool = False, name: str = SHORTCUT_NAME) -> List[Path]:
    target = Path(desktop_dir or windows_desktop_dir()) / f"{name}.url"
    icon = Path(game_dir) / "left4dead2.exe" if game_dir else None
    if not dry_run:
        atomic_write_text(target, url_shortcut(url, icon))
    return [target]
