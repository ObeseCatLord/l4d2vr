"""Starting the game in VR.

* SteamVR route (Windows, and Linux with launch options set): open
  ``steam://rungameid/550`` (or ``steam://run/550//<args>/`` when joining a
  server).
* Monado / WiVRn route (Linux, advanced): port of the Linux handoff's
  ``tools/launch.py``. The Windows game is started through the Steam Runtime
  entry point and Proton directly, keeping Steam app id 550, with
  ``VR_OVERRIDE`` pointing at an XRizer OpenVR runtime and
  ``XR_RUNTIME_JSON`` at the OpenXR runtime manifest.
"""

from __future__ import annotations

import datetime as dt
import glob
import json
import os
import stat
import subprocess
import sys
import urllib.parse
import zipfile
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Callable, Dict, List, Optional, Tuple

from . import APP_ID
from . import steam as steam_mod
from .fsutil import InstallerError, atomic_write_bytes, normalize_member, sha256_file

DEFAULT_GAME_ARGS = [
    "-insecure", "-dx9", "-heapsize", "524288", "-processheap", "-novid", "-windowed",
    "-w", "1920", "-h", "1080", "-condebug", "+con_enable", "1", "+mat_vsync", "0",
    "+mat_antialias", "0", "+mat_queue_mode", "0", "+cl_disablehtmlmotd", "1",
]


# ---------------------------------------------------------------------------
# Steam URL route
# ---------------------------------------------------------------------------

def connect_args(server: Optional[str], password: Optional[str]) -> List[str]:
    args: List[str] = []
    if server:
        if password:
            args += ["+password", password]
        args += ["+connect", server]
    return args


def steam_url(args: Optional[List[str]] = None) -> str:
    if not args:
        return f"steam://rungameid/{APP_ID}"
    joined = " ".join(args)
    return f"steam://run/{APP_ID}//{urllib.parse.quote(joined, safe='+:.-_')}/"


def open_steam_url(url: str, dry_run: bool = False) -> List[str]:
    """Hand a steam:// URL to the OS. Returns the command used (for logs)."""
    if sys.platform.startswith("win"):
        if not dry_run:
            os.startfile(url)  # type: ignore[attr-defined]
        return ["startfile", url]
    if sys.platform == "darwin":
        command = ["open", url]
    else:
        command = ["xdg-open", url]
        if not _which("xdg-open"):
            command = ["steam", url] if _which("steam") else ["flatpak", "run", "com.valvesoftware.Steam", url]
    if not dry_run:
        try:
            subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
        except OSError as error:
            raise InstallerError(f"Could not open Steam ({error}). Start the game from Steam instead.") from error
    return command


def _which(name: str) -> Optional[str]:
    from shutil import which
    return which(name)


# ---------------------------------------------------------------------------
# Monado / XRizer route
# ---------------------------------------------------------------------------

@dataclass
class MonadoConfig:
    game_dir: str = ""
    steam_dir: str = ""
    proton_dir: str = ""
    steam_runtime_entrypoint: str = ""
    openxr_manifest: str = ""
    xrizer_dir: str = ""
    state_dir: str = ""
    steam_socket: str = ""
    monado_socket: str = ""
    game_args: List[str] = field(default_factory=lambda: list(DEFAULT_GAME_ARGS))

    @classmethod
    def from_dict(cls, data: Optional[dict]) -> "MonadoConfig":
        data = data or {}
        known = {key: data[key] for key in cls.__dataclass_fields__ if key in data and data[key] is not None}
        return cls(**known)

    def to_dict(self) -> dict:
        return asdict(self)

    def path(self, name: str) -> Optional[Path]:
        value = getattr(self, name)
        return Path(os.path.expanduser(value)) if value else None


def _home(home: Optional[Path]) -> Path:
    return Path(home) if home else Path.home()


def runtime_dir() -> Path:
    getuid = getattr(os, "getuid", lambda: 0)
    return Path(os.environ.get("XDG_RUNTIME_DIR", f"/run/user/{getuid()}"))


