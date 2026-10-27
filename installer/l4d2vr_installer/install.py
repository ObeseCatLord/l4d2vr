"""Install, update, restore and uninstall the mod files.

Design rules:

* Every file that will be overwritten or removed is copied to
  ``<root>/.l4d2vr-backups/<timestamp>/files/`` and listed in that backup's
  ``index.json`` *before* the first game file changes.
* User files (configs, calibration) matching the preserve patterns are never
  overwritten once they exist.
* Restore/uninstall only touch files the installer itself recorded, and only
  when their content still matches what the installer wrote.
* All writes are atomic (temporary file + rename).
"""

from __future__ import annotations

import datetime as dt
import fnmatch
import json
import os
import shutil
import stat
import zipfile
from dataclasses import dataclass, field
from pathlib import Path, PurePosixPath
from typing import Callable, Dict, List, Optional, Tuple

from . import __version__
from .fsutil import (
    InstallerError, atomic_write_bytes, atomic_write_json, normalize_member, read_json,
    resolve_ci, sha256_bytes, sha256_file,
)

BACKUP_DIR = ".l4d2vr-backups"
STATE_SCHEMA = 1
INDEX_SCHEMA = 1
MAX_PACKAGE_BYTES = 4 << 30
PIN_FILE = "vr/disable_workshop_updates.txt"
PIN_TEXT = (
    "Created by the L4D2VR installer.\n"
    "While this file exists the mod does not offer to replace itself with the Steam Workshop\n"
    "build. Use the installer to update instead. Delete this file to re-enable Workshop updates.\n"
)

CLIENT_PRESERVE = (
    "vr/config.txt",
    "vr/config2.txt",
    "vr/config_migrations_applied.txt",
    "viewmodel_adjustments.txt",
    "viewmodel_auto_grip_cache.txt",
    "scope_adjustments.txt",
    "vr/viewmodel_adjustments.txt",
    "vr/viewmodel_auto_grip_cache.txt",
    "vr/scope_adjustments.txt",
    "vr/*adjustments*.txt",
    "vr/*calibration*",
)
SERVER_PRESERVE = ("*l4d2vr_server_config.txt", "cfg/*")

Log = Callable[[str], None]


def _noop(_message: str) -> None:
    pass


# ---------------------------------------------------------------------------
# Targets
# ---------------------------------------------------------------------------

@dataclass
class Target:
    """Where a package goes and which rules apply."""

    root: Path
    kind: str  # "client" or "server"
    state_rel: str
    preserve: List[str]
    required: Tuple[str, ...]  # fnmatch patterns; each must match a package file
    strip_prefixes: Tuple[str, ...] = ()
    blocked: Tuple[str, ...] = ()

    @property
    def backup_root(self) -> Path:
        return self.root / BACKUP_DIR

    @property
    def state_path(self) -> Path:
        return resolve_ci(self.root, self.state_rel)

    def is_preserved(self, rel: str) -> bool:
        lowered = rel.lower()
        return any(fnmatch.fnmatchcase(lowered, pattern.lower()) for pattern in self.preserve)

    def read_state(self) -> Optional[dict]:
        state = read_json(self.state_path)
        if state is not None and state.get("schema") != STATE_SCHEMA:
            raise InstallerError(f"{self.state_path} was written by an incompatible installer version")
        return state


def client_target(game_dir: Path, extra_preserve: Optional[List[str]] = None) -> Target:
    game_dir = Path(game_dir)
    preserve = list(CLIENT_PRESERVE)
    state = read_json(resolve_ci(game_dir, "vr/installed_release.json")) or {}
    preserve += [item for item in state.get("user_preserve", []) if item not in preserve]
    preserve += [item for item in (extra_preserve or []) if item not in preserve]
    return Target(game_dir, "client", "vr/installed_release.json", preserve,
                  required=("d3d9.dll",), blocked=("left4dead2.exe", "bin/*"))


