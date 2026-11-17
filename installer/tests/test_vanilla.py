"""Play without the mod (VAC-safe launch), installer 1.1.5 (vanilla.py)."""

import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

from l4d2vr_installer import cli, launchopts, vanilla
from l4d2vr_installer.fsutil import InstallerError

PRINT_ARGS = [sys.executable, "-c",
              "import os, sys, json; print(json.dumps([os.environ.get('WINEDLLOVERRIDES'), sys.argv[1:]]))"]


class Temp(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="l4d2vr-vanilla-"))

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)


@unittest.skipIf(sys.platform.startswith("win") or not shutil.which("sh"), "POSIX shell wrapper")
class WrapperScriptTests(Temp):
    def run_wrapper(self, overrides, args):
        wrapper = vanilla.install_wrapper(self.tmp)
        env = dict(os.environ)
        if overrides is None:
            env.pop("WINEDLLOVERRIDES", None)
        else:
            env["WINEDLLOVERRIDES"] = overrides
        out = subprocess.run([str(wrapper)] + PRINT_ARGS + args, env=env, capture_output=True, text=True, check=True)
        return json.loads(out.stdout)

    def test_vr_mode_leaves_everything(self):
        vanilla.write_mode(self.tmp, "vr")
        overrides, args = self.run_wrapper("d3d9=n,b", ["-dx9", "-insecure", "-novid"])
        self.assertEqual(overrides, "d3d9=n,b")
        self.assertEqual(args, ["-dx9", "-insecure", "-novid"])

    def test_no_mode_file_is_vr(self):
        overrides, args = self.run_wrapper("d3d9=n,b", ["-insecure"])
        self.assertEqual((overrides, args), ("d3d9=n,b", ["-insecure"]))

    def test_vanilla_uses_builtin_d3d9_and_drops_insecure_once(self):
        vanilla.write_mode(self.tmp, "vanilla")
        overrides, args = self.run_wrapper("dxgi=n,b;d3d9=n,b", ["-dx9", "-insecure", "-novid", "+map x"])
        self.assertEqual(overrides, "dxgi=n,b;d3d9=b")
        self.assertEqual(args, ["-dx9", "-novid", "+map x"])
        # One launch only.
        self.assertEqual(vanilla.read_mode(self.tmp), "vr")
        overrides, args = self.run_wrapper("d3d9=n,b", ["-insecure"])
        self.assertEqual((overrides, args), ("d3d9=n,b", ["-insecure"]))

    def test_vanilla_without_overrides(self):
        vanilla.write_mode(self.tmp, "vanilla")
        overrides, args = self.run_wrapper(None, ["-insecure"])
        self.assertEqual((overrides, args), ("d3d9=b", []))

    def test_stale_vanilla_request_is_vr(self):
        vanilla.write_mode(self.tmp, "vanilla", now=time.time() - vanilla.VANILLA_WINDOW_SECONDS - 60)
        self.assertEqual(vanilla.read_mode(self.tmp), "vr")
        overrides, args = self.run_wrapper("d3d9=n,b", ["-insecure"])
        self.assertEqual((overrides, args), ("d3d9=n,b", ["-insecure"]))


class LaunchOptionsTests(unittest.TestCase):
    WRAPPER = Path("/home/u/.config/l4d2vr-installer/l4d2vr-steam-wrapper.sh")

    def test_wrapper_goes_in_front_of_command(self):
        merged = launchopts.merge_launch_options("MANGOHUD=1 gamemoderun %command% -novid")[0]
        out = vanilla.with_wrapper(merged, self.WRAPPER)
        self.assertEqual(out, 'MANGOHUD=1 WINEDLLOVERRIDES="d3d9=n,b" PROTON_USE_WOW64=1 gamemoderun '
                         + str(self.WRAPPER) + " %command% -novid -dx9 -insecure")
        self.assertTrue(vanilla.has_wrapper(out))
        self.assertEqual(vanilla.with_wrapper(out, self.WRAPPER), out)  # idempotent
        self.assertTrue(launchopts.has_required_options(out))  # still counts as VR-ready

    def test_wrapper_with_space_is_quoted(self):
        out = vanilla.with_wrapper(launchopts.RECOMMENDED, Path("/home/a b/l4d2vr-steam-wrapper.sh"))
        self.assertIn("'/home/a b/l4d2vr-steam-wrapper.sh' %command%", out)
        self.assertTrue(vanilla.has_wrapper(out))

    def test_remove_strips_the_wrapper(self):
        out = vanilla.with_wrapper(launchopts.merge_launch_options("-novid")[0], self.WRAPPER)
        self.assertEqual(launchopts.strip_launch_options(out), "-novid")
        self.assertFalse(vanilla.has_wrapper(vanilla.without_wrapper(out)))

    def test_has_insecure(self):
        self.assertTrue(vanilla.has_insecure("-novid -insecure"))
        self.assertTrue(vanilla.has_insecure(launchopts.RECOMMENDED))
        self.assertFalse(vanilla.has_insecure("-novid"))
        self.assertFalse(vanilla.has_insecure("INSECURE=-insecure %command% -novid"))

    def test_vanilla_url_has_no_arguments(self):
        self.assertEqual(vanilla.steam_url(), "steam://rungameid/550")


