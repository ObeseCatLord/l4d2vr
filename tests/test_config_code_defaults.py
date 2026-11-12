"""Every value in the shipped config (L4D2VR/config.txt, which becomes
vr/config.sample) must equal the code default the DLL uses when the key is
missing (vr_viewmodel_config.inl: getX("Key", default) with the default either
a literal or a vr.h member initializer). A mismatch means a fresh install and
an updated install (whose missing keys are filled from the sample) disagree
with a config that lost the key, and it hides changed defaults from the
migration check below.

Also: every key whose shipped value changed in a release must have a one-shot
migration in dllmain.cpp (kOneShotConfigMigrations) that replaces the old
shipped value (KNOWN_SHIPPED_CHANGES lists them; add new changes there)."""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1] / 'L4D2VR'
KEY = re.compile(r'^([A-Za-z][A-Za-z0-9_]*)\s*=(.*)$', re.M)

# Keys parsed specially (not by a plain getX("Key", default) call) or whose
# code default intentionally differs from the sample.
EXEMPT = {
    # Colors are parsed into separate R/G/B/A members.
    'GameLaserSightColor', 'D3DAimLineOverlayColor', 'D3DAimLineOverlayOutlineColor', 'AimLineColor',
    'InventoryAnchorColor',
    # The scoped anchor defaults to the unscoped one when missing.
    'MouseModeScopedViewmodelAnchorOffset',
    # Defaults to ThirdPersonVRCameraOffset when missing.
    'ThirdPersonFrontVRCameraOffset',
}

# key -> list of (old shipped values) that a migration must replace.
# Fork releases 1.2.2 .. 2.0.4 and the upstream builds players update from.
KNOWN_SHIPPED_CHANGES = {
    # 2.0.11: open mic follows the game's microphone sensitivity (2.0.5-2.0.10 shipped 0.55).
    'OpusVoiceVadSensitivity': ['0.55'],
    # 2.0.10: Opus voice follows the game's voice setting (2.0.5-2.0.9 shipped ptt).
    'OpusVoiceMode': ['ptt'],
    # Fork releases (config.txt at 1.2.2, 2.0.0 .. 2.0.4).
    'AimLineEnabled': ['true'],
    'CrosshairEnabled': ['true'],
    'FirstPersonBodyEnabled': ['true'],
    'MagazineBoxDebugEnabled': ['true'],
    'ShowInventoryAnchors': ['true'],
    'ManualPumpForegripRacks': ['true'],
    'ManualThrowHandVelocityScale': ['3.0', '1.5'],
    'NativeViewmodelLeftHandPoseOffsetMeters': ['0,0,0'],
    'NativeViewmodelRightHandPoseOffsetMeters': ['0,0,0'],
    'NativeViewmodelLeftHandPoseRotationOffsetDeg': ['0,0,0'],
    'RightAmmoHudWidthMeters': ['0.8'],
    'ScopeEyeboxDeg': ['8'],
    'VrHandsTwoHandedGripTargetBoxScale': ['1.0'],
    # Hand settings from upstream builds and old code / panel defaults (2.0.5).
    'NativeViewmodelHandsOnly': ['false'],
    'NativeViewmodelArmCroppingEnabled': ['true'],
    'NativeViewmodelHandsOnlyWristKeepFraction': ['0'],
    'NativeViewmodelHandsOnlyArmBendScale': ['1'],
    'NativeViewmodelHandsOnlyTrimUnits': ['-1.25'],
    'NativeViewmodelRightHandAnimationKeepUnits': ['1'],
    'NativeViewmodelLeftHandFreezeAfterMapSeconds': ['3'],
    'NativeViewmodelHandsOnlyFreezePoseOffsetMeters': ['0.4,0.2,-0.27'],
    'NativeViewmodelHandsOnlyLeftFreezePoseRotationOffsetDeg': ['22,24,11'],
    'NativeViewmodelHandsOnlyRightFreezePoseRotationOffsetDeg': ['30,90,-30'],
    'NativeViewmodelLeftHandOpenVRCurlDirection': ['-1.0'],
    'NativeViewmodelLeftHandOpenVRCurlStrength': ['0.9'],
    'NativeViewmodelLeftHandOpenVRThumbInitialCurl': ['0.2'],
    'NativeViewmodelLeftHandOpenVRIndexInitialCurl': ['0.4'],
    'NativeViewmodelLeftHandOpenVRMiddleInitialCurl': ['0.4'],
    'NativeViewmodelLeftHandOpenVRRingInitialCurl': ['0.4'],
    'NativeViewmodelLeftHandOpenVRPinkyInitialCurl': ['0.4'],
    'NativeViewmodelLeftHandOpenVRThumbRootRotationOffsetDeg': ['0,-15,0'],
    'VrHandsGloveThumbMaxCurl': ['0.05'],
    'VrHandsGloveIndexMaxCurl': ['0.8'],
    'VrHandsGloveMiddleMaxCurl': ['0.8'],
    'VrHandsGloveRingMaxCurl': ['0.8'],
    'VrHandsGlovePinkyMaxCurl': ['0.8'],
    'VrHandsLeftHandedViewmodelPoseOffsetMeters': ['0.040,0.002,0.01'],
    'VrHandsEnabled': ['false'],
    'MagazineInteractionEnabled': ['false'],
    'ViewmodelAdjustEnabled': ['true'],
}


