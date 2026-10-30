import unittest
from pathlib import Path

from l4d2vr_installer import launchopts, vdf
from l4d2vr_installer.fsutil import InstallerError

from .helpers import TempDirTest

RECOMMENDED = 'WINEDLLOVERRIDES="d3d9=n,b" PROTON_USE_WOW64=1 %command% -dx9'
OLD_RECOMMENDED = 'WINEDLLOVERRIDES="d3d9,openvr_api=n,b" PROTON_USE_WOW64=1 %command% -dx9'


class MergeTests(unittest.TestCase):
    def test_empty_gives_recommended(self):
        self.assertEqual(launchopts.merge_launch_options("")[0], RECOMMENDED)
        self.assertEqual(launchopts.RECOMMENDED, RECOMMENDED)

    def test_idempotent(self):
        merged, _ = launchopts.merge_launch_options(RECOMMENDED)
        self.assertEqual(merged, RECOMMENDED)
        self.assertTrue(launchopts.has_required_options(RECOMMENDED))
        self.assertFalse(launchopts.has_required_options(""))
        self.assertFalse(launchopts.has_required_options("-novid"))

    def test_plain_game_args_are_kept(self):
        merged, _ = launchopts.merge_launch_options("-novid -high")
        self.assertEqual(merged, 'WINEDLLOVERRIDES="d3d9=n,b" PROTON_USE_WOW64=1 %command% -novid -high -dx9')

    def test_existing_overrides_and_wrappers_are_merged(self):
        merged, _ = launchopts.merge_launch_options('WINEDLLOVERRIDES="dxgi=n,b;d3d9=b" gamemoderun %command% -novid')
        self.assertEqual(
            merged,
            'WINEDLLOVERRIDES="dxgi=n,b;d3d9=n,b" PROTON_USE_WOW64=1 gamemoderun %command% -novid -dx9')

    def test_pre_openxr_options_are_upgraded(self):
        # 1.2.x wrote d3d9,openvr_api=n,b; the OpenXR-only release has no openvr_api.dll.
        self.assertFalse(launchopts.has_required_options(OLD_RECOMMENDED))
        merged, notes = launchopts.merge_launch_options(OLD_RECOMMENDED)
        self.assertEqual(merged, RECOMMENDED)
        self.assertTrue(any("openvr_api" in note for note in notes))
        merged, _ = launchopts.merge_launch_options('WINEDLLOVERRIDES="dxgi=n,b;d3d9,openvr_api=n,b" MANGOHUD=1 %command% -dx9')
        self.assertEqual(merged, 'WINEDLLOVERRIDES="dxgi=n,b;d3d9=n,b" MANGOHUD=1 PROTON_USE_WOW64=1 %command% -dx9')
        self.assertEqual(launchopts.strip_launch_options(OLD_RECOMMENDED), "")
        self.assertEqual(launchopts.strip_launch_options('WINEDLLOVERRIDES="dxgi=n,b;d3d9,openvr_api=n,b" %command% -dx9'),
                         'WINEDLLOVERRIDES="dxgi=n,b" %command%')

    def test_vulkan_is_removed(self):
        merged, notes = launchopts.merge_launch_options("%command% -vulkan")
        self.assertNotIn("-vulkan", merged)
        self.assertTrue(notes)

    def test_strip_reverses_merge(self):
        for original in ("-novid", "MANGOHUD=1 %command% -novid", 'WINEDLLOVERRIDES="dxgi=n,b" %command%'):
            merged, _ = launchopts.merge_launch_options(original)
            stripped = launchopts.strip_launch_options(merged)
            self.assertEqual(launchopts.strip_launch_options(stripped), stripped)
            self.assertNotIn("openvr_api", stripped)
            self.assertNotIn("PROTON_USE_WOW64", stripped)
        self.assertEqual(launchopts.strip_launch_options(RECOMMENDED), "")
        self.assertEqual(launchopts.strip_launch_options("MANGOHUD=1 " + RECOMMENDED.split(" ", 2)[2]),
                         "MANGOHUD=1 %command%")

    def test_unbalanced_quotes_raise(self):
        with self.assertRaises(InstallerError):
            launchopts.merge_launch_options('FOO="bar %command%')


