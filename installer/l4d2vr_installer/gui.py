"""tkinter GUI. All real work happens in :mod:`actions` on a worker thread;
the GUI only collects options and shows the log."""

from __future__ import annotations

import os
import queue
import subprocess
import sys
import threading
import tkinter as tk
from pathlib import Path
from tkinter import filedialog, messagebox, ttk
from tkinter.scrolledtext import ScrolledText

from . import DEFAULT_REPO, __version__, actions, install, launchopts, settings, shortcut, steam, vrlaunch
from .fsutil import InstallerError, user_config_dir

IS_WINDOWS = sys.platform.startswith("win")
PAD = {"padx": 6, "pady": 3}


class App:
    def __init__(self, root: tk.Tk):
        self.root = root
        self.saved = settings.load()
        self.queue: "queue.Queue[tuple]" = queue.Queue()
        self.busy = False
        self.buttons = []

        root.title(f"L4D2VR Installer {__version__}")
        root.minsize(720, 560)

        monado = self.saved.get("monado") or {}
        self.v = {
            "game_dir": tk.StringVar(value=self.saved.get("game_dir", "")),
            "source": tk.StringVar(value=self.saved.get("source", "github")),
            "repo": tk.StringVar(value=self.saved.get("repo") or DEFAULT_REPO),
            "channel": tk.StringVar(value=self.saved.get("channel", "stable")),
            "manifest_url": tk.StringVar(value=self.saved.get("manifest_url", "")),
            "route": tk.StringVar(value=self.saved.get("route", "steamvr")),
            "server": tk.StringVar(value=self.saved.get("server", "")),
            "server_dir": tk.StringVar(value=""),
            "server_platform": tk.StringVar(value="linux"),
            "xrizer_dir": tk.StringVar(value=monado.get("xrizer_dir", "")),
            "openxr_manifest": tk.StringVar(value=monado.get("openxr_manifest", "")),
            "proton_dir": tk.StringVar(value=monado.get("proton_dir", "")),
            "steam_runtime_entrypoint": tk.StringVar(value=monado.get("steam_runtime_entrypoint", "")),
            "status": tk.StringVar(value="Looking for Left 4 Dead 2..."),
            "launch_status": tk.StringVar(value=""),
            "openxr_status": tk.StringVar(value=""),
        }
        self.pin = tk.BooleanVar(value=self.saved.get("pin_workshop", True))
        self.dry_run = tk.BooleanVar(value=False)
        self.password = tk.StringVar(value="")

        self._build()
        self.root.after(100, self._poll)
        self.run(self._startup, label=None)

    # ------------------------------------------------------------------ layout
    def _button(self, parent, text, command, **grid):
        button = ttk.Button(parent, text=text, command=command)
        button.grid(**{**PAD, **grid})
        self.buttons.append(button)
        return button

    def _path_row(self, parent, row, label, var, directory=True):
        ttk.Label(parent, text=label).grid(row=row, column=0, sticky="w", **PAD)
        ttk.Entry(parent, textvariable=var).grid(row=row, column=1, sticky="ew", **PAD)

        def browse():
            chosen = filedialog.askdirectory() if directory else filedialog.askopenfilename()
            if chosen:
                var.set(chosen)
        self._button(parent, "Browse...", browse, row=row, column=2)

    def _build(self):
        outer = ttk.Frame(self.root, padding=8)
        outer.pack(fill="both", expand=True)
        outer.columnconfigure(0, weight=1)
        outer.rowconfigure(2, weight=1)

        game = ttk.LabelFrame(outer, text="Left 4 Dead 2 folder (contains left4dead2.exe)")
        game.grid(row=0, column=0, sticky="ew")
        game.columnconfigure(0, weight=1)
        ttk.Entry(game, textvariable=self.v["game_dir"]).grid(row=0, column=0, sticky="ew", **PAD)
        self._button(game, "Browse...", self.browse_game, row=0, column=1)
        self._button(game, "Detect", lambda: self.run(self._detect), row=0, column=2)
        ttk.Label(game, textvariable=self.v["status"], wraplength=680, justify="left").grid(
            row=1, column=0, columnspan=3, sticky="w", **PAD)

        notebook = ttk.Notebook(outer)
        notebook.grid(row=1, column=0, sticky="ew", pady=6)
        notebook.add(self._install_tab(notebook), text="Install / Update")
        notebook.add(self._play_tab(notebook), text="Play")
        if not IS_WINDOWS:
            notebook.add(self._linux_tab(notebook), text="Linux setup")
        notebook.add(self._server_tab(notebook), text="Server plugin")

        log_frame = ttk.LabelFrame(outer, text="Log")
        log_frame.grid(row=2, column=0, sticky="nsew")
        log_frame.columnconfigure(0, weight=1)
        log_frame.rowconfigure(0, weight=1)
        self.log_box = ScrolledText(log_frame, height=10, state="disabled", wrap="word")
        self.log_box.grid(row=0, column=0, sticky="nsew")
        self.progress = ttk.Progressbar(log_frame, mode="determinate", maximum=100)
        self.progress.grid(row=1, column=0, sticky="ew")

    def _install_tab(self, parent):
        tab = ttk.Frame(parent, padding=6)
        tab.columnconfigure(1, weight=1)
        ttk.Label(tab, text="Source").grid(row=0, column=0, sticky="w", **PAD)
        src = ttk.Frame(tab)
        src.grid(row=0, column=1, columnspan=2, sticky="w")
        ttk.Radiobutton(src, text="GitHub releases", value="github", variable=self.v["source"]).pack(side="left")
        ttk.Radiobutton(src, text="Manifest", value="manifest", variable=self.v["source"]).pack(side="left")
        ttk.Label(src, text="  Channel").pack(side="left")
        ttk.Combobox(src, textvariable=self.v["channel"], values=("stable", "beta"), width=8,
                     state="readonly").pack(side="left")
        ttk.Label(tab, text="GitHub repo").grid(row=1, column=0, sticky="w", **PAD)
        ttk.Entry(tab, textvariable=self.v["repo"]).grid(row=1, column=1, columnspan=2, sticky="ew", **PAD)
        ttk.Label(tab, text="Manifest URL").grid(row=2, column=0, sticky="w", **PAD)
        ttk.Entry(tab, textvariable=self.v["manifest_url"]).grid(row=2, column=1, columnspan=2, sticky="ew", **PAD)
        opts = ttk.Frame(tab)
        opts.grid(row=3, column=0, columnspan=3, sticky="w")
        ttk.Checkbutton(opts, text="Turn off the mod's Steam Workshop self-update (recommended)",
                        variable=self.pin).pack(side="left", **PAD)
        ttk.Checkbutton(opts, text="Dry run (show changes only)", variable=self.dry_run).pack(side="left", **PAD)
        row = ttk.Frame(tab)
        row.grid(row=4, column=0, columnspan=3, sticky="w")
        for column, (text, command) in enumerate((
            ("Install / Update", lambda: self.run(self._install)),
            ("Install from zip...", self.install_zip),
            ("Check for updates", lambda: self.run(self._check)),
            ("Restore last backup", self.restore),
            ("Uninstall", self.uninstall),
            ("Open folder", self.open_game_folder),
        )):
            self._button(row, text, command, row=0, column=column)
        return tab

    def _play_tab(self, parent):
        tab = ttk.Frame(parent, padding=6)
        tab.columnconfigure(1, weight=1)
        ttk.Label(tab, text="VR runtime").grid(row=0, column=0, sticky="w", **PAD)
        routes = ttk.Frame(tab)
        routes.grid(row=0, column=1, sticky="w")
        ttk.Radiobutton(routes, text="SteamVR", value="steamvr", variable=self.v["route"]).pack(side="left")
        monado = ttk.Radiobutton(routes, text="Monado / WiVRn via XRizer (advanced, Linux)", value="monado",
                                 variable=self.v["route"])
        monado.pack(side="left")
        if IS_WINDOWS:
            monado.state(["disabled"])
            self.v["route"].set("steamvr")
        ttk.Label(tab, text="Join server (optional)").grid(row=1, column=0, sticky="w", **PAD)
        ttk.Entry(tab, textvariable=self.v["server"]).grid(row=1, column=1, sticky="ew", **PAD)
        ttk.Label(tab, text="Server password").grid(row=2, column=0, sticky="w", **PAD)
        ttk.Entry(tab, textvariable=self.password, show="*").grid(row=2, column=1, sticky="ew", **PAD)
        row = ttk.Frame(tab)
        row.grid(row=3, column=0, columnspan=2, sticky="w")
        self._button(row, "Launch in VR", lambda: self.run(self._launch), row=0, column=0)
        self._button(row, "Create desktop shortcut", lambda: self.run(self._shortcut), row=0, column=1)
        ttk.Label(tab, text="The mod uses OpenXR. Windows: the active OpenXR runtime must support 32-bit games "
                            "(SteamVR 2.17 or newer, Virtual Desktop, Meta Quest Link; not Windows Mixed Reality's "
                            "own runtime). Start it, then press Launch. Linux needs the Proton version and launch "
                            "options from the 'Linux setup' tab. Without a usable runtime the game starts in desktop "
                            "mode and vrmod_log.txt says why.",
                  wraplength=660, justify="left").grid(row=4, column=0, columnspan=2, sticky="w", **PAD)
        ttk.Label(tab, textvariable=self.v["openxr_status"], wraplength=660, justify="left").grid(
            row=5, column=0, columnspan=2, sticky="w", **PAD)
        return tab

    def _linux_tab(self, parent):
        tab = ttk.Frame(parent, padding=6)
        tab.columnconfigure(1, weight=1)
        ttk.Label(tab, text="Steam launch options").grid(row=0, column=0, sticky="w", **PAD)
        entry = ttk.Entry(tab)
        entry.insert(0, launchopts.RECOMMENDED)
        entry.state(["readonly"])
        entry.grid(row=0, column=1, sticky="ew", **PAD)
        self._button(tab, "Copy", self.copy_launch_options, row=0, column=2)
        row = ttk.Frame(tab)
        row.grid(row=1, column=0, columnspan=3, sticky="w")
        self._button(row, "Write to Steam (Steam must be closed)", lambda: self.run(self._apply_launch_options),
                     row=0, column=0)
        self._button(row, "Remove from Steam", lambda: self.run(self._remove_launch_options), row=0, column=1)
        self._button(row, "Show current", lambda: self.run(self._show_launch_options), row=0, column=2)
        ttk.Label(tab, textvariable=self.v["launch_status"], wraplength=660, justify="left").grid(
            row=2, column=0, columnspan=3, sticky="w", **PAD)

        proton = ttk.LabelFrame(tab, text="Proton version (VR needs 32-bit OpenXR: Proton Hotfix or Experimental)")
        proton.grid(row=3, column=0, columnspan=3, sticky="ew", pady=6)
        prow = ttk.Frame(proton)
        prow.grid(row=0, column=0, sticky="w")
        self._button(prow, "Check", lambda: self.run(self._check_openxr), row=0, column=0)
        self._button(prow, "Use the right Proton (Steam must be closed)", lambda: self.run(self._apply_proton),
                     row=0, column=1)
        self._button(prow, "Undo", lambda: self.run(self._restore_proton), row=0, column=2)

        box = ttk.LabelFrame(tab, text="Monado / WiVRn route (advanced)")
        box.grid(row=4, column=0, columnspan=3, sticky="ew", pady=6)
        box.columnconfigure(1, weight=1)
        self._path_row(box, 0, "XRizer runtime folder", self.v["xrizer_dir"])
        self._path_row(box, 1, "OpenXR manifest", self.v["openxr_manifest"], directory=False)
        self._path_row(box, 2, "Proton folder", self.v["proton_dir"])
        self._path_row(box, 3, "Steam Runtime entry", self.v["steam_runtime_entrypoint"], directory=False)
        row = ttk.Frame(box)
        row.grid(row=4, column=0, columnspan=3, sticky="w")
        self._button(row, "Auto-detect", lambda: self.run(self._detect_monado), row=0, column=0)
        self._button(row, "Preflight check", lambda: self.run(self._preflight), row=0, column=1)
        self._button(row, "Get XRizer from release", lambda: self.run(self._xrizer), row=0, column=2)
        return tab

    def _server_tab(self, parent):
        tab = ttk.Frame(parent, padding=6)
        tab.columnconfigure(1, weight=1)
        self._path_row(tab, 0, "Server left4dead2 folder", self.v["server_dir"])
        ttk.Label(tab, text="Server OS").grid(row=1, column=0, sticky="w", **PAD)
        ttk.Combobox(tab, textvariable=self.v["server_platform"], values=("linux", "windows"),
                     state="readonly", width=10).grid(row=1, column=1, sticky="w", **PAD)
        self._button(tab, "Install / update server plugin", lambda: self.run(self._install_server),
                     row=2, column=0, sticky="w")
        ttk.Label(tab, text="Only for people running their own dedicated server. Players do not need this.",
                  wraplength=660).grid(row=3, column=0, columnspan=3, sticky="w", **PAD)
        return tab

    # ------------------------------------------------------------------ plumbing
    def log(self, message: str) -> None:
        self.queue.put(("log", message))

    def progress_cb(self, done, total):
        if total:
            self.queue.put(("progress", done * 100 / total))

    def _poll(self):
        try:
            while True:
                kind, value = self.queue.get_nowait()
                if kind == "log":
                    self.log_box.configure(state="normal")
                    self.log_box.insert("end", value + "\n")
                    self.log_box.see("end")
                    self.log_box.configure(state="disabled")
                elif kind == "progress":
                    self.progress["value"] = value
                elif kind == "status":
                    self.v["status"].set(value)
                elif kind == "launch_status":
                    self.v["launch_status"].set(value)
                elif kind == "openxr_status":
                    self.v["openxr_status"].set(value)
                elif kind == "set":
                    name, text = value
                    self.v[name].set(text)
                elif kind == "done":
                    self._set_busy(False)
                elif kind == "error":
                    messagebox.showerror("L4D2VR Installer", value)
                elif kind == "info":
                    messagebox.showinfo("L4D2VR Installer", value)
        except queue.Empty:
            pass
        self.root.after(100, self._poll)

    def _set_busy(self, busy: bool):
        self.busy = busy
        for button in self.buttons:
            button.state(["disabled"] if busy else ["!disabled"])
        if not busy:
            self.progress["value"] = 0

    def run(self, func, label="Working..."):
        if self.busy:
            return
        self._save()
        self._set_busy(True)
        snapshot = self._snapshot()

        def worker():
            try:
                func(snapshot)
            except InstallerError as error:
                self.log(f"ERROR: {error}")
                self.queue.put(("error", str(error)))
            except Exception as error:  # show unexpected errors instead of dying silently
                self.log(f"Unexpected error: {error!r}")
                self.queue.put(("error", f"Unexpected error: {error!r}"))
            finally:
                self.queue.put(("done", None))
        threading.Thread(target=worker, daemon=True).start()

    def _snapshot(self) -> dict:
        """Read all Tk variables on the main thread (Tk is not thread-safe)."""
        snap = {name: var.get() for name, var in self.v.items()}
        snap.update(pin=self.pin.get(), dry_run=self.dry_run.get(), password=self.password.get())
        return snap

    def _save(self):
        snap = self._snapshot()
        self.saved.update({key: snap[key] for key in ("game_dir", "source", "repo", "channel", "manifest_url",
                                                         "route", "server")})
        self.saved["pin_workshop"] = snap["pin"]
        self.saved["monado"] = {key: snap[key] for key in ("xrizer_dir", "openxr_manifest", "proton_dir",
                                                            "steam_runtime_entrypoint")}
        try:
            settings.save(self.saved)
        except (OSError, InstallerError):
            pass

    def _options(self, snap) -> actions.SourceOptions:
        return actions.SourceOptions(snap["source"], snap["repo"].strip() or DEFAULT_REPO, snap["channel"],
                                     snap["manifest_url"].strip() or None)

    def _game(self, snap) -> Path:
        return actions.require_game_dir(snap["game_dir"].strip() or None)

    # ------------------------------------------------------------------ actions (worker thread)
    def _startup(self, snap):
        self._detect(snap, quiet=True)
        try:
            self._check_openxr(snap, quiet=True)
        except InstallerError:
            pass
        try:
            info = actions.check_installer_update(self._options(snap), log=lambda _m: None)
            if info.newer:
                self.log(info.message)
                self.queue.put(("info", info.message + "\n\nDownload it from the release page; "
                                                       "this installer does not replace itself."))
        except InstallerError:
            pass

    def _detect(self, snap, quiet=False):
        game = Path(snap["game_dir"]) if snap["game_dir"] else None
        if game is None or not game.is_dir():
            installs = steam.find_game_installs()
            if not installs:
                self.queue.put(("status", "Left 4 Dead 2 was not found. Use Browse to pick its folder."))
                return
            game = installs[0].path
            self.queue.put(("set", ("game_dir", str(game))))
        depot = steam.game_depot(game)
        if depot == "linux":
            self.queue.put(("status", "Native Linux build found: switch the game to Proton first (see message)."))
            if not quiet:
                self.queue.put(("error", steam.NATIVE_LINUX_HELP))
            else:
                self.log(steam.NATIVE_LINUX_HELP)
            return
        if depot == "missing":
            self.queue.put(("status", f"{game} does not contain left4dead2.exe."))
            return
        state = install.client_target(game).read_state() or {}
        version = state.get("version")
        text = f"Found: {game}\n" + (f"Installed mod: {version} ({state.get('source')})" if version
                                     else "The mod is not installed by this installer yet.")
        self.queue.put(("status", text))
        if not quiet:
            for line in actions.status_lines(game):
                self.log(line)

    def _install(self, snap):
        game = self._game(snap)
        actions.install_client(game, options=self._options(snap), pin_workshop=snap["pin"],
                               extra_preserve=self.saved.get("extra_preserve"), dry_run=snap["dry_run"],
                               log=self.log, progress=self.progress_cb)
        self._detect(snap | {"game_dir": str(game)}, quiet=True)
        if not snap["dry_run"] and not IS_WINDOWS:
            self.log("Linux: now set the Steam launch options (Linux setup tab).")

    def install_zip(self):
        path = filedialog.askopenfilename(title="Choose l4d2vr.zip", filetypes=[("Zip archives", "*.zip")])
        if not path:
            return

        def job(snap):
            game = self._game(snap)
            actions.install_client(game, zip_path=Path(path), pin_workshop=snap["pin"],
                                   extra_preserve=self.saved.get("extra_preserve"),
                                   dry_run=snap["dry_run"], log=self.log)
            self._detect(snap | {"game_dir": str(game)}, quiet=True)
        self.run(job)

    def _check(self, snap):
        game = self._game(snap)
        result = actions.check_for_update(game, self._options(snap), log=self.log)
        self.log(result.message())
        self.queue.put(("info", result.message()))

    def restore(self):
        if not messagebox.askyesno("Restore", "Undo the most recent install/update using its backup?"):
            return

        def job(snap):
            target = install.client_target(self._game(snap))
            install.restore_backup(target, dry_run=snap["dry_run"], log=self.log)
            self._detect(snap, quiet=True)
        self.run(job)

    def uninstall(self):
        if not messagebox.askyesno("Uninstall", "Remove the mod files the installer added and put back the "
                                                "originals? Your own configs that you edited are kept."):
            return

        def job(snap):
            target = install.client_target(self._game(snap))
            install.uninstall(target, dry_run=snap["dry_run"], log=self.log)
            self._detect(snap, quiet=True)
        self.run(job)

    def browse_game(self):
        chosen = filedialog.askdirectory(title="Left 4 Dead 2 folder (with left4dead2.exe)")
        if chosen:
            self.v["game_dir"].set(chosen)
            self.run(self._detect)

    def open_game_folder(self):
        path = self.v["game_dir"].get()
        if not path or not os.path.isdir(path):
            return
        if IS_WINDOWS:
            os.startfile(path)  # type: ignore[attr-defined]
        else:
            subprocess.Popen(["xdg-open", path], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def copy_launch_options(self):
        self.root.clipboard_clear()
        self.root.clipboard_append(launchopts.RECOMMENDED)
        self.log("Copied. In Steam: right-click Left 4 Dead 2 > Properties > General > Launch options, paste.")

    def _users(self, snap):
        game = Path(snap["game_dir"]) if snap["game_dir"] else None
        return actions.launch_option_users(game)

    def _show_launch_options(self, snap):
        game = Path(snap["game_dir"]) if snap["game_dir"] else None
        lines = actions.launch_options_lines(game)
        for line in lines:
            self.log(line)
        self.queue.put(("launch_status", "\n".join(lines)))

    def _apply_launch_options(self, snap):
        if steam.is_steam_running():
            raise InstallerError("Steam is running. Exit Steam completely (Steam > Exit), then press this again "
                                 "- or use Copy and paste the options into the game's Properties yourself.")
        user = self._users(snap)[0]
        change = launchopts.apply_vr_launch_options(user.localconfig, steam_running=False,
                                                    dry_run=snap["dry_run"], state_dir=user_config_dir())
        for note in change.notes:
            self.log("Note: " + note)
        self.log(f"Launch options for {user.name or user.account_id}: {change.after}"
                 + (" (unchanged)" if not change.changed else " (dry run)" if snap["dry_run"] else " (saved)"))
        self._show_launch_options(snap)

    def _remove_launch_options(self, snap):
        if steam.is_steam_running():
            raise InstallerError("Steam is running. Exit Steam completely, then try again.")
        user = self._users(snap)[0]
        change = launchopts.remove_vr_launch_options(user.localconfig, steam_running=False,
                                                     dry_run=snap["dry_run"], state_dir=user_config_dir())
        self.log(f"Launch options now: {change.after or '(empty)'}")
        self._show_launch_options(snap)

    def _check_openxr(self, snap, quiet=False):
        game = Path(snap["game_dir"]) if snap["game_dir"] else None
        lines = actions.openxr_requirement_lines(game)
        problems = [line for line in lines if " OK" not in line and not line.startswith("OpenXR runtime: /")]
        self.queue.put(("openxr_status", "\n".join(lines)))
        if not quiet or problems:
            for line in lines:
                self.log(line)

    def _apply_proton(self, snap):
        if IS_WINDOWS:
            raise InstallerError("Proton is only used on Linux.")
        if steam.is_steam_running():
            raise InstallerError("Steam is running. Exit Steam completely (Steam > Exit), then press this again "
                                 "- or pick Proton Hotfix in the game's Properties > Compatibility yourself.")
        game = Path(snap["game_dir"]) if snap["game_dir"] else None
        actions.apply_proton(game, dry_run=snap["dry_run"], log=self.log)
        self._check_openxr(snap, quiet=True)

    def _restore_proton(self, snap):
        if steam.is_steam_running():
            raise InstallerError("Steam is running. Exit Steam completely, then try again.")
        game = Path(snap["game_dir"]) if snap["game_dir"] else None
        actions.restore_proton(game, dry_run=snap["dry_run"], log=self.log)
        self._check_openxr(snap, quiet=True)

    def _monado_config(self, snap, game: Path) -> vrlaunch.MonadoConfig:
        config = vrlaunch.MonadoConfig.from_dict({key: snap[key] for key in (
            "xrizer_dir", "openxr_manifest", "proton_dir", "steam_runtime_entrypoint")})
        config.game_dir = str(game)
        roots = steam.find_steam_roots()
        libraries = [lib for root in roots for lib in steam.library_folders(root)]
        return vrlaunch.autodetect(config, libraries, steam.steam_root_for_game(game, roots))

    def _detect_monado(self, snap):
        config = self._monado_config(snap, self._game(snap))
        for key in ("xrizer_dir", "openxr_manifest", "proton_dir", "steam_runtime_entrypoint"):
            self.queue.put(("set", (key, getattr(config, key))))
            self.log(f"{key}: {getattr(config, key) or 'not found'}")

    def _preflight(self, snap):
        problems, warnings = vrlaunch.validate(self._monado_config(snap, self._game(snap)))
        for warning in warnings:
            self.log("Warning: " + warning)
        for problem in problems:
            self.log("Problem: " + problem)
        self.log("Preflight passed." if not problems else "Preflight found problems (see above).")

    def _xrizer(self, snap):
        release = actions.fetch_release(self._options(snap), log=self.log)
        if release.xrizer is None:
            raise InstallerError("The latest release has no XRizer runtime. Select a runtime folder you built "
                                 "or received (it must contain libxrizer.so and bin/linux64/vrclient.so).")
        dest = vrlaunch.default_xrizer_dir()
        if dest.exists() and any(dest.iterdir()):
            raise InstallerError(f"{dest} already exists; select it as the XRizer folder or delete it first.")
        zip_path = actions.download_asset(release.xrizer, self.log, self.progress_cb)
        vrlaunch.install_xrizer_zip(zip_path, dest, dry_run=snap["dry_run"], log=self.log)
        self.queue.put(("set", ("xrizer_dir", str(dest))))

    def _launch(self, snap):
        game = self._game(snap)
        server = snap["server"].strip() or None
        password = snap["password"] or None
        if snap["route"] == "monado" and not IS_WINDOWS:
            vrlaunch.launch_monado(self._monado_config(snap, game), server, password, dry_run=snap["dry_run"],
                                   wait=False, log=self.log, game_running=steam.is_game_running)
            return
        if not IS_WINDOWS:
            try:
                user = self._users(snap)[0]
                if not launchopts.has_required_options(launchopts.read_launch_options(user.localconfig)):
                    self.log("Warning: Steam launch options for VR are not set (Linux setup tab). "
                             "The game will start without the mod's DLL overrides.")
            except InstallerError as error:
                self.log(f"Warning: {error}")
        url = vrlaunch.steam_url(vrlaunch.connect_args(server, password))
        vrlaunch.open_steam_url(url, dry_run=snap["dry_run"])
        self.log("Asked Steam to start Left 4 Dead 2." + (" Steam may ask you to confirm the server arguments."
                                                         if server else ""))

    def _shortcut(self, snap):
        game = self._game(snap)
        if IS_WINDOWS:
            paths = shortcut.install_windows_shortcut(vrlaunch.steam_url(), game, dry_run=snap["dry_run"])
        elif snap["route"] == "monado":
            paths = shortcut.install_linux_shortcut(
                shortcut.installer_command() + ["launch", "--route", "monado", "--game-dir", str(game)],
                dry_run=snap["dry_run"])
        else:
            paths = shortcut.install_linux_shortcut(["xdg-open", vrlaunch.steam_url()], dry_run=snap["dry_run"])
        for path in paths:
            self.log(("Would create " if snap["dry_run"] else "Created ") + str(path))

    def _install_server(self, snap):
        if not snap["server_dir"]:
            raise InstallerError("Choose the server's left4dead2 folder first.")
        actions.install_server(Path(snap["server_dir"]), platform=snap["server_platform"],
                               options=self._options(snap), dry_run=snap["dry_run"], log=self.log)


def _hide_own_console_on_windows() -> None:
    """The Windows build is a console program so the CLI can print. When it
    was double-clicked (the console belongs to this process alone), hide that
    console window behind the GUI."""
    if not IS_WINDOWS:
        return
    try:
        import ctypes
        kernel32 = ctypes.windll.kernel32  # type: ignore[attr-defined]
        processes = (ctypes.c_uint * 4)()
        if kernel32.GetConsoleProcessList(processes, 4) <= 2:  # this process (+ PyInstaller bootloader)
            window = kernel32.GetConsoleWindow()
            if window:
                ctypes.windll.user32.ShowWindow(window, 0)  # type: ignore[attr-defined]
    except (AttributeError, OSError):
        pass


def main() -> int:
    _hide_own_console_on_windows()
    try:
        root = tk.Tk()
    except tk.TclError as error:
        print(f"Cannot open a window ({error}). Use the command line: l4d2vr-installer --help", file=sys.stderr)
        return 2
    App(root)
    root.mainloop()
    return 0
