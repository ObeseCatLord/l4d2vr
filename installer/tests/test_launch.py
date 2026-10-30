import json
import os
import socket
import stat
import sys
import unittest
from pathlib import Path
from unittest import mock

from l4d2vr_installer import shortcut, vrlaunch
from l4d2vr_installer.fsutil import InstallerError

from .helpers import TempDirTest, make_game, make_zip, sha


def pe32() -> bytes:
    header = bytearray(64)
    header[:2] = b"MZ"
    header[0x3C:0x40] = (64).to_bytes(4, "little")
    return bytes(header) + b"PE\0\0L\x01" + b"\0" * 32


def executable(path: Path, text: str = "#!/bin/sh\nexit 0\n") -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)
    path.chmod(0o755)
    return path


class SteamUrlTests(unittest.TestCase):
    def test_plain_and_connect_urls(self):
        self.assertEqual(vrlaunch.steam_url(), "steam://rungameid/550")
        url = vrlaunch.steam_url(vrlaunch.connect_args("1.2.3.4:27015", "p w"))
        self.assertEqual(url, "steam://run/550//+password%20p%20w%20+connect%201.2.3.4:27015/")
        self.assertEqual(vrlaunch.connect_args(None, "x"), [])

    def test_redact(self):
        self.assertEqual(vrlaunch.redact(["a", "+password", "secret", "+connect", "h"]),
                         ["a", "+password", "****", "+connect", "h"])