def server_target(server_dir: Path, extra_preserve: Optional[List[str]] = None) -> Target:
    preserve = list(SERVER_PRESERVE) + list(extra_preserve or [])
    return Target(Path(server_dir), "server", "addons/l4d2vr_installed_release.json", preserve,
                  required=("addons/*.vdf",), strip_prefixes=("left4dead2/",),
                  blocked=("gameinfo.txt", "*.vpk"))


# ---------------------------------------------------------------------------
# Packages
# ---------------------------------------------------------------------------

@dataclass
class Package:
    path: Path
    sha256: str
    members: Dict[str, zipfile.ZipInfo]  # normalized relative path -> member
    mod_version: Optional[str] = None

    def read(self, rel: str) -> bytes:
        with zipfile.ZipFile(self.path) as archive:
            return archive.read(self.members[rel])


def _matches_all(names, patterns) -> bool:
    return all(any(fnmatch.fnmatchcase(name.lower(), pattern.lower()) for name in names) for pattern in patterns)


def open_package(zip_path: Path, target: Target) -> Package:
    """Validate an archive for ``target`` and index its files."""
    zip_path = Path(zip_path)
    if not zip_path.is_file():
        raise InstallerError(f"Archive not found: {zip_path}")
    try:
        archive = zipfile.ZipFile(zip_path)
    except zipfile.BadZipFile as error:
        raise InstallerError(f"{zip_path.name} is not a valid zip file ({error}). Download it again.") from error
    with archive:
        raw: Dict[str, zipfile.ZipInfo] = {}
        seen = set()
        total = 0
        for member in archive.infolist():
            if member.is_dir():
                continue
            if stat.S_ISLNK(member.external_attr >> 16):
                raise InstallerError(f"Archive contains a symbolic link, refusing: {member.filename}")
            rel = normalize_member(member.filename)
            if rel.lower() in seen:  # vr/X and VR/X are the same file on Windows
                raise InstallerError(f"Archive contains {rel} twice")
            seen.add(rel.lower())
            raw[rel] = member
            total += member.file_size
        if total > MAX_PACKAGE_BYTES:
            raise InstallerError("Archive is unreasonably large; refusing to extract it")
        members = _strip_wrapper(raw, target)
        for rel in members:
            first = rel.split("/", 1)[0].lower()
            if first == BACKUP_DIR or rel.lower() == target.state_rel.lower():
                raise InstallerError(f"Archive contains a reserved path: {rel}")
            if any(fnmatch.fnmatchcase(rel.lower(), pattern) for pattern in target.blocked):
                raise InstallerError(f"Archive would replace a core game file ({rel}); refusing")
        mod_version = None
        if "manifest.json" in members:
            try:
                manifest = json.loads(archive.read(members["manifest.json"]).decode("utf-8-sig"))
                mod_version = str(manifest.get("version")) if isinstance(manifest, dict) and manifest.get("version") else None
            except (ValueError, UnicodeDecodeError):
                mod_version = None
        if mod_version is None and "vr/version.txt" in members:
            mod_version = archive.read(members["vr/version.txt"]).decode("utf-8", "ignore").strip() or None
        bad = archive.testzip()
    if bad:
        raise InstallerError(f"Archive member {bad} is corrupt (CRC mismatch). Download it again.")
    return Package(zip_path, sha256_file(zip_path), members, mod_version)


def _strip_wrapper(raw: Dict[str, zipfile.ZipInfo], target: Target) -> Dict[str, zipfile.ZipInfo]:
    """Find the package root: as-is, after a known prefix such as
    ``left4dead2/`` (server packages), or inside a single wrapper folder."""
    candidates = [""] + list(target.strip_prefixes)
    tops = {rel.split("/", 1)[0] for rel in raw if "/" in rel}
    if len(tops) == 1 and all("/" in rel for rel in raw):
        top = tops.pop() + "/"
        candidates += [top] + [top + prefix for prefix in target.strip_prefixes]
    for prefix in candidates:
        if prefix and not all(rel.lower().startswith(prefix.lower()) for rel in raw):
            continue
        stripped = {rel[len(prefix):]: member for rel, member in raw.items()}
        if _matches_all(stripped, target.required):
            return stripped
    raise InstallerError(
        "This archive does not look like a L4D2VR "
        + ("client package (d3d9.dll missing). Did you pick the server zip?" if target.kind == "client"
           else "server plugin package (addons/*.vdf missing). Did you pick the client zip?")
    )


