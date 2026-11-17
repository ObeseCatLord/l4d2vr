"""Play without the mod (VAC-safe launch), installer 1.1.5.

The VR mod is a ``d3d9.dll`` that hooks the game, so it always needs the
``-insecure`` launch option and can never be used on VAC-secured servers.
This module starts Left 4 Dead 2 with the mod NOT loaded and without
``-insecure``, so the player can join VAC-secured servers, and puts the mod
back for the next VR launch. It never hides or disguises the mod.

Linux / Proton
    The Steam launch options are switched once (Steam closed, backed up) to
    run a small wrapper script in front of ``%command%``. The wrapper reads a
    one-shot mode file: ``vr`` (default) leaves the VR options alone;
    ``vanilla`` (written by "Play without the mod", valid for 10 minutes and
    reset to ``vr`` by the launch that uses it) sets
    ``WINEDLLOVERRIDES=d3d9=b`` (Wine's own d3d9, so the mod's ``d3d9.dll`` is
    never loaded) and removes ``-insecure`` from the game arguments. Nothing
    is renamed, so a crash leaves nothing to undo.

Windows
    The mod's files in the game folder (``d3d9.dll``, ``openxr_loader.dll``)
    are renamed to ``<name>.l4d2vr-off`` and the game is started through Steam.
    A state file records every rename before it happens; the files are put
    back by the next VR launch from the installer, by "Restore the mod", and
    at installer start-up (also after a crash). The installer never edits the
    saved Steam launch options on Windows: if they contain ``-insecure``, the
    player is told to remove it themselves.
"""

from __future__ import annotations

import os
import shlex
import stat
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, List, Optional

from . import APP_ID
from .fsutil import InstallerError, atomic_write_bytes, atomic_write_json, read_json, resolve_ci

Log = Callable[[str], None]

WRAPPER_NAME = "l4d2vr-steam-wrapper.sh"
MODE_NAME = "launch-mode"
STATE_NAME = "vanilla-launch.json"
OFF_SUFFIX = ".l4d2vr-off"
# The mod files that make the game load the mod (Windows). Everything else the
# mod installs only matters once d3d9.dll is loaded.
WINDOWS_MOD_FILES = ("d3d9.dll", "openxr_loader.dll")
VANILLA_WINDOW_SECONDS = 600

EXPLANATION = (
    "The VR mod hooks the game (it is a d3d9.dll loaded into Left 4 Dead 2), so it always needs the "
    "-insecure launch option and can never be used on VAC-secured servers. 'Play without the mod' starts "
    "the normal game with the mod not loaded and without -insecure, so you can join VAC-secured servers; "
    "the next VR launch from the installer uses the mod again. Classic mode (the mod's simple controls) "
    "works on servers without the L4D2VR plugin that allow -insecure clients."
)


def wrapper_script() -> str:
    """The POSIX shell wrapper Steam runs in front of ``%command%`` (Linux)."""
    return f"""#!/bin/sh
# L4D2VR Steam launch wrapper (written by the L4D2VR installer).
# "vr" (default): start the game as the launch options say (the VR mod loads).
# "vanilla <unix time>" in {MODE_NAME}, written by "Play without the mod" and
# used once within {VANILLA_WINDOW_SECONDS} s: start the game WITHOUT the mod (Wine's builtin d3d9)
# and without -insecure, so VAC-secured servers can be joined.
dir=$(dirname "$0")
mode_file="$dir/{MODE_NAME}"
mode=vr
if [ -f "$mode_file" ]; then
    read -r saved_mode saved_time < "$mode_file" || true
    now=$(date +%s)
    if [ "$saved_mode" = "vanilla" ] && [ -n "$saved_time" ] && \\
       [ "$now" -ge "$saved_time" ] 2>/dev/null && [ $((now - saved_time)) -le {VANILLA_WINDOW_SECONDS} ] 2>/dev/null; then
        mode=vanilla
    fi
    # One launch only: the next start is VR again.
    printf 'vr\\n' > "$mode_file"
fi
if [ "$mode" = "vanilla" ]; then
    overrides=$(printf '%s' "${{WINEDLLOVERRIDES:-}}" | tr ';' '\\n' | grep -v -i -E '(^|,)[[:space:]]*d3d9[[:space:]]*(,|=|$)' | tr '\\n' ';')
    WINEDLLOVERRIDES="${{overrides}}d3d9=b"
    export WINEDLLOVERRIDES
    count=$#
    i=0
    while [ "$i" -lt "$count" ]; do
        arg=$1
        shift
        if [ "$arg" != "-insecure" ]; then
            set -- "$@" "$arg"
        fi
        i=$((i + 1))
    done
    echo "L4D2VR: starting Left 4 Dead 2 without the mod (VAC-safe launch)" >&2
fi
exec "$@"
"""


def wrapper_path(config_dir: Path) -> Path:
    return Path(config_dir) / WRAPPER_NAME


