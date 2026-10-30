import json
import os
import unittest
import zipfile
from pathlib import Path
from unittest import mock

from l4d2vr_installer import install
from l4d2vr_installer.fsutil import InstallerError, resolve_ci

from .helpers import CLIENT_V1, CLIENT_V2, TempDirTest, make_game, make_zip, sha, snapshot


class InstallTests(TempDirTest):
    def setUp(self):
        super().setUp()
        self.game = make_game(self.tmp / "lib")
        self.v1 = make_zip(self.tmp / "dl" / "v1.zip", CLIENT_V1)
        self.v2 = make_zip(self.tmp / "dl" / "v2.zip", CLIENT_V2)
        self.logs = []

    def target(self, **kwargs):
        return install.client_target(self.game, **kwargs)

    def install(self, zip_path, metadata=None, **kwargs):
        return install.install_package(self.target(), zip_path, metadata or {}, log=self.logs.append, **kwargs)

    def state(self):
        return json.loads((self.game / "vr" / "installed_release.json").read_text())

    # -- basic install ----------------------------------------------------
    def test_fresh_install_writes_files_and_state(self):
        before = snapshot(self.game)
        self.install(self.v1, {"source": "github", "tag": "v1.0.0", "version": "1.0.0", "repo": "o/r"})
        for name, data in CLIENT_V1.items():
            self.assertEqual((self.game / name).read_bytes(), data, name)
        self.assertEqual((self.game / "left4dead2/materials/l4d2vr/holster_highlight.vmt").read_bytes(),
                         CLIENT_V1["left4dead2/materials/l4d2vr/holster_highlight.vmt"])
        self.assertTrue((self.game / install.PIN_FILE).is_file())
        state = self.state()
        self.assertEqual(state["tag"], "v1.0.0")
        self.assertEqual(state["mod_version"], "1.0.0")
        self.assertEqual(state["files"]["d3d9.dll"], sha(CLIENT_V1["d3d9.dll"]))
        self.assertEqual(state["archive_sha256"], sha(self.v1.read_bytes()))
        # Nothing was overwritten, but the backup index still lists the added files.
        backups = install.list_backups(self.target())
        self.assertEqual(len(backups), 1)
        actions = {e["path"]: e["action"] for e in backups[0].index["entries"]}
        self.assertEqual(actions["d3d9.dll"], "added")
        self.assertEqual(backups[0].index["status"], "complete")
        # Uninstall returns the folder to exactly what it was.
        install.uninstall(self.target(), log=self.logs.append)
        self.assertEqual(snapshot(self.game), before)
        self.assertFalse((self.game / "vr").exists())

    def test_dry_run_changes_nothing(self):
        before = snapshot(self.game)
        plan = self.install(self.v1, dry_run=True)
        self.assertEqual(snapshot(self.game), before)
        self.assertFalse((self.game / install.BACKUP_DIR).exists())
        self.assertTrue(plan.by_action("add"))

    def test_no_pin_option(self):
        self.install(self.v1, pin_workshop=False)
        self.assertFalse((self.game / install.PIN_FILE).exists())

    # -- OpenXR-only release: old OpenVR files go ------------------------------
    def openxr_release(self):
        files = dict(CLIENT_V2)
        for name in ("openvr_api.dll", "vr/SteamVRActionManifest/action_manifest.json"):
            del files[name]
        files["openxr_loader.dll"] = b"MZ khronos loader"
        files["manifest.json"] = json.dumps({"version": "1.3.0"}).encode()
        return make_zip(self.tmp / "dl" / "openxr.zip", files)

    def test_upgrade_to_openxr_removes_recorded_openvr_files(self):
        self.install(self.v1)
        before = snapshot(self.game)
        plan = self.install(self.openxr_release())
        removed = {item.rel for item in plan.by_action("remove")}
        self.assertIn("openvr_api.dll", removed)
        self.assertIn("vr/SteamVRActionManifest/action_manifest.json", removed)
        self.assertFalse((self.game / "openvr_api.dll").exists())
        self.assertFalse((self.game / "vr/SteamVRActionManifest").exists(), "the emptied folder is removed")
        self.assertTrue((self.game / "openxr_loader.dll").is_file())
        self.assertTrue(any("OpenXR only" in warning for warning in plan.warnings))
        # Backed up first: restoring brings them (and the folder) back.
        install.restore_backup(self.target(), log=self.logs.append)
        self.assertEqual(snapshot(self.game), before)

    def test_upgrade_removes_unrecorded_and_modified_openvr_files(self):
        # A manual 1.2 install (not recorded) plus a user-replaced openvr_api.dll (OpenComposite)
        # and a SteamVR binding file exported next to the manifest.
        (self.game / "openvr_api.dll").write_bytes(b"MZ opencomposite")
        (self.game / "VR/SteamVRActionManifest").mkdir(parents=True)
        (self.game / "VR/SteamVRActionManifest/bindings_knuckles.json").write_bytes(b"{}")
        (self.game / "VR/manifest.vrmanifest").write_bytes(b"{}")
        (self.game / "VR/SteamVRActionManifest/readme.txt").write_bytes(b"not a json")
        before = snapshot(self.game)
        plan = self.install(self.openxr_release())
        removed = {item.rel.lower() for item in plan.by_action("remove")}
        self.assertEqual(removed, {"openvr_api.dll", "vr/manifest.vrmanifest",
                                   "vr/steamvractionmanifest/bindings_knuckles.json"})
        self.assertFalse((self.game / "openvr_api.dll").exists())
        self.assertTrue((self.game / "VR/SteamVRActionManifest/readme.txt").is_file(), "only the OpenVR json files go")
        backups = install.list_backups(self.target())
        entries = {e["path"].lower(): e for e in backups[0].index["entries"]}
        self.assertEqual(entries["openvr_api.dll"]["action"], "removed")
        backup_copy = backups[0].path / "files" / "openvr_api.dll"
        self.assertEqual(backup_copy.read_bytes(), b"MZ opencomposite")
        install.uninstall(self.target(), log=self.logs.append)
        self.assertEqual(snapshot(self.game), before)

    def test_recorded_but_modified_openvr_file_is_removed_with_backup(self):
        self.install(self.v1)
        (self.game / "openvr_api.dll").write_bytes(b"MZ edited")
        plan = self.install(self.openxr_release())
        self.assertIn("openvr_api.dll", {item.rel for item in plan.by_action("remove")})
        self.assertFalse(plan.by_action("keep-modified"))
        self.assertFalse((self.game / "openvr_api.dll").exists())

    def test_a_package_that_still_ships_openvr_keeps_it(self):
        (self.game / "openvr_api.dll").write_bytes(b"MZ old")
        plan = self.install(self.v2)  # a legacy package that ships openvr_api.dll
        self.assertIn("openvr_api.dll", {item.rel for item in plan.by_action("overwrite")})
        self.assertEqual((self.game / "openvr_api.dll").read_bytes(), CLIENT_V2["openvr_api.dll"])

    # -- preserve -------------------------------------------------------------
    def test_user_configs_are_preserved(self):
        files = dict(CLIENT_V1)
        files["vr/config.txt"] = b"packaged default"
        files["vr/weapon_calibration.txt"] = b"packaged calibration"
        files["scope_adjustments.txt"] = b"packaged scope"
        package = make_zip(self.tmp / "dl" / "cfg.zip", files)
        user_files = {
            "vr/config.txt": b"mine", "vr/config2.txt": b"mine2", "vr/config_migrations_applied.txt": b"m",
            "vr/weapon_calibration.txt": b"my calibration", "scope_adjustments.txt": b"my scope",
            "viewmodel_adjustments.txt": b"vm", "viewmodel_auto_grip_cache.txt": b"grip",
            "vr/my_custom_adjustments_v2.txt": b"adj",
        }
        for name, data in user_files.items():
            (self.game / name).parent.mkdir(parents=True, exist_ok=True)
            (self.game / name).write_bytes(data)
        plan = self.install(package)
        self.assertEqual({i.rel for i in plan.by_action("preserve")},
                         {"vr/config.txt", "vr/weapon_calibration.txt", "scope_adjustments.txt"})
        for name, data in user_files.items():
            self.assertEqual((self.game / name).read_bytes(), data, name)
        install.uninstall(self.target())
        for name, data in user_files.items():
            self.assertEqual((self.game / name).read_bytes(), data, name)

    def test_packaged_config_installed_when_absent(self):
        files = dict(CLIENT_V1, **{"vr/config.txt": b"default"})
        self.install(make_zip(self.tmp / "dl" / "cfg.zip", files))
        self.assertEqual((self.game / "vr/config.txt").read_bytes(), b"default")

    def test_extra_preserve_pattern_is_remembered(self):
        (self.game / "vr").mkdir()
        (self.game / "vr/haptics_config.txt").write_bytes(b"my haptics")
        install.install_package(self.target(extra_preserve=["vr/haptics_config.txt"]), self.v1, {})
        self.assertEqual((self.game / "vr/haptics_config.txt").read_bytes(), b"my haptics")
        self.assertIn("vr/haptics_config.txt", self.state()["user_preserve"])
        # A later run without the option still keeps it.
        self.install(self.v2)
        self.assertEqual((self.game / "vr/haptics_config.txt").read_bytes(), b"my haptics")

    def test_existing_uppercase_vr_folder_is_reused(self):
        (self.game / "VR").mkdir()
        (self.game / "VR" / "config.txt").write_bytes(b"mine")
        self.install(self.v1)
        self.assertNotIn("vr", os.listdir(self.game))  # no second, lower-case folder
        self.assertTrue((self.game / "VR" / "haptics_config.txt").is_file())
        self.assertEqual((self.game / "VR" / "config.txt").read_bytes(), b"mine")
        self.assertEqual(resolve_ci(self.game, "vr/installed_release.json"),
                         self.game / "VR" / "installed_release.json")

    # -- update / backup / restore ---------------------------------------------
    def test_overwrite_backs_up_original_files(self):
        (self.game / "d3d9.dll").write_bytes(b"old manual install")
        self.install(self.v1)
        backup = install.list_backups(self.target())[0]
        self.assertEqual((backup.path / "files" / "d3d9.dll").read_bytes(), b"old manual install")
        entry = next(e for e in backup.index["entries"] if e["path"] == "d3d9.dll")
        self.assertEqual(entry["action"], "overwritten")
        self.assertEqual(entry["original_sha256"], sha(b"old manual install"))
        install.uninstall(self.target())
        self.assertEqual((self.game / "d3d9.dll").read_bytes(), b"old manual install")

    def test_update_restore_round_trip(self):
        pristine = snapshot(self.game)
        self.install(self.v1, {"tag": "v1", "version": "1"})
        after_v1 = snapshot(self.game)
        (self.game / "vr" / "config.txt").write_bytes(b"created by the mod at runtime")
        plan = self.install(self.v2, {"tag": "v2", "version": "2"})
        self.assertEqual([i.rel for i in plan.by_action("remove")], ["vr/old_only_in_v1.txt"])
        self.assertFalse((self.game / "vr/old_only_in_v1.txt").exists())
        self.assertEqual((self.game / "d3d9.dll").read_bytes(), CLIENT_V2["d3d9.dll"])
        self.assertEqual(self.state()["tag"], "v2")
        self.assertEqual(self.state()["backups"][0], install.list_backups(self.target())[1].name)

        # Restore = undo the update only.
        install.restore_backup(self.target(), log=self.logs.append)
        expected = dict(after_v1, **{"vr/config.txt": b"created by the mod at runtime"})
        self.assertEqual(snapshot(self.game), expected)
        self.assertEqual(self.state()["tag"], "v1")
        # Uninstall undoes the rest; the mod-created config survives.
        install.uninstall(self.target())
        self.assertEqual(snapshot(self.game), dict(pristine, **{"vr/config.txt": b"created by the mod at runtime"}))
        with self.assertRaises(InstallerError):
            install.restore_backup(self.target())

    def test_update_keeps_user_modified_stale_file(self):
        self.install(self.v1)
        (self.game / "vr/old_only_in_v1.txt").write_bytes(b"edited")
        plan = self.install(self.v2)
        self.assertEqual([i.rel for i in plan.by_action("keep-modified")], ["vr/old_only_in_v1.txt"])
        self.assertEqual((self.game / "vr/old_only_in_v1.txt").read_bytes(), b"edited")

    def test_restore_refuses_to_clobber_user_edits_without_force(self):
        (self.game / "d3d9.dll").write_bytes(b"original")
        self.install(self.v1)
        (self.game / "d3d9.dll").write_bytes(b"user hot-swapped dll")
        with self.assertRaises(InstallerError):
            install.restore_backup(self.target())
        self.assertEqual((self.game / "d3d9.dll").read_bytes(), b"user hot-swapped dll")
        backup = install.restore_backup(self.target(), force=True)
        self.assertEqual((self.game / "d3d9.dll").read_bytes(), b"original")
        self.assertEqual((backup.path / "replaced-on-restore" / "d3d9.dll").read_bytes(), b"user hot-swapped dll")

    def test_uninstall_keeps_modified_added_files(self):
        self.install(self.v1)
        (self.game / "vr/haptics_config.txt").write_bytes(b"user tuned")
        install.uninstall(self.target())
        self.assertEqual((self.game / "vr/haptics_config.txt").read_bytes(), b"user tuned")
        self.assertFalse((self.game / "d3d9.dll").exists())

    def test_restore_dry_run(self):
        self.install(self.v1)
        before = snapshot(self.game)
        install.restore_backup(self.target(), dry_run=True)
        install.uninstall(self.target(), dry_run=True)
        self.assertEqual(snapshot(self.game), before)

    def test_reinstall_same_archive_is_noop(self):
        self.install(self.v1)
        count = len(install.list_backups(self.target()))
        self.install(self.v1)
        self.assertEqual(len(install.list_backups(self.target())), count)

    def test_failed_install_rolls_back(self):
        (self.game / "d3d9.dll").write_bytes(b"original")
        before = snapshot(self.game)
        real_write = install.atomic_write_bytes
        calls = {"n": 0}

        def flaky(path, data):
            calls["n"] += 1
            if calls["n"] == 4:
                raise InstallerError("disk full")
            return real_write(path, data)

        with mock.patch.object(install, "atomic_write_bytes", side_effect=flaky):
            with self.assertRaises(InstallerError):
                self.install(self.v1)
        self.assertEqual(snapshot(self.game), before)
        backup = install.list_backups(self.target())[0]
        self.assertEqual(backup.index["status"], "rolled-back")

    def test_status_reports_modified_and_missing(self):
        self.install(self.v1, {"tag": "v1", "version": "1.0.0", "source": "github"})
        (self.game / "d3d9.dll").write_bytes(b"changed")
        os.remove(self.game / "updatelog.txt")
        status = install.status(self.target())
        self.assertEqual(status.modified, ["d3d9.dll"])
        self.assertEqual(status.missing, ["updatelog.txt"])
        text = "\n".join(status.lines())
        self.assertIn("1.0.0", text)
        self.assertIn("1 restorable", text)