@unittest.skipIf(sys.platform.startswith("win"), "Monado route is Linux-only")
class MonadoTests(TempDirTest):
    def setUp(self):
        super().setUp()
        self.steam = self.home / ".local/share/Steam"
        (self.steam / "steamapps").mkdir(parents=True)
        self.game = make_game(self.steam)
        (self.game / "left4dead2.exe").write_bytes(pe32())
        (self.game / "d3d9.dll").write_bytes(b"MZ")
        (self.game / "openxr_loader.dll").write_bytes(b"MZ")
        self.proton = self.steam / "steamapps/common/Proton Hotfix"
        executable(self.proton / "proton")
        wineopenxr32 = self.proton / "files/lib/wine/i386-windows/wineopenxr.dll"
        wineopenxr32.parent.mkdir(parents=True)
        wineopenxr32.write_bytes(b"MZ")
        executable(self.steam / "steamapps/common/Proton 9.0/proton")
        self.entry = executable(self.steam / "steamapps/common/SteamLinuxRuntime_4/_v2-entry-point")
        envision = self.home / ".local/share/envision/prefixes/abc/share/openxr/1/openxr_monado.json"
        envision.parent.mkdir(parents=True)
        envision.write_text("{}")
        active = self.home / ".config/openxr/1/active_runtime.json"
        active.parent.mkdir(parents=True)
        active.symlink_to(envision)
        self.manifest = envision
        self.xrizer = self.tmp / "xrizer"
        (self.xrizer / "bin/linux64").mkdir(parents=True)
        (self.xrizer / "libxrizer.so").write_bytes(b"lib")
        (self.xrizer / "bin/linux64/vrclient.so").write_bytes(b"client")
        (self.xrizer / "SHA256SUMS").write_text(f"{sha(b'lib')}  libxrizer.so\n{sha(b'client')}  bin/linux64/vrclient.so\n")
        self.sock = self.steam / "steam.sock"
        self.server = socket.socket(socket.AF_UNIX)
        self.server.bind(str(self.sock))
        self.addCleanup(self.server.close)

    def config(self):
        config = vrlaunch.MonadoConfig(game_dir=str(self.game), xrizer_dir=str(self.xrizer),
                                       state_dir=str(self.tmp / "state"))
        return vrlaunch.autodetect(config, [self.steam], self.steam, home=self.home)

    def test_autodetect_prefers_hotfix_and_active_runtime(self):
        config = self.config()
        self.assertEqual(Path(config.proton_dir), self.proton)
        self.assertEqual(Path(config.steam_runtime_entrypoint), self.entry)
        self.assertEqual(Path(config.openxr_manifest), self.manifest.resolve())
        self.assertEqual(Path(config.steam_dir), self.steam)

    def test_envision_selected_profile_preferred(self):
        os.remove(self.home / ".config/openxr/1/active_runtime.json")
        prefix = self.home / ".local/share/envision/prefixes/zzz"
        chosen = prefix / "share/openxr/1/openxr_monado.json"
        chosen.parent.mkdir(parents=True)
        chosen.write_text("{}")
        envision = self.home / ".config/envision/envision.json"
        envision.parent.mkdir(parents=True)
        envision.write_text(json.dumps({"selected_profile_uuid": "zzz",
                                        "user_profiles": [{"uuid": "zzz", "prefix": str(prefix)}]}))
        self.assertEqual(vrlaunch.detect_openxr_manifests(self.home)[0], chosen.resolve())

    def test_validate_passes_and_reports_problems(self):
        problems, warnings = vrlaunch.validate(self.config(), lambda: False)
        self.assertEqual(problems, [])
        self.assertTrue(any("Monado socket" in w for w in warnings))
        (self.xrizer / "libxrizer.so").write_bytes(b"tampered")
        os.remove(self.game / "d3d9.dll")
        problems, _ = vrlaunch.validate(self.config(), lambda: False)
        self.assertTrue(any("SHA256SUMS" in p for p in problems))
        self.assertTrue(any("d3d9.dll" in p for p in problems))
        self.assertTrue(vrlaunch.validate(vrlaunch.MonadoConfig(), lambda: False)[0])

    def test_validate_needs_openxr_loader_and_32bit_openxr_proton(self):
        os.remove(self.game / "openxr_loader.dll")
        problems, _ = vrlaunch.validate(self.config(), lambda: False)
        self.assertTrue(any("openxr_loader.dll" in p for p in problems))
        (self.game / "openxr_loader.dll").write_bytes(b"MZ")
        config = self.config()
        config.proton_dir = str(self.steam / "steamapps/common/Proton 9.0")
        problems, _ = vrlaunch.validate(config, lambda: False)
        self.assertTrue(any("no 32-bit OpenXR" in p and "Proton Hotfix" in p for p in problems))
        # The old OpenVR DLL is not needed any more.
        self.assertFalse(any("openvr_api" in p for p in vrlaunch.validate(self.config(), lambda: False)[0]))

    def test_autodetect_prefers_a_proton_with_32bit_openxr(self):
        os.remove(self.proton / "files/lib/wine/i386-windows/wineopenxr.dll")
        experimental = self.steam / "steamapps/common/Proton - Experimental"
        executable(experimental / "proton")
        (experimental / "files/lib/wine/i386-windows").mkdir(parents=True)
        (experimental / "files/lib/wine/i386-windows/wineopenxr.dll").write_bytes(b"MZ")
        self.assertEqual(vrlaunch.detect_proton([self.steam])[0], experimental)

    def test_build_command_matches_handoff_launcher(self):
        command, env = vrlaunch.build_command(self.config(), "1.2.3.4:1", "pw", ["-extra"], base_env={})
        self.assertEqual(command[:6], [str(self.entry), "--verb=run", "--", str(self.proton / "proton"),
                                       "waitforexitandrun", str(self.game / "left4dead2.exe")])
        self.assertIn("-dx9", command)
        self.assertEqual(command[-5:], ["+password", "pw", "+connect", "1.2.3.4:1", "-extra"])
        self.assertEqual(env["WINEDLLOVERRIDES"], "d3d9=n,b")
        self.assertEqual(env["PROTON_USE_WOW64"], "1")
        self.assertEqual(env["SteamAppId"], "550")
        self.assertEqual(env["VR_OVERRIDE"], str(self.xrizer))
        self.assertEqual(env["XR_RUNTIME_JSON"], str(self.manifest.resolve()))
        self.assertEqual(env["STEAM_COMPAT_INSTALL_PATH"], str(self.game))
        self.assertIn(str(self.steam), env["STEAM_COMPAT_LIBRARY_PATHS"])

    def test_launch_runs_command_and_writes_logs(self):
        marker = self.tmp / "ran.txt"
        executable(self.entry, f'#!/bin/sh\necho "$@" > "{marker}"\necho "$VR_OVERRIDE" >> "{marker}"\n')
        logs = []
        code = vrlaunch.launch_monado(self.config(), log=logs.append, game_running=lambda: False)
        self.assertEqual(code, 0)
        text = marker.read_text()
        self.assertIn("waitforexitandrun", text)
        self.assertIn(str(self.xrizer), text)
        runs = list((self.tmp / "state" / "runs").iterdir())
        self.assertEqual(len(runs), 1)
        self.assertIn("libxrizer.so", (runs[0] / "binary-hashes.txt").read_text())
        registry = self.home / ".config/openvr/openvrpaths.vrpath"
        self.assertEqual(json.loads(registry.read_text())["runtime"], [str(self.xrizer)])

    def test_launch_dry_run_and_refusals(self):
        logs = []
        vrlaunch.launch_monado(self.config(), "h:1", "secret", dry_run=True, log=logs.append)
        self.assertFalse((self.tmp / "state").exists())
        self.assertFalse(any("secret" in line for line in logs))
        with self.assertRaises(InstallerError):
            vrlaunch.launch_monado(self.config(), game_running=lambda: True, log=logs.append)
        self.server.close()
        os.remove(self.sock)
        with self.assertRaises(InstallerError) as raised:
            vrlaunch.launch_monado(self.config(), log=logs.append, steam_running=lambda: False)
        self.assertIn("Steam does not seem to be running", str(raised.exception))
        # A running Steam process is enough even without the socket (64-bit client).
        self.assertEqual(vrlaunch.validate(self.config(), lambda: True)[0], [])

    def test_existing_openvr_registry_untouched(self):
        registry = self.home / ".config/openvr/openvrpaths.vrpath"
        registry.parent.mkdir(parents=True)
        registry.write_text("keep")
        self.assertIsNone(vrlaunch.ensure_openvr_paths(self.xrizer))
        self.assertEqual(registry.read_text(), "keep")

    def test_xrizer_zip_install(self):
        package = make_zip(self.tmp / "x.zip", {"runtime/libxrizer.so": b"lib", "runtime/bin/linux64/vrclient.so": b"c"})
        dest = self.tmp / "out"
        vrlaunch.install_xrizer_zip(package, dest, log=lambda _m: None)
        self.assertEqual((dest / "libxrizer.so").read_bytes(), b"lib")
        self.assertTrue((dest / "bin/linux64/vrclient.so").stat().st_mode & stat.S_IXUSR)
        with self.assertRaises(InstallerError):
            vrlaunch.install_xrizer_zip(make_zip(self.tmp / "bad.zip", {"x": b"1"}), self.tmp / "o2",
                                        log=lambda _m: None)