def config_values(text):
    out = {}
    for k, v in KEY.findall(text):
        out[k] = v.split('#')[0].strip() if not v.strip().startswith('#') else v.strip()
    return out


def strip_comments(src):
    src = re.sub(r'/\*.*?\*/', '', src, flags=re.S)
    return re.sub(r'//[^\n]*', '', src)


def member_defaults(header):
    """m_Name = value; initializers in vr.h (bool/int/float/Vector/string)."""
    out = {}
    for m in re.finditer(r'\b(?:bool|int|float|double|uint32_t|Vector|QAngle|std::string)\s+(m_[A-Za-z0-9_]+)\s*(?:=\s*([^;]*?)|\{([^;]*?)\})\s*;', header):
        name, eq, brace = m.group(1), m.group(2), m.group(3)
        val = eq if eq is not None else brace
        out[name] = val.strip()
    return out


def find_call_default(src, key):
    """The default argument of the first getX("key", default ...) call."""
    m = re.search(r'get(Bool|Float|Int|Vector3|String|Color|FloatList)\(\s*"' + re.escape(key) + r'"\s*,', src)
    if not m:
        return None, None
    i = m.end()
    depth = 0
    j = i
    while j < len(src):
        c = src[j]
        if c in '({':
            depth += 1
        elif c in ')}':
            if depth == 0:
                break
            depth -= 1
        elif c == ',' and depth == 0:
            break
        j += 1
    return m.group(1), src[i:j].strip()


def numbers(text):
    return [float(x) for x in re.findall(r'[-+]?(?:\d+\.\d*|\.\d+|\d+)(?:[eE][-+]?\d+)?', text)]


def normalize(kind, value):
    v = value.strip().strip('"')
    if kind == 'Bool':
        lv = v.lower()
        if lv in ('true', '1'):
            return True
        if lv in ('false', '0'):
            return False
        return None
    if kind in ('Float', 'Int', 'Vector3', 'Color', 'FloatList'):
        nums = numbers(v.replace('f', ''))
        return tuple(round(x, 4) for x in nums) if nums else None
    return v


def resolve_default(kind, expr, members):
    expr = expr.strip()
    component = None
    if expr.startswith('m_'):
        m = re.match(r'(m_[A-Za-z0-9_]+)(?:\.([xyz]))?$', expr)
        if not m or m.group(1) not in members:
            return None
        component = m.group(2)
        expr = members[m.group(1)]
    if kind == 'String':
        m = re.match(r'^(?:std::string\()?"(.*)"\)?$', expr)
        return m.group(1).encode().decode('unicode_escape') if m else None
    if component:
        nums = numbers(expr.replace('f', ''))
        if len(nums) != 3:
            return None
        return (round(nums['xyz'.index(component)], 4),)
    if kind == 'Bool' and expr in ('true', 'false'):
        return expr == 'true'
    if re.search(r'[A-Za-z_]', re.sub(r'(?<=\d)f\b', '', expr).replace('Vector', '').replace('QAngle', '')):
        return None   # an expression (a constant name, a call): not checked
    return normalize(kind, expr)


