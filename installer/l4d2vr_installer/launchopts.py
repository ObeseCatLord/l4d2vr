"""Steam launch options for the Proton (Linux) route.

The mod needs Wine to load its own ``d3d9.dll`` and ``openvr_api.dll``
instead of Proton's built-ins, and the 32-bit game must use Proton's WoW64
mode. These match the working Linux launcher (tools/launch.py of the Linux
handoff): ``WINEDLLOVERRIDES=d3d9,openvr_api=n,b``, ``PROTON_USE_WOW64=1`` and
the ``-dx9`` game argument.

Launch options live in ``<steam>/userdata/<id>/config/localconfig.vdf`` under
``UserLocalConfigStore/Software/Valve/Steam/apps/550/LaunchOptions``. Steam
rewrites that file on exit, so it is only edited while Steam is closed.
"""

from __future__ import annotations

import datetime as dt
import re
import shlex
import shutil
from dataclasses import dataclass
from pathlib import Path
from typing import List, Optional, Tuple

from . import APP_ID, vdf
from .fsutil import InstallerError, atomic_write_text, read_json, atomic_write_json

DLL_OVERRIDES = ("d3d9", "openvr_api")
REQUIRED_ENV = (("WINEDLLOVERRIDES", "d3d9,openvr_api=n,b"), ("PROTON_USE_WOW64", "1"))
REQUIRED_ARGS = ("-dx9",)
HARMFUL_ARGS = ("-vulkan",)
RECOMMENDED = 'WINEDLLOVERRIDES="d3d9,openvr_api=n,b" PROTON_USE_WOW64=1 %command% -dx9'

_ASSIGNMENT = re.compile(r"^([A-Za-z_][A-Za-z0-9_]*)=(.*)$", re.S)
_SAFE_VALUE = re.compile(r"^[A-Za-z0-9_./:@%+-]*$")


def _format_assignment(name: str, value: str) -> str:
    if _SAFE_VALUE.match(value) and value:
        return f"{name}={value}"
    return f'{name}="' + value.replace("\\", "\\\\").replace('"', '\\"').replace("$", "\\$").replace("`", "\\`") + '"'


def _format_word(word: str) -> str:
    return word if word and re.match(r"^[A-Za-z0-9_./:@%+=,-]+$", word) else shlex.quote(word)


def _split(text: str) -> List[str]:
    try:
        return shlex.split(text, posix=True)
    except ValueError as error:
        raise InstallerError(
            f"Could not understand the existing launch options ({error}). "
            "Fix or clear them in Steam (Properties > General > Launch options) and retry."
        ) from error


def merge_overrides(existing: str) -> str:
    """Add ``d3d9,openvr_api=n,b`` to a WINEDLLOVERRIDES value, keeping other
    entries (e.g. ``dxgi=n,b`` for ReShade)."""
    kept = []
    for entry in filter(None, (part.strip() for part in existing.split(";"))):
        names, sep, mode = entry.partition("=")
        rest = [name for name in names.split(",") if name.strip() and name.strip().lower() not in DLL_OVERRIDES]
        if rest:
            kept.append(",".join(rest) + (sep + mode if sep else ""))
    kept.append("d3d9,openvr_api=n,b")
    return ";".join(kept)


def strip_overrides(existing: str) -> str:
    kept = []
    for entry in filter(None, (part.strip() for part in existing.split(";"))):
        names, sep, mode = entry.partition("=")
        rest = [name for name in names.split(",") if name.strip() and name.strip().lower() not in DLL_OVERRIDES]
        if rest:
            kept.append(",".join(rest) + (sep + mode if sep else ""))
    return ";".join(kept)


def _parts(existing: str) -> Tuple[List[str], List[str]]:
    existing = (existing or "").strip()
    if "%command%" in existing:
        prefix, _, suffix = existing.partition("%command%")
        return _split(prefix), _split(suffix)
    # Without %command% Steam treats the whole string as game arguments.
    return [], _split(existing)