class ShortcutTests(TempDirTest):
    def test_desktop_entry_quoting(self):
        text = shortcut.desktop_entry(["/opt/my games/l4d2vr-installer", "launch", "--game-dir", "/a/100%"])
        exec_line = next(line for line in text.splitlines() if line.startswith("Exec="))
        self.assertEqual(exec_line, 'Exec="/opt/my games/l4d2vr-installer" launch --game-dir /a/100%%')

    @unittest.skipIf(sys.platform.startswith("win"), "Linux desktop entries need POSIX permissions")
    def test_linux_shortcut_written(self):
        paths = shortcut.install_linux_shortcut(["xdg-open", "steam://rungameid/550"],
                                                desktop_dir=self.tmp / "Desktop")
        self.assertEqual(len(paths), 2)
        for path in paths:
            self.assertIn("Exec=xdg-open steam://rungameid/550", path.read_text())
            self.assertTrue(path.stat().st_mode & stat.S_IXUSR)

    def test_windows_url_shortcut(self):
        paths = shortcut.install_windows_shortcut("steam://rungameid/550", Path("C:/Games/L4D2"),
                                                  desktop_dir=self.tmp)
        text = paths[0].read_text()
        self.assertIn("URL=steam://rungameid/550", text)
        self.assertIn("left4dead2.exe", text)

    def test_xdg_desktop_dir(self):
        config = self.home / ".config"
        config.mkdir(parents=True, exist_ok=True)
        (config / "user-dirs.dirs").write_text('XDG_DESKTOP_DIR="$HOME/Schreibtisch"\n')
        with mock.patch.object(Path, "home", return_value=self.home):
            self.assertEqual(shortcut.linux_desktop_dir(), self.home / "Schreibtisch")


if __name__ == "__main__":
    unittest.main()
