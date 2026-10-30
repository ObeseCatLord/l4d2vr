"""What the OpenXR-only mod needs from the computer, and fixing it.

From mod 1.3 on the mod talks to the headset through OpenXR only (no
``openvr_api.dll``, no SteamVR action manifest).

* **Linux (Proton):** the 32-bit game reaches the OpenXR runtime through
  Proton's ``wineopenxr``. Only Proton builds that ship the 32-bit
  ``wineopenxr.dll`` work: Proton Hotfix 2026-10-07 or newer, Proton
  Experimental 11.0-20261014 or newer, with ``PROTON_USE_WOW64=1``. Proton
  stable 11.0, 10.0 and older, and GE-Proton builds without it, cannot start
  VR. Proton also only enables OpenXR after it has started an OpenVR runtime
  (SteamVR, or XRizer / OpenComposite for Monado and WiVRn), so one must stay
  registered in ``openvrpaths.vrpath``.
* **Windows:** the active OpenXR runtime must have a 32-bit registration
  (``HKLM\\SOFTWARE\\WOW6432Node\\Khronos\\OpenXR\\1\\ActiveRuntime``) with
  Vulkan: SteamVR 2.17+, Virtual Desktop (VDXR), Meta Quest Link. Windows Mixed
  Reality's own runtime is 64-bit only (use SteamVR with the WMR driver).

The compatibility tool for app 550 lives in ``<steam>/config/config.vdf``
(``InstallConfigStore/Software/Valve/Steam/CompatToolMapping/550``). Like the
launch options it is only edited while Steam is closed, with a backup.
"""

from __future__ import annotations

import datetime as dt
import os
import re
import shutil
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, List, Optional, Tuple

from . import APP_ID, vdf
from .fsutil import InstallerError, atomic_write_json, atomic_write_text, read_json

WINEOPENXR32 = Path("files") / "lib" / "wine" / "i386-windows" / "wineopenxr.dll"
SUPPORTED_PROTON = ("Proton Hotfix (2026-10-07 or newer) or Proton Experimental (11.0-20261014 or newer) "
                    "with PROTON_USE_WOW64=1")

# Steam's internal compat tool names for the Protons that carry 32-bit OpenXR,
# best first, with their install folder and Steam app id.
PREFERRED_TOOLS = (
    ("proton_hotfix", "Proton Hotfix", "2180100"),
    ("proton_experimental", "Proton - Experimental", "1493710"),
)
PREFERRED_NAMES = tuple(name for name, _, _ in PREFERRED_TOOLS)
_KNOWN_FOLDERS = {
    "proton_hotfix": ("Proton Hotfix",),
    "proton_experimental": ("Proton - Experimental", "Proton Experimental"),
    "proton_next": ("Proton Next",),
}
_DISPLAY = {"proton_hotfix": "Proton Hotfix", "proton_experimental": "Proton Experimental"}
COMPAT_PRIORITY = "250"  # what Steam writes for a per-game choice


# ---------------------------------------------------------------------------
# Proton builds
# ---------------------------------------------------------------------------

def proton_has_32bit_openxr(proton_dir: Path) -> bool:
    return (Path(proton_dir) / WINEOPENXR32).is_file()


def proton_version(proton_dir: Path) -> str:
    """``hotfix-20261007-x86_64`` from the ``version`` file (empty if unknown)."""
    try:
        parts = (Path(proton_dir) / "version").read_text(errors="replace").split()
    except OSError:
        return ""
    return parts[1] if len(parts) > 1 else (parts[0] if parts else "")


@dataclass
class ProtonTool:
    name: str            # Steam's internal name, e.g. proton_hotfix, GE-Proton10-34
    path: Optional[Path]  # install folder (None: not installed)
    display: str = ""

    @property
    def installed(self) -> bool:
        return self.path is not None and (self.path / "proton").is_file()

    @property
    def openxr32(self) -> bool:
        return self.installed and proton_has_32bit_openxr(self.path)

    @property
    def version(self) -> str:
        return proton_version(self.path) if self.installed else ""

    def describe(self) -> str:
        label = self.display or self.name
        if not self.installed:
            return f"{label} (not installed yet; Steam downloads it on the next launch)"
        version = self.version
        return f"{label}" + (f" ({version})" if version and version != label else "") + f" at {self.path}"


def _official_folders(name: str) -> Tuple[str, ...]:
    if name in _KNOWN_FOLDERS:
        return _KNOWN_FOLDERS[name]
    match = re.fullmatch(r"proton_(\d+)", name)
    if not match:
        return ()
    digits = match.group(1)
    if len(digits) == 1 or (len(digits) == 2 and int(digits) >= 10):
        version = f"{digits}.0"
    else:
        version = f"{digits[0]}.{digits[1:]}"
    return (f"Proton {version}",)


