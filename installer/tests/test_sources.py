import json
import unittest
from pathlib import Path

from l4d2vr_installer import sources
from l4d2vr_installer.fsutil import InstallerError

from .helpers import CLIENT_V1, TempDirTest, make_zip, sha


class FakeGitHub:
    """Lays out ``<root>/repos/<owner>/<name>/releases[/latest]`` JSON files
    and asset files so GitHubSource can read them over file:// URLs."""

    def __init__(self, root: Path, repo: str = "owner/l4d2vr"):
        self.root = root
        self.repo = repo
        self.assets_dir = root / "assets"
        self.assets_dir.mkdir(parents=True, exist_ok=True)
        self.api_base = root.as_uri()

    def asset(self, name: str, data: bytes, digest: bool = False, size=None) -> dict:
        path = self.assets_dir / name
        path.write_bytes(data)
        item = {"name": name, "size": len(data) if size is None else size, "browser_download_url": path.as_uri()}
        if digest:
            item["digest"] = "sha256:" + sha(data)
        return item

    def publish(self, tag: str, assets, prerelease: bool = False) -> dict:
        release = {"tag_name": tag, "html_url": f"https://example.invalid/{tag}", "body": "notes",
                   "prerelease": prerelease, "draft": False, "assets": assets}
        folder = self.root / "repos" / self.repo / "releases"
        folder.mkdir(parents=True, exist_ok=True)
        (folder / "latest").write_text(json.dumps(release))
        return release


class GitHubSourceTests(TempDirTest):
    def setUp(self):
        super().setUp()
        self.gh = FakeGitHub(self.tmp / "api")
        self.zip_bytes = make_zip(self.tmp / "build" / "l4d2vr.zip", CLIENT_V1).read_bytes()

    def source(self, channel="stable"):
        return sources.GitHubSource(self.gh.repo, channel, api_base=self.gh.api_base)

    def test_latest_release_with_sha_asset(self):
        client = self.gh.asset("l4d2vr.zip", self.zip_bytes)
        sums = self.gh.asset("l4d2vr.sha256", f"{sha(self.zip_bytes)}  l4d2vr.zip\n".encode())
        server = self.gh.asset("l4d2vr-server-linux.zip", b"server")
        installer = self.gh.asset("l4d2vr-installer-linux-v9.0.0", b"bin")
        self.gh.publish("v2.1.0", [client, sums, server, installer])
        release = self.source().latest()
        self.assertEqual(release.version, "2.1.0")
        self.assertEqual(release.client.sha256, sha(self.zip_bytes))
        self.assertIn("linux", release.servers)
        self.assertEqual(release.installer_version, "9.0.0")
        path = sources.download(release.client, self.tmp / "downloads")
        self.assertEqual(path.read_bytes(), self.zip_bytes)

    def test_github_digest_field_used(self):
        self.gh.publish("v1", [self.gh.asset("l4d2vr.zip", self.zip_bytes, digest=True)])
        self.assertEqual(self.source().latest().client.sha256, sha(self.zip_bytes))

    def test_checksum_mismatch_refused(self):
        client = self.gh.asset("l4d2vr.zip", self.zip_bytes)
        sums = self.gh.asset("l4d2vr.sha256", ("0" * 64 + "  l4d2vr.zip\n").encode())
        self.gh.publish("v1", [client, sums])
        release = self.source().latest()
        with self.assertRaises(InstallerError) as raised:
            sources.download(release.client, self.tmp / "downloads")
        self.assertIn("SHA-256 mismatch", str(raised.exception))
        self.assertFalse((self.tmp / "downloads" / "l4d2vr.zip").exists())
        self.assertEqual(list((self.tmp / "downloads").iterdir()), [])

    def test_size_mismatch_refused(self):
        self.gh.publish("v1", [self.gh.asset("l4d2vr.zip", self.zip_bytes, size=len(self.zip_bytes) + 1)])
        with self.assertRaises(InstallerError):
            sources.download(self.source().latest().client, self.tmp / "downloads")

    def test_manifest_asset_overrides_and_preserve(self):
        client = self.gh.asset("l4d2vr.zip", self.zip_bytes)
        manifest = {"version": "1", "url": "unused", "sha256": sha(self.zip_bytes), "size": len(self.zip_bytes),
                    "preserve": ["vr/extra.txt"], "installer": {"version": "1.5.0"}}
        self.gh.publish("v1", [client, self.gh.asset("l4d2vr-manifest.json", json.dumps(manifest).encode())])
        release = self.source().latest()
        self.assertEqual(release.client.sha256, sha(self.zip_bytes))
        self.assertEqual(release.preserve, ["vr/extra.txt"])
        self.assertEqual(release.installer_version, "1.5.0")

    def test_beta_channel_reads_release_list(self):
        listing_path = self.tmp / "api" / "repos" / self.gh.repo / "releases"
        listing_path.parent.mkdir(parents=True, exist_ok=True)
        beta = {"tag_name": "v3.0.0-beta.1", "prerelease": True, "draft": False,
                "assets": [self.gh.asset("l4d2vr.zip", self.zip_bytes)]}
        draft = {"tag_name": "v4-draft", "prerelease": False, "draft": True, "assets": []}
        listing_path.write_text(json.dumps([draft, beta]))
        release = self.source("beta").latest()
        self.assertEqual(release.tag, "v3.0.0-beta.1")
        self.assertTrue(release.prerelease)

    def test_bad_repo_name(self):
        with self.assertRaises(InstallerError):
            sources.GitHubSource("not a repo")

    def test_missing_release_is_clear_error(self):
        with self.assertRaises(InstallerError) as raised:
            self.source().latest()
        self.assertIn("Cannot reach", str(raised.exception))


