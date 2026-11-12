"""Steam launch options for the Proton (Linux) route.

The mod needs Wine to load its own ``d3d9.dll`` instead of Proton's built-in
one, and the 32-bit game must use Proton's new WoW64 mode, which is also what
makes Proton's 32-bit OpenXR (wineopenxr) work: ``WINEDLLOVERRIDES=d3d9=n,b``,
``PROTON_USE_WOW64=1`` and the ``-dx9`` game argument.

Before the OpenXR-only release (1.3) the override also named ``openvr_api``
(``d3d9,openvr_api=n,b``) for the mod's own ``openvr_api.dll``. The release no
longer ships that DLL; old overrides are rewritten to the new one on apply and
still recognised when removing.

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
from .fsutil import InstallerError, atomic_write_text, read_json, read_text_raw, atomic_write_json

DLL_OVERRIDES = ("d3d9", "openvr_api")  # managed (openvr_api: pre-1.3 releases)
OVERRIDE = "d3d9=n,b"
REQUIRED_ENV = (("WINEDLLOVERRIDES", OVERRIDE), ("PROTON_USE_WOW64", "1"))
REQUIRED_ARGS = ("-dx9", "-insecure")  # -insecure: never VAC with the d3d9.dll proxy
HARMFUL_ARGS = ("-vulkan",)
RECOMMENDED = 'WINEDLLOVERRIDES="d3d9=n,b" PROTON_USE_WOW64=1 %command% -dx9 -insecure'

_ASSIGNMENT = re.compile(r"^([A-Za-z_][A-Za-z0-9_]*)=(.*)$", re.S)
_SAFE_VALUE = re.compile(r"^[A-Za-z0-9_./:@%+-]*$")
_UNQUOTED_SAFE = re.compile(r"^[A-Za-z0-9_./:@%+,;=-]*$")


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


@dataclass
class _Token:
    """One shell word of the user's launch options, kept as the raw text the
    user wrote (``raw``, re-emitted unchanged) and its unquoted value
    (``value``, only used to recognise the words the installer manages)."""

    gap: str  # whitespace before the word, kept as written
    raw: str
    value: str


def _tokens(text: str) -> List[_Token]:
    """Split ``text`` into shell words without re-quoting anything, so
    ``$VAR``, ``~``, ``;`` and the user's quoting survive a merge unchanged."""
    tokens: List[_Token] = []
    i, n = 0, len(text)
    while i < n:
        gap_start = i
        while i < n and text[i] in " \t\r\n":
            i += 1
        if i >= n:
            break
        gap, start, value = text[gap_start:i], i, []
        while i < n and text[i] not in " \t\r\n":
            ch = text[i]
            if ch == "\\":
                if i + 1 < n:
                    value.append(text[i + 1])
                i += 2
            elif ch == "'":
                close = text.find("'", i + 1)
                if close < 0:
                    raise InstallerError(
                        "Could not understand the existing launch options (No closing quotation). "
                        "Fix or clear them in Steam (Properties > General > Launch options) and retry.")
                value.append(text[i + 1:close])
                i = close + 1
            elif ch == '"':
                i += 1
                while i < n and text[i] != '"':
                    if text[i] == "\\" and i + 1 < n and text[i + 1] in '"\\$`\n':
                        i += 1
                    value.append(text[i])
                    i += 1
                if i >= n:
                    raise InstallerError(
                        "Could not understand the existing launch options (No closing quotation). "
                        "Fix or clear them in Steam (Properties > General > Launch options) and retry.")
                i += 1
            else:
                value.append(ch)
                i += 1
        tokens.append(_Token(gap, text[start:i], "".join(value)))
    return tokens


def _assignment(token: _Token):
    """``NAME=value`` match for a word that is a shell assignment (the name
    part written unquoted), else None."""
    name, sep, _ = token.raw.partition("=")
    if not sep or not re.match(r"^[A-Za-z_][A-Za-z0-9_]*$", name):
        return None
    return _ASSIGNMENT.match(token.value)


