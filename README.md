<div align="center">
  
  # L4D2 VR Prototype
  ### Use this mod at your own risk of getting VAC banned.<br>Use the `-insecure` launch option to help protect yourself. (Also contains lots of flashing lights)
</div>

## Video demo
[<img width="640" height="360" alt="image" src="https://github.com/user-attachments/assets/63a16c57-621e-40fc-a581-5f64eafa9633" />](https://www.youtube.com/watch?v=J9vw8VXJWZM)

## About updates
It's best to subscribe to this mod directly on the Workshop to avoid missing updates. <br>[Click here](https://steamcommunity.com/sharedfiles/filedetails/?id=3724995607) to go to L4D2VR's Workshop page

## Things that work
* Singleplayer and [multiplayer](#how-to-play-multiplayer)
* 6DoF VR view
* Full controllers two-handed interaction
* Desktop mirror
* [Workshop content](https://steamcommunity.com/workshop/browse?appid=550)
* [ReShade](#reshade)
* Multi-core rendering **(Enable in [VR Config Menu](#vr-config-menu), not the game's video settings menu)**
* Anti-aliasing **(Enable in [VR Config Menu](#vr-config-menu), not the game's video settings menu)**
  
## Things that need fixing
* Interactions and throwables require you to aim with your face. (Except for VR-only servers)

## How to play multiplayer
* You can join any server to play, but if the server wasn't created by VR, you will only have *some* VR-exclusive features. 
* Versus works, but it's barely been tested.

## Installing
The easiest way to install or update this fork is the [L4D2VR Installer](installer/README.md) (Windows and Linux).
It finds the game, installs the latest release with SHA-256 checks, keeps your configs, backs up what it replaces
and has a **Launch in VR** button. On Linux it also checks for the Windows (Proton) build and sets the launch options.
Manual steps are below.

## Requirements (OpenXR)
The mod uses **OpenXR only** (no SteamVR/OpenVR files). Left 4 Dead 2 is a 32-bit game, so it needs an OpenXR runtime that supports 32-bit games with Vulkan. Details, error messages and what changed: [docs/openxr.md](docs/openxr.md).
* **Windows**: SteamVR 2.17 or newer set as the OpenXR runtime (SteamVR > Settings > OpenXR > *Set SteamVR as OpenXR Runtime*), Virtual Desktop (VDXR) or Meta Quest Link. Windows Mixed Reality's own runtime is 64-bit only: use SteamVR with the WMR for SteamVR driver.
* **Linux**: Proton Hotfix (2026-10-07 or newer) or Proton Experimental (11.0-20261014 or newer), the launch options `WINEDLLOVERRIDES="d3d9=n,b" PROTON_USE_WOW64=1 %command% -dx9`, an OpenVR runtime still registered (SteamVR, or XRizer / OpenComposite for Monado and WiVRn: Proton needs it before its OpenXR bridge starts) and an active OpenXR runtime (Monado, WiVRn or SteamVR). The [installer](installer/README.md) checks and sets the Proton version and launch options.
* Without a usable runtime the game starts in desktop mode and `vrmod_log.txt` and the game console say why.

What changed for players with OpenXR:
* Controller bindings are built into the mod; SteamVR's binding screen for the old action manifest no longer applies.
* The mod applies its own trigger and grip thresholds. Index grips need a firm squeeze (0.85 right, 0.8 left); other triggers and grips click at a quarter pull.
* Menus use the mod's controller laser, and text fields use the mod's VR keyboard.

## How to use
1. Download [L4D2VR.zip](https://github.com/keyou91/l4d2vr/releases) and extract the files to your Left 4 Dead 2 installation directory at: `steamapps/common/Left 4 Dead 2`
    - You can also access the installation directory by right-clicking Left 4 Dead 2 in Steam and going to: Manage > **Browse local files**
    - Updating by hand from an older (OpenVR) release: delete `openvr_api.dll`, `VR/manifest.vrmanifest` and `VR/SteamVRActionManifest/` (the installer does this for you, with a backup).
2. Start your OpenXR runtime (SteamVR starts by itself), then launch Left 4 Dead 2 with these [launch options](https://help.steampowered.com/en/faqs/view/7D01-D2DD-D75E-2955): <br>`-heapsize 524288 -processheap -high -novid -windowed`
    - If you use a desktop client resolution of 2k or higher, add `-bigfonts` to the launch options to make in-game text larger; otherwise, the text on the HUD will be very small.
3. Join or create your campaign and enjoy the game.
4. To recenter the camera height, press down on the left stick. To see the HUD, aim the left controller up or down.

## VR Config Menu
1. Start the game and get to the level.
2. Press the left controller **Y** button to open the pause menu.
3. The **VR Config** button will appear at the top right of the pause menu.
4. Click to open the L4D2VR Config Panel.
5. Change the settings and press the Save button to save, or press the Reload button to undo all changes.

## ReShade
1. Download [ReShade](https://reshade.me/).
2. Select the rendering API Left 4 Dead 2 uses: Vulkan + OpenXR.
> Optional: Select effects to install: [VRToolkit](https://github.com/retroluxfilm/reshade-vrtoolkit) by retroluxfilm.

The exact stereo depth bridge (`ReShadeVRCompat`) works only with ReShade's OpenVR runtime. It is off in the OpenXR release (the setting is ignored and logged), and ReShade keeps its own Generic Depth add-on.

## Troubleshooting
### If the game isn't loading in VR:
* Look for `[VR] OpenXR could not start:` in `vrmod_log.txt` (or the `[L4D2VR] VR is off` line in the game console). It names the problem, for example a 64-bit-only OpenXR runtime or a Proton without 32-bit OpenXR. The fixes are in [docs/openxr.md](docs/openxr.md#when-vr-does-not-start).
* Disable SteamVR theater in [SteamVR settings](https://img.itch.zone/aW1nLzE0MTMyNTY4LnBuZw==/original/pKSXNc.png)

### If the game shows "Failed to create D3D device!":
* L4D2VR uses a custom `d3d9.dll` based on DXVK. Even though the error says D3D, the failure is often the Vulkan backend failing to initialize.
* Update the GPU driver from NVIDIA, AMD, or Intel. Windows Update drivers are often too old for DXVK 2.x.
* Make sure the GPU and driver support Vulkan 1.3. Very old GPUs and some virtual/remote desktop display adapters will not work.
* Do not launch the Steam "Vulkan" launch option and do not add `-vulkan`. Launch the normal DirectX 9 game with the L4D2VR files installed.
* Remove forced display launch options such as `-w`, `-h`, `-fullscreen`, or unusual refresh-rate settings, then try windowed/default video settings.
* Check `left4dead2_d3d9.log` next to `left4dead2.exe`; lines such as "No adapters found" or "A Vulkan 1.3 capable driver is required" indicate a driver/GPU support problem.

### If the game is stuttering: 
* Steam Settings > Downloads > Shader Pre-Caching > **Allow background processing of Vulkan shaders**.

### DXVK async compilation and low-latency frame pacing

The bundled DXVK 2.6.1 includes a source port from
[DXVK-GPLALL 2.6.1-6](https://github.com/Digger1955/dxvk-gplall/tree/7927e64ac836da1ae31e4205f15a8e653d81d654):

* Asynchronous graphics-pipeline compilation is enabled by default. Set
  `DXVK_ASYNC=0`, or `dxvk.enableAsync = False` in `dxvk.conf`, to disable it
  if a newly encountered shader causes temporary missing draws or other visual
  problems.
* GPL/Async state-cache integration is enabled by default. It can be disabled
  with `DXVK_GPLASYNCCACHE=0` or `dxvk.gplAsyncCache = False`.
* Low-latency frame pacing is enabled by default. Select
  `max-frame-latency`, `low-latency`, or `min-latency` with
  `DXVK_FRAME_PACE`, or the matching `dxvk.framePace` value. Fine tuning is
  available through `DXVK_LOW_LATENCY_OFFSET` and
  `DXVK_LOW_LATENCY_ALLOW_CPU_FRAMES_OVERLAP`; see `dxvk_new/dxvk.conf`.
* A per-game `dxvk.conf` next to `left4dead2.exe` takes priority over the
  optional global `%APPDATA%/dxvk.conf`.

The frame pacer controls DXVK's D3D9 CPU/GPU and desktop-present scheduling.
It complements, but does not replace, SteamVR compositor timing or
reprojection. Async compilation originates from work by
[jomihaka](https://github.com/jomihaka/dxvk-poe-hack) and
[Sporif](https://github.com/Sporif/dxvk-async), GPL/Async integration from
[Ph42oN](https://gitlab.com/Ph42oN/dxvk-gplasync/), and low-latency frame
pacing from [netborg-afps](https://github.com/netborg-afps/dxvk/releases).

### If the game is crashing:
* Try lowering the game's video settings.
* Disabling all add-ons, then verifying integrity of game files:
  * Steam > Left 4 Dead 2 > Properties > Installed Files > **Verify integrity of game files**.
* Re-installing the game.

### How to disable it
If you want to return to normal game mode with no VR, there are two methods:
- Method 1: Add the launch option: `-nohmd`.
- Method 2: Find `d3d9.dll` in the game root directory and rename it or move it somewhere else. After launching the game, open the console and enter `crosshair 1` to enable the crosshair again, since L4D2VR disables it. <br>I still recommend keeping d3d9.dll, because it can also be used as DXVK for non-VR mode.

## Build instructions
For this interaction contribution, [build a review candidate](docs/playtest-candidate.md)
with `python -B -X utf8 tools/build_candidate.py`. It uses Visual Studio 2022,
prepares the pinned dependencies and runs the Release/Debug checks in TEMP.
It prints the candidate directory without modifying a game installation.

The original Visual Studio 2019 build route remains available:

1. `git clone --recurse-submodules https://github.com/keyou91/l4d2vr.git`
2. Initialize submodules:
   ```powershell
   git submodule update --init --recursive
   ```
3. Install [DirectX SDK](https://www.microsoft.com/en-us/download/details.aspx?id=6812): request **DirectX Headers and Libs**
4. Run the fixed build script (locks target to `Release|x86`):
   ```powershell
   .\build_release_x86.ps1
   ```
   or:
   ```cmd
   build_release_x86.cmd
   ```
5. (Optional/manual) Open l4d2vr.sln and build `Release|x86`.

The release build is OpenXR-only (no OpenVR backend, no `openvr_api.lib`). A legacy developer build with
the OpenVR backend: `msbuild l4d2vr.sln /t:L4D2VR /p:Configuration=Release /p:Platform=x86 /p:L4D2VRLegacyOpenVR=true`
(tests: `cmake -S tests -B build -DL4D2VR_LEGACY_OPENVR=ON`). See [docs/openxr.md](docs/openxr.md#developers-the-legacy-openvr-build).

> Note: After building, it will attempt to copy the new d3d9.dll to
> `steamapps/common/Left 4 Dead 2`. The pose relay is produced as
> `Release/l4d2vr_pose_server.dll`; copy it and
> `L4D2VR/l4d2vr_pose_server.vdf` to the server's `left4dead2/addons`
> directory.

## Dev note: VTable lookup
For quick vtable inspection, use [Asherkin's VTable Dumper](https://asherkin.github.io/vtable/).
It can be used for `server.dll`-side symbols and also for `engine.dll` targets (drop the binary and search symbol names like `bob`, `viewmodel`, etc.).

## Utilizes code from
* [VirtualFortress2](https://github.com/PinkMilkProductions/VirtualFortress2)
* [gmcl_openvr](https://github.com/Planimeter/gmcl_openvr/)
* [DXVK](https://github.com/doitsujin/dxvk)
* [hl2sdk/l4d2](https://github.com/alliedmodders/hl2sdk/tree/l4d2)
* [ozz-animation](https://github.com/guillaumeblanc/ozz-animation)
