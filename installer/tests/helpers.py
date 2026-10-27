"""Shared fixtures: temporary game folders, Steam trees and release zips."""

import hashlib
import json
import os
import shutil
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest import mock

CLIENT_V1 = {
    "d3d9.dll": b"MZ d3d9 v1",
    "openvr_api.dll": b"MZ openvr v1",
    "manifest.json": json.dumps({"version": "1.0.0"}).encode(),
    "updatelog.txt": b"v1 notes",
    "vr/config.sample": b"SampleKey=1\n",
    "vr/haptics_config.txt": b"haptics v1",
    "vr/SteamVRActionManifest/action_manifest.json": b"{}",
    "vr/UI Font Fix.vpk": b"vpk v1",
    "vr/old_only_in_v1.txt": b"stale",
    "left4dead2/materials/l4d2vr/holster_highlight.vmt": b'"UnlitGeneric" {}',
}

CLIENT_V2 = dict(CLIENT_V1)
CLIENT_V2.update({
    "d3d9.dll": b"MZ d3d9 v2",
    "manifest.json": json.dumps({"version": "2.0.0"}).encode(),
    "vr/haptics_config.txt": b"haptics v2",
    "vr/new_in_v2.txt": b"new",
})
del CLIENT_V2["vr/old_only_in_v1.txt"]

SERVER_FILES = {
    "left4dead2/addons/l4d2vr_server.so": b"\x7fELF server",
    "left4dead2/addons/l4d2vr_server.vdf": b'"Plugin" { "file" "addons/l4d2vr_server" }',
}


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def make_zip(path: Path, files: dict, prefix: str = "") -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as archive:
        for name, data in files.items():
            archive.writestr(prefix + name, data)
    return path


def make_game(root: Path, windows: bool = True) -> Path:
    game = root / "steamapps" / "common" / "Left 4 Dead 2"
    (game / "left4dead2").mkdir(parents=True, exist_ok=True)
    if windows:
        (game / "left4dead2.exe").write_bytes(b"MZ game")
    else:
        (game / "hl2_linux").write_bytes(b"\x7fELF")
    return game


def snapshot(root: Path, skip=(".l4d2vr-backups",)) -> dict:
    """{relative path: bytes} for every file under root (except backups)."""
    out = {}
    for path in sorted(Path(root).rglob("*")):
        rel = path.relative_to(root)
        if rel.parts and rel.parts[0] in skip:
            continue
        if path.is_file():
            out[rel.as_posix()] = path.read_bytes()
    return out


class TempDirTest(unittest.TestCase):
    """Creates ``self.tmp`` and points HOME/XDG dirs into it so nothing touches
    the real user profile, Steam install or settings."""

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="l4d2vr-test-"))
        home = self.tmp / "home"
        home.mkdir()
        env = {
            "HOME": str(home), "XDG_CONFIG_HOME": str(home / ".config"),
            "XDG_CACHE_HOME": str(home / ".cache"), "XDG_DATA_HOME": str(home / ".local/share"),
            "XDG_STATE_HOME": str(home / ".local/state"), "APPDATA": str(home / "AppData"),
            "LOCALAPPDATA": str(home / "LocalAppData"), "XDG_RUNTIME_DIR": str(self.tmp / "run"),
        }
        (self.tmp / "run").mkdir()
        patcher = mock.patch.dict(os.environ, env)
        patcher.start()
        self.addCleanup(patcher.stop)
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.home = home