# ---------------------------------------------------------------------------
# Planning
# ---------------------------------------------------------------------------

@dataclass
class PlanItem:
    rel: str  # path inside the package (normalized)
    dest: Path
    action: str  # add | overwrite | unchanged | preserve | remove | keep-modified
    new_sha: Optional[str] = None
    old_sha: Optional[str] = None
    data: Optional[bytes] = None  # generated content (not from the archive)


@dataclass
class Plan:
    target: Target
    package: Optional[Package]
    items: List[PlanItem] = field(default_factory=list)
    warnings: List[str] = field(default_factory=list)
    metadata: dict = field(default_factory=dict)

    def by_action(self, action: str) -> List[PlanItem]:
        return [item for item in self.items if item.action == action]

    @property
    def changes(self) -> List[PlanItem]:
        return [item for item in self.items if item.action in ("add", "overwrite", "remove")]

    def summary(self) -> str:
        counts = {}
        for item in self.items:
            counts[item.action] = counts.get(item.action, 0) + 1
        order = ("add", "overwrite", "remove", "unchanged", "preserve", "keep-modified")
        return ", ".join(f"{counts[name]} {name}" for name in order if counts.get(name))


def _member_sha(package: Package, rel: str) -> str:
    return sha256_bytes(package.read(rel))


def _check_dest(dest: Path) -> None:
    if dest.is_symlink():
        raise InstallerError(f"Refusing to write through a symbolic link: {dest}")
    if dest.exists() and not dest.is_file():
        raise InstallerError(f"Expected a file but found a folder: {dest}")


def plan_install(target: Target, package: Package, metadata: Optional[dict] = None,
                 pin_workshop: bool = True) -> Plan:
    previous = target.read_state() or {}
    plan = Plan(target, package, metadata=dict(metadata or {}))
    seen_dest = set()
    for rel in sorted(package.members):
        dest = resolve_ci(target.root, rel)
        _check_dest(dest)
        seen_dest.add(os.path.normcase(str(dest)))
        new_sha = _member_sha(package, rel)
        exists = dest.is_file()
        old_sha = sha256_file(dest) if exists else None
        if exists and target.is_preserved(rel):
            action = "preserve"
        elif not exists:
            action = "add"
        elif old_sha == new_sha:
            action = "unchanged"
        else:
            action = "overwrite"
        plan.items.append(PlanItem(rel, dest, action, new_sha, old_sha))

    if target.kind == "client" and pin_workshop:
        dest = resolve_ci(target.root, PIN_FILE)
        if os.path.normcase(str(dest)) not in seen_dest:
            _check_dest(dest)
            data = PIN_TEXT.encode()
            seen_dest.add(os.path.normcase(str(dest)))
            if dest.is_file():
                plan.items.append(PlanItem(PIN_FILE, dest, "unchanged", sha256_file(dest), sha256_file(dest)))
            else:
                plan.items.append(PlanItem(PIN_FILE, dest, "add", sha256_bytes(data), None, data))

    # Files the previous install wrote that the new package no longer ships.
    for rel, recorded in sorted((previous.get("files") or {}).items()):
        dest = resolve_ci(target.root, rel)
        if os.path.normcase(str(dest)) in seen_dest or not dest.is_file() or target.is_preserved(rel):
            continue
        current = sha256_file(dest)
        if current == recorded:
            plan.items.append(PlanItem(rel, dest, "remove", None, current))
        else:
            plan.items.append(PlanItem(rel, dest, "keep-modified", None, current))
            plan.warnings.append(f"{rel} is no longer part of the mod but you changed it; leaving it in place.")
    plan.metadata.setdefault("previous_version", previous.get("version"))
    plan.metadata.setdefault("previous_tag", previous.get("tag"))
    return plan


