"""High-level operations shared by the command line and the GUI."""

from __future__ import annotations

import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, List, Optional

from . import DEFAULT_REPO, __version__, install, launchopts, openxr, sources, steam
from .fsutil import InstallerError, sha256_file

Log = Callable[[str], None]


def _print(message: str) -> None:
    print(message, flush=True)


# ---------------------------------------------------------------------------
# Game folder
# ---------------------------------------------------------------------------

def detect_game_dir(explicit: Optional[str] = None, remembered: Optional[str] = None) -> Optional[Path]:
    """Choose the game folder: explicit argument, remembered setting, then
    the first Windows-depot install found in the Steam libraries."""
    for value in (explicit, remembered):
        if value:
            return Path(value).expanduser()
    installs = steam.find_game_installs()
    return installs[0].path if installs else None


def require_game_dir(explicit: Optional[str] = None, remembered: Optional[str] = None) -> Path:
    game = detect_game_dir(explicit, remembered)
    if game is None:
        raise InstallerError(
            "Could not find Left 4 Dead 2. Install it in Steam, or pass --game-dir "
            "\"<Steam library>/steamapps/common/Left 4 Dead 2\"."
        )
    steam.check_game_dir(game)
    return game


# ---------------------------------------------------------------------------
# Releases
# ---------------------------------------------------------------------------

@dataclass
class SourceOptions:
    kind: str = "github"
    repo: str = DEFAULT_REPO
    channel: str = "stable"
    manifest_url: Optional[str] = None
    api_base: str = sources.GITHUB_API

    def make(self):
        return sources.make_source(self.kind, self.repo, self.channel, self.manifest_url, self.api_base)


def fetch_release(options: SourceOptions, log: Log = _print) -> sources.Release:
    source = options.make()
    log(f"Checking {source.describe()} ...")
    return source.latest()


@dataclass
class UpdateCheck:
    installed_version: Optional[str]
    installed_tag: Optional[str]
    release: sources.Release
    installed_archive_sha256: Optional[str] = None

    @property
    def installed(self) -> bool:
        return bool(self.installed_version or self.installed_tag)

    @property
    def update_available(self) -> bool:
        if not self.installed:
            return True
        if self.installed_tag:
            return sources.is_newer(self.release.tag, self.installed_tag)
        # Installed from a local zip: no tag to compare, so compare the archive.
        client = self.release.client
        if client and client.sha256 and self.installed_archive_sha256:
            return client.sha256.lower() != self.installed_archive_sha256.lower()
        return sources.is_newer(self.release.version, self.installed_version)

    def message(self) -> str:
        latest = self.release.version + (f" ({self.release.tag})" if self.release.tag != self.release.version else "")
        if not self.installed:
            return f"Latest release: {latest}. The mod is not installed by this installer yet."
        current = self.installed_version or self.installed_tag
        if self.update_available:
            return f"Update available: {current} -> {latest}"
        return f"Up to date ({current})."


def check_for_update(game_dir: Path, options: SourceOptions, log: Log = _print) -> UpdateCheck:
    state = install.client_target(game_dir).read_state() or {}
    release = fetch_release(options, log)
    return UpdateCheck(state.get("version"), state.get("tag"), release, state.get("archive_sha256"))


def _metadata(release: Optional[sources.Release], options: Optional[SourceOptions], asset_name: str) -> dict:
    if release is None:
        return {"source": "local", "asset": asset_name}
    return {
        "source": release.source, "repo": options.repo if options else None,
        "channel": options.channel if options else None, "tag": release.tag,
        "version": release.version, "asset": asset_name,
    }


def download_asset(asset: sources.Asset, log: Log, progress=None) -> Path:
    if not asset.sha256:
        log(f"Warning: the release publishes no SHA-256 for {asset.name}; only the size can be checked.")
    log(f"Downloading {asset.describe()} ...")
    path = sources.download(asset, progress=progress)
    log(f"Verified {asset.name}" + (f" (SHA-256 {asset.sha256[:16]}...)" if asset.sha256 else ""))
    return path


def bundled_client_zip() -> Optional[Path]:
    """l4d2vr.zip shipped next to the installer (the full release archive)."""
    base = Path(sys.executable).parent if getattr(sys, "frozen", False) else Path(__file__).resolve().parent.parent
    candidate = base / sources.CLIENT_ASSET
    return candidate if candidate.is_file() else None