class PackageValidationTests(TempDirTest):
    def setUp(self):
        super().setUp()
        self.game = make_game(self.tmp / "lib")
        self.target = install.client_target(self.game)

    def test_wrapper_folder_is_stripped(self):
        package = install.open_package(make_zip(self.tmp / "w.zip", CLIENT_V1, prefix="l4d2vr-1.0/"), self.target)
        self.assertIn("d3d9.dll", package.members)

    def test_backslash_names_are_normalized(self):
        path = self.tmp / "bs.zip"
        with zipfile.ZipFile(path, "w") as archive:
            archive.writestr("d3d9.dll", b"x")
            archive.writestr("vr\\config.sample", b"y")
        self.assertIn("vr/config.sample", install.open_package(path, self.target).members)

    def test_zip_slip_rejected(self):
        for name in ("../evil.dll", "/etc/passwd", "C:/Windows/evil.dll", "vr/../../evil"):
            path = make_zip(self.tmp / "bad.zip", {"d3d9.dll": b"x", name: b"y"})
            with self.assertRaises(InstallerError, msg=name):
                install.open_package(path, self.target)

    def test_symlink_member_rejected(self):
        path = self.tmp / "link.zip"
        with zipfile.ZipFile(path, "w") as archive:
            archive.writestr("d3d9.dll", b"x")
            info = zipfile.ZipInfo("vr/link")
            info.external_attr = (0o120777 << 16)
            archive.writestr(info, "/etc/passwd")
        with self.assertRaises(InstallerError):
            install.open_package(path, self.target)

    def test_server_zip_rejected_for_client(self):
        path = make_zip(self.tmp / "s.zip", {"left4dead2/addons/x.vdf": b"x"})
        with self.assertRaises(InstallerError) as raised:
            install.open_package(path, self.target)
        self.assertIn("server zip", str(raised.exception))

    def test_core_game_file_rejected(self):
        path = make_zip(self.tmp / "c.zip", {"d3d9.dll": b"x", "left4dead2.exe": b"MZ"})
        with self.assertRaises(InstallerError):
            install.open_package(path, self.target)

    def test_not_a_zip(self):
        bad = self.tmp / "bad.zip"
        bad.write_bytes(b"not a zip")
        with self.assertRaises(InstallerError):
            install.open_package(bad, self.target)

    def test_symlinked_destination_rejected(self):
        outside = self.tmp / "outside.txt"
        outside.write_bytes(b"secret")
        (self.game / "d3d9.dll").symlink_to(outside)
        package = install.open_package(make_zip(self.tmp / "v.zip", CLIENT_V1), self.target)
        with self.assertRaises(InstallerError):
            install.plan_install(self.target, package)
        self.assertEqual(outside.read_bytes(), b"secret")

    def test_mod_version_from_manifest_or_version_txt(self):
        package = install.open_package(make_zip(self.tmp / "a.zip", CLIENT_V1), self.target)
        self.assertEqual(package.mod_version, "1.0.0")
        files = {"d3d9.dll": b"x", "vr/version.txt": b"1.2.3\n"}
        package = install.open_package(make_zip(self.tmp / "b.zip", files), self.target)
        self.assertEqual(package.mod_version, "1.2.3")


if __name__ == "__main__":
    unittest.main()