def _is_shell_syntax(token: _Token) -> bool:
    """A word that is (or holds) an unquoted shell operator."""
    return any(ch in token.raw for ch in ";&|<>") and not token.raw.startswith(("'", '"'))


def _render(tokens: List[_Token]) -> str:
    out = []
    for index, token in enumerate(tokens):
        gap = token.gap if index and token.gap else (" " if index else "")
        out.append(gap + token.raw)
    return "".join(out)


def _new_token(raw: str) -> _Token:
    return _Token(" ", raw, raw)


def _rewrite_assignment(token: _Token, transform) -> Optional[str]:
    """Return the raw text of ``NAME=<transform(value)>`` written in the same
    quoting style as the user's token (None: the transform emptied it)."""
    name, _, raw_value = token.raw.partition("=")
    match = _ASSIGNMENT.match(token.value)
    if raw_value[:1] in ('"', "'") and len(raw_value) >= 2 and raw_value[-1] == raw_value[0] \
            and raw_value[0] not in raw_value[1:-1]:
        quote, inner = raw_value[0], raw_value[1:-1]
    elif not any(c in raw_value for c in "\"'\\"):
        quote, inner = "", raw_value
    else:  # mixed quoting: fall back to a literal re-quote of the unquoted value
        value = transform(match.group(2))
        return _format_assignment(name, value) if value else None
    value = transform(inner)
    if not value:
        return None
    if not quote and not _UNQUOTED_SAFE.match(value):
        quote = '"'
    if not quote and ";" in value:
        quote = '"'
    return f"{name}={quote}{value}{quote}"


def _split_raw(existing: str) -> Tuple[Optional[str], str]:
    """``(prefix, suffix)`` raw text around ``%command%`` (prefix None when
    there is no ``%command%``: Steam then treats everything as game arguments)."""
    existing = (existing or "").strip()
    if "%command%" in existing:
        prefix, _, suffix = existing.partition("%command%")
        return prefix, suffix
    return None, existing


def _parts(existing: str) -> Tuple[List[str], List[str]]:
    prefix, suffix = _split_raw(existing)
    return (_split(prefix) if prefix is not None else []), _split(suffix)


def _assemble(prefix: List[_Token], suffix: List[_Token], suffix_glued: bool) -> str:
    head = _render(prefix)
    tail = _render(suffix)
    out = (head + " " if head else "") + "%command%"
    if tail:
        out += ("" if suffix_glued else " ") + tail
    return out


def _has_override(value: str) -> bool:
    for entry in filter(None, (part.strip() for part in value.split(";"))):
        names, _, mode = entry.partition("=")
        if mode.strip() == "n,b" and "d3d9" in [name.strip().lower() for name in names.split(",")]:
            return True
    return False


def merge_overrides(existing: str) -> str:
    """Add ``d3d9=n,b`` to a WINEDLLOVERRIDES value, keeping other entries
    (e.g. ``dxgi=n,b`` for ReShade) and dropping the old ``openvr_api`` one."""
    entries = [part.strip() for part in existing.split(";") if part.strip()]
    if _has_override(existing) and "openvr_api" not in existing.lower():
        return ";".join(entries)  # already right: keep the user's order
    kept = []
    for entry in entries:
        names, sep, mode = entry.partition("=")
        rest = [name for name in names.split(",") if name.strip() and name.strip().lower() not in DLL_OVERRIDES]
        if rest:
            kept.append(",".join(rest) + (sep + mode if sep else ""))
    kept.append(OVERRIDE)
    return ";".join(kept)


def strip_overrides(existing: str) -> str:
    kept = []
    for entry in filter(None, (part.strip() for part in existing.split(";"))):
        names, sep, mode = entry.partition("=")
        rest = [name for name in names.split(",") if name.strip() and name.strip().lower() not in DLL_OVERRIDES]
        if rest:
            kept.append(",".join(rest) + (sep + mode if sep else ""))
    return ";".join(kept)


