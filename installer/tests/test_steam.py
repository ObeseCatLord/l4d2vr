from pathlib import Path

from l4d2vr_installer import steam
from l4d2vr_installer.fsutil import InstallerError

from .helpers import TempDirTest, make_game


def write(path: Path, text: str) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)
    return path


class SteamDiscoveryTests(TempDirTest):
    def make_steam(self, root: Path, libraries=()):
        (root / "steamapps").mkdir(parents=True, exist_ok=True)
        entries = "".join(f'\t"{i + 1}"\n\t{{\n\t\t"path"\t\t"{lib}"\n\t}}\n' for i, lib in enumerate(libraries))
        write(root / "steamapps" / "libraryfolders.vdf",
              f'"libraryfolders"\n{{\n\t"0"\n\t{{\n\t\t"path"\t\t"{root}"\n\t}}\n{entries}}}\n')
        return root

    def test_linux_candidates_include_flatpak_and_snap(self):
        candidates = steam.steam_root_candidates(home=self.home, platform="linux")
        self.assertIn(self.home / ".local/share/Steam", candidates)
        self.assertIn(self.home / ".steam/steam", candidates)
        self.assertIn(self.home / ".var/app/com.valvesoftware.Steam/.local/share/Steam", candidates)

    def test_windows_candidates_use_registry_first(self):
        candidates = steam.steam_root_candidates(platform="win32", registry=lambda: [Path("E:/Steam")])
        self.assertEqual(candidates[0], Path("E:/Steam"))

    def test_finds_game_in_secondary_library(self):
        root = self.make_steam(self.home / ".local/share/Steam", [self.tmp / "lib2"])
        game = make_game(self.tmp / "lib2")
        roots = steam.find_steam_roots(steam.steam_root_candidates(home=self.home, platform="linux"))
        self.assertEqual(roots, [root])
        installs = steam.find_game_installs(roots)
        self.assertEqual([i.path for i in installs], [game])
        self.assertEqual(installs[0].depot, "windows")
        self.assertEqual(steam.steam_root_for_game(game, roots), root)

    def test_symlinked_roots_are_deduplicated(self):
        root = self.make_steam(self.home / ".local/share/Steam")
        (self.home / ".steam").mkdir()
        (self.home / ".steam/steam").symlink_to(root)
        roots = steam.find_steam_roots(steam.steam_root_candidates(home=self.home, platform="linux"))
        self.assertEqual(len(roots), 1)

    def test_appmanifest_installdir_is_used(self):
        root = self.make_steam(self.tmp / "Steam")
        write(root / "steamapps" / "appmanifest_550.acf",
              '"AppState"\n{\n\t"appid"\t\t"550"\n\t"installdir"\t\t"L4D2 Custom"\n}\n')
        game = root / "steamapps" / "common" / "L4D2 Custom"
        game.mkdir(parents=True)
        (game / "left4dead2.exe").write_bytes(b"MZ")
        self.assertEqual([i.path for i in steam.find_game_installs([root])], [game])

    def test_legacy_libraryfolders_format(self):
        root = self.tmp / "Steam"
        (root / "steamapps").mkdir(parents=True)
        write(root / "steamapps" / "libraryfolders.vdf",
              f'"LibraryFolders"\n{{\n\t"TimeNextStatsReport"\t\t"1"\n\t"1"\t\t"{self.tmp / "old"}"\n}}\n')
        self.assertIn(self.tmp / "old", steam.library_folders(root))

    def test_depot_detection_and_native_linux_help(self):
        windows = make_game(self.tmp / "a")
        linux = make_game(self.tmp / "b", windows=False)
        self.assertEqual(steam.check_game_dir(windows), "windows")
        with self.assertRaises(InstallerError) as raised:
            steam.check_game_dir(linux)
        self.assertIn("Compatibility", str(raised.exception))
        with self.assertRaises(InstallerError):
            steam.check_game_dir(self.tmp / "missing")
        empty = self.tmp / "empty"
        empty.mkdir()
        self.assertEqual(steam.game_depot(empty), "missing")

    def test_windows_depot_listed_first(self):
        root = self.make_steam(self.tmp / "Steam", [self.tmp / "lib2"])
        make_game(root, windows=False)
        game = make_game(self.tmp / "lib2")
        installs = steam.find_game_installs([root])
        self.assertEqual(installs[0].path, game)

    def test_users_most_recent_first(self):
        root = self.make_steam(self.tmp / "Steam")
        for account in ("111", "222"):
            write(root / "userdata" / account / "config" / "localconfig.vdf", '"UserLocalConfigStore"\n{\n}\n')
        id64 = str(steam.STEAMID64_BASE + 222)
        write(root / "config" / "loginusers.vdf",
              f'"users"\n{{\n\t"{id64}"\n\t{{\n\t\t"PersonaName"\t\t"Zoey"\n\t\t"MostRecent"\t\t"1"\n\t}}\n}}\n')
        users = steam.steam_users(root)
        self.assertEqual([u.account_id for u in users], ["222", "111"])
        self.assertEqual(users[0].name, "Zoey")
        self.assertTrue(users[0].most_recent)

    def test_compat_tool(self):
        root = self.tmp / "Steam"
        write(root / "config" / "config.vdf",
              '"InstallConfigStore"\n{\n"Software"\n{\n"Valve"\n{\n"Steam"\n{\n"CompatToolMapping"\n{\n'
              '"550"\n{\n"name"\t\t"proton_hotfix"\n}\n}\n}\n}\n}\n}\n')
        self.assertEqual(steam.compat_tool_for_game(root), "proton_hotfix")

    def fake_proc(self, processes):
        proc = self.tmp / "proc"
        for pid, (comm, cmdline) in enumerate(processes, start=100):
            folder = proc / str(pid)
            folder.mkdir(parents=True)
            (folder / "comm").write_text(comm + "\n")
            (folder / "cmdline").write_bytes(b"\0".join(arg.encode() for arg in cmdline) + b"\0")
        return proc

    def test_steam_running_detection(self):
        proc = self.fake_proc([("bash", ["bash"]), ("steam", ["/home/u/.local/share/Steam/ubuntu12_32/steam"])])
        self.assertTrue(steam.is_steam_running("linux", proc))
        idle = self.tmp / "proc-idle" / "7"
        idle.mkdir(parents=True)
        (idle / "comm").write_text("bash\n")
        (idle / "cmdline").write_bytes(b"bash\0")
        self.assertFalse(steam.is_steam_running("linux", self.tmp / "proc-idle"))

    def test_game_running_detection(self):
        proc = self.fake_proc([("python3", ["proton", "waitforexitandrun", "Z:/x/left4dead2.exe"])])
        self.assertTrue(steam.is_game_running("linux", proc))

    def test_script_mentioning_the_game_is_not_the_game(self):
        proc = self.fake_proc([("bash", ["bash", "-c", "touch game/left4dead2.exe && ./install"])])
        self.assertFalse(steam.is_game_running("linux", proc))

    def test_wine_game_process_detected(self):
        wine = self.fake_proc([("left4dead2.exe", ["S:\\steamapps\\common\\Left 4 Dead 2\\left4dead2.exe", "-insecure"])])
        self.assertTrue(steam.is_game_running("linux", wine))