def _custom_tool_names(tool_dir: Path) -> List[str]:
    manifest = tool_dir / "compatibilitytool.vdf"
    names = []
    if manifest.is_file():
        try:
            tools = vdf.load(manifest).get_path("compatibilitytools", "compat_tools")
            if isinstance(tools, vdf.VdfNode):
                names = [key for key, _ in tools]
        except (OSError, vdf.VdfError):
            pass
    return names


def resolve_tool(name: str, libraries: List[Path], steam_root: Optional[Path]) -> ProtonTool:
    """Find the install folder of the compat tool ``name``."""
    display = _DISPLAY.get(name, name)
    folders = _official_folders(name)
    for library in libraries:
        common = Path(library) / "steamapps" / "common"
        for folder in folders:
            if (common / folder / "proton").is_file():
                return ProtonTool(name, common / folder, display)
        if folders and common.is_dir():
            # "Proton 9.0 (Beta)" and similar.
            for child in sorted(common.iterdir()):
                if any(child.name.startswith(folder) for folder in folders) and (child / "proton").is_file():
                    return ProtonTool(name, child, display)
    if steam_root:
        tools = Path(steam_root) / "compatibilitytools.d"
        if tools.is_dir():
            for child in sorted(tools.iterdir()):
                if child.is_dir() and (child.name == name or name in _custom_tool_names(child)) and (child / "proton").is_file():
                    return ProtonTool(name, child, display)
    return ProtonTool(name, None, display)


def compat_tool_mapping(steam_root: Path) -> Optional[dict]:
    """``{"name": ..., "config": ..., "priority": ...}`` for app 550, or None."""
    config = Path(steam_root) / "config" / "config.vdf"
    if not config.is_file():
        return None
    try:
        data = vdf.load(config)
    except (OSError, vdf.VdfError):
        return None
    entry = data.get_path("InstallConfigStore", "Software", "Valve", "Steam", "CompatToolMapping", APP_ID)
    if not isinstance(entry, vdf.VdfNode):
        return None
    return {key: value for key, value in entry if isinstance(value, str)}


@dataclass
class ProtonCheck:
    current: Optional[ProtonTool]
    recommended: Optional[ProtonTool]

    @property
    def ok(self) -> bool:
        if self.current is None:
            return False
        if self.current.openxr32:
            return True
        # Hotfix/Experimental not downloaded yet: Steam installs a current build.
        return not self.current.installed and self.current.name in PREFERRED_NAMES

    def lines(self) -> List[str]:
        if self.current is None:
            out = ["Proton: no compatibility tool is forced for Left 4 Dead 2 (Steam would run the native Linux "
                   "build, which cannot load the mod)."]
        elif self.ok:
            out = [f"Proton: {self.current.describe()} - has 32-bit OpenXR, OK."]
        else:
            out = [f"Proton: {self.current.describe()} has no 32-bit OpenXR, so VR cannot start (the game runs "
                   f"in desktop mode). VR needs {SUPPORTED_PROTON}."]
        if not self.ok and self.recommended is not None:
            out.append(f"Fix: 'l4d2vr-installer proton apply' (Steam closed) switches it to {self.recommended.describe()}, "
                       "or in Steam: right-click Left 4 Dead 2 > Properties > Compatibility > "
                       f"{self.recommended.display}.")
        return out


def check_proton(steam_root: Path, libraries: List[Path]) -> ProtonCheck:
    mapping = compat_tool_mapping(steam_root)
    current = resolve_tool(mapping["name"], libraries, steam_root) if mapping and mapping.get("name") else None
    return ProtonCheck(current, recommended_tool(libraries, steam_root))


def recommended_tool(libraries: List[Path], steam_root: Optional[Path]) -> ProtonTool:
    """The best Proton for the mod: an installed Hotfix/Experimental with 32-bit
    OpenXR, else Proton Hotfix (Steam downloads it)."""
    candidates = [resolve_tool(name, libraries, steam_root) for name, _, _ in PREFERRED_TOOLS]
    for tool in candidates:
        if tool.openxr32:
            return tool
    for tool in candidates:
        if not tool.installed:
            return tool
    return candidates[0]


@dataclass
class CompatToolChange:
    config: Path
    before: Optional[dict]
    after: dict
    backup: Optional[Path] = None

    @property
    def changed(self) -> bool:
        return (self.before or {}).get("name") != self.after.get("name")


def _state_path(state_dir: Path) -> Path:
    return Path(state_dir) / "compat_tool_state.json"


def _mapping_node(root: vdf.VdfNode) -> vdf.VdfNode:
    node = root
    for key in ("InstallConfigStore", "Software", "Valve", "Steam", "CompatToolMapping"):
        node = node.ensure_node(key)
    return node