class CodeDefaults(unittest.TestCase):
    def test_sample_equals_code_defaults(self):
        cfg = config_values((ROOT / 'config.txt').read_text(encoding='utf-8'))
        src = strip_comments((ROOT / 'vr' / 'vr_viewmodel_config.inl').read_text(encoding='utf-8', errors='replace'))
        members = member_defaults(strip_comments((ROOT / 'vr.h').read_text(encoding='utf-8', errors='replace')))
        bad = []
        for key, shipped in cfg.items():
            if key in EXEMPT:
                continue
            kind, expr = find_call_default(src, key)
            if kind is None:
                continue
            code = resolve_default(kind, expr, members)
            if code is None:
                continue
            value = normalize(kind, shipped)
            if kind == 'Int' and shipped.lower() in ('true', 'false'):
                value = (1.0,) if shipped.lower() == 'true' else (0.0,)
            if value != code:
                bad.append(f'{key}: config.txt={shipped!r} code={expr!r} -> {code!r}')
        self.assertEqual(bad, [], 'config.txt and code defaults differ:\n' + '\n'.join(bad))

    def test_changed_defaults_have_migrations(self):
        cfg = config_values((ROOT / 'config.txt').read_text(encoding='utf-8'))
        main = (ROOT / 'dllmain.cpp').read_text(encoding='utf-8', errors='replace')
        table = main[main.find('kOneShotConfigMigrations[] ='):]
        table = table[:table.find('};')]
        entries = re.findall(r'\{\s*"([^"]+)",\s*"([A-Za-z0-9_]+)",\s*("[^"]*"|[A-Za-z0-9_:]+)(?:,\s*(true|false))?(?:,\s*("[^"]*"))?\s*\}', table)
        by_key = {}
        for mid, key, value, legacy, old in entries:
            by_key.setdefault(key, []).append((value.strip(), (old or '').strip().strip('"'), legacy == 'true'))
        missing = []
        for key, olds in KNOWN_SHIPPED_CHANGES.items():
            migs = by_key.get(key, [])
            for old in olds:
                def same(a, b):
                    na, nb = numbers(a), numbers(b)
                    if na and nb and not re.search(r'[a-zA-Z]', a + b):
                        return len(na) == len(nb) and all(abs(x - y) < 1e-4 for x, y in zip(na, nb))
                    return a.strip().lower() == b.strip().lower()
                ok = any((m_old == '' and not legacy) or (m_old and same(m_old, old)) or (legacy and same(old, '0,0,0'))
                         for _, m_old, legacy in migs)
                if not ok:
                    missing.append(f'{key}: old shipped {old!r} has no migration')
            # The migrations must set the current shipped value.
            for value, _, _ in migs[-1:]:
                v = value.strip('"')
                if v.startswith('l4d2vr_'):
                    continue
                self.assertEqual(normalize('Float' if numbers(v) else 'String', v),
                                 normalize('Float' if numbers(v) else 'String', cfg.get(key, v)),
                                 f'{key}: last migration sets {v}, config.txt ships {cfg.get(key)}')
        self.assertEqual(missing, [], '\n'.join(missing))


class PanelDefaults(unittest.TestCase):
    def test_panel_defaults_equal_sample(self):
        """The config panel writes its default for a key missing from the
        config; it must be the shipped value (2.0.4 and older wrote e.g.
        NativeViewmodelArmCroppingEnabled=true and VrHandsEnabled=false)."""
        cfg = config_values((ROOT / 'config.txt').read_text(encoding='utf-8'))
        text = (ROOT / 'vr_config_overlay_embedded.inl').read_text(encoding='utf-8', errors='replace')
        kinds = {'Bool': 'Bool', 'Float': 'Float', 'Int': 'Int', 'Vec3': 'Vector3', 'Color': 'Color'}
        bad = []
        for key, typ, default in re.findall(r'\{ "([A-Za-z0-9_]+)", CfgOptionType::(\w+), [^\n]*?, "([^"]*)" \},', text):
            if key not in cfg:
                continue
            kind = kinds.get(typ, 'String')
            if normalize(kind, default) != normalize(kind, cfg[key]):
                bad.append(f'{key}: panel={default!r} config.txt={cfg[key]!r}')
        self.assertEqual(bad, [], '\n'.join(bad))


if __name__ == '__main__':
    unittest.main()
