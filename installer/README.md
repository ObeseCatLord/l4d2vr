# L4D2VR Installer

A one-click installer and updater for the Left 4 Dead 2 VR mod
([ObeseCatLord/l4d2vr](https://github.com/ObeseCatLord/l4d2vr)), for Windows and Linux.
It finds your game, installs or updates the mod from the latest release (with
SHA-256 checks), keeps your settings, backs up everything it replaces, and has
a **Launch in VR** button. It also has a full command line for headless use.

## What it does

- **Finds Left 4 Dead 2** in every Steam library (registry + `libraryfolders.vdf`
  on Windows; `~/.local/share/Steam`, `~/.steam/steam`, Flatpak and Snap Steam on Linux).
- **Installs / updates** `l4d2vr.zip` from the latest GitHub release into the game
  folder (next to `left4dead2.exe`). Downloads are checked against the release's
  size and SHA-256 (a `l4d2vr.sha256` asset, a `l4d2vr-manifest.json` asset, or
  GitHub's own asset digest). A mismatch stops the install before any file changes.
- **Keeps your files**: `vr/config.txt`, `vr/config2.txt`,
  `vr/config_migrations_applied.txt`, `viewmodel_adjustments.txt`,
  `viewmodel_auto_grip_cache.txt`, `scope_adjustments.txt`, any
  `vr/*adjustments*.txt` and `vr/*calibration*` are never overwritten once they
  exist. Add your own patterns with `--preserve` (remembered for later updates).
- **Backs up** every file it overwrites or removes to
  `<game>/.l4d2vr-backups/<timestamp>/` with an `index.json`, before touching
  anything. **Restore** undoes the last install/update; **Uninstall** undoes all of
  them, returning the folder to how it was before the first install (files you
  edited since are left alone).
- Records what it installed (release tag, per-file SHA-256) in
  `<game>/vr/installed_release.json`; **Update** compares that with the latest release.
- Creates `vr/disable_workshop_updates.txt` so the mod does not replace this
  build with the Steam Workshop build (untick the option / `--allow-workshop-updates`
  to skip).
- **Linux**: checks you have the Windows build of the game (needed under Proton),
  sets the Proton launch options for you (only while Steam is closed), and offers an
  advanced Monado/WiVRn route via XRizer.
- **Launch in VR**, optional desktop shortcut, optional server join on start.
- Tells you when a newer installer is available (it never replaces itself).

## Windows

1. Install Left 4 Dead 2 in Steam and start it once.
2. Download `l4d2vr-installer-windows.exe` from the
   [latest release](https://github.com/ObeseCatLord/l4d2vr/releases/latest) and run it.
   (SmartScreen may warn about an unsigned program: *More info > Run anyway*.)
3. Check the game folder at the top (use **Browse...** if it was not found), then
   press **Install / Update**.
4. Start SteamVR, open the **Play** tab and press **Launch in VR**
   (this opens `steam://rungameid/550`). **Create desktop shortcut** adds a
   `Left 4 Dead 2 VR.url` to your desktop.

No special Steam launch options are needed on Windows. The mod's recommended
options are in the main [README](../README.md#how-to-use).

## Linux

The mod is a Windows DLL, so the game must run the **Windows build under Proton**.

1. In Steam: right-click Left 4 Dead 2 > **Properties > Compatibility** > tick
   *Force the use of a specific Steam Play compatibility tool* > pick **Proton Hotfix**
   (or Proton Experimental). Let Steam download the files. The installer refuses a
   native Linux install (no `left4dead2.exe`) and tells you this.
2. Run the installer: `chmod +x l4d2vr-installer-linux && ./l4d2vr-installer-linux`
   (or from source: `python3 installer/l4d2vr-installer.py`; needs `python3-tk`).
   Press **Install / Update**.
3. **Launch options** (Linux setup tab). Steam must start the game with:

   ```
   WINEDLLOVERRIDES="d3d9,openvr_api=n,b" PROTON_USE_WOW64=1 %command% -dx9
   ```

   - **Write to Steam**: quit Steam completely first (Steam > Exit). The installer
     edits `userdata/<id>/config/localconfig.vdf` for app 550 only, keeps your
     other options (e.g. `gamemoderun`, `MANGOHUD=1`, a ReShade `dxgi=n,b`
     override), removes `-vulkan`, and saves a `localconfig.vdf.l4d2vr-*.bak` copy.
     **Remove from Steam** puts back what you had before.
   - Or press **Copy** and paste into *Properties > General > Launch options* yourself
     (works while Steam is running).
4. **SteamVR route (default)**: start SteamVR, then **Launch in VR**.
5. **Monado / WiVRn route (advanced)**: for people using Monado (e.g. through
   Envision) or WiVRn instead of SteamVR. It needs an XRizer OpenVR runtime built
   for this game (the repaired one from the Linux handoff bundle, or a
   `xrizer-l4d2-runtime.zip` release asset via **Get XRizer from release**), a
   Steam Runtime entry point (`~/.local/share/umu/steamrt4/_v2-entry-point` or
   *Steam Linux Runtime 4.0* from Steam's Tools) and Proton Hotfix.
   **Auto-detect** fills in Proton, the runtime entry point and the OpenXR manifest
   (active runtime, then the Envision profile selected in Envision, then other
   Envision prefixes and system manifests). Choose the **XRizer runtime folder**,
   run **Preflight check**, pick *Monado / WiVRn* on the Play tab and press
   **Launch in VR**. The game is started directly through the Steam Runtime and
   Proton (Steam must be running), keeping app id 550, with `VR_OVERRIDE` set to
   the XRizer folder and `XR_RUNTIME_JSON` to the OpenXR manifest, in its own
   Proton prefix (`~/.local/state/l4d2vr-installer/proton-prefix`). Logs go to
   `~/.local/state/l4d2vr-installer/runs/<time>/`. Steam launch options are not
   used on this route.

**Create desktop shortcut** writes `~/.local/share/applications/l4d2vr.desktop`
(and one on the Desktop) for the selected route.

## Command line

Run `l4d2vr-installer --help` (or `python3 installer/l4d2vr-installer.py --help`).
Every command that changes files accepts `--dry-run`.

| Command | What it does |
|---|---|
| `detect` | Show Steam folders, game installs, users, Proton builds, OpenXR runtimes |
| `status [--online] [--server-dir D]` | Installed version, changed files, backups, launch options, compat tool |
| `check` | Is a newer mod release (and installer) available? |
| `install` / `update` | Install the latest release; `--zip l4d2vr.zip` installs a local file offline; `--force` reinstalls |
| `restore [--list] [--backup NAME] [--force]` | Undo the most recent install/update |
| `uninstall [--force]` | Undo every install; leaves files you edited |
| `launch-options show\|apply\|remove` | Linux Proton launch options (`--all-users`, `--steam-user ID`) |
| `launch [--route steamvr\|monado] [--connect HOST:PORT] [--password PW]` | Start the game; Monado route: `--xrizer-dir`, `--openxr-manifest`, `--proton-dir`, `--runtime-entrypoint`, `--check`, `--save` |
| `shortcut [--route ...] [--no-desktop]` | Desktop / menu shortcut |
| `install-server --server-dir <srcds>/left4dead2 [--platform linux\|windows] [--zip Z]` | Dedicated-server plugin (server admins) |
| `xrizer [--zip Z] [--dest D]` | Install an XRizer runtime archive (Monado route) |
| `self-update-check` | Is a newer installer build available? |
| `make-manifest` | Release helper (see below) |

Common options: `--game-dir`, `--repo owner/name` (another fork),
`--source github|manifest`, `--manifest-url URL`, `--channel stable|beta`
(beta includes pre-releases). Set `GITHUB_TOKEN` if you hit GitHub's API rate limit.
The server password can also come from `L4D2VR_SERVER_PASSWORD`. Passwords are never saved.

Settings (game folder, source, route, Monado paths) are remembered in
`~/.config/l4d2vr-installer/settings.json` or `%APPDATA%\L4D2VR Installer\settings.json`.

## Server admins

```
l4d2vr-installer install-server --server-dir /srv/l4d2/left4dead2
```

Installs the `l4d2vr-server-linux.zip` (or Windows server) release asset into
`addons/` with the same backup/restore rules, recorded in
`addons/l4d2vr_installed_release.json`. `restore`/`uninstall`/`status` accept
`--server-dir`. Restart the server or change map, then check with `plugin_print`.

## Troubleshooting

- **"native Linux build"**: force Proton for the game (Linux step 1), let Steam
  finish downloading, run the installer again.
- **"Steam is running" when writing launch options**: Steam rewrites
  `localconfig.vdf` when it exits, so the installer only edits it while Steam is
  closed. Exit Steam fully (tray icon > Exit) or paste the options yourself.
- **"Left 4 Dead 2 is running"**: close the game; Windows locks `d3d9.dll` while it runs.
- **SHA-256 / size mismatch**: the download was corrupted or altered. Try again; if it
  persists, report it. Nothing was changed.
- **GitHub rate limit**: wait an hour, set `GITHUB_TOKEN`, or download `l4d2vr.zip` in a
  browser and use **Install from zip...** / `--zip`.
- **"changed since install" on restore**: you edited a file the installer wrote.
  `--force` restores anyway and keeps your copy in the backup's `replaced-on-restore/`.
- **Game starts without VR on Linux**: check `launch-options show`; the
  `WINEDLLOVERRIDES` part is what makes Proton load the mod's `d3d9.dll`.
- **"Failed to create D3D device"**: see the main README (GPU driver / Vulkan 1.3).
- **Monado route**: run `l4d2vr-installer launch --route monado --check`. Start your
  Monado/Envision/WiVRn profile and Steam first. See the Linux handoff README for
  XRizer details and rebuilding.
- **No window opens (Linux, from source)**: install `python3-tk` (`tk` on Arch), or use
  the command line.

## For maintainers

### Release assets

Attach to each GitHub release:

| Asset | Purpose |
|---|---|
| `l4d2vr.zip` | Client package (root: `d3d9.dll`, `openvr_api.dll`, `openxr_loader.dll` (32-bit Khronos loader for `VrRuntime=openxr`), `manifest.json`, `vr/`, `left4dead2/`) |
| `l4d2vr.sha256` | `sha256sum` line(s) for the zips (optional: GitHub's digest is used otherwise) |
| `l4d2vr-server-linux.zip` / `l4d2vr-server-windows.zip` | Server plugin (`left4dead2/addons/...` or `addons/...`) |
| `l4d2vr-installer-windows.exe`, `l4d2vr-installer-linux` | Installer builds; add `-v1.2.0` to the name (or ship `l4d2vr-manifest.json` with `installer.version`) so older installers can tell a newer one exists |
| `xrizer-l4d2-runtime.zip` | Optional XRizer runtime for the Monado route |

`l4d2vr-installer make-manifest --zip l4d2vr.zip --version 1.2.0 --tag v1.2.0 --url <download URL> --output installer/manifest.json --sha-file l4d2vr.sha256`
updates the repo manifest (`--source manifest`, BigWalk-style: `version`, `url`,
`sha256`, `size`, `preserve`, `channels.beta`, `installer`) and writes the checksum file.

### Building the binaries

PyInstaller, one file per OS, built on that OS with a Python that includes tkinter:

```
python installer/packaging/build.py --test
```

This creates a private venv (`installer/.build-venv`), installs PyInstaller, builds
from `installer/packaging/l4d2vr-installer.spec` and writes
`installer/dist/l4d2vr-installer-<os>[.exe]` plus a `.sha256`. For a Linux binary
that works on older distributions, build on an old glibc (e.g. Debian 11 container).

### Tests

```
cd installer && python3 -m unittest
```

Standard library only; tests use temporary folders, fake Steam trees, fake
`/proc` entries and `file://` URLs instead of the network, and never touch the
real Steam install or game.

### Layout

| File | Role |
|---|---|
| `l4d2vr_installer/vdf.py` | Valve KeyValues reader/writer (order/case preserving) |
| `l4d2vr_installer/steam.py` | Steam/library/game/user discovery, depot check, process checks |
| `l4d2vr_installer/launchopts.py` | Proton launch option merge/strip and `localconfig.vdf` editing |
| `l4d2vr_installer/sources.py` | GitHub releases, manifest, download + checksum verification |
| `l4d2vr_installer/install.py` | Install/update/backup/restore/uninstall engine |
| `l4d2vr_installer/vrlaunch.py` | Steam URL launch and the Monado/XRizer launcher |
| `l4d2vr_installer/actions.py` | Operations shared by CLI and GUI |
| `l4d2vr_installer/cli.py`, `gui.py` | Command line and tkinter front ends |
