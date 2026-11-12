import unittest
from pathlib import Path

from l4d2vr_installer import launchopts, vdf
from l4d2vr_installer.fsutil import InstallerError

from .helpers import TempDirTest

RECOMMENDED = 'WINEDLLOVERRIDES="d3d9=n,b" PROTON_USE_WOW64=1 %command% -dx9 -insecure'
OLD_RECOMMENDED = 'WINEDLLOVERRIDES="d3d9,openvr_api=n,b" PROTON_USE_WOW64=1 %command% -dx9 -insecure'


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
        self.assertEqual(merged, 'WINEDLLOVERRIDES="d3d9=n,b" PROTON_USE_WOW64=1 %command% -novid -high -dx9 -insecure')

    def test_existing_overrides_and_wrappers_are_merged(self):
        merged, _ = launchopts.merge_launch_options('WINEDLLOVERRIDES="dxgi=n,b;d3d9=b" gamemoderun %command% -novid')
        self.assertEqual(
            merged,
            'WINEDLLOVERRIDES="dxgi=n,b;d3d9=n,b" PROTON_USE_WOW64=1 gamemoderun %command% -novid -dx9 -insecure')

    def test_pre_openxr_options_are_upgraded(self):
        # 1.2.x wrote d3d9,openvr_api=n,b; the OpenXR-only release has no openvr_api.dll.
        self.assertFalse(launchopts.has_required_options(OLD_RECOMMENDED))
        merged, notes = launchopts.merge_launch_options(OLD_RECOMMENDED)
        self.assertEqual(merged, RECOMMENDED)
        self.assertTrue(any("openvr_api" in note for note in notes))
        merged, _ = launchopts.merge_launch_options('WINEDLLOVERRIDES="dxgi=n,b;d3d9,openvr_api=n,b" MANGOHUD=1 %command% -dx9')
        self.assertEqual(merged, 'WINEDLLOVERRIDES="dxgi=n,b;d3d9=n,b" MANGOHUD=1 PROTON_USE_WOW64=1 %command% -dx9 -insecure')
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


class RawPreservationTests(unittest.TestCase):
    """The user's own words are never re-quoted (review B40)."""

    CASES = [
        'LD_PRELOAD="$LD_PRELOAD:/usr/lib/libgamemodeauto.so.0" %command%',
        'PATH="$HOME/bin:$PATH" gamemoderun %command%',
        '~/bin/wrap %command%',
        "MANGOHUD_CONFIG='fps_limit=90' mangohud %command% -novid",
        'gamemoderun %command% -novid | tee "$HOME/l4d2.log"',
    ]

    def test_user_words_survive_byte_for_byte(self):
        for text in self.CASES:
            merged, _ = launchopts.merge_launch_options(text)
            for word in text.replace("%command%", " ").split():
                self.assertIn(word, merged, text)
            self.assertNotIn("\\$", merged)
            self.assertNotIn("'~", merged)

    def test_apply_twice_and_remove_are_stable(self):
        for text in self.CASES + ['%command%; echo done']:
            merged, _ = launchopts.merge_launch_options(text)
            self.assertEqual(launchopts.merge_launch_options(merged)[0], merged, text)
            self.assertTrue(launchopts.has_required_options(merged), text)
            stripped = launchopts.strip_launch_options(merged)
            self.assertEqual(launchopts.merge_launch_options(stripped)[0], merged, text)

    def test_game_args_go_before_shell_syntax(self):
        merged, _ = launchopts.merge_launch_options("%command% -novid; echo done")
        self.assertTrue(merged.endswith("%command% -dx9 -insecure -novid; echo done") or
                        merged.endswith("%command% -novid -dx9 -insecure; echo done") or
                        "-dx9 -insecure" in merged.split(";")[0], merged)
        self.assertIn("%command%", launchopts.strip_launch_options("%command%; echo done"))

    def test_variable_in_overrides_is_kept(self):
        merged, _ = launchopts.merge_launch_options('WINEDLLOVERRIDES="$WINEDLLOVERRIDES;dxgi=n,b" %command%')
        self.assertTrue(merged.startswith('WINEDLLOVERRIDES="$WINEDLLOVERRIDES;dxgi=n,b;d3d9=n,b" '), merged)


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


class NonUtf8LocalConfigTests(TempDirTest):
    def test_invalid_utf8_and_crlf_round_trip(self):
        config = self.tmp / "localconfig.vdf"
        raw = LOCALCONFIG.replace("\n", "\r\n").encode().replace(b"-other", b"-other \xff\xfe")
        config.write_bytes(raw)
        launchopts.apply_vr_launch_options(config, steam_running=False)
        data = config.read_bytes()
        self.assertIn(b"\xff\xfe", data)
        self.assertNotIn(b"\n", data.replace(b"\r\n", b""))
        launchopts.remove_vr_launch_options(config, steam_running=False)
        self.assertEqual(config.read_bytes(), raw)