def merge_launch_options(existing: str) -> Tuple[str, List[str]]:
    """Return ``(new_options, notes)`` with the VR requirements added.

    Only the words the installer manages (WINEDLLOVERRIDES, PROTON_USE_WOW64,
    -dx9, -insecure, -vulkan) are touched; every other part of the user's
    options (``$VAR`` references, ``~`` paths, wrappers such as gamemoderun,
    shell syntax after ``%command%``) is kept byte for byte.
    """
    if not (existing or "").strip():
        return RECOMMENDED, ["Launch options were empty; using the recommended set."]
    prefix_raw, suffix_raw = _split_raw(existing)
    prefix = _tokens(prefix_raw or "")
    suffix = _tokens(suffix_raw)
    notes: List[str] = []
    seen = set()
    for token in prefix:
        match = _assignment(token)
        if not match:
            continue
        if match.group(1) == "WINEDLLOVERRIDES":
            seen.add("WINEDLLOVERRIDES")
            if "openvr_api" in match.group(2).lower():
                notes.append("Dropped openvr_api from WINEDLLOVERRIDES: the OpenXR release has no openvr_api.dll.")
            if not (_has_override(match.group(2)) and "openvr_api" not in match.group(2).lower()):
                token.raw = _rewrite_assignment(token, merge_overrides) or f'WINEDLLOVERRIDES="{OVERRIDE}"'
        elif match.group(1) == "PROTON_USE_WOW64":
            seen.add("PROTON_USE_WOW64")
            if match.group(2) != "1":
                token.raw = "PROTON_USE_WOW64=1"
    # Environment assignments must come before wrapper commands.
    insert_at = 0
    while insert_at < len(prefix) and _assignment(prefix[insert_at]):
        insert_at += 1
    for name, value in REQUIRED_ENV:
        if name not in seen:
            prefix.insert(insert_at, _new_token(f'{name}="{value}"' if "," in value else f"{name}={value}"))
            insert_at += 1
    kept_suffix = []
    for token in suffix:
        if token.value.lower() in HARMFUL_ARGS:
            notes.append(f"Removed {token.value}: the mod uses its own DX9/DXVK renderer.")
            continue
        kept_suffix.append(token)
    present = {token.value.lower() for token in kept_suffix}
    # Game arguments go before shell syntax such as "; echo done" or "| tee log".
    insert_at = next((index for index, token in enumerate(kept_suffix) if _is_shell_syntax(token)),
                     len(kept_suffix))
    for arg in REQUIRED_ARGS:
        if arg not in present:
            kept_suffix.insert(insert_at, _new_token(arg))
            insert_at += 1
    # "%command%;..." stays glued as written.
    glued = prefix_raw is not None and bool(suffix) and not suffix_raw[:1].isspace() \
        and kept_suffix[0] is suffix[0]
    return _assemble(prefix, kept_suffix, glued), notes


def strip_launch_options(existing: str) -> str:
    """Remove what :func:`merge_launch_options` adds; keep everything else
    exactly as written."""
    if not (existing or "").strip():
        return ""
    prefix_raw, suffix_raw = _split_raw(existing)
    prefix = []
    for token in _tokens(prefix_raw or ""):
        match = _assignment(token)
        if match and match.group(1) == "WINEDLLOVERRIDES":
            rewritten = _rewrite_assignment(token, strip_overrides)
            if rewritten:
                token.raw = rewritten
                prefix.append(token)
        elif match and match.group(1) == "PROTON_USE_WOW64":
            continue
        else:
            prefix.append(token)
    suffix = [token for token in _tokens(suffix_raw) if token.value.lower() not in REQUIRED_ARGS]
    if not prefix and not suffix:
        return ""
    if not prefix and prefix_raw is not None and any(_is_shell_syntax(token) for token in suffix):
        return _assemble([], suffix, not suffix_raw[:1].isspace())
    if not prefix:
        return _render(suffix)
    return _assemble(prefix, suffix, False)


def has_required_options(existing: str) -> bool:
    try:
        merged, _ = merge_launch_options(existing)
    except InstallerError:
        return False
    if not (existing or "").strip():
        return False
    if merged == existing.strip():
        return True
    try:
        return _parts(merged) == _parts(existing)
    except InstallerError:
        return False


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
    text = read_text_raw(localconfig)
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
