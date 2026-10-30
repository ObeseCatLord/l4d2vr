import json
import os
import unittest
from pathlib import Path

from l4d2vr_installer import openxr, vdf
from l4d2vr_installer.fsutil import InstallerError

from .helpers import TempDirTest

CONFIG_VDF = '''"InstallConfigStore"
{
\t"Software"
\t{
\t\t"Valve"
\t\t{
\t\t\t"Steam"
\t\t\t{
\t\t\t\t"BaseInstallFolder_1"\t\t"/mnt/games\\\\Steam"
\t\t\t\t"CompatToolMapping"
\t\t\t\t{
\t\t\t\t\t"0"
\t\t\t\t\t{
\t\t\t\t\t\t"name"\t\t"proton_9"
\t\t\t\t\t\t"config"\t\t""
\t\t\t\t\t\t"priority"\t\t"75"
\t\t\t\t\t}
\t\t\t\t\t"550"
\t\t\t\t\t{
\t\t\t\t\t\t"name"\t\t"GE-Proton10-34"
\t\t\t\t\t\t"config"\t\t""
\t\t\t\t\t\t"priority"\t\t"250"
\t\t\t\t\t}
\t\t\t\t\t"620"
\t\t\t\t\t{
\t\t\t\t\t\t"name"\t\t"proton_hotfix"
\t\t\t\t\t\t"config"\t\t""
\t\t\t\t\t\t"priority"\t\t"250"
\t\t\t\t\t}
\t\t\t\t}
\t\t\t}
\t\t}
\t}
}
'''

GE_TOOL_VDF = '''"compatibilitytools"
{
  "compat_tools"
  {
    "GE-Proton10-34" // Internal name of this tool
    {
      "install_path" "."
      "display_name" "GE-Proton10-34"
      "from_oslist"  "windows"
      "to_oslist"    "linux"
    }
  }
}
'''


def make_proton(folder: Path, version: str, openxr32: bool) -> Path:
    (folder / "files/lib/wine/x86_64-windows").mkdir(parents=True, exist_ok=True)
    (folder / "proton").write_text("#!/bin/sh\n")
    (folder / "version").write_text(f"1791407340 {version}\n")
    (folder / "files/lib/wine/x86_64-windows/wineopenxr.dll").write_bytes(b"MZ")
    if openxr32:
        (folder / "files/lib/wine/i386-windows").mkdir(parents=True, exist_ok=True)
        (folder / "files/lib/wine/i386-windows/wineopenxr.dll").write_bytes(b"MZ")
    return folder