class WindowsRenameTests(Temp):
    def setUp(self):
        super().setUp()
        self.game = self.tmp / "Left 4 Dead 2"
        self.state = self.tmp / "state"
        self.game.mkdir()
        (self.game / "d3d9.dll").write_bytes(b"mod d3d9")
        (self.game / "openxr_loader.dll").write_bytes(b"loader")
        (self.game / "left4dead2.exe").write_bytes(b"game")

    def test_disable_and_restore(self):
        done = vanilla.disable_mod_files(self.game, self.state, log=lambda _m: None)
        self.assertEqual(sorted(done), ["d3d9.dll", "openxr_loader.dll"])
        self.assertFalse((self.game / "d3d9.dll").exists())
        self.assertEqual((self.game / "d3d9.dll.l4d2vr-off").read_bytes(), b"mod d3d9")
        self.assertTrue(vanilla.pending_restore(self.state))
        result = vanilla.restore_mod_files(self.state, log=lambda _m: None)
        self.assertTrue(result.complete)
        self.assertEqual(sorted(result.restored), ["d3d9.dll", "openxr_loader.dll"])
        self.assertEqual((self.game / "d3d9.dll").read_bytes(), b"mod d3d9")
        self.assertIsNone(vanilla.pending_restore(self.state))

    def test_crash_between_state_and_rename(self):
        # The state lists a file that was never renamed: restoring finds it in place.
        self.state.mkdir()
        (self.state / vanilla.STATE_NAME).write_text(json.dumps(
            {"game_dir": str(self.game), "renamed": ["d3d9.dll"], "started": 1}))
        result = vanilla.restore_mod_files(self.state, log=lambda _m: None)
        self.assertEqual(result.restored, ["d3d9.dll"])
        self.assertIsNone(vanilla.pending_restore(self.state))

    def test_both_names_exist_keeps_both(self):
        vanilla.disable_mod_files(self.game, self.state, log=lambda _m: None)
        (self.game / "d3d9.dll").write_bytes(b"reinstalled")
        result = vanilla.restore_mod_files(self.state, log=lambda _m: None)
        self.assertEqual(result.conflicts, ["d3d9.dll"])
        self.assertEqual((self.game / "d3d9.dll").read_bytes(), b"reinstalled")
        self.assertTrue((self.game / "d3d9.dll.l4d2vr-off").exists())
        self.assertTrue(vanilla.pending_restore(self.state))  # still listed until resolved
        with self.assertRaises(InstallerError):
            vanilla.prepare_vr_launch(windows=True, state_dir=self.state, game_running=lambda: False,
                                      log=lambda _m: None)

    def test_missing_file_is_reported(self):
        vanilla.disable_mod_files(self.game, self.state, log=lambda _m: None)
        (self.game / "openxr_loader.dll.l4d2vr-off").unlink()
        result = vanilla.restore_mod_files(self.state, log=lambda _m: None)
        self.assertEqual(result.missing, ["openxr_loader.dll"])
        self.assertEqual(result.restored, ["d3d9.dll"])

    def test_auto_restore_waits_for_the_game(self):
        vanilla.disable_mod_files(self.game, self.state, log=lambda _m: None)
        self.assertIsNone(vanilla.auto_restore(self.state, lambda: True, log=lambda _m: None))
        self.assertFalse((self.game / "d3d9.dll").exists())
        self.assertTrue(vanilla.auto_restore(self.state, lambda: False, log=lambda _m: None).complete)
        self.assertTrue((self.game / "d3d9.dll").exists())

    def test_existing_off_file_refuses(self):
        (self.game / "d3d9.dll.l4d2vr-off").write_bytes(b"old")
        with self.assertRaises(InstallerError):
            vanilla.disable_mod_files(self.game, self.state, log=lambda _m: None)
        self.assertTrue((self.game / "d3d9.dll").exists())

    def test_dry_run_changes_nothing(self):
        vanilla.disable_mod_files(self.game, self.state, dry_run=True, log=lambda _m: None)
        self.assertTrue((self.game / "d3d9.dll").exists())
        self.assertIsNone(vanilla.pending_restore(self.state))