def detect_openxr_manifests(home: Optional[Path] = None) -> List[Path]:
    """OpenXR runtime manifests: the active runtime first, then Envision
    profiles, then system-wide Monado/WiVRn manifests."""
    if home is None:
        home = Path.home()
        config_home = Path(os.environ.get("XDG_CONFIG_HOME") or str(home / ".config"))
    else:
        config_home = Path(home) / ".config"
    found: List[Path] = []
    active = config_home / "openxr" / "1" / "active_runtime.json"
    if active.exists():
        try:
            found.append(active.resolve(strict=True))
        except OSError:
            pass
    selected = envision_selected_manifest(config_home)
    if selected and selected not in found:
        found.append(selected)
    patterns = [
        str(home / ".local/share/envision/prefixes/*/share/openxr/1/openxr_*.json"),
        "/usr/share/openxr/1/openxr_monado.json",
        "/usr/share/openxr/1/openxr_wivrn.json",
        "/usr/local/share/openxr/1/openxr_monado.json",
        "/etc/xdg/openxr/1/active_runtime.json",
    ]
    for pattern in patterns:
        for match in sorted(glob.glob(pattern)):
            path = Path(match)
            try:
                path = path.resolve(strict=True)
            except OSError:
                continue
            if path not in found:
                found.append(path)
    return found


def envision_selected_manifest(config_home: Path) -> Optional[Path]:
    """OpenXR manifest of the profile currently selected in Envision."""
    try:
        data = json.loads((Path(config_home) / "envision" / "envision.json").read_text())
        selected = data.get("selected_profile_uuid")
        for profile in data.get("user_profiles") or []:
            if profile.get("uuid") == selected and profile.get("prefix"):
                for name in ("openxr_monado.json", "openxr_wivrn.json"):
                    manifest = Path(profile["prefix"]) / "share" / "openxr" / "1" / name
                    if manifest.is_file():
                        return manifest.resolve()
    except (OSError, ValueError, AttributeError, TypeError):
        pass
    return None


def detect_proton(libraries: List[Path], steam_root: Optional[Path] = None) -> List[Path]:
    """Installed Proton builds, Proton Hotfix first (the tested one)."""
    found: List[Path] = []
    for library in libraries:
        common = Path(library) / "steamapps" / "common"
        if common.is_dir():
            for child in common.iterdir():
                if child.name.startswith("Proton") and (child / "proton").is_file():
                    found.append(child)
    if steam_root:
        tools = Path(steam_root) / "compatibilitytools.d"
        if tools.is_dir():
            found += [child for child in tools.iterdir() if (child / "proton").is_file()]

    def rank(path: Path):
        name = path.name.lower()
        return (0 if "hotfix" in name else 1 if "experimental" in name else 2, name)

    return sorted(dict.fromkeys(found), key=rank)


def detect_runtime_entrypoints(libraries: List[Path], home: Optional[Path] = None) -> List[Path]:
    home = _home(home)
    candidates = [home / ".local/share/umu/steamrt4/_v2-entry-point"]
    for library in libraries:
        common = Path(library) / "steamapps" / "common"
        candidates += [common / "SteamLinuxRuntime_4" / "_v2-entry-point",
                       common / "SteamLinuxRuntime_sniper" / "_v2-entry-point"]
    candidates.append(home / ".local/share/umu/steamrt3/_v2-entry-point")
    return [path for path in candidates if path.is_file()]


def default_state_dir() -> Path:
    base = os.environ.get("XDG_STATE_HOME") or str(Path.home() / ".local" / "state")
    return Path(base) / "l4d2vr-installer"


def default_xrizer_dir() -> Path:
    base = os.environ.get("XDG_DATA_HOME") or str(Path.home() / ".local" / "share")
    return Path(base) / "l4d2vr-installer" / "xrizer"