# ---------------------------------------------------------------------------
# Backups
# ---------------------------------------------------------------------------

def _relative(root: Path, path: Path) -> str:
    return PurePosixPath(*Path(os.path.abspath(path)).relative_to(os.path.abspath(root)).parts).as_posix()


def _new_backup_dir(target: Target) -> Path:
    stamp = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    candidate = target.backup_root / stamp
    counter = 1
    while candidate.exists():
        counter += 1
        candidate = target.backup_root / f"{stamp}-{counter}"
    return candidate


@dataclass
class Backup:
    path: Path
    index: dict

    @property
    def name(self) -> str:
        return self.path.name

    @property
    def restored(self) -> bool:
        return bool(self.index.get("restored_at"))

    def describe(self) -> str:
        index = self.index
        before = index.get("version_before") or "nothing"
        after = index.get("version_after") or "?"
        flag = " (already restored)" if self.restored else ""
        return (f"{self.name}: {index.get('operation', 'install')} {before} -> {after}, "
                f"{len(index.get('entries', []))} file(s){flag}")


def list_backups(target: Target, include_restored: bool = True) -> List[Backup]:
    out = []
    root = target.backup_root
    if not root.is_dir():
        return out
    for child in sorted(root.iterdir(), reverse=True):
        index_path = child / "index.json"
        if not child.is_dir() or not index_path.is_file():
            continue
        try:
            index = read_json(index_path)
        except InstallerError:
            continue
        if not isinstance(index, dict) or index.get("schema") != INDEX_SCHEMA or index.get("kind") != target.kind:
            continue
        backup = Backup(child, index)
        if include_restored or not backup.restored:
            out.append(backup)
    return out


# ---------------------------------------------------------------------------
# Applying an install
# ---------------------------------------------------------------------------

def _state_document(plan: Plan, backup_name: Optional[str]) -> dict:
    target, package = plan.target, plan.package
    previous = target.read_state() or {}
    # Preserved files only count as ours when this run created them (e.g. a
    # default config on first install); after that they belong to the user.
    files = {item.rel: item.new_sha for item in plan.items
             if item.action in ("add", "overwrite", "unchanged")}
    meta = plan.metadata
    backups = list(previous.get("backups") or [])
    if backup_name:
        backups.append(backup_name)
    return {
        "schema": STATE_SCHEMA,
        "kind": target.kind,
        "installer_version": __version__,
        "installed_at": dt.datetime.now(dt.timezone.utc).isoformat(timespec="seconds"),
        "source": meta.get("source", "local"),
        "repo": meta.get("repo"),
        "channel": meta.get("channel"),
        "tag": meta.get("tag"),
        "version": meta.get("version") or (package.mod_version if package else None),
        "mod_version": package.mod_version if package else None,
        "asset": meta.get("asset") or (package.path.name if package else None),
        "archive_sha256": package.sha256 if package else None,
        "workshop_updates_pinned": any(item.rel == PIN_FILE for item in plan.items),
        "user_preserve": [p for p in target.preserve if p not in CLIENT_PRESERVE and p not in SERVER_PRESERVE],
        "preserved": [item.rel for item in plan.items if item.action == "preserve"],
        "files": files,
        "backups": backups,
    }