class ProtonTests(TempDirTest):
    def setUp(self):
        super().setUp()
        self.steam = self.home / ".local/share/Steam"
        self.common = self.steam / "steamapps/common"
        self.library = self.tmp / "lib2"
        (self.library / "steamapps/common").mkdir(parents=True)
        (self.steam / "config").mkdir(parents=True)
        self.config = self.steam / "config/config.vdf"
        self.config.write_text(CONFIG_VDF)
        self.hotfix = make_proton(self.common / "Proton Hotfix", "hotfix-20261007-x86_64", True)
        self.experimental = make_proton(self.library / "steamapps/common/Proton - Experimental",
                                        "experimental-11.0-20261014-x86_64", True)
        self.p10 = make_proton(self.library / "steamapps/common/Proton 10.0", "proton-10.0-4b", False)
        self.ge = make_proton(self.steam / "compatibilitytools.d/GE-Proton10-34", "GE-Proton10-34", False)
        (self.ge / "compatibilitytool.vdf").write_text(GE_TOOL_VDF)
        self.libraries = [self.steam, self.library]
        self.state = self.tmp / "state"

    def test_32bit_openxr_detection_and_versions(self):
        self.assertTrue(openxr.proton_has_32bit_openxr(self.hotfix))
        self.assertFalse(openxr.proton_has_32bit_openxr(self.p10))
        self.assertEqual(openxr.proton_version(self.experimental), "experimental-11.0-20261014-x86_64")
        self.assertEqual(openxr.proton_version(self.tmp), "")

    def test_resolve_official_numbered_and_custom_tools(self):
        self.assertEqual(openxr.resolve_tool("proton_hotfix", self.libraries, self.steam).path, self.hotfix)
        self.assertEqual(openxr.resolve_tool("proton_experimental", self.libraries, self.steam).path, self.experimental)
        self.assertEqual(openxr.resolve_tool("proton_10", self.libraries, self.steam).path, self.p10)
        ge = openxr.resolve_tool("GE-Proton10-34", self.libraries, self.steam)
        self.assertEqual(ge.path, self.ge)
        self.assertFalse(ge.openxr32)
        missing = openxr.resolve_tool("proton_8", self.libraries, self.steam)
        self.assertFalse(missing.installed)
        # A custom tool whose folder name differs from its internal name.
        other = make_proton(self.steam / "compatibilitytools.d/my-build", "x", True)
        (other / "compatibilitytool.vdf").write_text(GE_TOOL_VDF.replace("GE-Proton10-34", "MyProton"))
        self.assertEqual(openxr.resolve_tool("MyProton", self.libraries, self.steam).path, other)

    def test_check_flags_ge_proton_and_recommends_hotfix(self):
        check = openxr.check_proton(self.steam, self.libraries)
        self.assertEqual(check.current.name, "GE-Proton10-34")
        self.assertFalse(check.ok)
        self.assertEqual(check.recommended.name, "proton_hotfix")
        text = "\n".join(check.lines())
        self.assertIn("has no 32-bit OpenXR", text)
        self.assertIn("proton apply", text)
        self.assertIn("Properties > Compatibility", text)

    def test_recommendation_falls_back_to_experimental_or_download(self):
        os.remove(self.hotfix / "files/lib/wine/i386-windows/wineopenxr.dll")  # an old Hotfix build
        self.assertEqual(openxr.recommended_tool(self.libraries, self.steam).name, "proton_experimental")
        os.remove(self.experimental / "files/lib/wine/i386-windows/wineopenxr.dll")
        import shutil
        shutil.rmtree(self.experimental)
        tool = openxr.recommended_tool(self.libraries, self.steam)
        self.assertEqual(tool.name, "proton_experimental")
        self.assertFalse(tool.installed)
        self.assertIn("Steam downloads it", tool.describe())

    def test_supported_and_missing_mappings(self):
        openxr.set_compat_tool(self.steam, "proton_hotfix", steam_running=False)
        check = openxr.check_proton(self.steam, self.libraries)
        self.assertTrue(check.ok)
        self.assertIn("OK", check.lines()[0])
        self.assertEqual(len(check.lines()), 1)
        self.config.write_text(CONFIG_VDF.replace('"550"', '"551"'))
        check = openxr.check_proton(self.steam, self.libraries)
        self.assertIsNone(check.current)
        self.assertIn("no compatibility tool is forced", check.lines()[0])
        self.assertIsNone(openxr.compat_tool_mapping(self.tmp / "nowhere"))

    def test_hotfix_not_downloaded_yet_counts_as_ok(self):
        import shutil
        shutil.rmtree(self.hotfix)
        openxr.set_compat_tool(self.steam, "proton_hotfix", steam_running=False)
        self.assertTrue(openxr.check_proton(self.steam, self.libraries).ok)

    def test_set_and_restore_compat_tool(self):
        change = openxr.set_compat_tool(self.steam, "proton_hotfix", steam_running=False, state_dir=self.state)
        self.assertTrue(change.changed)
        self.assertEqual(change.backup.read_text(), CONFIG_VDF)
        self.assertEqual(openxr.compat_tool_mapping(self.steam),
                         {"name": "proton_hotfix", "config": "", "priority": "250"})
        data = vdf.load(self.config)
        mapping = data.get_path("InstallConfigStore", "Software", "Valve", "Steam", "CompatToolMapping")
        self.assertEqual(mapping.get_node("620").get("name"), "proton_hotfix")
        self.assertEqual(mapping.get_node("0").get("name"), "proton_9")
        self.assertEqual(data.get_path("InstallConfigStore", "Software", "Valve", "Steam", "BaseInstallFolder_1"),
                         "/mnt/games\\Steam")
        self.assertIn('"BaseInstallFolder_1"\t\t"/mnt/games\\\\Steam"', self.config.read_text())
        # Idempotent.
        self.assertFalse(openxr.set_compat_tool(self.steam, "proton_hotfix", steam_running=False,
                                                state_dir=self.state).changed)
        restored = openxr.restore_compat_tool(self.steam, steam_running=False, state_dir=self.state)
        self.assertIsNotNone(restored)
        self.assertEqual(self.config.read_text(), CONFIG_VDF)
        self.assertIsNone(openxr.restore_compat_tool(self.steam, steam_running=False, state_dir=self.state))

    def test_restore_removes_a_mapping_the_installer_created(self):
        self.config.write_text(CONFIG_VDF.replace('"550"', '"551"'))
        original = self.config.read_text()
        openxr.set_compat_tool(self.steam, "proton_experimental", steam_running=False, state_dir=self.state)
        self.assertEqual(openxr.compat_tool_mapping(self.steam)["name"], "proton_experimental")
        openxr.restore_compat_tool(self.steam, steam_running=False, state_dir=self.state)
        self.assertEqual(self.config.read_text(), original)

    def test_restore_leaves_a_later_user_choice_alone(self):
        openxr.set_compat_tool(self.steam, "proton_hotfix", steam_running=False, state_dir=self.state)
        openxr.set_compat_tool(self.steam, "proton_experimental", steam_running=False)  # the user, later
        self.assertIsNone(openxr.restore_compat_tool(self.steam, steam_running=False, state_dir=self.state))
        self.assertEqual(openxr.compat_tool_mapping(self.steam)["name"], "proton_experimental")

    def test_never_while_steam_runs_and_dry_run(self):
        with self.assertRaises(InstallerError):
            openxr.set_compat_tool(self.steam, "proton_hotfix", steam_running=True)
        self.assertEqual(self.config.read_text(), CONFIG_VDF)
        change = openxr.set_compat_tool(self.steam, "proton_hotfix", steam_running=True, dry_run=True)
        self.assertTrue(change.changed)
        self.assertEqual(self.config.read_text(), CONFIG_VDF)
        self.assertEqual(sorted(p.name for p in self.config.parent.iterdir()), ["config.vdf"])
        self.assertEqual(openxr.describe_change(change), "GE-Proton10-34 -> proton_hotfix")

    def test_rejects_other_files(self):
        self.config.write_text('"libraryfolders"\n{\n}\n')
        with self.assertRaises(InstallerError):
            openxr.set_compat_tool(self.steam, "proton_hotfix", steam_running=False)
        with self.assertRaises(InstallerError):
            openxr.set_compat_tool(self.tmp / "nosteam", "proton_hotfix", steam_running=False)