class ManifestSourceTests(TempDirTest):
    def write_manifest(self, data) -> str:
        path = self.tmp / "manifest.json"
        path.write_text(json.dumps(data))
        return path.as_uri()

    def test_stable_and_beta_channels(self):
        zip_path = make_zip(self.tmp / "l4d2vr.zip", CLIENT_V1)
        data = zip_path.read_bytes()
        url = self.write_manifest({
            "schema": 1, "version": "1.0.0", "url": zip_path.as_uri(), "sha256": sha(data), "size": len(data),
            "preserve": ["vr/mine.txt"],
            "channels": {"beta": {"version": "1.1.0-beta", "url": zip_path.as_uri(), "sha256": sha(data)}},
            "installer": {"version": "2.0.0", "assets": {"linux": {"url": "file:///x/l4d2vr-installer-linux"}}},
        })
        stable = sources.ManifestSource(url).latest()
        self.assertEqual(stable.version, "1.0.0")
        self.assertEqual(stable.client.size, len(data))
        self.assertEqual(stable.preserve, ["vr/mine.txt"])
        self.assertEqual(sources.download(stable.client, self.tmp / "dl").read_bytes(), data)
        beta = sources.ManifestSource(url, "beta").latest()
        self.assertEqual(beta.version, "1.1.0-beta")
        self.assertIsNone(beta.client.size)
        self.assertEqual(stable.installer_version, "2.0.0")

    def test_invalid_manifests(self):
        for bad in ({"version": "1"}, {"url": "x"}, {"version": "1", "url": "x", "sha256": "abc"},
                    {"schema": 2, "version": "1", "url": "x"}):
            with self.assertRaises(InstallerError, msg=bad):
                sources.ManifestSource(self.write_manifest(bad)).latest()
        with self.assertRaises(InstallerError):
            sources.ManifestSource(self.write_manifest({"version": "1", "url": "x"}), "beta").latest()

    def test_repo_manifest_file_is_valid(self):
        repo_manifest = Path(__file__).resolve().parents[1] / "manifest.json"
        release = sources.ManifestSource(repo_manifest.as_uri()).latest()
        self.assertTrue(release.client.url.startswith("https://github.com/ObeseCatLord/l4d2vr/"))


class VersionTests(unittest.TestCase):
    def test_version_ordering(self):
        self.assertTrue(sources.is_newer("V.0.8.3", "V.0.8.2"))
        self.assertTrue(sources.is_newer("v1.0.0", "v1.0.0-beta.2"))
        self.assertFalse(sources.is_newer("1.0.0", "1.0.0"))
        self.assertTrue(sources.is_newer("1.10.0", "1.9.9"))
        self.assertTrue(sources.is_newer("1.0", None))
        self.assertFalse(sources.is_newer(None, "1.0"))

    def test_sha_list_parsing(self):
        digest = "a" * 64
        parsed = sources.parse_sha256_list(f"{digest}  l4d2vr.zip\n{'b' * 64} *dir/other.zip\n# c\n{'c' * 64}\n")
        self.assertEqual(parsed["l4d2vr.zip"], digest)
        self.assertEqual(parsed["other.zip"], "b" * 64)
        self.assertEqual(parsed[""], "c" * 64)

    def test_installer_update_check(self):
        release = sources.Release("1", "v1", "github")
        self.assertFalse(sources.check_installer_update(release, "1.0.0").newer)
        release.installers[sources.platform_key()] = sources.Asset("l4d2vr-installer", "https://x/y", 1, "a" * 64)
        release.installer_version = "1.2.0"
        self.assertTrue(sources.check_installer_update(release, "1.0.0").newer)
        self.assertFalse(sources.check_installer_update(release, "1.2.0").newer)
        release.installer_version = None
        self.assertTrue(sources.check_installer_update(release, "1.0.0", running_sha256="b" * 64).newer)
        self.assertFalse(sources.check_installer_update(release, "1.0.0", running_sha256="a" * 64).newer)


if __name__ == "__main__":
    unittest.main()