def autodetect(config: MonadoConfig, libraries: List[Path], steam_root: Optional[Path],
               home: Optional[Path] = None) -> MonadoConfig:
    """Fill empty fields of ``config`` with detected values."""
    if not config.steam_dir and steam_root:
        config.steam_dir = str(steam_root)
    if not config.proton_dir:
        protons = detect_proton(libraries, steam_root)
        if protons:
            config.proton_dir = str(protons[0])
    if not config.steam_runtime_entrypoint:
        entries = detect_runtime_entrypoints(libraries, home)
        if entries:
            config.steam_runtime_entrypoint = str(entries[0])
    if not config.openxr_manifest:
        manifests = detect_openxr_manifests(home)
        if manifests:
            config.openxr_manifest = str(manifests[0])
    if not config.state_dir:
        config.state_dir = str(default_state_dir())
    if not config.xrizer_dir and (default_xrizer_dir() / "libxrizer.so").is_file():
        config.xrizer_dir = str(default_xrizer_dir())
    return config


def _is_socket(path: Path) -> bool:
    try:
        return stat.S_ISSOCK(path.stat().st_mode)
    except OSError:
        return False


def is_pe32(path: Path) -> bool:
    try:
        with open(path, "rb") as source:
            header = source.read(64)
            if header[:2] != b"MZ" or len(header) < 64:
                return False
            source.seek(int.from_bytes(header[0x3C:0x40], "little"))
            return source.read(6) == b"PE\0\0L\x01"
    except OSError:
        return False


def xrizer_integrity(xrizer_dir: Path) -> Optional[str]:
    """Check ``SHA256SUMS`` in the XRizer folder when it exists. Returns a
    problem description or None."""
    sums = xrizer_dir / "SHA256SUMS"
    if not sums.is_file():
        return None
    expected = {}
    for line in sums.read_text(errors="replace").splitlines():
        parts = line.split(None, 1)
        if len(parts) == 2:
            expected[parts[1].strip().lstrip("*")] = parts[0].lower()
    for name in ("libxrizer.so", "bin/linux64/vrclient.so"):
        path = xrizer_dir / name
        if name in expected and (not path.is_file() or sha256_file(path) != expected[name]):
            return f"XRizer runtime file does not match its SHA256SUMS: {path}"
    return None


def validate(config: MonadoConfig, steam_running: Optional[Callable[[], bool]] = None) -> Tuple[List[str], List[str]]:
    """Return ``(problems, warnings)``; launching needs no problems.

    ``steam_running`` defaults to a process check; newer 64-bit Steam clients
    no longer create ``steam.sock``, so either signal is accepted.
    """
    problems: List[str] = []
    warnings: List[str] = []
    labels = {
        "game_dir": "game folder", "steam_dir": "Steam folder", "proton_dir": "Proton folder",
        "steam_runtime_entrypoint": "Steam Runtime entry point (_v2-entry-point)",
        "openxr_manifest": "OpenXR runtime manifest", "xrizer_dir": "XRizer runtime folder",
    }
    for name, label in labels.items():
        if not getattr(config, name):
            problems.append(f"Not set: {label}")
    if problems:
        return problems, warnings
    game = config.path("game_dir")
    checks = [
        (game, "dir", "game folder"), (config.path("steam_dir"), "dir", "Steam folder"),
        (config.path("proton_dir") / "proton", "exec", "Proton executable"),
        (config.path("steam_runtime_entrypoint"), "exec", "Steam Runtime entry point"),
        (config.path("openxr_manifest"), "file", "OpenXR runtime manifest"),
        (config.path("xrizer_dir") / "libxrizer.so", "file", "XRizer runtime (libxrizer.so)"),
        (config.path("xrizer_dir") / "bin" / "linux64" / "vrclient.so", "file", "XRizer vrclient.so"),
    ]
    for path, kind, label in checks:
        ok = path.is_dir() if kind == "dir" else path.is_file() and (kind != "exec" or os.access(path, os.X_OK))
        if not ok:
            problems.append(f"Missing {label}: {path}")
    exe = game / "left4dead2.exe"
    if not exe.is_file():
        problems.append(f"Missing {exe} (the Windows build of the game is required)")
    elif not is_pe32(exe):
        problems.append(f"{exe} is not a 32-bit Windows executable")
    for name in ("d3d9.dll", "openvr_api.dll"):
        if not (game / name).is_file():
            problems.append(f"The mod is not installed ({name} missing in {game})")
    integrity = xrizer_integrity(config.path("xrizer_dir"))
    if integrity:
        problems.append(integrity)
    elif not (config.path("xrizer_dir") / "SHA256SUMS").is_file():
        warnings.append("XRizer folder has no SHA256SUMS; its files could not be verified.")
    steam_socket = config.path("steam_socket") or (config.path("steam_dir") / "steam.sock")
    if not _is_socket(steam_socket) and not (steam_running or steam_mod.is_steam_running)():
        problems.append("Steam does not seem to be running. Start Steam and log in first.")
    monado_socket = config.path("monado_socket") or (runtime_dir() / "monado_comp_ipc")
    if not _is_socket(monado_socket):
        warnings.append(f"No Monado socket at {monado_socket}. Start your Monado/Envision/WiVRn profile first "
                        "(WiVRn users can ignore this if WiVRn is running).")
    return problems, warnings


