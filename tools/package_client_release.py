#!/usr/bin/env python3
"""Build the client release zip (l4d2vr.zip).

Usage:
  package_client_release.py --base BASE.zip --dll d3d9.dll --version 1.3.0 --out l4d2vr.zip

BASE.zip supplies the binary assets this repository does not track (sounds,
overlays, images, UI Font Fix.vpk, ...), e.g. a previous l4d2vr.zip.
Everything the repository does track is taken from the checkout.

The release is OpenXR-only: it ships the 32-bit Khronos openxr_loader.dll and
no openvr_api.dll, SteamVR action manifest, bindings or manifest.vrmanifest
(those stay in the repository for legacy developer builds made with
/p:L4D2VRLegacyOpenVR=true). OpenVR files coming from BASE.zip are dropped, and
a d3d9.dll that imports openvr_api.dll (a legacy build) is refused.

vr/config.sample is written with every parameter commented out ("#Key=value").
On startup the mod forces each *uncommented* sample value onto the player's
config.txt, so a fully commented sample only fills in missing keys; changed
defaults for existing players belong in the one-shot config migrations.
"""
import argparse
import fnmatch
import io
import json
import re
import struct
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
KEY_LINE = re.compile(r"^[A-Za-z][A-Za-z0-9_]*\s*=")

# The OpenXR backend loads openxr_loader.dll from d3d9.dll's folder
# (thirdparty/openxr/README.md: Khronos OpenXR-SDK release-1.1.65, Win32),
# shipped with its licences (Apache-2.0; jsoncpp inside the loader).
OPENXR_RUNTIME_FILES = {
    "openxr_loader.dll": "thirdparty/openxr/bin/win32/openxr_loader.dll",
    "vr/licenses/openxr_loader-LICENSE.txt": "thirdparty/openxr/LICENSE",
    "vr/licenses/jsoncpp-LICENSE.txt": "thirdparty/openxr/LICENSE.jsoncpp",
    "vr/THIRD_PARTY_NOTICES.md": "L4D2VR/THIRD_PARTY_NOTICES.md",
}

DROP_FROM_BASE = {"SHOTGUN-SHELL-FIX.txt", "patch-shotgun-shell-release.py", "updatelog.txt"}

# OpenVR files of the releases before the OpenXR-only one. Never shipped now
# (the installer also removes them from players' game folders on update).
OPENVR_FILES = ("openvr_api.dll", "vr/manifest.vrmanifest", "vr/SteamVRActionManifest/*")
REQUIRED_FILES = ("d3d9.dll", "openxr_loader.dll", "manifest.json", "vr/config.sample", "vr/version.txt")

