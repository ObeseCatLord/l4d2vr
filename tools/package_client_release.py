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
