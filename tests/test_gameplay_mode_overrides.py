"""Classic gameplay mode (2.0.13, L4D2VR/vr/vr_gameplay_mode.inl): every option
the classic overlay forces must be a real config option parsed into the same
member, and the removed cheat-like features must stay removed (no config key,
no parse, retired at start-up)."""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1] / 'L4D2VR'
MODE = (ROOT / 'vr' / 'vr_gameplay_mode.inl').read_text(encoding='utf-8')
PARSE = (ROOT / 'vr' / 'vr_viewmodel_config.inl').read_text(encoding='utf-8', errors='replace')
CONFIG = (ROOT / 'config.txt').read_text(encoding='utf-8')
DLLMAIN = (ROOT / 'dllmain.cpp').read_text(encoding='utf-8', errors='replace')
ENTRY = re.compile(r'\{ "(\w+)", &VR::(m_\w+), (true|false) \}')

# Option families removed in 2.0.13 (all modes).
REMOVED_PREFIXES = (
    'AutoBunnyHop', 'AutoAirStrafe', 'LedgeGuard', 'AutoRepeat', 'AutoFastMelee',
    'EffectiveAttackRangeAutoFire', 'SpecialInfected', 'RearMirror', 'DesktopRearMirror',
    'DesktopIntentSense', 'TeleportVisualScout', 'HitSound', 'KillSound', 'HitIndicator',
    'KillIndicator', 'HeadshotSound', 'ShadowTweaks', 'ShadowEntityTweaks', 'FlashlightEnhancement',
    'LocalVScriptConvars', 'ItemModelLabel',
)


class ClassicOverrides(unittest.TestCase):
    def test_entries_are_parsed_options(self):
        entries = ENTRY.findall(MODE)
        self.assertGreaterEqual(len(entries), 30)
        for key, member, _ in entries:
            with self.subTest(key=key):
                pattern = re.compile(re.escape(member) + r'\s*=\s*(?:std::\w+\()?\s*getBool\(\s*"' + re.escape(key) + '"')
                self.assertRegex(PARSE, pattern, f'{member} is not parsed from {key}')

    def test_classic_values(self):
        entries = {key: value for key, _, value in ENTRY.findall(MODE)}
        # The user's classic: button reload with the native auto reload, no
        # akimbo, no recoil system, native spread with the crosshair, holsters
        # as selectors with the stock draw, no gestures or hand throws.
        for key in ('MagazineInteractionEnabled', 'MagazineInteractionSuppressEmptyClipAutoReload',
                    'MagazineEmptyGlowEnabled', 'DualPistolsIndependentHandsEnabled', 'RecoilEnabled',
                    'SpreadDisabled', 'HolsterInstantDraw', 'GripToggle', 'ChestSlingEnabled',
                    'ManualThrowEnabled', 'MotionGesturesEnabled', 'PhysicalShoveEnabled',
                    'GunBashSweepEnabled', 'MeleeInertiaEnabled', 'ConsumableGesturesEnabled',
                    'ObjectPullEnabled', 'BlockFireOnFriendlyAimEnabled'):
            self.assertEqual(entries.get(key), 'false', key)
        self.assertEqual(entries.get('CrosshairEnabled'), 'true')
        self.assertEqual(entries.get('MotionOnlyMeleeAndShove'), 'false')

    def test_gameplay_mode_option(self):
        self.assertRegex(CONFIG, r'(?m)^GameplayMode=auto$')
        self.assertIn('getString("GameplayMode", "auto")', PARSE)

    def test_removed_features_stay_removed(self):
        keys = re.findall(r'(?m)^([A-Za-z]\w*)\s*=', CONFIG)
        parsed = re.findall(r'get\w+\("(\w+)"', PARSE)
        for key in keys + parsed:
            with self.subTest(key=key):
                self.assertFalse(key.startswith(REMOVED_PREFIXES), f'{key} is a removed feature')
        retired = DLLMAIN.split('RetiredConfigParameterKeys()')[1].split('return keys;')[0]
        for key in ('AutoBunnyHop', 'LedgeGuardEnabled', 'SpecialInfectedArrowEnabled', 'RearMirrorEnabled',
                    'TeleportVisualScoutOnNonVRServerEnabled', 'HitSoundEnabled', 'KillIndicatorEnabled',
                    'ShadowTweaksEnabled', 'LocalVScriptConvarsEnabled', 'AutoRepeatSemiAutoFire',
                    'AutoFastMelee', 'EffectiveAttackRangeAutoFireEnabled', 'FlashlightEnhancementEnabled'):
            self.assertIn(f'"{key}"', retired, f'{key} is not retired at start-up')


if __name__ == '__main__':
    unittest.main()