def set_compat_tool(steam_root: Path, name: str, *, steam_running: bool, dry_run: bool = False,
                    state_dir: Optional[Path] = None) -> CompatToolChange:
    """Force compat tool ``name`` for app 550 in ``config.vdf``.

    Refuses while Steam runs (Steam rewrites the file on exit). The original
    file is copied next to it first, and the previous choice is remembered in
    ``state_dir`` so :func:`restore_compat_tool` can put it back.
    """
    config = Path(steam_root) / "config" / "config.vdf"
    if not config.is_file():
        raise InstallerError(f"{config} not found. Start Steam once, then try again.")
    text = config.read_text(encoding="utf-8", errors="surrogateescape")
    try:
        root = vdf.loads(text)
    except vdf.VdfError as error:
        raise InstallerError(f"Cannot parse {config}: {error}") from error
    if root.get_node("InstallConfigStore") is None:
        raise InstallerError(f"{config} is not a Steam config.vdf file")
    mapping = _mapping_node(root)
    entry = mapping.get_node(APP_ID)
    before = {key: value for key, value in entry if isinstance(value, str)} if entry is not None else None
    after = {"name": name, "config": "", "priority": COMPAT_PRIORITY}
    change = CompatToolChange(config, before, after)
    if not change.changed or dry_run:
        return change
    if steam_running:
        raise InstallerError(
            "Steam is running. Close Steam completely (Steam > Exit) before changing the Proton version, or "
            "choose it yourself: right-click Left 4 Dead 2 > Properties > Compatibility.")
    if entry is None:
        entry = mapping.ensure_node(APP_ID)
    for key, value in after.items():
        entry.set(key, value)
    stamp = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    backup = config.with_name(f"{config.name}.l4d2vr-{stamp}.bak")
    shutil.copy2(config, backup)
    change.backup = backup
    atomic_write_text(config, vdf.dumps(root, vdf.detect_newline(text)))
    if state_dir is not None:
        state = read_json(_state_path(state_dir), {}) or {}
        record = state.get(str(config))
        if record is None or record.get("written") != (before or {}).get("name"):
            state[str(config)] = {"previous": before, "written": name}
        else:
            record["written"] = name
        atomic_write_json(_state_path(state_dir), state)
    return change


def restore_compat_tool(steam_root: Path, *, steam_running: bool, dry_run: bool = False,
                        state_dir: Path) -> Optional[CompatToolChange]:
    """Put back the compat tool from before the installer changed it (None:
    the installer never changed it, or it was changed again since)."""
    config = Path(steam_root) / "config" / "config.vdf"
    state = read_json(_state_path(state_dir), {}) or {}
    record = state.get(str(config))
    current = compat_tool_mapping(steam_root)
    if not record or (current or {}).get("name") != record.get("written"):
        return None
    previous = record.get("previous")
    if dry_run:
        return CompatToolChange(config, current, previous or {})
    if steam_running:
        raise InstallerError("Steam is running. Close Steam completely, then try again.")
    text = config.read_text(encoding="utf-8", errors="surrogateescape")
    root = vdf.loads(text)
    mapping = _mapping_node(root)
    if previous:
        entry = mapping.ensure_node(APP_ID)
        for key in [key for key, _ in entry]:
            entry.remove(key)
        for key, value in previous.items():
            entry.set(key, value)
    else:
        mapping.remove(APP_ID)
    stamp = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    backup = config.with_name(f"{config.name}.l4d2vr-{stamp}.bak")
    shutil.copy2(config, backup)
    atomic_write_text(config, vdf.dumps(root, vdf.detect_newline(text)))
    state.pop(str(config), None)
    atomic_write_json(_state_path(state_dir), state)
    return CompatToolChange(config, current, previous or {}, backup)


# ---------------------------------------------------------------------------
# The OpenVR runtime Proton needs (Linux)
# ---------------------------------------------------------------------------

def openvr_paths_file(home: Optional[Path] = None) -> Path:
    if home is not None:
        config_home = Path(home) / ".config"
    else:
        config_home = Path(os.environ.get("XDG_CONFIG_HOME") or str(Path.home() / ".config"))
    return config_home / "openvr" / "openvrpaths.vrpath"


def openvr_runtime(home: Optional[Path] = None) -> Optional[Path]:
    """The first runtime listed in openvrpaths.vrpath (what Proton loads)."""
    data = read_json(openvr_paths_file(home), None)
    try:
        runtimes = data.get("runtime") or []
    except AttributeError:
        return None
    for entry in runtimes:
        if isinstance(entry, str) and entry.strip():
            return Path(entry)
    return None


