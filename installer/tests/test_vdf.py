import unittest

from l4d2vr_installer import vdf

LIBRARYFOLDERS = '''"libraryfolders"
{
\t"0"
\t{
\t\t"path"\t\t"C:\\\\Program Files (x86)\\\\Steam"
\t\t"label"\t\t""
\t\t"apps"
\t\t{
\t\t\t"228980"\t\t"0"
\t\t}
\t}
\t"1"
\t{
\t\t"path"\t\t"D:\\\\SteamLibrary"
\t\t"apps"
\t\t{
\t\t\t"550"\t\t"12345"
\t\t}
\t}
}
'''

LOCALCONFIG = '''"UserLocalConfigStore"
{
\t"Software"
\t{
\t\t"Valve"
\t\t{
\t\t\t"Steam"
\t\t\t{
\t\t\t\t"apps"
\t\t\t\t{
\t\t\t\t\t"550"
\t\t\t\t\t{
\t\t\t\t\t\t"LaunchOptions"\t\t"FOO=\\"a b\\" %command% -novid"
\t\t\t\t\t\t"LastPlayed"\t\t"1700000000"
\t\t\t\t\t}
\t\t\t\t}
\t\t\t}
\t\t}
\t}
}
'''


class VdfTests(unittest.TestCase):
    def test_parse_library_folders(self):
        data = vdf.loads(LIBRARYFOLDERS)
        folders = data.get_node("libraryfolders")
        self.assertEqual(folders.get_node("0").get("path"), "C:\\Program Files (x86)\\Steam")
        self.assertEqual(folders.get_path("1", "apps", "550"), "12345")

    def test_case_insensitive_lookup(self):
        data = vdf.loads(LIBRARYFOLDERS)
        self.assertIsNotNone(data.get_node("LibraryFolders"))
        self.assertIsNone(data.get_path("libraryfolders", "9"))

    def test_escaped_quotes(self):
        data = vdf.loads(LOCALCONFIG)
        options = data.get_path("UserLocalConfigStore", "Software", "Valve", "Steam", "apps", "550", "LaunchOptions")
        self.assertEqual(options, 'FOO="a b" %command% -novid')

    def test_round_trip_is_byte_identical(self):
        for text in (LIBRARYFOLDERS, LOCALCONFIG):
            self.assertEqual(vdf.dumps(vdf.loads(text)), text)

    def test_round_trip_crlf(self):
        text = LOCALCONFIG.replace("\n", "\r\n")
        self.assertEqual(vdf.dumps(vdf.loads(text), vdf.detect_newline(text)), text)

    def test_comments_unquoted_and_conditions(self):
        text = '// comment\nroot\n{\n  key value // trailing\n  "cond" "1" [$WIN32]\n}\n'
        data = vdf.loads(text)
        self.assertEqual(data.get_path("root", "key"), "value")
        self.assertEqual(data.get_path("root", "cond"), "1")
        self.assertIn("[$WIN32]", vdf.dumps(data))

    def test_duplicate_keys_preserved(self):
        data = vdf.loads('"a" { "k" "1" "k" "2" }')
        self.assertEqual([v for _, v in data.get_node("a")], ["1", "2"])
        self.assertEqual(data.get_path("a", "k"), "1")

    def test_set_and_ensure_node(self):
        data = vdf.loads('"root" { }')
        node = data.get_node("root").ensure_node("apps").ensure_node("550")
        node.set("LaunchOptions", 'X="1" %command%')
        again = vdf.loads(vdf.dumps(data))
        self.assertEqual(again.get_path("root", "apps", "550", "LaunchOptions"), 'X="1" %command%')
        node.set("LaunchOptions", "y")
        self.assertEqual(len(node), 1)
        self.assertTrue(node.remove("LaunchOptions"))
        self.assertFalse(node.remove("LaunchOptions"))

    def test_errors(self):
        for text in ('"a" {', '"a" "b" }', '"a"', '"unterminated'):
            with self.assertRaises(vdf.VdfError):
                vdf.loads(text)


if __name__ == "__main__":
    unittest.main()
