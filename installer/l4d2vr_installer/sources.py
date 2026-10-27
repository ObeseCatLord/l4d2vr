"""Where releases come from: GitHub releases (default) or a BigWalk-style
manifest JSON, plus download and checksum verification.

All network access goes through :func:`open_url`, which uses urllib. Tests
point the sources at ``file://`` URLs, so no network is needed for them.
"""

from __future__ import annotations

import hashlib
import json
import os
import re
import sys
import tempfile
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Dict, List, Optional

from . import CLIENT_ASSET, DEFAULT_REPO, __version__
from .fsutil import InstallerError, user_cache_dir

GITHUB_API = "https://api.github.com"
USER_AGENT = f"l4d2vr-installer/{__version__}"
SHA_ASSETS = ("l4d2vr.sha256", "SHA256SUMS", "SHA256SUMS.txt", "sha256sums.txt")
MANIFEST_ASSETS = ("l4d2vr-manifest.json", "installer-manifest.json")
XRIZER_ASSET = "xrizer-l4d2-runtime.zip"
SERVER_ASSETS = {
    "linux": ("l4d2vr-server-linux.zip",),
    "windows": ("l4d2vr-server-windows.zip", "l4d2vr.dedicated.server.zip"),
}

ProgressCallback = Callable[[int, Optional[int]], None]


def default_manifest_url(repo: str = DEFAULT_REPO) -> str:
    return f"https://raw.githubusercontent.com/{repo}/HEAD/installer/manifest.json"


# ---------------------------------------------------------------------------
# Data model
# ---------------------------------------------------------------------------

@dataclass
class Asset:
    name: str
    url: str
    size: Optional[int] = None
    sha256: Optional[str] = None

    def describe(self) -> str:
        size = f"{self.size / 1048576:.1f} MiB" if self.size else "unknown size"
        return f"{self.name} ({size})"


@dataclass
class Release:
    version: str
    tag: str
    source: str  # "github", "manifest" or "local"
    client: Optional[Asset] = None
    servers: Dict[str, Asset] = field(default_factory=dict)
    xrizer: Optional[Asset] = None
    installers: Dict[str, Asset] = field(default_factory=dict)
    installer_version: Optional[str] = None
    preserve: List[str] = field(default_factory=list)
    notes: str = ""
    page_url: str = ""
    prerelease: bool = False


# ---------------------------------------------------------------------------
# Networking
# ---------------------------------------------------------------------------

def open_url(url: str, accept: Optional[str] = None, timeout: float = 30.0):
    headers = {"User-Agent": USER_AGENT}
    if accept:
        headers["Accept"] = accept
    host = urllib.parse.urlparse(url).hostname or ""
    token = os.environ.get("GITHUB_TOKEN") or os.environ.get("GH_TOKEN")
    # Only ever send a token to the GitHub API host itself.
    if token and host == "api.github.com":
        headers["Authorization"] = f"Bearer {token}"
    request = urllib.request.Request(url, headers=headers)
    try:
        return urllib.request.urlopen(request, timeout=timeout)
    except urllib.error.HTTPError as error:
        if error.code == 403 and host == "api.github.com":
            raise InstallerError(
                "GitHub refused the request (rate limit?). Wait an hour, set GITHUB_TOKEN, "
                "or install from a downloaded zip with --zip."
            ) from error
        if error.code == 404:
            raise InstallerError(f"Not found: {url}") from error
        raise InstallerError(f"Download failed ({error.code}): {url}") from error
    except (urllib.error.URLError, OSError) as error:
        reason = getattr(error, "reason", error)
        raise InstallerError(f"Cannot reach {url}: {reason}. Check your internet connection.") from error