CHANGES = [
    "Pavlov-style body holsters: grip an item at its holster to draw it instantly.",
    "Physical reloading: magazine release button, hand-pulled AK magazine, chambering, spare magazine at the waist, dropped magazines, glowing magazine when empty.",
    "True akimbo, motion melee with weapon inertia, physical shoves, hand-thrown grenades, pills to the mouth and adrenaline stabs.",
    "Crosshair aligned with the gun sights and a dynamic spread crosshair; picture-in-picture scope lens.",
    "Loading screens and intermissions stay visible in the headset; wrist HUDs on by default; background audio.",
    "Other VR players' bodies crouch with them; desktop players with the mod see VR bodies, and the mod starts in desktop mode when no headset is found.",
    "Dedicated server plugin (Linux and Windows) with full support for every feature; plain servers use client-side fallbacks.",
    "1.2.1: items drawn from a holster are held by the grip and dropped when you let go (HolsterReleaseDrop).",
    "1.2.1: hand pills or adrenaline to a teammate by holding them against their chest.",
    "1.2.1: Half-Life 2 VR style two-handed aim with an adaptive stabilizer; the front hand no longer drifts.",
    "1.2.2: toggle grips (GripToggle): a firm squeeze takes an item, the next squeeze holsters, swaps or drops it.",
    "1.2.2: reloading tuned to Pavlov VR per gun (slide/bolt/pump pull, chamber point, magazine depth), with a forgiving magazine well and a pouch glow.",
    "1.2.2: reload hints: the spare magazine and the bolt, slide or pump glow when the reload needs them.",
    "1.2.2: point at menus with either controller and click with the trigger (MenuLaserPointerEnabled).",
    "1.2.2: right stick forward jumps and back crouches (StickJumpCrouch); grips are only for gripping.",
    "1.2.2: holsters use Pavlov VR's body layout (hips, chest pockets, back) and follow your head; look down to see them; a haptic tick on hover; either hand swaps items at any holster. Turning no longer triggers melee bashes.",
    "1.2.2: a reload takes the full process, magazine in and then the slide; the release button drops one magazine and returns its rounds to the reserve.",
    "1.2.2: spare magazines and shells are held the way they sit in the gun; glows are a soft, steady tint.",
    "1.2.2: the wrist HUD uses the game's own health bars and font with full teammate names; your own bar is the large one.",
    "1.2.2: right stick down toggles crouch; guns never play the swing or bash animation.",
    "1.2.2: the two-handed grip lets go when the support hand moves 20 cm from the foregrip; a shot the server never fired (right after a draw) no longer needs a rack or pump.",
    "1.2.2: melee and shoves are motion only (no trigger bash or shove animation); the melee grip sits closer to the base of the handle.",
    "1.2.2: Pavlov magazine slap on the AK-47, SG552 and military sniper: grab a fresh magazine with the old one still in and strike the old one out (MagazineSlapEnabled).",
    "OpenXR: the mod now talks to the headset through OpenXR only (no SteamVR/OpenVR files). Windows needs a 32-bit OpenXR runtime: SteamVR 2.17 or newer, Virtual Desktop or Meta Quest Link. Linux needs Proton Hotfix or Experimental with PROTON_USE_WOW64=1 (the installer sets both).",
    "OpenXR: controller bindings are built into the mod (SteamVR's binding screen no longer applies); menus use the controller laser and the in-mod keyboard.",
    "OpenXR: without a usable OpenXR runtime the game starts in desktop mode and vrmod_log.txt and the game console say why.",
    "2.0.0: Pavlov VR recoil per gun (same values as Pavlov's equivalent guns): the gun kicks in your hand and recovers; holding the foregrip with the other hand tames heavy recoil. The gun no longer sways or plays L4D2's firing animation in your hand (GunRigidInHand).",
    "2.0.0: no bullet spread for VR players: bullets go exactly along the iron sights, aligned on every gun; the crosshair is off by default (desktop players keep vanilla spread and crosshair).",
    "2.0.0: shotguns fire the same evenly spread pellet pattern every shot, at L4D2's own pattern size (on servers running the mod).",
    "2.0.0: ambidextrous: the hand that grabs an item holds and uses it; grips grab, triggers do magazines, slides, bolts and pumps (as in Pavlov); right stick click toggles the flashlight; no draw animation when switching.",
    "2.0.0: physical magazine slap, hand-thrown grenades that leave your hand with its real motion, items you let go drop at your hand, and a more reliable motion bash that never triggers from turning.",
    "2.0.0: dual pistols reload physically: magazine release, bring one pistol to the other hand to stack them, take the double magazine with the free hand, then rack each empty slide (DualPistolsPhysicalReloadEnabled).",
    "2.0.0: melee swings feel like Ancient Dungeon VR: the grip stays in your hand and the blade trails behind fast swings; only fast, committed swings count; hits are felt in the controller.",
    "2.0.0: survivor hand poses follow Pavlov's hand poses for holding guns, magazines, slides, grenades and items. The separate SteamVR glove models are retired.",
    "2.0.2: easier shotgun pumping: the pump cocks at 7 of its 10 cm and chambers anywhere in the last 3.5 cm, a fast rack is never missed at any frame rate, the pump is caught from further away and stays caught through a tracking blip, and holding the foregrip racks it too (ManualPump... settings).",
    "2.0.2: stance recoil: guns kick less crouched, much more while jumping, and more the faster you move, following each gun's own L4D2 accuracy values (RecoilStanceEnabled and per-part scales).",
    "2.0.4: the shotgun pump needs the off hand's trigger: hold the foregrip, squeeze that hand's trigger and pump. The foregrip's grip alone no longer racks it (ManualPumpForegripRacks, now off).",
    "2.0.4: pump shotguns fire shell after shell; the pump is only needed to chamber a shell loaded after the gun ran dry (ManualPumpEveryShot=true brings back a pump after every shot).",
    "2.0.4: the support hand stays on the pump shotgun's foregrip through each shot and while it pumps; the gun keeps aiming from both hands while the pump waits.",
    "2.0.4: scopes work: configs from before the physical overhaul kept ScopeEnabled=false, so scopes never rendered; it is switched on once. The lens shows the zoomed picture whenever your eye is behind the scope within ScopeEyeboxDeg (now 15 degrees, was 8) of its axis; before, the eye had to sit within a few millimetres of the axis.",
    "2.0.4: grenades are thrown like in the original L4D2VR mod by default (GrenadeThrowMode=arc): holding one shows the arc from your hand, aim with the hand, pull and let go of the trigger and the game's own throw lands where the arc shows, on any server. The Pavlov-style hand throw is GrenadeThrowMode=physics, and now takes your hand's peak speed in the last 80 ms with a 2.5x boost (was 1.5x; a dedicated server needs the new plugin for that boost).",
    "2.0.4: the SCAR's 3-round burst kicks like Pavlov's burst rifle (the M16) instead of a semi-auto marksman rifle, about 5x less; two-handed it barely climbs.",
    "2.0.4: reloads closer to Pavlov: the pistol, magnum, M16, SCAR and hunting rifle lock open when empty (take the slide or bolt and let go to chamber); pistol, magnum, hunting rifle and scout magazines and shotgun shells go in as soon as they are lined up in the well; magazines seat at 95% of their depth; the longest bolts pull 11.4 cm.",
    "2.0.4: grenades and pills sit visibly on your chest: the chest pockets moved 10 cm forward and 4 cm down (they were under your chin, out of view) and take a slightly wider reach.",
    "2.0.4: an item in your left hand is always held by a mirrored left hand, on every hand setting (also with NativeViewmodelHandsOnly off or arm cropping on); the right hand is never moved onto the left controller (ItemHandMirror retired).",
    "2.0.4: chest sling: letting go of a rifle, SMG, shotgun or sniper near your body hangs it on your chest with both hands free; grab its handle or foregrip with either hand to take it (that hand becomes the trigger hand); let go at arm's length to drop it (ChestSlingEnabled).",
    "2.0.4: grabbing closer to Pavlov: the support hand catches the foregrip from further away (VrHandsTwoHandedGripTargetBoxScale 1.5), and a slung gun is taken anywhere along its body.",
    "2.0.4: the weapon idle poses learned by older versions are relearned once: cached ones put the hand on the auto shotgun and SPAS 21 cm away from the controller.",
    "2.0.4: wrist HUDs come back: they were hidden whenever the runtime reported a controller as valid but not tracked; they now also keep the last picture while a menu is open and fall back to the simple wrist panels if the game HUD cannot be captured; both wrist HUDs are switched back on once for existing configs.",
    "2.0.4: no more debug boxes around the magazine and bolt while two-handing (MagazineBoxDebugEnabled and ShowInventoryAnchors off, also for existing configs).",
    "2.0.4: the full first-person body is now off by default (FirstPersonBodyEnabled; turned off once for existing configs). When you turn it on, your body's arms reach your VR hands (two-bone IK from the body's own shoulders, natural elbows, one hand per side, a small stretch instead of a detached hand) and your torso turns to follow your head (after a 30 degree deadband, at once when a hand goes far out to the side); other VR players' bodies turn the same way (FirstPersonBodyArmIK, FirstPersonBodyYawDeadbandDeg).",
    "2.0.5: every gun's hand sits on your controller (GunHandOnController): the drawn hand came from each gun's idle animation, so the magnum's hand was 9 cm off, the Uzi's and MAC-10's 13 cm behind and the military sniper's 17 cm ahead; now every gun is within a millimetre of where your empty hand sits, in both hand modes and in either hand. Only the gun's position moves, so the sights keep their line.",
    "2.0.5: the aim laser (when you turn it on) starts at your controller like in the original L4D2VR mod instead of running along the gun's sights, and still ends where your bullet lands (VanillaLaserPlacement); bullets keep following the sights.",
    "2.0.5: an item in your left hand is held by the survivor's own left arm and hand (ItemHandLeftArmModel) instead of the right arm drawn mirrored; the right arm is your free hand.",
    "2.0.5: updating keeps proper defaults: hand settings carried over from older builds (classic viewmodel with cropped arms, arm bend and trim, old freeze-pose offsets, inverted and pre-curled finger curls, glove curls, the left-handed viewmodel offset, VR hands or physical reloads off, viewmodel adjust on) are switched to the current defaults once, but only where your config still has the old default; values you changed are kept.",
    "2.0.5: the code and config-panel defaults now match the shipped config (the panel used to write e.g. cropped arms and VR hands off for keys missing from your config), and a new install no longer runs the update migrations on its second start.",
]


