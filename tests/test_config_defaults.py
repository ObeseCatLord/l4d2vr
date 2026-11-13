"""Shipped defaults must never turn on debug visuals (session 6: wireframe
magazine/bolt boxes showed up for every player because config.txt shipped
MagazineBoxDebugEnabled=true). Checks L4D2VR/config.txt (it becomes
vr/config.sample)."""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1] / 'L4D2VR'
KEY = re.compile(r'^([A-Za-z][A-Za-z0-9_]*)\s*=\s*(\S*)', re.M)
# Keys that only draw debug geometry or labels.
DEBUG_VISUAL = re.compile(r'(Debug(Enabled|Boxes?|Draw|Overlay)?$|BoxDebug|ShowInventoryAnchors|BoneLabels|DebugBox)', re.I)
LOG_ONLY = re.compile(r'(Log|Logging|Trace)$', re.I)


def shipped_values(text):
    return {k: v for k, v in KEY.findall(text)}


class ConfigDefaults(unittest.TestCase):
    def check(self, values, where):
        bad = [k for k, v in values.items()
               if DEBUG_VISUAL.search(k) and not LOG_ONLY.search(k) and v.lower() in ('true', '1')]
        self.assertEqual(bad, [], f'debug visuals on by default in {where}: {bad}')

    def test_config_txt(self):
        values = shipped_values((ROOT / 'config.txt').read_text(encoding='utf-8'))
        self.assertEqual(values.get('MagazineBoxDebugEnabled'), 'false')
        self.assertEqual(values.get('ShowInventoryAnchors'), 'false')
        self.check(values, 'config.txt')

    def test_code_defaults(self):
        header = (ROOT / 'vr.h').read_text(encoding='utf-8', errors='replace')
        self.assertIn('bool m_MagazineBoxDebugEnabled = false;', header)
        self.assertIn('bool m_DrawInventoryAnchors = false;', header)

    def test_existing_configs_are_migrated(self):
        main = (ROOT / 'dllmain.cpp').read_text(encoding='utf-8', errors='replace')
        self.assertRegex(main, r'"MagazineBoxDebugEnabled", "false"')
        self.assertRegex(main, r'"ShowInventoryAnchors", "false"')


if __name__ == '__main__':
    unittest.main()
