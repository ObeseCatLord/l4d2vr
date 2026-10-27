"""Persistent installer settings (game folder, source, VR route...).

Stored in ``~/.config/l4d2vr-installer/settings.json`` (Linux) or
``%APPDATA%\\L4D2VR Installer\\settings.json`` (Windows). Server passwords
are never stored.
"""

from __future__ import annotations

from pathlib import Path
from typing import Optional

from . import DEFAULT_REPO
from .fsutil import InstallerError, atomic_write_json, read_json, user_config_dir

DEFAULTS = {
    "game_dir": "",
    "source": "github",
    "repo": DEFAULT_REPO,
    "channel": "stable",
    "manifest_url": "",
    "pin_workshop": True,
    "route": "steamvr",  # or "monado"
    "server": "",
    "monado": {},
    "extra_preserve": [],
}


def settings_path(config_dir: Optional[Path] = None) -> Path:
    return Path(config_dir or user_config_dir()) / "settings.json"


def load(config_dir: Optional[Path] = None) -> dict:
    data = dict(DEFAULTS)
    try:
        stored = read_json(settings_path(config_dir), {}) or {}
    except InstallerError:
        stored = {}
    if isinstance(stored, dict):
        data.update({key: value for key, value in stored.items() if key in DEFAULTS})
    return data


def save(data: dict, config_dir: Optional[Path] = None) -> None:
    clean = {key: data.get(key, DEFAULTS[key]) for key in DEFAULTS}
    atomic_write_json(settings_path(config_dir), clean)
