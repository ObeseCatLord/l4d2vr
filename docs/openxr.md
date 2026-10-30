# OpenXR

The mod talks to the headset through **OpenXR only**. The release has no OpenVR
backend, no `openvr_api.dll`, and no SteamVR action manifest or binding files. It ships the
32-bit Khronos OpenXR loader (`openxr_loader.dll`, next to `d3d9.dll`).

Left 4 Dead 2 is a 32-bit game. It needs an OpenXR runtime that supports **32-bit applications
with Vulkan**. Without one, the game starts in desktop mode and says why (see
[When VR does not start](#when-vr-does-not-start)).

## Requirements

### Windows

| Runtime | Works? | How |
|---|---|---|
| **SteamVR 2.17 or newer** | yes | SteamVR > Settings > OpenXR > *Set SteamVR as OpenXR Runtime*. SteamVR starts by itself when the game starts. Older SteamVR has no 32-bit OpenXR runtime: update it. |
| **Meta Quest Link / Air Link** (Meta Horizon Link app) | expected | The Meta app registers a 32-bit OpenXR runtime when it is set as the active runtime (Meta app > Settings > General > OpenXR Runtime). If VR does not start, make SteamVR the active OpenXR runtime instead and connect through Link. |
| **Virtual Desktop** (VDXR) | yes | Virtual Desktop's own OpenXR runtime registers a 32-bit version. Pick *VDXR* as the OpenXR runtime in the Virtual Desktop Streamer, or use SteamVR through Virtual Desktop. |
| **Windows Mixed Reality** | through SteamVR only | WMR's own OpenXR runtime is 64-bit only and has no Vulkan. Install *Windows Mixed Reality for SteamVR* and make SteamVR the active OpenXR runtime. |
| Pico, other vendor runtimes | only with a 32-bit Vulkan OpenXR runtime | Otherwise use SteamVR as the active runtime. |

The game reads the active runtime from
`HKLM\SOFTWARE\WOW6432Node\Khronos\OpenXR\1\ActiveRuntime`. That is the 32-bit registration, which
can differ from the 64-bit one other games use. The installer's `status` command (and the GUI's
Play tab) shows both.

No launch options are needed. Running the game as administrator makes the loader ignore
`XR_RUNTIME_JSON` and `XR_API_LAYER_PATH`, so do not rely on those variables to pick a runtime.

### Linux (Proton)

1. **Proton with 32-bit OpenXR**:
   * Proton **Hotfix** 2026-10-07 or newer, or Proton **Experimental** 11.0-20261014 or newer.
   * Proton stable 11.0, 10.0 and older, and GE-Proton builds without the 32-bit `wineopenxr.dll`,
     cannot start VR.
   * Set it under Steam > Left 4 Dead 2 > Properties > Compatibility. Or close Steam and run
     `l4d2vr-installer proton apply` (GUI: Linux setup > *Use the right Proton*). `proton restore`
     undoes it.
2. **Launch options**: `WINEDLLOVERRIDES="d3d9=n,b" PROTON_USE_WOW64=1 %command% -dx9`. The
   installer writes them with Steam closed (`launch-options apply`). Options from older releases
   (`d3d9,openvr_api=n,b`) are rewritten to the new form.
3. **An OpenVR runtime must stay registered**, even though the mod no longer uses OpenVR.
   * Proton only starts its OpenXR bridge (wineopenxr) after it has started an OpenVR runtime from
     `~/.config/openvr/openvrpaths.vrpath`: SteamVR, or XRizer / OpenComposite for Monado and WiVRn.
     Envision registers one.
   * The installer's Monado/WiVRn route keeps passing its XRizer runtime as `VR_OVERRIDE`.
   * If you uninstall the OpenVR runtime, OpenXR stops working too.
4. **An OpenXR runtime**, set as the active one in `~/.config/openxr/1/active_runtime.json`:
   Monado (Envision), WiVRn or SteamVR. Start it before the game.
5. Start the game **from Steam** (or the installer's Monado route, which keeps Steam app id 550).
   Proton only enables VR for a Steam game launch.

`l4d2vr-installer status` checks all of this: the Proton version forced for app 550, the
registered OpenVR runtime, the active OpenXR runtime and the launch options.

## When VR does not start

The mod first checks cheaply, without loading the OpenXR loader:

* **Windows:** an active runtime with a 32-bit registration whose manifest exists.
* **Proton:** a 32-bit `wineopenxr.dll` in the prefix.

It then starts the runtime. If any step fails and `DesktopModeAuto=true` (the default):

* The game runs in **desktop mode**. You can still play and see VR players' bodies.
* `vrmod_log.txt` says why, for example:
  ```
  [VR] OpenXR could not start: the active OpenXR runtime (C:\Windows\system32\MixedRealityRuntime.json) is 64-bit only, and Left 4 Dead 2 is a 32-bit game. Make a 32-bit capable OpenXR runtime the active one: ...; starting in desktop mode
  ```
* The game console shows the same reason once you are in a map (`[L4D2VR] VR is off, desktop mode: ...`).

With `DesktopModeAuto=false` the game shows the error in a message box and exits instead.

| Reason in the log | What to do |
|---|---|
| `no OpenXR runtime is installed` | Install SteamVR 2.17+ (or Virtual Desktop / Meta Link) and set it as the OpenXR runtime. Desktop players can ignore it. |
| `... is 64-bit only` | Typical for WMR or an old SteamVR. Make SteamVR 2.17+ the active OpenXR runtime. |
| `the active 32-bit OpenXR runtime manifest (...) does not exist` | Reinstall the runtime, or select it as the OpenXR runtime again. |
| `the active 32-bit OpenXR runtime (...) is not available (result -51)` | Start the runtime and switch on the headset before the game. |
| `found no headset (XR_ERROR_FORM_FACTOR_UNAVAILABLE)` | Connect and switch on the headset. The mod retries for 4 s. |
| `does not support Vulkan (XR_KHR_vulkan_enable)` | The runtime is Direct3D-only (WMR). Use SteamVR. |
| `this Proton version has no 32-bit OpenXR support` | Switch to Proton Hotfix or Experimental (`l4d2vr-installer proton apply`). |
| `Proton could not start an OpenVR runtime` | Register SteamVR, XRizer or OpenComposite, and start your OpenXR runtime. |
| `Proton did not prepare VR for this launch` | Start the game from Steam. |
| `wineopenxr found no host OpenXR runtime` | Start Monado/WiVRn/SteamVR and set it as the active OpenXR runtime. |

The backend logs every `[VR][OpenXR]` step: the runtime registration it found, the Wine VR state,
the instance (runtime name and version, extensions), the session states and frame counters every
5 s.

## What changed for players

* **Controller bindings are built into the mod.** SteamVR's binding screen for the old OpenVR
  action manifest no longer applies, and old custom SteamVR bindings are not used.
  * The mod suggests its own OpenXR bindings for Index, Touch (Rift, Quest through Link, Virtual
    Desktop, WiVRn, Quest Pro, Quest 3), Vive wands, Vive Cosmos, Vive Focus 3, WMR, HP Reverb G2,
    Pico 4 and the simple controller. The layout is the one in
    [physical-overhaul.md](physical-overhaul.md#controls-physical-layout).
  * SteamVR's OpenXR binding screen can still remap the OpenXR actions. Monado cannot.
* **Trigger and grip thresholds are the mod's**, not SteamVR's per-binding settings:
  * Index grips click at squeeze 0.85 (right, released below 0.5) and 0.8 (left, released below
    0.45), as in the old SteamVR file. A firm, full-hand squeeze is needed.
  * Other analog triggers and grips click at 0.25 and release below 0.2.
  * The Index trackpad press is a force of 0.25.
  * The sticks have a 10% radial deadzone.
* **Menus use the mod's laser**: point either controller at a menu, the HUD, the config panel or its
  button. A light-blue beam and dot appear; the trigger clicks and drags, the stick scrolls. SteamVR's
  dashboard laser is not involved.
* **Text fields** in the config panel open the mod's own VR keyboard below the view. SteamVR's
  keyboard is not used.
* **Hands** look the same: the survivor's own hands (the separate SteamVR glove models are retired;
  `VrHandsGlovesEnabled` is ignored).
* **ReShade**: the exact stereo depth bridge (`ReShadeVRCompat`) needs ReShade's OpenVR runtime. It is
  off in the OpenXR release; the setting is ignored and logged, and ReShade keeps its own Generic Depth
  add-on. ReShade can still be installed as before; only the depth bridge is off.
* `VrRuntime` in `vr/config.txt` and the `-l4d2vr_openxr` / `-l4d2vr_openvr` launch options do
  nothing in the release (a request for OpenVR is logged as ignored).
* The installer removes `openvr_api.dll`, `vr/manifest.vrmanifest` and `vr/SteamVRActionManifest/`
  when it updates an older install. It backs them up first; *Restore* brings them back.

## Headset test checklist (OpenXR)

Run the [general checklist](physical-overhaul.md#headset-test-checklist) on each runtime you
have. Also check the following, and send `vrmod_log.txt` for each session.

Per runtime (SteamVR 2.17+ on Windows; Meta Link; Virtual Desktop; Monado/WiVRn or SteamVR on Linux):

- The log has `[VR] VR runtime: OpenXR (...); OpenXR-only build`, `[VR][OpenXR] instance ready`
  with the runtime's name, and `session FOCUSED`.
- The view is sharp and stable when you turn your head. There is no swimming and no doubled image.
  Canted-display headsets (Pimax, Varjo): check the edges of the view.
- Each controller's profile (`[VR][OpenXR][Input] ... profile`) is the one you hold.
- Index:
  - The grips need a firm squeeze to take an item, and a light touch does nothing.
  - The trigger fires at about a quarter pull.
  - The trackpad press works as a button.
  - The finger tracking follows your fingers.
- Touch/Quest: every button and the stick clicks work, and the trigger and grip fire at about a
  quarter pull.
- The laser and keyboard:
  - The beam appears on the main menu, pause menu, HUD and config panel.
  - The trigger clicks, and dragging a slider works.
  - The stick scrolls lists.
  - A text field opens the keyboard, typing works, and *Done* keeps the text.
  - While the beam is on a panel, the trigger does not fire the gun.
- Overlays: menus, loading screens, the HUD, wrist and ammo HUDs, scope, mirror and kill indicators
  appear where they did with OpenVR.
- Haptics: holster hover ticks, firing and reload pulses.
- Recentre (left stick click), seated and standing height.
- Quit from the main menu: the game exits without hanging. The log shows
  `xrRequestExitSession` and `session ended cleanly` if the game destroys its device first.
- The runtime's dashboard or system menu (SteamVR, Meta, Virtual Desktop) opens over the game,
  and the game continues afterwards.
- No runtime:
  - Turn off or deactivate the OpenXR runtime (or, on Linux, pick an old Proton): the game
    starts in desktop mode, and the log and the console say why.
  - With `DesktopModeAuto=false` the same reason appears in a message box.
- Linux: `l4d2vr-installer status` reports the Proton, OpenVR runtime and OpenXR runtime as OK
  before the session.

## Developers: the legacy OpenVR build

The OpenVR backend stays in the repository (`L4D2VR/vr_backend/vr_backend_openvr.cpp`, the DXVK
OpenVR provider, `L4D2VR/SteamVRActionManifest/`, `L4D2VR/manifest.vrmanifest`). It is not part of
the release.

* Build it: `msbuild l4d2vr.sln /t:L4D2VR /p:Configuration=Release /p:Platform=x86
  /p:L4D2VRLegacyOpenVR=true`. This defines `L4D2VR_LEGACY_OPENVR=1` and links `openvr_api.lib`.
  * That DLL needs `openvr_api.dll` and `vr/SteamVRActionManifest/` next to it.
  * It selects the runtime with `VrRuntime=auto|openxr|openvr` (auto = OpenVR) and the
    `-l4d2vr_openxr` / `-l4d2vr_openvr` launch options.
* Tests: `cmake -S tests -B build -DL4D2VR_LEGACY_OPENVR=ON`. The default configuration tests the
  release policy.
* `tools/package_client_release.py` refuses a `d3d9.dll` that imports `openvr_api.dll`.
* The git tag `openvr-legacy-base` marks the last commit before the release became OpenXR-only.