def is_openvr_file(name: str) -> bool:
    lowered = name.lower()
    return any(fnmatch.fnmatchcase(lowered, pattern.lower()) for pattern in OPENVR_FILES)


def pe_imports(data: bytes) -> list:
    """DLL names in a PE file's import table (lower case); [] if unreadable."""
    try:
        if data[:2] != b"MZ":
            return []
        pe = struct.unpack_from("<I", data, 0x3C)[0]
        if data[pe:pe + 4] != b"PE\0\0":
            return []
        sections = struct.unpack_from("<H", data, pe + 6)[0]
        optional_size = struct.unpack_from("<H", data, pe + 20)[0]
        optional = pe + 24
        magic = struct.unpack_from("<H", data, optional)[0]
        directories = optional + (96 if magic == 0x10B else 112)
        import_rva = struct.unpack_from("<I", data, directories + 8)[0]
        table = optional + optional_size
        ranges = []
        for i in range(sections):
            entry = table + 40 * i
            vsize, va, rsize, raw = struct.unpack_from("<IIII", data, entry + 8)
            ranges.append((va, max(vsize, rsize), raw))

        def offset(rva):
            for va, size, raw in ranges:
                if va <= rva < va + size:
                    return rva - va + raw
            raise ValueError(rva)

        names = []
        cursor = offset(import_rva)
        while True:
            name_rva = struct.unpack_from("<I", data, cursor + 12)[0]
            if name_rva == 0:
                break
            start = offset(name_rva)
            names.append(data[start:data.index(b"\0", start)].decode("ascii", "replace").lower())
            cursor += 20
        return names
    except (struct.error, ValueError, IndexError):
        return []