LOCALCONFIG = '''"UserLocalConfigStore"
{
\t"friends"
\t{
\t\t"PersonaName"\t\t"Zoey"
\t}
\t"Software"
\t{
\t\t"valve"
\t\t{
\t\t\t"Steam"
\t\t\t{
\t\t\t\t"Apps"
\t\t\t\t{
\t\t\t\t\t"550"
\t\t\t\t\t{
\t\t\t\t\t\t"LastPlayed"\t\t"1700000000"
\t\t\t\t\t\t"LaunchOptions"\t\t"-novid"
\t\t\t\t\t}
\t\t\t\t\t"620"
\t\t\t\t\t{
\t\t\t\t\t\t"LaunchOptions"\t\t"-other"
\t\t\t\t\t}
\t\t\t\t}
\t\t\t}
\t\t}
\t}
}
'''


class LocalConfigTests(TempDirTest):
    def setUp(self):
        super().setUp()
        self.config = self.tmp / "userdata" / "123" / "config" / "localconfig.vdf"
        self.config.parent.mkdir(parents=True)
        self.config.write_text(LOCALCONFIG)
        self.state = self.tmp / "state"

    def test_apply_and_remove_round_trip(self):
        change = launchopts.apply_vr_launch_options(self.config, steam_running=False, state_dir=self.state)
        self.assertTrue(change.changed)
        self.assertTrue(change.backup.is_file())
        self.assertEqual(change.backup.read_text(), LOCALCONFIG)
        self.assertEqual(launchopts.read_launch_options(self.config), RECOMMENDED.replace("-dx9", "-novid -dx9"))
        data = vdf.load(self.config)
        # Other apps, other sections and key case are untouched.
        self.assertEqual(data.get_path("UserLocalConfigStore", "Software", "Valve", "Steam", "apps", "620",
                                       "LaunchOptions"), "-other")
        self.assertIn('"valve"', self.config.read_text())
        self.assertIn('"Apps"', self.config.read_text())
        # Applying again changes nothing.
        self.assertFalse(launchopts.apply_vr_launch_options(self.config, steam_running=False,
                                                            state_dir=self.state).changed)
        removed = launchopts.remove_vr_launch_options(self.config, steam_running=False, state_dir=self.state)
        self.assertEqual(removed.after, "-novid")
        self.assertEqual(self.config.read_text(), LOCALCONFIG)

    def test_creates_app_entry_when_missing(self):
        self.config.write_text('"UserLocalConfigStore"\n{\n}\n')
        launchopts.apply_vr_launch_options(self.config, steam_running=False)
        self.assertEqual(launchopts.read_launch_options(self.config), RECOMMENDED)

    def test_refuses_while_steam_running(self):
        with self.assertRaises(InstallerError):
            launchopts.apply_vr_launch_options(self.config, steam_running=True)
        self.assertEqual(self.config.read_text(), LOCALCONFIG)

    def test_dry_run_changes_nothing(self):
        change = launchopts.apply_vr_launch_options(self.config, steam_running=True, dry_run=True)
        self.assertTrue(change.changed)
        self.assertEqual(self.config.read_text(), LOCALCONFIG)
        self.assertEqual(list(self.config.parent.iterdir()), [self.config])

    def test_rejects_non_localconfig(self):
        self.config.write_text('"libraryfolders"\n{\n}\n')
        with self.assertRaises(InstallerError):
            launchopts.apply_vr_launch_options(self.config, steam_running=False)

    def test_remove_strips_when_user_edited_afterwards(self):
        launchopts.apply_vr_launch_options(self.config, steam_running=False, state_dir=self.state)
        launchopts.write_launch_options(self.config, RECOMMENDED + " -high", steam_running=False)
        launchopts.remove_vr_launch_options(self.config, steam_running=False, state_dir=self.state)
        self.assertEqual(launchopts.read_launch_options(self.config), "-high")


if __name__ == "__main__":
    unittest.main()
