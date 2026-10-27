"""Small filesystem helpers: hashing, atomic writes and safe relative paths."""

from __future__ import annotations

import hashlib
import json
import os
import re
import stat
import sys
import tempfile
from pathlib import Path, PurePosixPath

CHUNK = 1024 * 1024


class InstallerError(Exception):
    """An error with a message that is safe and useful to show to the user."""


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: Path) -> str:
    hasher = hashlib.sha256()
    with open(path, "rb") as source:
        for part in iter(lambda: source.read(CHUNK), b""):
            hasher.update(part)
    return hasher.hexdigest()


def atomic_write_bytes(path: Path, data: bytes) -> None:
    """Write ``data`` to ``path`` via a temporary file and ``os.replace``.

    Readers never see a half-written file. Permission bits of an existing file
    are kept.
    """
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    mode = stat.S_IMODE(path.stat().st_mode) if path.exists() else None
    fd, temporary = tempfile.mkstemp(prefix=".l4d2vr-tmp-", dir=str(path.parent))
    try:
        with os.fdopen(fd, "wb") as handle:
            handle.write(data)
            handle.flush()
            os.fsync(handle.fileno())
        if mode is not None:
            os.chmod(temporary, mode)
        os.replace(temporary, path)
    except PermissionError as error:
        raise InstallerError(
            f"Cannot write {path}: {error}. If the game is running, close it and try again."
        ) from error
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def atomic_write_text(path: Path, text: str) -> None:
    atomic_write_bytes(path, text.encode("utf-8"))


def atomic_write_json(path: Path, value) -> None:
    atomic_write_text(path, json.dumps(value, indent=2, sort_keys=False) + "\n")


def read_json(path: Path, default=None):
    try:
        with open(path, "r", encoding="utf-8-sig") as handle:
            return json.load(handle)
    except FileNotFoundError:
        return default
    except (OSError, json.JSONDecodeError) as error:
        raise InstallerError(f"Cannot read {path}: {error}") from error


def normalize_member(name: str) -> str:
    """Return a safe POSIX relative path for an archive member name.

    Backslashes (written by some Windows zip tools) are treated as separators.
    Absolute paths, drive letters and ``..`` components are rejected.
    """
    cleaned = name.replace("\\", "/")
    rel = PurePosixPath(cleaned)
    parts = [part for part in rel.parts if part not in ("", ".")]
    if (
        not parts
        or cleaned.startswith("/")
        or ".." in parts
        or re.match(r"^[A-Za-z]:", parts[0])
    ):
        raise InstallerError(f"Unsafe path in archive: {name!r}")
    return "/".join(parts)


def resolve_ci(root: Path, rel: str) -> Path:
    """Map a POSIX relative path onto ``root``, reusing existing entries
    whose names differ only by letter case.

    Windows (and Wine/Proton) treat ``VR`` and ``vr`` as the same folder. On a
    case-sensitive Linux filesystem we must write into the folder that already
    exists instead of creating a second one next to it.
    """
    current = Path(root)
    parts = rel.split("/")
    for index, part in enumerate(parts):
        candidate = current / part
        if not candidate.exists() and not candidate.is_symlink() and current.is_dir():
            lowered = part.lower()
            try:
                matches = sorted(child for child in os.listdir(current) if child.lower() == lowered)
            except OSError:
                matches = []
            if matches:
                candidate = current / matches[0]
        current = candidate
        if index < len(parts) - 1 and current.exists() and not current.is_dir():
            raise InstallerError(f"Expected a folder but found a file: {current}")
    return current


def is_within(path: Path, root: Path) -> bool:
    try:
        Path(os.path.abspath(path)).relative_to(os.path.abspath(root))
        return True
    except ValueError:
        return False


def user_config_dir() -> Path:
    """Per-user settings directory for the installer itself."""
    if sys.platform.startswith("win"):
        base = os.environ.get("APPDATA") or str(Path.home() / "AppData" / "Roaming")
        return Path(base) / "L4D2VR Installer"
    base = os.environ.get("XDG_CONFIG_HOME") or str(Path.home() / ".config")
    return Path(base) / "l4d2vr-installer"


def user_cache_dir() -> Path:
    if sys.platform.startswith("win"):
        base = os.environ.get("LOCALAPPDATA") or str(Path.home() / "AppData" / "Local")
        return Path(base) / "L4D2VR Installer" / "cache"
    base = os.environ.get("XDG_CACHE_HOME") or str(Path.home() / ".cache")
    return Path(base) / "l4d2vr-installer"