def openvr_runtime_lines(home: Optional[Path] = None) -> List[str]:
    runtime = openvr_runtime(home)
    if runtime is None:
        return ["OpenVR runtime for Proton: none registered. Proton only starts OpenXR after an OpenVR runtime: "
                "SteamVR (start it once), or XRizer / OpenComposite for Monado and WiVRn (Envision registers one; "
                "the installer's Monado route sets up XRizer)."]
    kind = ("SteamVR" if "steamvr" in str(runtime).lower() else
            "XRizer" if "xrizer" in str(runtime).lower() else
            "OpenComposite" if "opencomposite" in str(runtime).lower() else "OpenVR runtime")
    if not (runtime / "bin" / "linux64" / "vrclient.so").is_file():
        return [f"OpenVR runtime for Proton: {runtime} is registered but has no bin/linux64/vrclient.so; Proton "
                "cannot start OpenXR with it. Re-register a working runtime (SteamVR, XRizer or OpenComposite)."]
    return [f"OpenVR runtime for Proton: {kind} at {runtime} - OK (Proton needs it before OpenXR works; keep it)."]


def linux_active_openxr_runtime(home: Optional[Path] = None) -> Optional[Path]:
    if home is not None:
        config_home = Path(home) / ".config"
    else:
        config_home = Path(os.environ.get("XDG_CONFIG_HOME") or str(Path.home() / ".config"))
    active = config_home / "openxr" / "1" / "active_runtime.json"
    for candidate in (active, Path("/etc/xdg/openxr/1/active_runtime.json")):
        if candidate.exists():
            try:
                return candidate.resolve(strict=True)
            except OSError:
                continue
    return None


def linux_openxr_lines(home: Optional[Path] = None) -> List[str]:
    active = linux_active_openxr_runtime(home)
    if active is None:
        return ["OpenXR runtime: no active runtime (~/.config/openxr/1/active_runtime.json). Set one: SteamVR "
                "(Settings > OpenXR > Set SteamVR as OpenXR Runtime), Monado/Envision or WiVRn."]
    return [f"OpenXR runtime: {active}"]


# ---------------------------------------------------------------------------
# Windows
# ---------------------------------------------------------------------------

def _read_active_runtime(view: str) -> str:
    try:
        import winreg  # type: ignore
    except ImportError:
        return ""
    flag = winreg.KEY_WOW64_32KEY if view == "32" else winreg.KEY_WOW64_64KEY
    try:
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\Khronos\OpenXR\1", 0, winreg.KEY_READ | flag) as key:
            value, _ = winreg.QueryValueEx(key, "ActiveRuntime")
            return str(value or "")
    except OSError:
        return ""


def windows_openxr_lines(reader: Optional[Callable[[str], str]] = None) -> List[str]:
    """Is the active OpenXR runtime usable by the 32-bit game?"""
    read = reader or _read_active_runtime
    runtime32, runtime64 = read("32"), read("64")
    if runtime32:
        if not Path(runtime32).is_file():
            return [f"OpenXR runtime (32-bit): {runtime32} is registered but missing. Reinstall the runtime or set "
                    "it as the OpenXR runtime again."]
        return [f"OpenXR runtime (32-bit): {runtime32} - OK. The headset's runtime must be running when the game "
                "starts (SteamVR starts by itself)."]
    if runtime64:
        return [f"OpenXR runtime: {runtime64} is 64-bit only; Left 4 Dead 2 is 32-bit, so VR will not start. "
                "Make SteamVR 2.17 or newer (SteamVR Settings > OpenXR > Set SteamVR as OpenXR Runtime), Virtual "
                "Desktop or Meta Quest Link the active OpenXR runtime. Windows Mixed Reality: use SteamVR with the "
                "WMR for SteamVR driver."]
    return ["OpenXR runtime: none is active. Install SteamVR 2.17 or newer and set it as the OpenXR runtime "
            "(SteamVR Settings > OpenXR), or use Virtual Desktop or Meta Quest Link. Without one the mod starts in "
            "desktop mode."]


def requirement_lines(steam_root: Optional[Path], libraries: List[Path], platform: Optional[str] = None,
                      home: Optional[Path] = None) -> List[str]:
    """Human-readable OpenXR checks for ``status``/``detect`` and the GUI."""
    if (platform or sys.platform).startswith("win"):
        return windows_openxr_lines()
    lines: List[str] = []
    if steam_root is not None:
        lines += check_proton(steam_root, libraries).lines()
    lines += openvr_runtime_lines(home)
    lines += linux_openxr_lines(home)
    return lines


def describe_change(change: CompatToolChange) -> str:
    before = (change.before or {}).get("name") or "(none)"
    return f"{before} -> {change.after.get('name') or '(none)'}"


def to_json(check: ProtonCheck) -> dict:
    def tool(item: Optional[ProtonTool]):
        if item is None:
            return None
        return {"name": item.name, "path": str(item.path) if item.path else None, "installed": item.installed,
                "version": item.version, "openxr32": item.openxr32}
    return {"current": tool(check.current), "recommended": tool(check.recommended), "ok": check.ok}