def _join(prefix: List[str], suffix: List[str]) -> str:
    def render(word: str) -> str:
        match = _ASSIGNMENT.match(word)
        return _format_assignment(match.group(1), match.group(2)) if match else _format_word(word)

    pieces = [render(word) for word in prefix] + ["%command%"] + [_format_word(word) for word in suffix]
    return " ".join(pieces)


def merge_launch_options(existing: str) -> Tuple[str, List[str]]:
    """Return ``(new_options, notes)`` with the VR requirements added.

    Other environment variables, wrappers (gamemoderun, mangohud) and game
    arguments are kept. ``-vulkan`` is removed because the mod relies on its
    own DX9 path.
    """
    if not (existing or "").strip():
        return RECOMMENDED, ["Launch options were empty; using the recommended set."]
    prefix, suffix = _parts(existing)
    notes: List[str] = []
    seen = set()
    new_prefix = []
    for word in prefix:
        match = _ASSIGNMENT.match(word)
        if match and match.group(1) == "WINEDLLOVERRIDES":
            seen.add("WINEDLLOVERRIDES")
            new_prefix.append("WINEDLLOVERRIDES=" + merge_overrides(match.group(2)))
        elif match and match.group(1) == "PROTON_USE_WOW64":
            seen.add("PROTON_USE_WOW64")
            new_prefix.append("PROTON_USE_WOW64=1")
        else:
            new_prefix.append(word)
    # Environment assignments must come before wrapper commands.
    insert_at = 0
    while insert_at < len(new_prefix) and _ASSIGNMENT.match(new_prefix[insert_at]):
        insert_at += 1
    for name, value in REQUIRED_ENV:
        if name not in seen:
            new_prefix.insert(insert_at, f"{name}={value}")
            insert_at += 1
    new_suffix = []
    for word in suffix:
        if word.lower() in HARMFUL_ARGS:
            notes.append(f"Removed {word}: the mod uses its own DX9/DXVK renderer.")
            continue
        new_suffix.append(word)
    for arg in REQUIRED_ARGS:
        if arg not in (word.lower() for word in new_suffix):
            new_suffix.append(arg)
    return _join(new_prefix, new_suffix), notes


def strip_launch_options(existing: str) -> str:
    """Remove what :func:`merge_launch_options` adds; keep everything else."""
    if not (existing or "").strip():
        return ""
    prefix, suffix = _parts(existing)
    new_prefix = []
    for word in prefix:
        match = _ASSIGNMENT.match(word)
        if match and match.group(1) == "WINEDLLOVERRIDES":
            rest = strip_overrides(match.group(2))
            if rest:
                new_prefix.append("WINEDLLOVERRIDES=" + rest)
        elif match and match.group(1) == "PROTON_USE_WOW64":
            continue
        else:
            new_prefix.append(word)
    new_suffix = [word for word in suffix if word.lower() not in REQUIRED_ARGS]
    if not new_prefix and not new_suffix:
        return ""
    if not new_prefix:
        return " ".join(_format_word(word) for word in new_suffix)
    return _join(new_prefix, new_suffix)


def has_required_options(existing: str) -> bool:
    try:
        merged, _ = merge_launch_options(existing)
    except InstallerError:
        return False
    return bool((existing or "").strip()) and _parts(merged) == _parts(existing)


# ---------------------------------------------------------------------------
# localconfig.vdf editing
# ---------------------------------------------------------------------------

def _app_node(root: vdf.VdfNode, create: bool) -> Optional[vdf.VdfNode]:
    node = root
    for key in ("UserLocalConfigStore", "Software", "Valve", "Steam", "apps", APP_ID):
        child = node.get_node(key)
        if child is None:
            if not create:
                return None
            child = node.ensure_node(key)
        node = child
    return node