def install_wrapper(config_dir: Path, dry_run: bool = False) -> Path:
    path = wrapper_path(config_dir)
    if not dry_run:
        Path(config_dir).mkdir(parents=True, exist_ok=True)
        atomic_write_bytes(path, wrapper_script().encode("utf-8"), executable=True)
        try:
            path.chmod(path.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
        except OSError:
            pass
    return path


def is_wrapper_token(value: str) -> bool:
    return os.path.basename(value) == WRAPPER_NAME


def with_wrapper(options: str, wrapper: Path) -> str:
    """Put the wrapper right in front of ``%command%`` (after the environment
    assignments and any other wrapper the player uses). Idempotent."""
    options = (options or "").strip()
    token = shlex.quote(str(wrapper))
    if "%command%" not in options:
        options = "%command% " + options if options else "%command%"
    prefix, _, suffix = options.partition("%command%")
    words = shlex.split(prefix) if prefix.strip() else []
    if any(is_wrapper_token(word) for word in words):
        return options
    head = prefix.rstrip()
    return ((head + " ") if head else "") + token + " %command%" + suffix


def without_wrapper(options: str) -> str:
    if "%command%" not in (options or ""):
        return options or ""
    prefix, _, suffix = options.partition("%command%")
    kept = [word for word in prefix.split() if not is_wrapper_token(word.strip("'\""))]
    head = " ".join(kept)
    return ((head + " ") if head else "") + "%command%" + suffix


def has_wrapper(options: str) -> bool:
    if "%command%" not in (options or ""):
        return False
    prefix = options.partition("%command%")[0]
    try:
        words = shlex.split(prefix)
    except ValueError:
        return False
    return any(is_wrapper_token(word) for word in words)


def has_insecure(options: str) -> bool:
    """``-insecure`` among the game arguments of saved launch options."""
    suffix = options.partition("%command%")[2] if "%command%" in (options or "") else (options or "")
    try:
        return "-insecure" in shlex.split(suffix)
    except ValueError:
        return "-insecure" in suffix.split()


def write_mode(config_dir: Path, mode: str, now: Optional[float] = None) -> Path:
    path = Path(config_dir) / MODE_NAME
    Path(config_dir).mkdir(parents=True, exist_ok=True)
    text = "vr\n" if mode != "vanilla" else f"vanilla {int(now if now is not None else time.time())}\n"
    atomic_write_bytes(path, text.encode("ascii"))
    return path


def read_mode(config_dir: Path, now: Optional[float] = None) -> str:
    """The mode the wrapper would use now (``vr`` or ``vanilla``)."""
    try:
        words = (Path(config_dir) / MODE_NAME).read_text(encoding="ascii").split()
    except (OSError, UnicodeError):
        return "vr"
    if len(words) >= 2 and words[0] == "vanilla":
        try:
            age = (now if now is not None else time.time()) - int(words[1])
        except ValueError:
            return "vr"
        if 0 <= age <= VANILLA_WINDOW_SECONDS:
            return "vanilla"
    return "vr"


# ---------------------------------------------------------------------------
# Windows: rename the mod files away and back
# ---------------------------------------------------------------------------

def _state_path(state_dir: Path) -> Path:
    return Path(state_dir) / STATE_NAME


@dataclass
class RestoreResult:
    restored: List[str] = field(default_factory=list)
    conflicts: List[str] = field(default_factory=list)
    missing: List[str] = field(default_factory=list)

    @property
    def complete(self) -> bool:
        return not self.conflicts


def pending_restore(state_dir: Path) -> Optional[dict]:
    state = read_json(_state_path(state_dir), None)
    return state if isinstance(state, dict) and state.get("renamed") else None


def disable_mod_files(game_dir: Path, state_dir: Path, *, dry_run: bool = False, log: Log = print) -> List[str]:
    """Rename the mod's files in ``game_dir`` to ``<name>.l4d2vr-off``. The
    state file lists every rename before it is made, so a crash in between is
    put right by :func:`restore_mod_files`."""
    game_dir = Path(game_dir)
    state = pending_restore(state_dir) or {"game_dir": str(game_dir), "renamed": [], "started": int(time.time())}
    if os.path.normcase(str(state.get("game_dir", ""))) != os.path.normcase(str(game_dir)):
        raise InstallerError("Mod files of another game folder are still switched off: restore them first "
                             f"({state.get('game_dir')}).")
    done = []
    for name in WINDOWS_MOD_FILES:
        source = resolve_ci(game_dir, name)
        if not source.is_file():
            continue
        target = source.with_name(source.name + OFF_SUFFIX)
        if target.exists():
            raise InstallerError(f"{target} already exists: restore the mod first, or remove that file.")
        rel = source.name
        if dry_run:
            log(f"Would switch off {rel}")
            done.append(rel)
            continue
        if rel not in state["renamed"]:
            state["renamed"].append(rel)
            atomic_write_json(_state_path(state_dir), state)
        os.replace(source, target)
        done.append(rel)
        log(f"Switched off {rel} (renamed to {target.name})")
    return done


def restore_mod_files(state_dir: Path, *, dry_run: bool = False, log: Log = print) -> RestoreResult:
    """Put back every file :func:`disable_mod_files` renamed. A file that
    exists under both names is left alone (and reported) so nothing is lost."""
    result = RestoreResult()
    state = pending_restore(state_dir)
    if not state:
        return result
    game_dir = Path(state.get("game_dir", ""))
    remaining = []
    for rel in state.get("renamed", []):
        live = game_dir / rel
        off = game_dir / (rel + OFF_SUFFIX)
        if off.exists() and not live.exists():
            if not dry_run:
                os.replace(off, live)
            result.restored.append(rel)
            log(f"Restored {rel}")
        elif off.exists() and live.exists():
            result.conflicts.append(rel)
            remaining.append(rel)
            log(f"Both {rel} and {rel + OFF_SUFFIX} exist in {game_dir}: kept both. Delete the one you do not "
                f"want (the mod's file is {rel + OFF_SUFFIX} unless you reinstalled since), then restore again.")
        elif live.exists():
            result.restored.append(rel)  # already back (restored by hand or by a reinstall)
        else:
            result.missing.append(rel)
            log(f"{rel} is missing from {game_dir}: reinstall the mod (Install / Update with Repair).")
    if dry_run:
        return result
    if remaining:
        state["renamed"] = remaining
        atomic_write_json(_state_path(state_dir), state)
    else:
        try:
            _state_path(state_dir).unlink()
        except FileNotFoundError:
            pass
    return result


# ---------------------------------------------------------------------------
# The action
# ---------------------------------------------------------------------------

def steam_url() -> str:
    """No arguments at all: in particular no -insecure."""
    return f"steam://rungameid/{APP_ID}"


def play_vanilla(game_dir: Path, *, windows: bool, state_dir: Path, steam_running: Callable[[], bool],
                 game_running: Callable[[], bool], open_url: Callable[[str], object],
                 localconfigs: List[Path], read_options: Callable[[Path], str],
                 write_options: Callable[[Path, str], object], merge_vr_options: Callable[[str], str],
                 dry_run: bool = False, log: Log = print) -> None:
    """Start Left 4 Dead 2 without the mod (see the module docstring)."""
    if game_running():
        raise InstallerError("Left 4 Dead 2 is running. Quit the game first.")
    log(EXPLANATION)
    if windows:
        for localconfig in localconfigs[:1]:
            try:
                if has_insecure(read_options(localconfig)):
                    log("Warning: your Steam launch options for Left 4 Dead 2 contain -insecure, so this start "
                        "is still insecure and cannot join VAC-secured servers. Remove -insecure yourself in "
                        "Steam > Left 4 Dead 2 > Properties > General > Launch options (the installer adds it "
                        "to its own VR launches, so VR keeps working).")
            except InstallerError as error:
                log(f"Note: could not read the Steam launch options ({error}).")
        renamed = disable_mod_files(game_dir, state_dir, dry_run=dry_run, log=log)
        if not renamed:
            log("No mod files were found in the game folder: the game starts without the mod anyway.")
    else:
        if not localconfigs:
            raise InstallerError("No Steam user with launch options was found for Left 4 Dead 2.")
        localconfig = localconfigs[0]
        current = read_options(localconfig)
        wrapper = install_wrapper(state_dir, dry_run=dry_run)
        if not has_wrapper(current):
            if steam_running():
                raise InstallerError(
                    "The VAC-safe launch needs a one-time change of the Steam launch options (a small wrapper "
                    "script in front of %command%; your options are backed up). Exit Steam completely "
                    "(Steam > Exit), press this again, then start Steam.")
            new_options = with_wrapper(merge_vr_options(current), wrapper)
            write_options(localconfig, new_options)
            log(f"Steam launch options set up for the VAC-safe launch: {new_options}")
            if not dry_run:
                log("Start Steam, then press 'Play without the mod' again.")
                return
        if not dry_run:
            write_mode(state_dir, "vanilla")
        log("The next start of Left 4 Dead 2 (within 10 minutes) runs without the mod and without -insecure.")
    url = steam_url()
    if not dry_run:
        open_url(url)
    log(f"Asked Steam to start Left 4 Dead 2 without the mod ({url}).")


def prepare_vr_launch(*, windows: bool, state_dir: Path, game_running: Callable[[], bool],
                      dry_run: bool = False, log: Log = print) -> None:
    """Before a VR launch: put the mod back (Windows) / clear a pending vanilla start (Linux)."""
    if windows:
        if pending_restore(state_dir):
            if game_running():
                raise InstallerError("Left 4 Dead 2 is running without the mod. Quit it, then launch VR.")
            result = restore_mod_files(state_dir, dry_run=dry_run, log=log)
            if result.conflicts:
                raise InstallerError("Could not put the mod's files back (see the log); VR would not start.")
    elif not dry_run and (Path(state_dir) / MODE_NAME).exists():
        write_mode(state_dir, "vr")


def auto_restore(state_dir: Path, game_running: Callable[[], bool], log: Log = print) -> Optional[RestoreResult]:
    """Installer start-up: undo a VAC-safe launch once the game has closed
    (also after a crash or a reboot in between)."""
    if not pending_restore(state_dir) or game_running():
        return None
    log("Putting the mod back after a launch without it ...")
    return restore_mod_files(state_dir, log=log)