def fetch_json(url: str, accept: Optional[str] = None):
    with open_url(url, accept=accept) as response:
        raw = response.read()
    try:
        return json.loads(raw.decode("utf-8-sig"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise InstallerError(f"Invalid JSON from {url}: {error}") from error


def fetch_text(url: str, limit: int = 1 << 20) -> str:
    with open_url(url) as response:
        return response.read(limit).decode("utf-8", "replace")


def download(asset: Asset, dest_dir: Optional[Path] = None,
             progress: Optional[ProgressCallback] = None) -> Path:
    """Download ``asset`` and verify its size and SHA-256 (when known).

    The file is written under a temporary name and only renamed into place
    after verification succeeds.
    """
    dest_dir = Path(dest_dir) if dest_dir else user_cache_dir() / "downloads"
    dest_dir.mkdir(parents=True, exist_ok=True)
    target = dest_dir / asset.name
    hasher = hashlib.sha256()
    received = 0
    fd, temporary = tempfile.mkstemp(prefix=".part-", dir=str(dest_dir))
    try:
        with os.fdopen(fd, "wb") as out, open_url(asset.url, accept="application/octet-stream",
                                                   timeout=60.0) as response:
            total = asset.size
            if total is None:
                length = response.headers.get("Content-Length") if hasattr(response, "headers") else None
                total = int(length) if length and length.isdigit() else None
            while True:
                chunk = response.read(256 * 1024)
                if not chunk:
                    break
                out.write(chunk)
                hasher.update(chunk)
                received += len(chunk)
                if progress:
                    progress(received, total)
        verify(asset, received, hasher.hexdigest())
        os.replace(temporary, target)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)
    return target


def verify(asset: Asset, size: int, sha256: str) -> None:
    if asset.size is not None and size != asset.size:
        raise InstallerError(
            f"{asset.name}: downloaded {size} bytes but the release says {asset.size}. "
            "The download was incomplete or altered; try again."
        )
    if asset.sha256 and sha256.lower() != asset.sha256.lower():
        raise InstallerError(
            f"{asset.name}: SHA-256 mismatch (expected {asset.sha256}, got {sha256}). "
            "Refusing to install."
        )


def parse_sha256_list(text: str) -> Dict[str, str]:
    """Parse ``sha256sum`` output (``<hash>  <name>`` or ``<hash> *<name>``).

    A file holding just a bare hash maps to the key ``""``.
    """
    out: Dict[str, str] = {}
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        match = re.match(r"^([0-9a-fA-F]{64})(?:\s+\*?(.+))?$", line)
        if match:
            name = (match.group(2) or "").strip()
            out[os.path.basename(name.replace("\\", "/"))] = match.group(1).lower()
    return out


# ---------------------------------------------------------------------------
# Versions
# ---------------------------------------------------------------------------

def version_key(version: str):
    """Sort key for tags like ``V.0.8.2``, ``v1.2.0-beta.1`` or ``1.1.7``."""
    text = (version or "").strip().lstrip("vV").lstrip(".")
    main, _, pre = text.partition("-")
    numbers = [int(part) for part in re.findall(r"\d+", main)]
    # A release sorts after its pre-releases.
    return numbers, 0 if pre else 1, pre


def is_newer(candidate: Optional[str], current: Optional[str]) -> bool:
    if not candidate:
        return False
    if not current:
        return True
    return version_key(candidate) > version_key(current)


def clean_version(tag: str) -> str:
    return (tag or "").strip().lstrip("vV").lstrip(".")


def platform_key() -> str:
    return "windows" if sys.platform.startswith("win") else "linux"


# ---------------------------------------------------------------------------
# GitHub releases
# ---------------------------------------------------------------------------

def _asset_from_github(item: dict) -> Asset:
    digest = item.get("digest") or ""
    sha = digest.split(":", 1)[1] if digest.lower().startswith("sha256:") else None
    return Asset(item.get("name", ""), item.get("browser_download_url", ""), item.get("size"), sha)


def _installer_assets(assets: Dict[str, Asset]):
    installers: Dict[str, Asset] = {}
    version = None
    for name, asset in assets.items():
        match = re.match(r"^l4d2vr-installer-(windows|linux)(?:-v?(\d[\w.\-]*?))?(?:\.exe)?$", name)
        if match:
            installers[match.group(1)] = asset
            if match.group(2):
                version = match.group(2)
    return installers, version


class GitHubSource:
    def __init__(self, repo: str = DEFAULT_REPO, channel: str = "stable", api_base: str = GITHUB_API):
        if not re.match(r"^[\w.-]+/[\w.-]+$", repo or ""):
            raise InstallerError(f"--repo must look like owner/name, got {repo!r}")
        self.repo = repo
        self.channel = channel
        self.api_base = api_base.rstrip("/")

    def describe(self) -> str:
        return f"GitHub releases of {self.repo}" + (" (including pre-releases)" if self.channel != "stable" else "")

    def latest(self) -> Release:
        accept = "application/vnd.github+json"
        if self.channel == "stable":
            data = fetch_json(f"{self.api_base}/repos/{self.repo}/releases/latest", accept)
        else:
            releases = fetch_json(f"{self.api_base}/repos/{self.repo}/releases", accept)
            releases = [item for item in releases if not item.get("draft")]
            if not releases:
                raise InstallerError(f"{self.repo} has no releases yet.")
            data = releases[0]
        return self.release_from_json(data)

    def release_from_json(self, data: dict) -> Release:
        assets = {item.get("name", ""): _asset_from_github(item) for item in data.get("assets", [])}
        tag = data.get("tag_name") or ""
        release = Release(
            version=clean_version(tag), tag=tag, source="github",
            client=assets.get(CLIENT_ASSET), xrizer=assets.get(XRIZER_ASSET),
            notes=data.get("body") or "", page_url=data.get("html_url") or "",
            prerelease=bool(data.get("prerelease")),
        )
        for platform, names in SERVER_ASSETS.items():
            for name in names:
                if name in assets:
                    release.servers[platform] = assets[name]
                    break
        release.installers, release.installer_version = _installer_assets(assets)
        self._apply_checksums(release, assets)
        return release

    def _apply_checksums(self, release: Release, assets: Dict[str, Asset]) -> None:
        """Fill in SHA-256/size from a checksum or manifest asset.

        Checksum assets override GitHub's own ``digest`` field; both are
        verified against the downloaded bytes later.
        """
        downloadable = [release.client, release.xrizer, *release.servers.values(), *release.installers.values()]
        downloadable = [asset for asset in downloadable if asset]
        for name in SHA_ASSETS:
            if name in assets:
                sums = parse_sha256_list(fetch_text(assets[name].url))
                for asset in downloadable:
                    expected = sums.get(asset.name) or (sums.get("") if asset is release.client else None)
                    if expected:
                        asset.sha256 = expected
                break
        for name in MANIFEST_ASSETS:
            if name in assets:
                manifest = fetch_json(assets[name].url)
                entry = _manifest_channel(manifest, "stable")
                if release.client and entry.get("sha256"):
                    release.client.sha256 = entry["sha256"].lower()
                if release.client and entry.get("size"):
                    release.client.size = int(entry["size"])
                release.preserve = list(manifest.get("preserve") or [])
                installer = manifest.get("installer") or {}
                release.installer_version = installer.get("version") or release.installer_version
                break


# ---------------------------------------------------------------------------
# Manifest
# ---------------------------------------------------------------------------

def _manifest_channel(manifest: dict, channel: str) -> dict:
    """Top-level fields are the stable channel; ``channels.<name>`` entries
    override them (BigWalk-style)."""
    base = {key: manifest.get(key) for key in ("version", "tag", "url", "sha256", "size", "notes", "server", "xrizer")
            if manifest.get(key) is not None}
    channels = manifest.get("channels") or {}
    if channel in channels:
        override = channels[channel] or {}
        if "url" in override:
            # A different download must not inherit the stable file's checksum/size.
            for key in ("sha256", "size", "tag", "notes", "server", "xrizer"):
                base.pop(key, None)
        base.update(override)
    elif channel != "stable":
        raise InstallerError(f"Manifest has no '{channel}' channel (available: stable, {', '.join(channels)})")
    return base


def _manifest_asset(entry, default_name: str) -> Optional[Asset]:
    if not isinstance(entry, dict) or not entry.get("url"):
        return None
    size = entry.get("size")
    return Asset(entry.get("name") or os.path.basename(urllib.parse.urlparse(entry["url"]).path) or default_name,
                 entry["url"], int(size) if size else None, (entry.get("sha256") or None))


class ManifestSource:
    def __init__(self, url: str, channel: str = "stable"):
        self.url = url
        self.channel = channel

    def describe(self) -> str:
        return f"manifest {self.url} ({self.channel})"

    def latest(self) -> Release:
        return self.release_from_json(fetch_json(self.url))

    def release_from_json(self, manifest: dict) -> Release:
        if not isinstance(manifest, dict):
            raise InstallerError("Manifest must be a JSON object")
        if int(manifest.get("schema", 1)) > 1:
            raise InstallerError("This manifest needs a newer installer. Download the latest installer.")
        entry = _manifest_channel(manifest, self.channel)
        if not entry.get("version") or not entry.get("url"):
            raise InstallerError(f"Manifest channel '{self.channel}' needs 'version' and 'url'")
        sha = entry.get("sha256")
        if sha is not None and not re.match(r"^[0-9a-fA-F]{64}$", str(sha)):
            raise InstallerError(f"Manifest sha256 is not a SHA-256 hex digest: {sha!r}")
        client = _manifest_asset({"name": CLIENT_ASSET, **entry}, CLIENT_ASSET)
        release = Release(
            version=clean_version(entry["version"]), tag=entry.get("tag") or entry["version"],
            source="manifest", client=client, notes=entry.get("notes") or "",
            preserve=list(manifest.get("preserve") or []), prerelease=self.channel != "stable",
        )
        for platform, server in (entry.get("server") or {}).items():
            asset = _manifest_asset(server, f"l4d2vr-server-{platform}.zip")
            if asset:
                release.servers[platform] = asset
        release.xrizer = _manifest_asset(entry.get("xrizer"), XRIZER_ASSET)
        installer = manifest.get("installer") or {}
        release.installer_version = installer.get("version")
        for platform, item in (installer.get("assets") or {}).items():
            asset = _manifest_asset(item, f"l4d2vr-installer-{platform}")
            if asset:
                release.installers[platform] = asset
        return release


def make_source(kind: str = "github", repo: str = DEFAULT_REPO, channel: str = "stable",
                manifest_url: Optional[str] = None, api_base: str = GITHUB_API):
    if kind == "manifest":
        return ManifestSource(manifest_url or default_manifest_url(repo), channel)
    if kind != "github":
        raise InstallerError(f"Unknown source {kind!r}")
    return GitHubSource(repo, channel, api_base)


# ---------------------------------------------------------------------------
# Installer self-update check
# ---------------------------------------------------------------------------

@dataclass
class SelfUpdateInfo:
    newer: bool
    message: str
    asset: Optional[Asset] = None


def check_installer_update(release: Release, current_version: str = __version__,
                           running_sha256: Optional[str] = None) -> SelfUpdateInfo:
    """Compare this installer with the ``l4d2vr-installer-<os>`` asset.

    The version comes from the asset name (``l4d2vr-installer-linux-v1.2.0``)
    or a manifest; without one, a frozen build compares its own SHA-256 with
    the asset digest. Nothing is downloaded or replaced.
    """
    asset = release.installers.get(platform_key())
    if asset is None:
        return SelfUpdateInfo(False, "No installer build is attached to the latest release.")
    if release.installer_version:
        if is_newer(release.installer_version, current_version):
            return SelfUpdateInfo(True, f"Installer {release.installer_version} is available "
                                        f"(you have {current_version}): {asset.url}", asset)
        return SelfUpdateInfo(False, f"Installer {current_version} is up to date.", asset)
    if running_sha256 and asset.sha256:
        if running_sha256.lower() != asset.sha256.lower():
            return SelfUpdateInfo(True, f"A different installer build is available: {asset.url}", asset)
        return SelfUpdateInfo(False, "Installer is up to date.", asset)
    return SelfUpdateInfo(False, f"Latest installer download: {asset.url} (version unknown).", asset)