class PlayVanillaTests(Temp):
    def setUp(self):
        super().setUp()
        self.game = self.tmp / "game"
        self.game.mkdir()
        (self.game / "d3d9.dll").write_bytes(b"mod")
        self.state = self.tmp / "state"
        self.options = {"cfg": launchopts.RECOMMENDED}
        self.opened = []
        self.written = []

    def play(self, *, windows, steam_running=False, game_running=False):
        logs = []
        vanilla.play_vanilla(
            self.game, windows=windows, state_dir=self.state, steam_running=lambda: steam_running,
            game_running=lambda: game_running, open_url=self.opened.append, localconfigs=[Path("cfg")],
            read_options=lambda p: self.options[str(p)],
            write_options=lambda p, v: (self.written.append(v), self.options.__setitem__(str(p), v)),
            merge_vr_options=lambda t: launchopts.merge_launch_options(t)[0], log=logs.append)
        return logs

    def test_game_running_refuses(self):
        with self.assertRaises(InstallerError):
            self.play(windows=False, game_running=True)
        self.assertEqual(self.opened, [])

    def test_linux_first_time_needs_steam_closed(self):
        with self.assertRaises(InstallerError):
            self.play(windows=False, steam_running=True)
        self.assertEqual(self.written, [])
        self.play(windows=False)  # Steam closed: launch options get the wrapper, nothing starts yet
        self.assertTrue(vanilla.has_wrapper(self.options["cfg"]))
        self.assertEqual(self.opened, [])
        self.assertEqual(vanilla.read_mode(self.state), "vr")

    def test_linux_with_wrapper_starts_vanilla(self):
        self.options["cfg"] = vanilla.with_wrapper(launchopts.RECOMMENDED, vanilla.wrapper_path(self.state))
        self.play(windows=False, steam_running=True)
        self.assertEqual(self.opened, ["steam://rungameid/550"])
        self.assertEqual(vanilla.read_mode(self.state), "vanilla")
        self.assertTrue((self.game / "d3d9.dll").exists())  # nothing renamed on Linux
        vanilla.prepare_vr_launch(windows=False, state_dir=self.state, game_running=lambda: False,
                                  log=lambda _m: None)
        self.assertEqual(vanilla.read_mode(self.state), "vr")

    def test_windows_renames_and_warns_about_insecure(self):
        self.options["cfg"] = "-novid -insecure"
        logs = self.play(windows=True, steam_running=True)
        self.assertFalse((self.game / "d3d9.dll").exists())
        self.assertEqual(self.opened, ["steam://rungameid/550"])
        self.assertTrue(any("-insecure" in line and "Remove" in line for line in logs))
        self.assertEqual(self.written, [])  # never edits the saved launch options on Windows
        vanilla.prepare_vr_launch(windows=True, state_dir=self.state, game_running=lambda: False,
                                  log=lambda _m: None)
        self.assertTrue((self.game / "d3d9.dll").exists())


class CliTests(unittest.TestCase):
    def test_commands_exist(self):
        parser = cli.build_parser()
        self.assertEqual(parser.parse_args(["play-vanilla", "--dry-run"]).command, "play-vanilla")
        self.assertEqual(parser.parse_args(["restore-mod"]).command, "restore-mod")
        self.assertIn("play-vanilla", cli.COMMANDS)
        self.assertIn("restore-mod", cli.COMMANDS)


if __name__ == "__main__":
    unittest.main()
