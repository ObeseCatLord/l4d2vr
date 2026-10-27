"""Steam discovery: Steam roots, libraries, the L4D2 install, users and
running-process checks."""

from __future__ import annotations

import os
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Iterable, List, Optional

from . import APP_ID, vdf
from .fsutil import InstallerError

GAME_FOLDER = "Left 4 Dead 2"
STEAMID64_BASE = 76561197960265728


def is_windows(platform: Optional[str] = None) -> bool:
    return (platform or sys.platform).startswith("win")


# ---------------------------------------------------------------------------
# Steam roots and libraries
# ---------------------------------------------------------------------------

def _registry_steam_paths() -> List[Path]:
    """Steam install paths from the Windows registry (empty elsewhere)."""
    try:
        import winreg  # type: ignore
    except ImportError:
        return []
    found = []
    locations = [
        (winreg.HKEY_CURRENT_USER, r"Software\Valve\Steam", "SteamPath"),
        (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\WOW6432Node\Valve\Steam", "InstallPath"),
        (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\Valve\Steam", "InstallPath"),
    ]
    for hive, key, value in locations:
        try:
            with winreg.OpenKey(hive, key) as handle:
                path, _ = winreg.QueryValueEx(handle, value)
                if path:
                    found.append(Path(path))
        except OSError:
            continue
    return found


def steam_root_candidates(home: Optional[Path] = None, platform: Optional[str] = None,
                          registry: Optional[Callable[[], List[Path]]] = None) -> List[Path]:
    """Places Steam is commonly installed, most likely first."""
    if is_windows(platform):
        candidates = list((registry or _registry_steam_paths)())
        for env in ("ProgramFiles(x86)", "ProgramFiles"):
            base = os.environ.get(env)
            if base:
                candidates.append(Path(base) / "Steam")
        candidates.append(Path(r"C:\Program Files (x86)\Steam"))
        return candidates
    home = Path(home) if home else Path.home()
    return [
        home / ".local/share/Steam",
        home / ".steam/steam",
        home / ".steam/root",
        home / ".var/app/com.valvesoftware.Steam/.local/share/Steam",
        home / "snap/steam/common/.local/share/Steam",
    ]


def _dedupe(paths: Iterable[Path]) -> List[Path]:
    seen, out = set(), []
    for path in paths:
        try:
            key = os.path.normcase(str(Path(path).resolve()))
        except OSError:
            key = os.path.normcase(str(path))
        if key not in seen:
            seen.add(key)
            out.append(Path(path))
    return out


def find_steam_roots(candidates: Optional[List[Path]] = None, **kwargs) -> List[Path]:
    """Existing Steam client folders (those containing ``steamapps``)."""
    if candidates is None:
        candidates = steam_root_candidates(**kwargs)
    return _dedupe(path for path in candidates if (Path(path) / "steamapps").is_dir())


def library_folders(steam_root: Path) -> List[Path]:
    """Steam library roots listed in ``steamapps/libraryfolders.vdf``.

    Handles both the current format (``"0" { "path" "..." }``) and the
    legacy one (``"1" "D:\\\\SteamLibrary"``). The Steam root itself is
    always included first.
    """
    steam_root = Path(steam_root)
    libraries = [steam_root]
    for name in ("steamapps/libraryfolders.vdf", "config/libraryfolders.vdf"):
        path = steam_root / name
        if not path.is_file():
            continue
        try:
            data = vdf.load(path)
        except (OSError, vdf.VdfError):
            continue
        section = data.get_node("libraryfolders") or data.get_node("LibraryFolders")
        if section is None:
            continue
        for key, value in section:
            if not key.isdigit():
                continue
            if isinstance(value, vdf.VdfNode):
                library = value.get("path")
            else:
                library = value
            if library:
                libraries.append(Path(library))
    return _dedupe(libraries)


@dataclass
class GameInstall:
    path: Path
    library: Optional[Path] = None
    steam_root: Optional[Path] = None
    depot: str = "unknown"  # "windows", "linux" or "missing"

    @property
    def is_windows_depot(self) -> bool:
        return self.depot == "windows"


def game_depot(game_dir: Path) -> str:
    """Which build of the game is in ``game_dir``.

    ``windows``: ``left4dead2.exe`` exists (what the mod needs, also under
    Proton). ``linux``: only the native Linux build. ``missing``: neither.
    """
    game_dir = Path(game_dir)
    if (game_dir / "left4dead2.exe").is_file():
        return "windows"
    if (game_dir / "hl2_linux").is_file() or (game_dir / "hl2.sh").is_file():
        return "linux"
    return "missing"


def _installdir_from_manifest(library: Path) -> Optional[str]:
    manifest = Path(library) / "steamapps" / f"appmanifest_{APP_ID}.acf"
    if not manifest.is_file():
        return None
    try:
        data = vdf.load(manifest)
    except (OSError, vdf.VdfError):
        return None
    return data.get_path("AppState", "installdir")


def find_game_installs(steam_roots: Optional[List[Path]] = None, **kwargs) -> List[GameInstall]:
    """Every Left 4 Dead 2 install found in any known Steam library."""
    if steam_roots is None:
        steam_roots = find_steam_roots(**kwargs)
    found: List[GameInstall] = []
    seen = set()
    for root in steam_roots:
        for library in library_folders(root):
            installdir = _installdir_from_manifest(library) or GAME_FOLDER
            for name in dict.fromkeys([installdir, GAME_FOLDER]):
                game = Path(library) / "steamapps" / "common" / name
                if not game.is_dir():
                    continue
                key = os.path.normcase(str(game.resolve()))
                if key in seen:
                    continue
                seen.add(key)
                found.append(GameInstall(game, Path(library), Path(root), game_depot(game)))
    # Windows-depot installs first: those are the ones the mod can use.
    found.sort(key=lambda item: 0 if item.is_windows_depot else 1)
    return found


def steam_root_for_game(game_dir: Path, steam_roots: Optional[List[Path]] = None) -> Optional[Path]:
    """The Steam client root whose libraries contain ``game_dir``."""
    game_dir = Path(game_dir).resolve()
    roots = steam_roots if steam_roots is not None else find_steam_roots()
    for root in roots:
        for library in library_folders(root):
            try:
                game_dir.relative_to(Path(library).resolve())
                return Path(root)
            except (ValueError, OSError):
                continue
    return roots[0] if roots else None


NATIVE_LINUX_HELP = (
    "This Left 4 Dead 2 install is the native Linux build, but the VR mod needs the "
    "Windows build running under Proton.\n\n"
    "1. In Steam, right-click Left 4 Dead 2 > Properties > Compatibility.\n"
    "2. Tick 'Force the use of a specific Steam Play compatibility tool' and pick "
    "Proton Hotfix (or Proton Experimental).\n"
    "3. Let Steam download the Windows files, then check that left4dead2.exe exists "
    "in the game folder (Manage > Browse local files).\n"
    "4. Run this installer again."
)


def check_game_dir(game_dir: Path, platform: Optional[str] = None) -> str:
    """Raise InstallerError with clear guidance when ``game_dir`` is unusable.

    Returns the depot type.
    """
    game_dir = Path(game_dir)
    if not game_dir.is_dir():
        raise InstallerError(f"Game folder does not exist: {game_dir}")
    depot = game_depot(game_dir)
    if depot == "linux":
        raise InstallerError(NATIVE_LINUX_HELP)
    if depot == "missing":
        raise InstallerError(
            f"{game_dir} does not look like a Left 4 Dead 2 folder (left4dead2.exe not found). "
            "Pick the folder that contains left4dead2.exe (Steam: right-click the game > "
            "Manage > Browse local files)."
        )
    return depot


# ---------------------------------------------------------------------------
# Users, compat tools
# ---------------------------------------------------------------------------

@dataclass
class SteamUser:
    account_id: str
    steam_id64: Optional[str] = None
    name: str = ""
    most_recent: bool = False
    localconfig: Optional[Path] = None


def steam_users(steam_root: Path) -> List[SteamUser]:
    """Users with a ``userdata/<id>/config/localconfig.vdf``; the most recently
    logged-in account (from ``config/loginusers.vdf``) comes first."""
    steam_root = Path(steam_root)
    known = {}
    login = steam_root / "config" / "loginusers.vdf"
    if login.is_file():
        try:
            users = vdf.load(login).get_node("users")
        except (OSError, vdf.VdfError):
            users = None
        for id64, info in (users or []):
            if not id64.isdigit() or not isinstance(info, vdf.VdfNode):
                continue
            account = str(int(id64) - STEAMID64_BASE)
            known[account] = SteamUser(
                account, id64, info.get("PersonaName") or info.get("AccountName") or "",
                str(info.get("MostRecent", "0")) == "1",
            )
    out = []
    userdata = steam_root / "userdata"
    if userdata.is_dir():
        for child in sorted(userdata.iterdir()):
            config = child / "config" / "localconfig.vdf"
            if child.name.isdigit() and child.name != "0" and config.is_file():
                user = known.get(child.name, SteamUser(child.name))
                user.localconfig = config
                out.append(user)
    out.sort(key=lambda user: 0 if user.most_recent else 1)
    return out


def compat_tool_for_game(steam_root: Path) -> Optional[str]:
    """Name of the compatibility tool forced for app 550, e.g. ``proton_hotfix``."""
    config = Path(steam_root) / "config" / "config.vdf"
    if not config.is_file():
        return None
    try:
        data = vdf.load(config)
    except (OSError, vdf.VdfError):
        return None
    mapping = data.get_path("InstallConfigStore", "Software", "Valve", "Steam", "CompatToolMapping")
    if not isinstance(mapping, vdf.VdfNode):
        return None
    entry = mapping.get(APP_ID)
    if isinstance(entry, vdf.VdfNode):
        return entry.get("name") or None
    return None


# ---------------------------------------------------------------------------
# Processes
# ---------------------------------------------------------------------------

def _proc_entries(proc_root: Path):
    proc_root = Path(proc_root)
    if not proc_root.is_dir():
        return
    for child in proc_root.iterdir():
        if not child.name.isdigit() or int(child.name) == os.getpid():
            continue
        try:
            comm = (child / "comm").read_text(errors="ignore").strip()
        except OSError:
            comm = ""
        try:
            cmdline = (child / "cmdline").read_bytes().split(b"\0")
            args = [part.decode("utf-8", "ignore") for part in cmdline if part]
        except OSError:
            args = []
        yield comm, args


def _windows_process_running(image: str) -> bool:
    try:
        output = subprocess.run(
            ["tasklist", "/FI", f"IMAGENAME eq {image}", "/NH"],
            capture_output=True, text=True, timeout=15,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
        ).stdout
    except (OSError, subprocess.SubprocessError):
        return False
    return image.lower() in output.lower()


def is_steam_running(platform: Optional[str] = None, proc_root: Path = Path("/proc")) -> bool:
    if is_windows(platform):
        return _windows_process_running("steam.exe")
    for comm, args in _proc_entries(proc_root):
        exe = os.path.basename(args[0]) if args else ""
        if comm == "steam" or exe in ("steam", "steam.sh"):
            return True
    return False


def is_game_running(platform: Optional[str] = None, proc_root: Path = Path("/proc")) -> bool:
    if is_windows(platform):
        return _windows_process_running("left4dead2.exe")
    for comm, args in _proc_entries(proc_root):
        if comm.lower().startswith("left4dead2") or any(
            "left4dead2.exe" in arg.lower() for arg in args[:3]
        ):
            return True
    return False