def apply_plan(plan: Plan, dry_run: bool = False, log: Log = _noop) -> Optional[Path]:
    """Carry out ``plan``. Returns the backup folder (None for a dry run or
    when nothing had to be backed up or changed)."""
    target = plan.target
    for warning in plan.warnings:
        log("Warning: " + warning)
    log(f"Plan for {target.root}: {plan.summary() or 'nothing to do'}")
    for item in plan.changes:
        log(f"  {item.action:9} {item.rel}")
    for item in plan.by_action("preserve"):
        log(f"  keep      {item.rel} (your file)")
    if dry_run:
        log("Dry run: no files were changed.")
        return None

    backup_dir = _new_backup_dir(target)
    state_dest = target.state_path
    state_existed = state_dest.is_file()
    index = {
        "schema": INDEX_SCHEMA,
        "kind": target.kind,
        "operation": "update" if plan.metadata.get("previous_version") or state_existed else "install",
        "created_at": dt.datetime.now(dt.timezone.utc).isoformat(timespec="seconds"),
        "installer_version": __version__,
        "version_before": plan.metadata.get("previous_version"),
        "version_after": plan.metadata.get("version") or (plan.package.mod_version if plan.package else None),
        "status": "in-progress",
        "entries": [],
        "created_dirs": [],
    }

    # 1. Copy everything that will be overwritten or removed, then write the index.
    files_dir = backup_dir / "files"
    for item in plan.items:
        if item.action not in ("overwrite", "remove"):
            continue
        rel_disk = _relative(target.root, item.dest)
        copy = files_dir / rel_disk
        copy.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(item.dest, copy)
        if sha256_file(copy) != item.old_sha:
            raise InstallerError(f"Backup copy of {rel_disk} does not match the original; aborting before any change")
        index["entries"].append({
            "path": rel_disk,
            "action": "overwritten" if item.action == "overwrite" else "removed",
            "original_sha256": item.old_sha,
            "installed_sha256": item.new_sha,
        })
    for item in plan.items:
        if item.action == "add":
            index["entries"].append({
                "path": _relative(target.root, item.dest), "action": "added",
                "original_sha256": None, "installed_sha256": item.new_sha,
            })

    # The state file is an installed file like any other, so restoring a
    # backup also rolls the recorded version back.
    state = _state_document(plan, backup_dir.name)
    state_bytes = (json.dumps(state, indent=2) + "\n").encode()
    state_rel_disk = _relative(target.root, state_dest)
    if state_existed:
        copy = files_dir / state_rel_disk
        copy.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(state_dest, copy)
        index["entries"].append({"path": state_rel_disk, "action": "overwritten",
                                 "original_sha256": sha256_file(state_dest),
                                 "installed_sha256": sha256_bytes(state_bytes)})
    else:
        index["entries"].append({"path": state_rel_disk, "action": "added", "original_sha256": None,
                                 "installed_sha256": sha256_bytes(state_bytes)})

    created_dirs = set()
    for item in plan.items + [PlanItem(target.state_rel, state_dest, "add")]:
        if item.action != "add":
            continue
        parent = item.dest.parent
        while not parent.exists() and parent != target.root:
            created_dirs.add(_relative(target.root, parent))
            parent = parent.parent
    index["created_dirs"] = sorted(created_dirs, key=lambda p: -p.count("/"))
    backup_dir.mkdir(parents=True, exist_ok=True)
    atomic_write_json(backup_dir / "index.json", index)
    log(f"Backup: {backup_dir}")

    # 2. Change the game files; roll back automatically on failure.
    try:
        for item in plan.items:
            if item.action in ("add", "overwrite"):
                data = item.data if item.data is not None else plan.package.read(item.rel)
                if sha256_bytes(data) != item.new_sha:
                    raise InstallerError(f"Archive content changed while installing {item.rel}")
                atomic_write_bytes(item.dest, data)
            elif item.action == "remove":
                os.remove(item.dest)
        atomic_write_bytes(state_dest, state_bytes)
    except Exception as error:
        log(f"Install failed ({error}); restoring the previous files...")
        try:
            _restore_entries(target, Backup(backup_dir, index), force=True, log=log)
            index["status"] = "rolled-back"
            index["restored_at"] = dt.datetime.now(dt.timezone.utc).isoformat(timespec="seconds")
            atomic_write_json(backup_dir / "index.json", index)
        except Exception as rollback_error:  # pragma: no cover - last resort
            log(f"Automatic rollback failed: {rollback_error}. Use 'restore' with backup {backup_dir.name}.")
        raise
    index["status"] = "complete"
    atomic_write_json(backup_dir / "index.json", index)
    log("Done.")
    return backup_dir