class RuntimeTests(TempDirTest):
    def write_vrpath(self, runtime):
        path = self.home / ".config/openvr/openvrpaths.vrpath"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps({"jsonid": "vrpathreg", "runtime": runtime, "version": 1}))

    def test_openvr_runtime_lines(self):
        self.assertIn("none registered", openxr.openvr_runtime_lines(self.home)[0])
        xrizer = self.tmp / "xrizer"
        self.write_vrpath([str(xrizer)])
        self.assertIn("has no bin/linux64/vrclient.so", openxr.openvr_runtime_lines(self.home)[0])
        (xrizer / "bin/linux64").mkdir(parents=True)
        (xrizer / "bin/linux64/vrclient.so").write_bytes(b"so")
        line = openxr.openvr_runtime_lines(self.home)[0]
        self.assertIn("XRizer", line)
        self.assertIn("OK", line)
        self.assertEqual(openxr.openvr_runtime(self.home), xrizer)
        self.write_vrpath([])
        self.assertIsNone(openxr.openvr_runtime(self.home))
        path = self.home / ".config/openvr/openvrpaths.vrpath"
        path.write_text("[1, 2]")
        self.assertIsNone(openxr.openvr_runtime(self.home))

    def test_linux_active_runtime(self):
        if not Path("/etc/xdg/openxr/1/active_runtime.json").exists():
            self.assertIn("no active runtime", openxr.linux_openxr_lines(self.home)[0])
        target = self.tmp / "openxr_monado.json"
        target.write_text("{}")
        active = self.home / ".config/openxr/1/active_runtime.json"
        active.parent.mkdir(parents=True)
        active.symlink_to(target)
        self.assertEqual(openxr.linux_active_openxr_runtime(self.home), target.resolve())
        self.assertIn(str(target.resolve()), openxr.linux_openxr_lines(self.home)[0])

    def test_windows_runtime_lines(self):
        manifest32 = self.tmp / "steamxr_win32.json"
        manifest32.write_text("{}")
        lines = openxr.windows_openxr_lines(lambda view: {"32": str(manifest32), "64": "x64.json"}[view])
        self.assertIn("OK", lines[0])
        lines = openxr.windows_openxr_lines(lambda view: {"32": str(self.tmp / "gone.json"), "64": ""}[view])
        self.assertIn("registered but missing", lines[0])
        lines = openxr.windows_openxr_lines(lambda view: {"32": "", "64": r"C:\WMR\MixedRealityRuntime.json"}[view])
        self.assertIn("64-bit only", lines[0])
        self.assertIn("SteamVR 2.17", lines[0])
        self.assertIn("WMR for SteamVR", lines[0])
        lines = openxr.windows_openxr_lines(lambda view: "")
        self.assertIn("none is active", lines[0])
        self.assertIn("desktop mode", lines[0])

    @unittest.skipUnless(os.name == "nt", "reads the real Windows registry")
    def test_windows_registry_read_for_real(self):
        lines = openxr.windows_openxr_lines()
        self.assertEqual(len(lines), 1)
        self.assertTrue(lines[0].startswith("OpenXR runtime"))
        print("\n  this machine: " + lines[0])

    def test_requirement_lines_per_platform(self):
        self.assertEqual(len(openxr.requirement_lines(None, [], platform="win32")), 1)
        lines = openxr.requirement_lines(None, [], platform="linux", home=self.home)
        self.assertTrue(any(line.startswith("OpenVR runtime for Proton") for line in lines))
        self.assertTrue(any(line.startswith("OpenXR runtime") for line in lines))


if __name__ == "__main__":
    unittest.main()