def library_root_for(game_dir: Path) -> Path:
    if game_dir.parent.name == "common" and game_dir.parent.parent.name == "steamapps":
        return game_dir.parent.parent.parent
    return game_dir.parent


def build_command(config: MonadoConfig, server: Optional[str] = None, password: Optional[str] = None,
                  extra: Optional[List[str]] = None, log_dir: Optional[Path] = None,
                  base_env: Optional[Dict[str, str]] = None) -> Tuple[List[str], Dict[str, str]]:
    """Build the argv and environment for the Monado/XRizer launch."""
    game = config.path("game_dir")
    state = config.path("state_dir") or default_state_dir()
    manifest = config.path("openxr_manifest")
    try:
        manifest = manifest.resolve(strict=True)  # relative library paths need the real file
    except OSError:
        pass
    env = dict(os.environ if base_env is None else base_env)
    env.update({
        "STEAM_COMPAT_CLIENT_INSTALL_PATH": str(config.path("steam_dir")),
        "STEAM_COMPAT_DATA_PATH": str(state / "proton-prefix"),
        "STEAM_COMPAT_INSTALL_PATH": str(game),
        "STEAM_COMPAT_LIBRARY_PATHS": os.pathsep.join((str(library_root_for(game)), str(config.path("steam_dir")))),
        "SteamAppId": APP_ID, "SteamGameId": APP_ID, "STEAM_COMPAT_APP_ID": APP_ID,
        "PROTON_USE_WOW64": "1", "WINEDLLOVERRIDES": "d3d9,openvr_api=n,b",
        "XDG_RUNTIME_DIR": str(runtime_dir()),
        "XR_RUNTIME_JSON": str(manifest), "VR_OVERRIDE": str(config.path("xrizer_dir")),
        "RUST_LOG": "error,xrizer::overlay=warn", "DXVK_FRAME_PACE": "max-frame-latency",
        "PROTON_ENABLE_WAYLAND": "0", "PROTON_USE_WAYLAND": "0", "PROTON_LOG": "0", "WINEDEBUG": "-all",
    })
    if log_dir is not None:
        env.update({"XDG_STATE_HOME": str(log_dir / "state"), "DXVK_LOG_LEVEL": "info",
                    "DXVK_LOG_PATH": str(log_dir)})
    args = list(config.game_args or DEFAULT_GAME_ARGS) + connect_args(server, password) + list(extra or [])
    command = [str(config.path("steam_runtime_entrypoint")), "--verb=run", "--",
               str(config.path("proton_dir") / "proton"), "waitforexitandrun", str(game / "left4dead2.exe")] + args
    return command, env


def ensure_openvr_paths(xrizer_dir: Path, dry_run: bool = False) -> Optional[Path]:
    """Create a minimal OpenVR path registry if none exists (an existing one
    is never modified)."""
    config_home = Path(os.environ.get("XDG_CONFIG_HOME", str(Path.home() / ".config")))
    path = config_home / "openvr" / "openvrpaths.vrpath"
    if path.exists():
        return None
    if not dry_run:
        payload = {"jsonid": "vrpathreg", "runtime": [str(xrizer_dir)], "version": 1}
        atomic_write_bytes(path, (json.dumps(payload) + "\n").encode())
    return path