def install_package(target: Target, zip_path: Path, metadata: Optional[dict] = None, *,
                    pin_workshop: bool = True, dry_run: bool = False, log: Log = _noop) -> Plan:
    package = open_package(zip_path, target)
    plan = plan_install(target, package, metadata, pin_workshop=pin_workshop)
    if not plan.changes and not dry_run and target.state_path.is_file():
        state = target.read_state() or {}
        if state.get("archive_sha256") == package.sha256:
            log("Already installed and unchanged; nothing to do.")
            return plan
    apply_plan(plan, dry_run=dry_run, log=log)
    return plan


# ---------------------------------------------------------------------------
# Restore / uninstall
# ---------------------------------------------------------------------------

@dataclass
class RestoreStep:
    path: str
    action: str  # restore | delete | skip | keep-modified | conflict
    reason: str = ""


def plan_restore(target: Target, backup: Backup, force: bool = False) -> List[RestoreStep]:
    steps = []
    for entry in reversed(backup.index.get("entries", [])):
        rel = entry["path"]
        dest = target.root / rel
        if dest.is_symlink():
            steps.append(RestoreStep(rel, "conflict", "is now a symbolic link"))
            continue
        current = sha256_file(dest) if dest.is_file() else None
        original, installed = entry.get("original_sha256"), entry.get("installed_sha256")
        action = entry["action"]
        if action == "added":
            if current is None:
                steps.append(RestoreStep(rel, "skip", "already gone"))
            elif current == installed:
                steps.append(RestoreStep(rel, "delete"))
            else:
                steps.append(RestoreStep(rel, "keep-modified", "changed since install; left in place"))
        else:  # overwritten / removed
            if current == original:
                steps.append(RestoreStep(rel, "skip", "already original"))
            elif current is None or current == installed or force:
                steps.append(RestoreStep(rel, "restore"))
            else:
                steps.append(RestoreStep(rel, "conflict", "changed since install"))
    return steps


def _restore_entries(target: Target, backup: Backup, force: bool, log: Log) -> List[RestoreStep]:
    steps = plan_restore(target, backup, force=force)
    conflicts = [step for step in steps if step.action == "conflict"]
    if conflicts:
        raise InstallerError(
            "These files changed after the install, so restoring would lose your edits:\n  "
            + "\n  ".join(step.path for step in conflicts)
            + "\nRe-run with --force to restore anyway (your current copies are saved in the backup folder)."
        )
    entries = {entry["path"]: entry for entry in backup.index.get("entries", [])}
    for step in steps:
        dest = target.root / step.path
        if step.action == "delete":
            os.remove(dest)
        elif step.action == "restore":
            entry = entries[step.path]
            source = backup.path / "files" / step.path
            if not source.is_file() or sha256_file(source) != entry.get("original_sha256"):
                raise InstallerError(f"Backup copy of {step.path} is missing or damaged; cannot restore it")
            if dest.is_file() and sha256_file(dest) not in (entry.get("installed_sha256"),):
                # --force: keep the user's current version next to the backup.
                aside = backup.path / "replaced-on-restore" / step.path
                aside.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(dest, aside)
            atomic_write_bytes(dest, source.read_bytes())
        elif step.action == "keep-modified":
            log(f"  kept {step.path} ({step.reason})")
    for rel in backup.index.get("created_dirs", []):
        folder = target.root / rel
        try:
            if folder.is_dir() and not folder.is_symlink() and not any(folder.iterdir()):
                folder.rmdir()
        except OSError:
            pass
    return steps


