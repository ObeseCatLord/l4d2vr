import sys
import unittest
import contextlib
import io
import json
from pathlib import Path
from unittest import mock

from l4d2vr_installer import actions, cli, install, settings, sources, steam

from .helpers import CLIENT_V1, CLIENT_V2, SERVER_FILES, TempDirTest, make_game, make_zip, sha, snapshot
from .test_sources import FakeGitHub


class CliTestCase(TempDirTest):
    def setUp(self):
        super().setUp()
        self.game = make_game(self.tmp / "lib")
        # Never look at real processes from the tests.
        for name in ("is_game_running", "is_steam_running"):
            patcher = mock.patch.object(steam, name, return_value=False)
            patcher.start()
            self.addCleanup(patcher.stop)

    def run_cli(self, *argv):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = cli.main([str(a) for a in argv])
        return code, out.getvalue(), err.getvalue()


class CliTests(CliTestCase):
    def test_offline_zip_install_status_restore(self):
        v1 = make_zip(self.tmp / "v1.zip", CLIENT_V1)
        code, out, err = self.run_cli("install", "--game-dir", self.game, "--zip", v1, "--dry-run")
        self.assertEqual(code, 0, err)
        self.assertIn("Dry run", out)
        self.assertFalse((self.game / "d3d9.dll").exists())
        code, out, err = self.run_cli("install", "--game-dir", self.game, "--zip", v1)
        self.assertEqual(code, 0, err)
        self.assertTrue((self.game / "d3d9.dll").exists())
        code, out, _ = self.run_cli("status", "--game-dir", self.game)
        self.assertIn("Mod: 1.0.0", out)
        code, out, _ = self.run_cli("restore", "--game-dir", self.game, "--list")
        self.assertIn("install nothing -> 1.0.0", out)
        code, out, err = self.run_cli("uninstall", "--game-dir", self.game)
        self.assertEqual(code, 0, err)
        self.assertFalse((self.game / "d3d9.dll").exists())
        # The game folder is remembered in the (temporary) settings file.
        settings_file = settings.settings_path()
        self.assertEqual(json.loads(settings_file.read_text())["game_dir"], str(self.game))

    def test_errors_are_reported_with_exit_code(self):
        code, _, err = self.run_cli("install", "--game-dir", self.tmp / "nope", "--zip", self.tmp / "x.zip")
        self.assertEqual(code, 1)
        self.assertIn("ERROR", err)
        linux_game = make_game(self.tmp / "native", windows=False)
        code, _, err = self.run_cli("install", "--game-dir", linux_game, "--zip", self.tmp / "x.zip")
        self.assertEqual(code, 1)
        self.assertIn("Proton", err)

    def test_install_and_update_from_fake_github(self):
        gh = FakeGitHub(self.tmp / "api", "someone/fork")
        v1_bytes = make_zip(self.tmp / "b1" / "l4d2vr.zip", CLIENT_V1).read_bytes()
        gh.publish("v1.0.0", [gh.asset("l4d2vr.zip", v1_bytes, digest=True)])
        common = ["--game-dir", self.game, "--repo", "someone/fork", "--api-base", gh.api_base]
        code, out, err = self.run_cli("install", *common)
        self.assertEqual(code, 0, err)
        state = install.client_target(self.game).read_state()
        self.assertEqual((state["tag"], state["repo"], state["source"]), ("v1.0.0", "someone/fork", "github"))
        code, out, _ = self.run_cli("check", *common)
        self.assertIn("Up to date", out)
        code, out, _ = self.run_cli("update", *common)
        self.assertIn("Already up to date", out)

        v2_bytes = make_zip(self.tmp / "b2" / "l4d2vr.zip", CLIENT_V2).read_bytes()
        gh.publish("v1.1.0", [gh.asset("l4d2vr.zip", v2_bytes, digest=True)])
        code, out, _ = self.run_cli("check", *common)
        self.assertIn("Update available", out)
        code, out, err = self.run_cli("update", *common)
        self.assertEqual(code, 0, err)
        self.assertEqual((self.game / "d3d9.dll").read_bytes(), CLIENT_V2["d3d9.dll"])
        self.assertFalse((self.game / "vr/old_only_in_v1.txt").exists())

    def test_tampered_download_is_not_installed(self):
        gh = FakeGitHub(self.tmp / "api", "someone/fork")
        data = make_zip(self.tmp / "b1" / "l4d2vr.zip", CLIENT_V1).read_bytes()
        item = gh.asset("l4d2vr.zip", data)
        item["digest"] = "sha256:" + "0" * 64
        gh.publish("v1", [item])
        code, _, err = self.run_cli("install", "--game-dir", self.game, "--repo", "someone/fork",
                                    "--api-base", gh.api_base)
        self.assertEqual(code, 1)
        self.assertIn("mismatch", err)
        self.assertFalse((self.game / "d3d9.dll").exists())

    def test_make_manifest(self):
        zip_path = make_zip(self.tmp / "l4d2vr.zip", CLIENT_V1)
        out_file = self.tmp / "manifest.json"
        sha_file = self.tmp / "l4d2vr.sha256"
        code, _, err = self.run_cli("make-manifest", "--zip", zip_path, "--version", "1.2.3", "--tag", "v1.2.3",
                                    "--url", zip_path.as_uri(), "--output", out_file, "--sha-file", sha_file)
        self.assertEqual(code, 0, err)
        manifest = json.loads(out_file.read_text())
        self.assertEqual(manifest["sha256"], sha(zip_path.read_bytes()))
        self.assertEqual(sources.parse_sha256_list(sha_file.read_text())["l4d2vr.zip"], manifest["sha256"])
        release = sources.ManifestSource(out_file.as_uri()).latest()
        self.assertEqual(release.version, "1.2.3")
        code, _, _ = self.run_cli("make-manifest", "--zip", zip_path, "--version", "1.3.0-beta", "--url",
                                  zip_path.as_uri(), "--channel", "beta", "--output", out_file)
        self.assertEqual(sources.ManifestSource(out_file.as_uri(), "beta").latest().version, "1.3.0-beta")
        self.assertEqual(sources.ManifestSource(out_file.as_uri()).latest().version, "1.2.3")

    def test_launch_steamvr_dry_run(self):
        code, out, err = self.run_cli("launch", "--game-dir", self.game, "--route", "steamvr",
                                      "--connect", "1.2.3.4:27015", "--password", "pw", "--dry-run")
        self.assertEqual(code, 0, err)
        self.assertNotIn("pw", out)

    def test_shortcut_dry_run(self):
        code, out, err = self.run_cli("shortcut", "--game-dir", self.game, "--dry-run")
        self.assertEqual(code, 0, err)
        self.assertIn("Would write", out)
        self.assertFalse((self.home / ".local/share/applications/l4d2vr.desktop").exists())

    @unittest.skipIf(sys.platform.startswith("win"), "Steam launch options are only changed on Linux")
    def test_launch_options_cli_with_fake_steam(self):
        steam_root = self.home / ".local/share/Steam"
        (steam_root / "steamapps").mkdir(parents=True)
        game = make_game(steam_root)
        config = steam_root / "userdata" / "42" / "config" / "localconfig.vdf"
        config.parent.mkdir(parents=True)
        config.write_text('"UserLocalConfigStore"\n{\n}\n')
        with mock.patch.object(steam, "steam_root_candidates", return_value=[steam_root]):
            code, out, err = self.run_cli("launch-options", "apply", "--game-dir", game, "--dry-run")
            self.assertEqual(code, 0, err)
            self.assertIn("Dry run", out)
            self.assertNotIn("WINEDLLOVERRIDES", config.read_text())
            code, out, err = self.run_cli("launch-options", "apply", "--game-dir", game)
            self.assertEqual(code, 0, err)
            self.assertIn("PROTON_USE_WOW64=1", config.read_text())
            code, out, err = self.run_cli("launch-options", "show", "--game-dir", game)
            self.assertIn(": OK", out)
            code, out, err = self.run_cli("launch-options", "remove", "--game-dir", game)
            self.assertEqual(config.read_text().count("LaunchOptions"), 0)


