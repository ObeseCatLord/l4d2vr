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
