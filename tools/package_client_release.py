#!/usr/bin/env python3
"""Build the client release zip (l4d2vr.zip).

Usage:
  package_client_release.py --base BASE.zip --dll d3d9.dll --version 1.2.0 --out l4d2vr.zip

BASE.zip supplies the binary assets this repository does not track (sounds,
overlays, images, openvr_api.dll, UI Font Fix.vpk, ...), e.g. a previous
l4d2vr.zip. Everything the repository does track is taken from the checkout.

vr/config.sample is written with every parameter commented out ("#Key=value").
On startup the mod forces each *uncommented* sample value onto the player's
config.txt, so a fully commented sample only fills in missing keys; changed
defaults for existing players belong in the one-shot config migrations.
"""
import argparse
import io
import json
import re
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
KEY_LINE = re.compile(r"^[A-Za-z][A-Za-z0-9_]*\s*=")

# The OpenXR backend loads openxr_loader.dll from d3d9.dll's folder
# (thirdparty/openxr/README.md: Khronos OpenXR-SDK release-1.1.65, Win32).
OPENXR_RUNTIME_FILES = {"openxr_loader.dll": "thirdparty/openxr/bin/win32/openxr_loader.dll"}

DROP_FROM_BASE = {"SHOTGUN-SHELL-FIX.txt", "patch-shotgun-shell-release.py", "updatelog.txt"}

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
]


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
    with zipfile.ZipFile(args.base) as base:
        for info in base.infolist():
            if info.is_dir() or info.filename in DROP_FROM_BASE:
                continue
            files[info.filename] = base.read(info)

    src = REPO / "L4D2VR"
    files["d3d9.dll"] = args.dll.read_bytes()
    # VR runtime files shipped next to d3d9.dll. openvr_api.dll still comes from
    # BASE.zip (OpenVR backend); the 32-bit Khronos OpenXR loader is tracked here.
    for name, path in OPENXR_RUNTIME_FILES.items():
        files[name] = (REPO / path).read_bytes()
    for manifest in sorted((src / "SteamVRActionManifest").glob("*.json")):
        files[f"vr/SteamVRActionManifest/{manifest.name}"] = manifest.read_bytes()
    for name in ("haptics_config.txt", "local_client_convars.nut", "manifest.vrmanifest"):
        files[f"vr/{name}"] = (src / name).read_bytes()
    files["vr/config.sample"] = commented_sample((src / "config.txt").read_text(encoding="utf-8")).encode("utf-8")
    files["vr/version.txt"] = args.version.encode("ascii")
    for resource in sorted((REPO / "resources").rglob("*")):
        if resource.is_file():
            files[resource.relative_to(REPO / "resources").as_posix()] = resource.read_bytes()
    files["PHYSICAL-OVERHAUL.md"] = (REPO / "docs" / "physical-overhaul.md").read_bytes()
    files["manifest.json"] = json.dumps(
        {"version": args.version, "changes": CHANGES, "changesh": []}, ensure_ascii=False, indent=2).encode("utf-8")

    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, "w", zipfile.ZIP_DEFLATED) as out:
        for name in sorted(files):
            out.writestr(name, files[name])
    args.out.write_bytes(buffer.getvalue())
    print(f"{args.out}: {len(files)} files")


if __name__ == "__main__":
    main()