def check_package(files: dict) -> list:
    """Problems that must stop the release (empty = fine)."""
    problems = [f"missing {name}" for name in REQUIRED_FILES if name not in files]
    problems += [f"OpenVR file in an OpenXR-only release: {name}" for name in sorted(files) if is_openvr_file(name)]
    imports = pe_imports(files.get("d3d9.dll", b""))
    if not imports:
        problems.append("d3d9.dll has no readable import table (not a PE DLL?)")
    elif "openvr_api.dll" in imports:
        problems.append("d3d9.dll imports openvr_api.dll: that is a legacy OpenVR build (/p:L4D2VRLegacyOpenVR=true), "
                        "not the OpenXR-only release")
    return problems


def commented_sample(text: str) -> str:
    out = []
    for line in text.splitlines():
        out.append("#" + line if KEY_LINE.match(line) else line)
    return "\n".join(out) + "\n"


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", required=True, type=Path)
    ap.add_argument("--dll", required=True, type=Path)
    ap.add_argument("--version", required=True)
    ap.add_argument("--out", required=True, type=Path)
    args = ap.parse_args()

    files: dict[str, bytes] = {}
    dropped = []
    with zipfile.ZipFile(args.base) as base:
        for info in base.infolist():
            if info.is_dir() or info.filename in DROP_FROM_BASE:
                continue
            if is_openvr_file(info.filename):
                dropped.append(info.filename)
                continue
            files[info.filename] = base.read(info)

    src = REPO / "L4D2VR"
    files["d3d9.dll"] = args.dll.read_bytes()
    # The 32-bit Khronos OpenXR loader (and its licences) next to d3d9.dll.
    for name, path in OPENXR_RUNTIME_FILES.items():
        files[name] = (REPO / path).read_bytes()
    for name in ("haptics_config.txt", "local_client_convars.nut"):
        files[f"vr/{name}"] = (src / name).read_bytes()
    files["vr/config.sample"] = commented_sample((src / "config.txt").read_text(encoding="utf-8")).encode("utf-8")
    files["vr/version.txt"] = args.version.encode("ascii")
    for resource in sorted((REPO / "resources").rglob("*")):
        if resource.is_file():
            files[resource.relative_to(REPO / "resources").as_posix()] = resource.read_bytes()
    files["PHYSICAL-OVERHAUL.md"] = (REPO / "docs" / "physical-overhaul.md").read_bytes()
    files["manifest.json"] = json.dumps(
        {"version": args.version, "changes": CHANGES, "changesh": []}, ensure_ascii=False, indent=2).encode("utf-8")

    problems = check_package(files)
    if problems:
        raise SystemExit("refusing to package:\n  " + "\n  ".join(problems))
    if dropped:
        print(f"dropped OpenVR files from {args.base.name}: {', '.join(sorted(dropped))}")

    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, "w", zipfile.ZIP_DEFLATED) as out:
        for name in sorted(files):
            out.writestr(name, files[name])
    args.out.write_bytes(buffer.getvalue())
    print(f"{args.out}: {len(files)} files")


if __name__ == "__main__":
    main()