def read_launch_options(localconfig: Path) -> str:
    try:
        root = vdf.load(localconfig)
    except (OSError, vdf.VdfError) as error:
        raise InstallerError(f"Cannot read {localconfig}: {error}") from error
    app = _app_node(root, create=False)
    return (app.get("LaunchOptions") if app is not None else None) or ""


@dataclass
class LaunchOptionsChange:
    localconfig: Path
    before: str
    after: str
    backup: Optional[Path] = None
    notes: Tuple[str, ...] = ()

    @property
    def changed(self) -> bool:
        return self.before != self.after


def _state_path(state_dir: Path) -> Path:
    return Path(state_dir) / "launch_options_state.json"


def write_launch_options(localconfig: Path, new_value: str, *, steam_running: bool,
                         dry_run: bool = False, state_dir: Optional[Path] = None,
                         notes: Tuple[str, ...] = ()) -> LaunchOptionsChange:
    """Set app 550's launch options in ``localconfig``.

    Refuses while Steam is running (Steam would overwrite the change on exit).
    A timestamped copy of the original file is written next to it first.
    """
    localconfig = Path(localconfig)
    text = localconfig.read_text(encoding="utf-8", errors="surrogateescape")
    try:
        root = vdf.loads(text)
    except vdf.VdfError as error:
        raise InstallerError(f"Cannot parse {localconfig}: {error}") from error
    if root.get_node("UserLocalConfigStore") is None:
        raise InstallerError(f"{localconfig} is not a Steam localconfig.vdf file")
    app = _app_node(root, create=True)
    before = app.get("LaunchOptions") or ""
    change = LaunchOptionsChange(localconfig, before, new_value, notes=tuple(notes))
    if not change.changed or dry_run:
        return change
    if steam_running:
        raise InstallerError(
            "Steam is running. Close Steam completely (Steam > Exit) before writing launch "
            "options, or copy them into Properties > General > Launch options yourself."
        )
    if new_value:
        app.set("LaunchOptions", new_value)
    else:
        app.remove("LaunchOptions")
    stamp = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    backup = localconfig.with_name(f"{localconfig.name}.l4d2vr-{stamp}.bak")
    shutil.copy2(localconfig, backup)
    change.backup = backup
    atomic_write_text(localconfig, vdf.dumps(root, vdf.detect_newline(text)))
    if state_dir is not None:
        state = read_json(_state_path(state_dir), {}) or {}
        record = state.get(str(localconfig))
        # Remember the value from before the *first* installer change.
        if record is None or record.get("written") != before:
            state[str(localconfig)] = {"previous": before, "written": new_value}
        else:
            record["written"] = new_value
        atomic_write_json(_state_path(state_dir), state)
    return change


def apply_vr_launch_options(localconfig: Path, *, steam_running: bool, dry_run: bool = False,
                            state_dir: Optional[Path] = None) -> LaunchOptionsChange:
    before = read_launch_options(localconfig)
    merged, notes = merge_launch_options(before)
    return write_launch_options(localconfig, merged, steam_running=steam_running,
                                dry_run=dry_run, state_dir=state_dir, notes=tuple(notes))


def remove_vr_launch_options(localconfig: Path, *, steam_running: bool, dry_run: bool = False,
                             state_dir: Optional[Path] = None) -> LaunchOptionsChange:
    """Restore the launch options from before the installer changed them, or
    strip the VR additions if they were edited since."""
    current = read_launch_options(localconfig)
    target = strip_launch_options(current)
    if state_dir is not None:
        record = (read_json(_state_path(state_dir), {}) or {}).get(str(localconfig))
        if record and record.get("written") == current:
            target = record.get("previous", "")
    change = write_launch_options(localconfig, target, steam_running=steam_running,
                                  dry_run=dry_run, state_dir=None)
    if change.changed and not dry_run and state_dir is not None:
        state = read_json(_state_path(state_dir), {}) or {}
        state.pop(str(localconfig), None)
        atomic_write_json(_state_path(state_dir), state)
    return change