def redact(command: List[str]) -> List[str]:
    out, hide = [], False
    for word in command:
        out.append("****" if hide else word)
        hide = word == "+password"
    return out


def launch_monado(config: MonadoConfig, server: Optional[str] = None, password: Optional[str] = None,
                  extra: Optional[List[str]] = None, *, dry_run: bool = False, wait: bool = True,
                  log=print, game_running=None, steam_running=None) -> int:
    problems, warnings = validate(config, steam_running)
    for warning in warnings:
        log("Warning: " + warning)
    if problems:
        raise InstallerError("Cannot launch:\n  " + "\n  ".join(problems))
    if game_running and game_running():
        raise InstallerError("Left 4 Dead 2 is already running. Close it first.")
    state = config.path("state_dir") or default_state_dir()
    stamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    log_dir = state / "runs" / stamp
    command, env = build_command(config, server, password, extra, log_dir)
    log("Command: " + " ".join(redact(command)))
    if dry_run:
        log("Dry run: not launching.")
        return 0
    created = ensure_openvr_paths(config.path("xrizer_dir"))
    if created:
        log(f"Created OpenVR path registry {created}")
    state.mkdir(parents=True, exist_ok=True)
    lock = open(state / "launch.lock", "a+")
    try:
        try:
            import fcntl
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except ImportError:  # pragma: no cover - Windows has no Monado route
            pass
        except BlockingIOError:
            raise InstallerError("Another L4D2VR launch is already active.")
        log_dir.mkdir(parents=True, exist_ok=False)
        with open(log_dir / "binary-hashes.txt", "w") as report:
            for item in (config.path("game_dir") / "d3d9.dll", config.path("game_dir") / "openvr_api.dll",
                         config.path("game_dir") / "openxr_loader.dll", config.path("xrizer_dir") / "libxrizer.so"):
                if item.is_file():
                    report.write(f"{sha256_file(item)}  {item.name}\n")
        output = open(log_dir / "launch.log", "w")
        output.write(f"Started {stamp}; connection arguments withheld.\n")
        output.flush()
        log(f"Logs: {log_dir}")
        process = subprocess.Popen(command, cwd=str(config.path("game_dir")), env=env,
                                   stdout=output, stderr=subprocess.STDOUT)
        if not wait:
            output.close()  # the child keeps its own handle
            return 0
        code = process.wait()
        output.close()
        log(f"Game exited with code {code}")
        return code
    finally:
        lock.close()


# ---------------------------------------------------------------------------
# Optional XRizer runtime download (only when the release ships one)
# ---------------------------------------------------------------------------

def install_xrizer_zip(zip_path: Path, dest: Path, dry_run: bool = False, log=print) -> Path:
    """Extract an XRizer runtime archive into ``dest`` (a fresh folder)."""
    with zipfile.ZipFile(zip_path) as archive:
        members = [(normalize_member(info.filename), info) for info in archive.infolist() if not info.is_dir()]
        names = [name for name, _ in members]
        prefix = ""
        if "libxrizer.so" not in names:
            hits = [name for name in names if name.endswith("/libxrizer.so")]
            if len(hits) != 1:
                raise InstallerError("The XRizer archive does not contain libxrizer.so")
            prefix = hits[0][: -len("libxrizer.so")]
        log(f"Installing XRizer runtime into {dest}")
        if dry_run:
            return dest
        for name, info in members:
            if not name.startswith(prefix):
                continue
            if stat.S_ISLNK(info.external_attr >> 16):
                raise InstallerError(f"XRizer archive contains a symbolic link: {name}")
            target = dest / name[len(prefix):]
            atomic_write_bytes(target, archive.read(info))
            mode = (info.external_attr >> 16) & 0o777
            if mode & 0o111 or target.suffix == ".so":
                os.chmod(target, 0o755)
    return dest
