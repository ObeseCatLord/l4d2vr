"""2.0.14 Pavlov handling polish (research/pavlov/handling/SPEC.md): the new
Physical-mode defaults, their one-shot migrations (old shipped values only),
the Classic overlay that keeps Classic as it was, and the Physical-only gates."""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1] / 'L4D2VR'
CONFIG = (ROOT / 'config.txt').read_text(encoding='utf-8')
PARSE = (ROOT / 'vr' / 'vr_viewmodel_config.inl').read_text(encoding='utf-8', errors='replace')
MAIN = (ROOT / 'dllmain.cpp').read_text(encoding='utf-8', errors='replace')
MODE = (ROOT / 'vr' / 'vr_gameplay_mode.inl').read_text(encoding='utf-8')
AIMING = (ROOT / 'vr' / 'vr_aiming.inl').read_text(encoding='utf-8', errors='replace')
HAPTICS = (ROOT / 'haptics_config.txt').read_text(encoding='utf-8')
VRCPP = (ROOT / 'vr.cpp').read_text(encoding='utf-8', errors='replace')
CREATEMOVE = (ROOT / 'hooks' / 'hooks_createmove.inl').read_text(encoding='utf-8', errors='replace')
THROW = (ROOT / 'vr' / 'vr_hand_throw.inl').read_text(encoding='utf-8', errors='replace')
TRACKING = (ROOT / 'vr' / 'vr_tracking.inl').read_text(encoding='utf-8', errors='replace')
SCOPE = (ROOT / 'vr' / 'vr_physical_scope.inl').read_text(encoding='utf-8', errors='replace')


def shipped(key):
    m = re.search(r'(?m)^' + re.escape(key) + r'=(.*)$', CONFIG)
    return m.group(1).strip() if m else None


def migrations():
    table = MAIN[MAIN.find('kOneShotConfigMigrations[] ='):]
    table = table[:table.find('};')]
    return re.findall(r'\{\s*"([^"]+)",\s*"([A-Za-z0-9_]+)",\s*"([^"]*)"(?:,\s*(true|false))?(?:,\s*"([^"]*)")?\s*\}',
                      table)


class Defaults(unittest.TestCase):
    def test_new_physical_defaults(self):
        expected = {
            'VrHandsTwoHandedStabilizerEnabled': 'false',
            'VrHandsTwoHandedAimSmoothingSeconds': '0',
            'ScopeStabilizationEnabled': 'false',
            'ScopeActivateDistanceMeters': '0.40',
            'ScopeActivateAxisMeters': '0.05',
            'ScopePavlovOpticFov': 'true',
            'MuzzleWallBlockEnabled': 'true',
            'ManualThrowPavlovSmoothing': 'true',
            'HolsterYawFollowSpeed': '0',
            'HolsterLayout': 'forefront',   # the forefront default stays
        }
        for key, value in expected.items():
            with self.subTest(key=key):
                self.assertEqual(shipped(key), value)

    def test_new_keys_are_parsed(self):
        for key in ('ScopeActivateDistanceMeters', 'ScopeActivateAxisMeters', 'ScopePavlovOpticFov',
                    'MuzzleWallBlockEnabled', 'ManualThrowPavlovSmoothing'):
            with self.subTest(key=key):
                self.assertRegex(PARSE, r'get(Bool|Float)\("' + key + '"')
        # HolsterYawFollowSpeed accepts -1 (instant for any layout).
        self.assertIn('getFloat("HolsterYawFollowSpeed", 0.0f), -1.0f, 60.0f)', PARSE)

    def test_haptics_config(self):
        self.assertRegex(HAPTICS, r'(?m)^weapon\.pavlov_envelopes=1$')
        self.assertIn('getBool("weapon.pavlov_envelopes", true)', VRCPP)


class Migrations(unittest.TestCase):
    def test_old_shipped_values_only(self):
        by_key = {}
        for mid, key, value, _legacy, old in migrations():
            by_key.setdefault(key, []).append((mid, value, old))
        for key, new, old in (('VrHandsTwoHandedStabilizerEnabled', 'false', 'true'),
                              ('VrHandsTwoHandedAimSmoothingSeconds', '0', '0.025'),
                              ('ScopeStabilizationEnabled', 'false', 'true')):
            with self.subTest(key=key):
                entries = by_key.get(key, [])
                self.assertTrue(entries, f'{key} has no migration')
                mid, value, only_if = entries[-1]
                self.assertTrue(mid.startswith('2026-11-18_pavlov_'), mid)
                self.assertEqual(value, new)
                # Only the old shipped value changes, never a tuned one.
                self.assertEqual(only_if, old)

    def test_ids_unique(self):
        ids = [mid for mid, *_ in migrations()]
        self.assertEqual(len(ids), len(set(ids)))


class ClassicUnchanged(unittest.TestCase):
    def test_classic_keeps_the_stabilisers(self):
        entries = dict((k, v) for k, _, v in re.findall(r'\{ "(\w+)", &VR::(m_\w+), (true|false) \}', MODE))
        self.assertEqual(entries.get('VrHandsTwoHandedStabilizerEnabled'), 'true')
        self.assertEqual(entries.get('ScopeStabilizationEnabled'), 'true')

    def test_pavlov_hips_physical_only(self):
        self.assertRegex(MODE, r'm_HolsterLayout\.pavlovHips = m_HolsterLayout\.layoutKind == '
                               r'l4d2vr_holster::LayoutKind::Pavlov &&\s*r\.effective == Effective::Physical;')

    def test_physical_gates(self):
        # Pavlov haptics never play in Classic.
        play = AIMING[AIMING.find('void VR::PlayPavlovHaptic'):]
        play = play[:play.find('\n}\n')]
        self.assertIn('PhysicalGameplayActive()', play)
        fire = AIMING[AIMING.find('void VR::TriggerWeaponFireHaptics'):]
        fire = fire[:fire.find('\n}\n')]
        self.assertIn('m_PavlovFireHapticsEnabled && PhysicalGameplayActive()', fire)
        wall = AIMING[AIMING.find('void VR::UpdateMuzzleWallBlock'):]
        wall = wall[:wall.find('\n}\n')]
        self.assertIn('!PhysicalGameplayActive()', wall)
        self.assertIn('m_VR->MuzzleWallBlocksFire() &&\n\t\tm_VR->PhysicalGameplayActive()', CREATEMOVE)
        self.assertIn('m_ManualThrowPavlovSmoothing && PhysicalGameplayActive()', THROW)
        self.assertIn('const bool easeAllowed = PhysicalGameplayActive()', TRACKING)
        self.assertIn('PhysicalGameplayActive() && (m_ScopeActivateDistanceMeters', SCOPE)

    def test_wall_block_only_holds_attack(self):
        # The fire block only clears IN_ATTACK: a plain server never sees anything else.
        block = CREATEMOVE[CREATEMOVE.find('m_VR->MuzzleWallBlocksFire()'):]
        block = block[:block.find('}')]
        self.assertIn('cmd->buttons &= ~(1 << 0)', block)
        self.assertNotIn('impulse', block)
        self.assertNotIn('ClientCmd', block)


if __name__ == '__main__':
    unittest.main()