def install_client(game_dir: Path, *, zip_path: Optional[Path] = None, options: Optional[SourceOptions] = None,
                   pin_workshop: bool = True, extra_preserve: Optional[List[str]] = None,
                   force: bool = False, dry_run: bool = False, log: Log = _print,
                   progress=None, check_running: bool = True) -> install.Plan:
    """Install or update the client mod (from a local zip or the latest release)."""
    steam.check_game_dir(game_dir)
    if check_running and not dry_run and steam.is_game_running():
        raise InstallerError("Left 4 Dead 2 is running. Close the game, then install.")
    target = install.client_target(game_dir, extra_preserve)
    release = None
    if zip_path is None:
        options = options or SourceOptions()
        try:
            release = fetch_release(options, log)
            if release.client is None:
                raise InstallerError(f"The latest release ({release.tag}) has no {sources.CLIENT_ASSET} asset.")
            state = target.read_state() or {}
            same_archive = (not release.client.sha256
                            or state.get("archive_sha256") == release.client.sha256.lower())
            if not force and state.get("tag") == release.tag and same_archive:
                log(f"Already up to date ({release.tag}). Use --force to reinstall.")
                return install.Plan(target, None)
            for pattern in release.preserve:
                if pattern not in target.preserve:
                    target.preserve.append(pattern)
            zip_path = download_asset(release.client, log, progress)
            log(f"Installing {release.tag} into {game_dir}")
        except InstallerError as error:
            # Offline: the full release archive ships l4d2vr.zip next to the installer.
            bundled = bundled_client_zip()
            if bundled is None:
                raise
            release = None
            zip_path = bundled
            log(f"Could not get the latest release ({error}); installing the bundled {bundled.name} instead.")
            log(f"Installing {bundled.name} into {game_dir}")
    else:
        log(f"Installing {Path(zip_path).name} into {game_dir}")
    return install.install_package(target, Path(zip_path), _metadata(release, options, Path(zip_path).name),
                                   pin_workshop=pin_workshop, dry_run=dry_run, log=log)


# ---------------------------------------------------------------------------
# Dedicated server plugin
# ---------------------------------------------------------------------------

def normalize_server_dir(server_dir: Path, log: Log = _print) -> Path:
    server_dir = Path(server_dir).expanduser()
    if not server_dir.is_dir():
        raise InstallerError(f"Server folder does not exist: {server_dir}")
    if server_dir.name.lower() != "left4dead2" and (server_dir / "left4dead2").is_dir():
        log(f"Using {server_dir / 'left4dead2'} (the game folder inside the server install).")
        server_dir = server_dir / "left4dead2"
    if server_dir.name.lower() != "left4dead2" and not (server_dir / "gameinfo.txt").is_file():
        raise InstallerError(
            f"{server_dir} does not look like a L4D2 server game folder. Pass "
            "--server-dir <srcds>/left4dead2 (the folder with gameinfo.txt and addons/)."
        )
    return server_dir


def install_server(server_dir: Path, *, platform: Optional[str] = None, zip_path: Optional[Path] = None,
                   options: Optional[SourceOptions] = None, dry_run: bool = False,
                   log: Log = _print) -> install.Plan:
    server_dir = normalize_server_dir(server_dir, log)
    platform = platform or sources.platform_key()
    target = install.server_target(server_dir)
    release = None
    if zip_path is None:
        options = options or SourceOptions()
        release = fetch_release(options, log)
        asset = release.servers.get(platform)
        if asset is None:
            raise InstallerError(f"The latest release ({release.tag}) has no {platform} server plugin.")
        zip_path = download_asset(asset, log)
    plan = install.install_package(target, Path(zip_path), _metadata(release, options, Path(zip_path).name),
                                   dry_run=dry_run, log=log)
    if not dry_run:
        log("Restart the server (or change map) to load the plugin; check with 'plugin_print' in the server console.")
    return plan


# ---------------------------------------------------------------------------
# Launch options
# ---------------------------------------------------------------------------

def launch_option_users(game_dir: Optional[Path] = None, steam_root: Optional[Path] = None) -> List[steam.SteamUser]:
    root = steam_root or (steam.steam_root_for_game(game_dir) if game_dir else None)
    if root is None:
        roots = steam.find_steam_roots()
        root = roots[0] if roots else None
    if root is None:
        raise InstallerError("Steam was not found, so launch options cannot be edited automatically.")
    users = steam.steam_users(root)
    if not users:
        raise InstallerError(f"No Steam user configuration found under {root}/userdata. Start Steam and log in once.")
    return users


def launch_options_lines(game_dir: Optional[Path]) -> List[str]:
    lines = []
    try:
        users = launch_option_users(game_dir)
    except InstallerError as error:
        return [f"Launch options: {error}"]
    for user in users:
        try:
            current = launchopts.read_launch_options(user.localconfig)
        except InstallerError as error:
            lines.append(f"Launch options ({user.name or user.account_id}): {error}")
            continue
        ok = launchopts.has_required_options(current)
        lines.append(f"Launch options ({user.name or user.account_id}{', most recent' if user.most_recent else ''}): "
                     + ("OK" if ok else "missing VR settings") + f" -> {current or '(empty)'}")
    return lines


# ---------------------------------------------------------------------------
# OpenXR requirements (Proton version, OpenVR runtime for Proton, Windows runtime)
# ---------------------------------------------------------------------------

def _steam_context(game_dir: Optional[Path], roots: Optional[List[Path]] = None):
    roots = roots if roots is not None else steam.find_steam_roots()
    root = steam.steam_root_for_game(game_dir, roots) if game_dir else (roots[0] if roots else None)
    libraries = steam.library_folders(root) if root else []
    for other in roots:
        if other != root:
            libraries += [lib for lib in steam.library_folders(other) if lib not in libraries]
    return root, libraries