class ServerInstallTests(CliTestCase):
    def setUp(self):
        super().setUp()
        self.srcds = self.tmp / "srcds"
        self.server_dir = self.srcds / "left4dead2"
        (self.server_dir / "addons").mkdir(parents=True)
        (self.server_dir / "gameinfo.txt").write_text("gameinfo")
        (self.server_dir / "addons" / "other_plugin.vdf").write_text("other")

    def test_install_server_plugin_round_trip(self):
        package = make_zip(self.tmp / "l4d2vr-server-linux.zip", SERVER_FILES)
        before = snapshot(self.server_dir)
        code, out, err = self.run_cli("install-server", "--server-dir", self.srcds, "--zip", package, "--dry-run")
        self.assertEqual(code, 0, err)
        self.assertEqual(snapshot(self.server_dir), before)
        code, out, err = self.run_cli("install-server", "--server-dir", self.server_dir, "--zip", package)
        self.assertEqual(code, 0, err)
        self.assertEqual((self.server_dir / "addons" / "l4d2vr_server.so").read_bytes(),
                         SERVER_FILES["left4dead2/addons/l4d2vr_server.so"])
        self.assertTrue((self.server_dir / "addons" / "l4d2vr_installed_release.json").is_file())
        code, out, _ = self.run_cli("status", "--server-dir", self.server_dir, "--game-dir", self.game)
        self.assertIn("Server folder", out)
        code, out, err = self.run_cli("uninstall", "--server-dir", self.server_dir)
        self.assertEqual(code, 0, err)
        self.assertEqual(snapshot(self.server_dir), before)

    def test_server_from_release(self):
        gh = FakeGitHub(self.tmp / "api", "o/r")
        data = make_zip(self.tmp / "b" / "s.zip", SERVER_FILES).read_bytes()
        gh.publish("v1", [gh.asset("l4d2vr-server-linux.zip", data, digest=True)])
        code, out, err = self.run_cli("install-server", "--server-dir", self.server_dir, "--platform", "linux",
                                      "--repo", "o/r", "--api-base", gh.api_base)
        self.assertEqual(code, 0, err)
        self.assertTrue((self.server_dir / "addons" / "l4d2vr_server.vdf").is_file())
        code, _, err = self.run_cli("install-server", "--server-dir", self.server_dir, "--platform", "windows",
                                    "--repo", "o/r", "--api-base", gh.api_base)
        self.assertEqual(code, 1)
        self.assertIn("windows server plugin", err)

    def test_client_zip_rejected_for_server(self):
        package = make_zip(self.tmp / "client.zip", CLIENT_V1)
        code, _, err = self.run_cli("install-server", "--server-dir", self.server_dir, "--zip", package)
        self.assertEqual(code, 1)
        self.assertIn("client zip", err)

    def test_wrong_server_dir(self):
        other = self.tmp / "random"
        other.mkdir()
        code, _, err = self.run_cli("install-server", "--server-dir", other, "--zip", self.tmp / "x.zip")
        self.assertEqual(code, 1)
        self.assertIn("gameinfo.txt", err)


class BundledZipTests(TempDirTest):
    def test_offline_falls_back_to_bundled_zip(self):
        game = make_game(self.tmp)
        bundled = make_zip(self.tmp / "dist" / "l4d2vr.zip", CLIENT_V1)
        with mock.patch.object(actions, "fetch_release", side_effect=actions.InstallerError("offline")), \
                mock.patch.object(actions, "bundled_client_zip", return_value=bundled):
            actions.install_client(game, check_running=False, log=lambda *_: None)
        self.assertEqual((game / "d3d9.dll").read_bytes(), CLIENT_V1["d3d9.dll"])

    def test_offline_without_bundled_zip_still_fails(self):
        game = make_game(self.tmp)
        with mock.patch.object(actions, "fetch_release", side_effect=actions.InstallerError("offline")), \
                mock.patch.object(actions, "bundled_client_zip", return_value=None):
            with self.assertRaises(actions.InstallerError):
                actions.install_client(game, check_running=False, log=lambda *_: None)