def restore_backup(target: Target, backup: Optional[Backup] = None, *, force: bool = False,
                   dry_run: bool = False, log: Log = _noop) -> Optional[Backup]:
    """Undo one install/update (the most recent one by default)."""
    if backup is None:
        candidates = list_backups(target, include_restored=False)
        if not candidates:
            raise InstallerError(f"No backups to restore in {target.backup_root}")
        backup = candidates[0]
    if backup.restored:
        raise InstallerError(f"Backup {backup.name} was already restored")
    log(f"Restoring {backup.describe()}")
    steps = plan_restore(target, backup, force=force)
    for step in steps:
        if step.action != "skip":
            log(f"  {step.action:13} {step.path}" + (f" ({step.reason})" if step.reason else ""))
    if dry_run:
        conflicts = [s for s in steps if s.action == "conflict"]
        if conflicts:
            log(f"Dry run: {len(conflicts)} conflict(s) would stop the restore without --force.")
        log("Dry run: no files were changed.")
        return backup
    _restore_entries(target, backup, force=force, log=log)
    backup.index["restored_at"] = dt.datetime.now(dt.timezone.utc).isoformat(timespec="seconds")
    atomic_write_json(backup.path / "index.json", backup.index)
    log("Restore complete.")
    return backup


def uninstall(target: Target, *, force: bool = False, dry_run: bool = False, log: Log = _noop) -> int:
    """Undo every installer change, newest first, returning the game folder
    to how it was before the first install. User-modified files are kept."""
    backups = list_backups(target, include_restored=False)
    if not backups and not target.state_path.is_file():
        raise InstallerError("Nothing to uninstall: the installer has not installed the mod here.")
    if dry_run:
        log(f"Dry run: would undo {len(backups)} install(s):")
        for backup in backups:
            log("  " + backup.describe())
        if backups:
            restore_backup(target, backups[0], force=force, dry_run=True, log=log)
        return len(backups)
    for backup in backups:
        restore_backup(target, backup, force=force, log=log)
    if target.state_path.is_file():
        # Only possible if the state file was written outside a backup run.
        state = target.read_state() or {}
        for rel, recorded in (state.get("files") or {}).items():
            dest = resolve_ci(target.root, rel)
            if dest.is_file() and sha256_file(dest) == recorded and not target.is_preserved(rel):
                os.remove(dest)
        os.remove(target.state_path)
    log("Uninstall complete. Backups are kept in " + str(target.backup_root))
    return len(backups)


# ---------------------------------------------------------------------------
# Status
# ---------------------------------------------------------------------------

@dataclass
class InstallStatus:
    installed: bool
    state: dict
    modified: List[str]
    missing: List[str]
    backups: List[Backup]

    def lines(self) -> List[str]:
        if not self.installed:
            out = ["Mod: not installed by this installer."]
        else:
            state = self.state
            version = state.get("version") or "?"
            tag = state.get("tag")
            out = [f"Mod: {version}" + (f" (tag {tag})" if tag and tag != version else "")
                   + f", from {state.get('source')}" + (f" {state.get('repo')}" if state.get("repo") else ""),
                   f"Installed: {state.get('installed_at')}, {len(state.get('files', {}))} files"]
            if self.modified:
                out.append(f"Changed since install: {', '.join(self.modified[:8])}"
                           + (" ..." if len(self.modified) > 8 else ""))
            if self.missing:
                out.append(f"Missing: {', '.join(self.missing[:8])}" + (" ..." if len(self.missing) > 8 else ""))
            if state.get("kind") == "client":
                out.append("Workshop auto-update: " + ("disabled (pinned)" if state.get("workshop_updates_pinned") else "allowed"))
        active = [backup for backup in self.backups if not backup.restored]
        out.append(f"Backups: {len(active)} restorable, {len(self.backups) - len(active)} already restored")
        for backup in self.backups[:5]:
            out.append("  " + backup.describe())
        return out


def status(target: Target) -> InstallStatus:
    state = target.read_state() or {}
    modified, missing = [], []
    for rel, recorded in sorted((state.get("files") or {}).items()):
        dest = resolve_ci(target.root, rel)
        if not dest.is_file():
            missing.append(rel)
        elif not target.is_preserved(rel) and sha256_file(dest) != recorded:
            modified.append(rel)
    return InstallStatus(bool(state), state, modified, missing, list_backups(target))