def openxr_requirement_lines(game_dir: Optional[Path], roots: Optional[List[Path]] = None) -> List[str]:
    root, libraries = _steam_context(game_dir, roots)
    return openxr.requirement_lines(root, libraries)


def proton_check(game_dir: Optional[Path]) -> openxr.ProtonCheck:
    root, libraries = _steam_context(game_dir)
    if root is None:
        raise InstallerError("Steam was not found, so the Proton version cannot be checked.")
    return openxr.check_proton(root, libraries)


def apply_proton(game_dir: Optional[Path], tool: Optional[str] = None, *, dry_run: bool = False,
                 log: Log = _print) -> Optional[openxr.CompatToolChange]:
    """Force a Proton with 32-bit OpenXR for app 550 (Steam must be closed).
    Does nothing when the current one already has it (unless ``tool`` is given)."""
    from .fsutil import user_config_dir
    root, libraries = _steam_context(game_dir)
    if root is None:
        raise InstallerError("Steam was not found, so the Proton version cannot be changed.")
    check = openxr.check_proton(root, libraries)
    if tool is None:
        if check.ok:
            log(check.lines()[0])
            return None
        tool = check.recommended.name
    change = openxr.set_compat_tool(root, tool, steam_running=steam.is_steam_running(), dry_run=dry_run,
                                    state_dir=user_config_dir())
    if not change.changed:
        log(f"Proton for Left 4 Dead 2 is already {tool}.")
    elif dry_run:
        log(f"Dry run: would set Proton for Left 4 Dead 2: {openxr.describe_change(change)}")
    else:
        log(f"Proton for Left 4 Dead 2: {openxr.describe_change(change)} (backup: {change.backup}). "
            "Steam downloads that Proton on the next start if it is missing.")
    return change


def restore_proton(game_dir: Optional[Path], *, dry_run: bool = False, log: Log = _print) -> None:
    from .fsutil import user_config_dir
    root, _ = _steam_context(game_dir)
    if root is None:
        raise InstallerError("Steam was not found.")
    change = openxr.restore_compat_tool(root, steam_running=steam.is_steam_running(), dry_run=dry_run,
                                        state_dir=user_config_dir())
    if change is None:
        log("The installer did not change the Proton version (or it was changed again since); nothing to restore.")
    else:
        log(("Dry run: would restore " if dry_run else "Restored ") + f"Proton: {openxr.describe_change(change)}")


# ---------------------------------------------------------------------------
# Status
# ---------------------------------------------------------------------------

def status_lines(game_dir: Optional[Path], server_dir: Optional[Path] = None,
                 options: Optional[SourceOptions] = None) -> List[str]:
    lines = [f"Installer: {__version__} ({sys.platform})"]
    roots = steam.find_steam_roots()
    lines.append("Steam: " + (", ".join(str(root) for root in roots) if roots else "not found")
                 + (" (running)" if steam.is_steam_running() else ""))
    if game_dir is not None:
        depot = steam.game_depot(game_dir)
        lines.append(f"Game folder: {game_dir} [{ {'windows': 'Windows build', 'linux': 'native Linux build (needs Proton)', 'missing': 'not found'}[depot] }]")
        target = install.client_target(game_dir)
        lines += install.status(target).lines()
        lines += openxr_requirement_lines(game_dir, roots)
        if not sys.platform.startswith("win"):
            lines += launch_options_lines(game_dir)
    if server_dir is not None:
        lines.append(f"Server folder: {server_dir}")
        lines += install.status(install.server_target(server_dir)).lines()
    if options is not None and game_dir is not None:
        try:
            lines.append(check_for_update(game_dir, options, log=lambda _m: None).message())
        except InstallerError as error:
            lines.append(f"Update check failed: {error}")
    return lines


# ---------------------------------------------------------------------------
# Self-update check
# ---------------------------------------------------------------------------

def check_installer_update(options: SourceOptions, log: Log = _print) -> sources.SelfUpdateInfo:
    release = fetch_release(options, log=lambda _m: None)
    running = sha256_file(Path(sys.executable)) if getattr(sys, "frozen", False) else None
    return sources.check_installer_update(release, __version__, running)


# ---------------------------------------------------------------------------
# Release tooling
# ---------------------------------------------------------------------------

def make_manifest(zip_path: Path, version: str, url: str, tag: Optional[str] = None,
                  channel: str = "stable", existing: Optional[dict] = None) -> dict:
    """Produce/update a manifest entry for a built ``l4d2vr.zip``."""
    zip_path = Path(zip_path)
    manifest = dict(existing or {"schema": 1, "name": "L4D2VR", "repo": DEFAULT_REPO})
    entry = {"version": version, "tag": tag or version, "url": url,
             "sha256": sha256_file(zip_path), "size": zip_path.stat().st_size}
    if channel == "stable":
        manifest.update(entry)
    else:
        manifest.setdefault("channels", {})[channel] = entry
    manifest.setdefault("preserve", [])
    return manifest
