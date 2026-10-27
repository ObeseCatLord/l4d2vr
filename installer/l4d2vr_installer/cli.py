"""Command line interface. Run without arguments to open the GUI."""

from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path
from typing import List, Optional

from . import DEFAULT_REPO, __version__, actions, install, launchopts, settings, shortcut, sources, steam, vrlaunch
from .fsutil import InstallerError, atomic_write_json, read_json, user_config_dir

IS_WINDOWS = sys.platform.startswith("win")


def _add_source_args(parser: argparse.ArgumentParser) -> None:
    group = parser.add_argument_group("release source")
    group.add_argument("--repo", help=f"GitHub owner/name (default {DEFAULT_REPO})")
    group.add_argument("--source", choices=("github", "manifest"), help="where to look for releases (default github)")
    group.add_argument("--manifest-url", help="manifest JSON URL (with --source manifest)")
    group.add_argument("--channel", choices=("stable", "beta"), help="beta includes pre-releases")
    group.add_argument("--api-base", default=sources.GITHUB_API, help=argparse.SUPPRESS)


def _add_game_arg(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--game-dir", help="Left 4 Dead 2 folder (the one containing left4dead2.exe)")


def _add_dry_run(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--dry-run", action="store_true", help="show what would change without changing anything")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="l4d2vr-installer",
        description="Install, update and launch the Left 4 Dead 2 VR mod. Run without a command for the GUI.",
    )
    parser.add_argument("--version", action="version", version=f"%(prog)s {__version__}")
    sub = parser.add_subparsers(dest="command", metavar="command")

    sub.add_parser("gui", help="open the graphical installer")

    p = sub.add_parser("detect", help="show Steam, game and VR runtime locations that were found")
    p.add_argument("--json", action="store_true")

    p = sub.add_parser("status", help="installed version, changed files, backups, launch options")
    _add_game_arg(p)
    p.add_argument("--server-dir", help="also show a dedicated server plugin install")
    p.add_argument("--online", action="store_true", help="also check the latest release")
    _add_source_args(p)

    p = sub.add_parser("check", help="check for a newer mod release (and a newer installer)")
    _add_game_arg(p)
    _add_source_args(p)

    for name, text in (("install", "install the mod (latest release, or --zip)"),
                       ("update", "update to the latest release (same as install)")):
        p = sub.add_parser(name, help=text)
        _add_game_arg(p)
        _add_source_args(p)
        _add_dry_run(p)
        p.add_argument("--zip", help="install this local l4d2vr.zip instead of downloading (offline)")
        p.add_argument("--force", action="store_true", help="reinstall even if already up to date")
        p.add_argument("--preserve", action="append", default=[], metavar="PATTERN",
                       help="extra file pattern to keep on updates, e.g. 'vr/my_*.txt' (repeatable, remembered)")
        p.add_argument("--allow-workshop-updates", action="store_true",
                       help="do not create vr/disable_workshop_updates.txt")

    for name, text in (("restore", "undo the most recent install/update from its backup"),
                       ("uninstall", "remove everything the installer installed (restores originals)")):
        p = sub.add_parser(name, help=text)
        _add_game_arg(p)
        _add_dry_run(p)
        p.add_argument("--server-dir", help="act on a dedicated server plugin install instead")
        p.add_argument("--force", action="store_true", help="restore files even if you changed them since")
        if name == "restore":
            p.add_argument("--backup", help="backup folder name (default: most recent)")
            p.add_argument("--list", action="store_true", help="only list backups")

    p = sub.add_parser("launch-options", help="Steam launch options needed under Proton (Linux)")
    p.add_argument("action", choices=("show", "apply", "remove"), nargs="?", default="show")
    _add_game_arg(p)
    _add_dry_run(p)
    p.add_argument("--steam-user", help="Steam account id (folder name in userdata); default most recent user")
    p.add_argument("--all-users", action="store_true", help="apply to every Steam user on this computer")

    p = sub.add_parser("launch", help="start the game in VR")
    _add_game_arg(p)
    _add_dry_run(p)
    p.add_argument("--route", choices=("steamvr", "monado"), help="VR route (default from settings: steamvr)")
    p.add_argument("--connect", metavar="HOST:PORT", help="join this server after start")
    p.add_argument("--password", help="server password (or set L4D2VR_SERVER_PASSWORD)")
    p.add_argument("--xrizer-dir", help="Monado route: XRizer OpenVR runtime folder")
    p.add_argument("--openxr-manifest", help="Monado route: OpenXR runtime manifest JSON")
    p.add_argument("--proton-dir", help="Monado route: Proton folder (default: Proton Hotfix)")
    p.add_argument("--runtime-entrypoint", help="Monado route: Steam Runtime _v2-entry-point")
    p.add_argument("--check", action="store_true", help="Monado route: only run the preflight checks")
    p.add_argument("--save", action="store_true", help="remember the Monado paths for next time")
    p.add_argument("--no-wait", action="store_true", help="return right after starting the game")
    p.add_argument("extra", nargs=argparse.REMAINDER, help="extra game arguments after --")

    p = sub.add_parser("shortcut", help="create a desktop / application-menu shortcut")
    _add_game_arg(p)
    _add_dry_run(p)
    p.add_argument("--route", choices=("steamvr", "monado"))
    p.add_argument("--no-desktop", action="store_true", help="Linux: application menu only")

    p = sub.add_parser("install-server", help="install the dedicated-server plugin (server admins)")
    p.add_argument("--server-dir", required=True, help="<srcds>/left4dead2")
    p.add_argument("--platform", choices=("linux", "windows"), help="server OS (default: this computer's)")
    p.add_argument("--zip", help="install this local server zip instead of downloading")
    _add_source_args(p)
    _add_dry_run(p)

    p = sub.add_parser("xrizer", help="Monado route: install the XRizer runtime from a release asset or zip")
    p.add_argument("--zip", help="local xrizer-l4d2-runtime.zip")
    p.add_argument("--dest", help=f"target folder (default {vrlaunch.default_xrizer_dir()})")
    _add_source_args(p)
    _add_dry_run(p)

    p = sub.add_parser("self-update-check", help="check whether a newer installer is available")
    _add_source_args(p)

    p = sub.add_parser("make-manifest", help="release helper: write a manifest entry and l4d2vr.sha256 for a zip")
    p.add_argument("--zip", required=True)
    p.add_argument("--version", dest="mod_version", required=True)
    p.add_argument("--url", required=True, help="public download URL of the zip")
    p.add_argument("--tag")
    p.add_argument("--channel", default="stable")
    p.add_argument("--output", help="manifest file to create or update (default: print)")
    p.add_argument("--sha-file", help="also write a sha256sum-style file (e.g. l4d2vr.sha256)")
    return parser


# ---------------------------------------------------------------------------

def _source_options(args, saved: dict) -> actions.SourceOptions:
    return actions.SourceOptions(
        kind=getattr(args, "source", None) or saved.get("source") or "github",
        repo=getattr(args, "repo", None) or saved.get("repo") or DEFAULT_REPO,
        channel=getattr(args, "channel", None) or saved.get("channel") or "stable",
        manifest_url=getattr(args, "manifest_url", None) or saved.get("manifest_url") or None,
        api_base=getattr(args, "api_base", sources.GITHUB_API),
    )


def _monado_config(args, saved: dict, game_dir: Path) -> vrlaunch.MonadoConfig:
    config = vrlaunch.MonadoConfig.from_dict(saved.get("monado"))
    config.game_dir = str(game_dir)
    for attr, field in (("xrizer_dir", "xrizer_dir"), ("openxr_manifest", "openxr_manifest"),
                        ("proton_dir", "proton_dir"), ("runtime_entrypoint", "steam_runtime_entrypoint")):
        value = getattr(args, attr, None)
        if value:
            setattr(config, field, str(Path(value).expanduser()))
    roots = steam.find_steam_roots()
    root = steam.steam_root_for_game(game_dir, roots)
    libraries = [lib for r in roots for lib in steam.library_folders(r)]
    return vrlaunch.autodetect(config, libraries, root)


def cmd_detect(args, saved) -> int:
    roots = steam.find_steam_roots()
    installs = steam.find_game_installs(roots)
    data = {
        "steam_roots": [str(root) for root in roots],
        "steam_running": steam.is_steam_running(),
        "libraries": {str(root): [str(lib) for lib in steam.library_folders(root)] for root in roots},
        "games": [{"path": str(item.path), "depot": item.depot} for item in installs],
        "users": {str(root): [{"id": u.account_id, "name": u.name, "most_recent": u.most_recent}
                              for u in steam.steam_users(root)] for root in roots},
    }
    if not IS_WINDOWS:
        libraries = [lib for root in roots for lib in steam.library_folders(root)]
        data["proton"] = [str(p) for p in vrlaunch.detect_proton(libraries, roots[0] if roots else None)]
        data["steam_runtime_entrypoints"] = [str(p) for p in vrlaunch.detect_runtime_entrypoints(libraries)]
        data["openxr_manifests"] = [str(p) for p in vrlaunch.detect_openxr_manifests()]
    if args.json:
        print(json.dumps(data, indent=2))
        return 0
    print("Steam:", ", ".join(data["steam_roots"]) or "not found", "(running)" if data["steam_running"] else "")
    for game in installs:
        note = {"windows": "Windows build - OK", "linux": "native Linux build - needs Proton", "missing": "no game files here"}[game.depot]
        print(f"Left 4 Dead 2: {game.path} [{note}]")
    if not installs:
        print("Left 4 Dead 2: not found")
    for root, users in data["users"].items():
        for user in users:
            print(f"Steam user {user['id']} {user['name']}" + (" (most recent)" if user["most_recent"] else ""))
    for key in ("proton", "steam_runtime_entrypoints", "openxr_manifests"):
        if key in data:
            print(f"{key.replace('_', ' ')}: " + (", ".join(data[key]) or "none"))
    return 0


def cmd_status(args, saved) -> int:
    game = actions.detect_game_dir(args.game_dir, saved.get("game_dir"))
    server = actions.normalize_server_dir(Path(args.server_dir), log=lambda _m: None) if args.server_dir else None
    options = _source_options(args, saved) if args.online else None
    for line in actions.status_lines(game, server, options):
        print(line)
    return 0


def cmd_check(args, saved) -> int:
    game = actions.require_game_dir(args.game_dir, saved.get("game_dir"))
    options = _source_options(args, saved)
    result = actions.check_for_update(game, options)
    print(result.message())
    if result.release.page_url:
        print(f"Release page: {result.release.page_url}")
    print(sources.check_installer_update(result.release).message)
    return 0


def _progress_printer():
    state = {"last": -1}

    def progress(done: int, total: Optional[int]) -> None:
        if not total:
            return
        percent = int(done * 100 / total)
        if percent // 10 != state["last"] // 10 or done == total:
            state["last"] = percent
            print(f"  {percent:3d}% ({done // 1048576} / {total // 1048576} MiB)", flush=True)
    return progress


def cmd_install(args, saved) -> int:
    game = actions.require_game_dir(args.game_dir, saved.get("game_dir"))
    options = _source_options(args, saved)
    extra = list(saved.get("extra_preserve") or []) + list(args.preserve)
    zip_path = Path(args.zip).expanduser() if args.zip else None
    actions.install_client(game, zip_path=zip_path, options=options,
                           pin_workshop=not args.allow_workshop_updates and saved.get("pin_workshop", True),
                           extra_preserve=extra, force=args.force, dry_run=args.dry_run,
                           progress=_progress_printer())
    if not args.dry_run:
        saved["game_dir"] = str(game)
        if args.preserve:
            saved["extra_preserve"] = sorted(set(extra))
        settings.save(saved)
        if not IS_WINDOWS:
            print()
            print("Next (Linux): Steam needs these launch options for Left 4 Dead 2:")
            print("  " + launchopts.RECOMMENDED)
            print("Run 'launch-options apply' with Steam closed, or paste them into "
                  "Properties > General > Launch options.")
    return 0


def _target(args, saved) -> install.Target:
    if getattr(args, "server_dir", None):
        return install.server_target(actions.normalize_server_dir(Path(args.server_dir)))
    game = actions.require_game_dir(args.game_dir, saved.get("game_dir"))
    if not args.dry_run and steam.is_game_running():
        raise InstallerError("Left 4 Dead 2 is running. Close the game first.")
    return install.client_target(game)


def cmd_restore(args, saved) -> int:
    target = _target(args, saved)
    backups = install.list_backups(target)
    if args.list:
        for backup in backups:
            print(backup.describe())
        if not backups:
            print("No backups.")
        return 0
    chosen = None
    if args.backup:
        chosen = next((b for b in backups if b.name == args.backup), None)
        if chosen is None:
            raise InstallerError(f"No backup named {args.backup}. Use 'restore --list'.")
    install.restore_backup(target, chosen, force=args.force, dry_run=args.dry_run, log=print)
    return 0


def cmd_uninstall(args, saved) -> int:
    target = _target(args, saved)
    install.uninstall(target, force=args.force, dry_run=args.dry_run, log=print)
    if not args.dry_run and target.kind == "client" and not IS_WINDOWS:
        print("If you added VR launch options in Steam, run 'launch-options remove' (with Steam closed).")
    return 0


def cmd_launch_options(args, saved) -> int:
    print("Recommended launch options (Linux / Proton):")
    print("  " + launchopts.RECOMMENDED)
    if IS_WINDOWS:
        print("On Windows no special launch options are required.")
        if args.action != "show":
            return 0
    game = actions.detect_game_dir(args.game_dir, saved.get("game_dir"))
    users = actions.launch_option_users(game)
    if args.steam_user:
        users = [u for u in users if u.account_id == args.steam_user]
        if not users:
            raise InstallerError(f"No Steam user {args.steam_user} with a localconfig.vdf")
    elif not args.all_users and args.action != "show":
        users = users[:1]
    running = steam.is_steam_running()
    for user in users:
        label = f"{user.name or user.account_id} ({user.localconfig})"
        if args.action == "show":
            current = launchopts.read_launch_options(user.localconfig)
            state = "OK" if launchopts.has_required_options(current) else "missing VR settings"
            print(f"{label}: {state}\n  current: {current or '(empty)'}")
            continue
        func = launchopts.apply_vr_launch_options if args.action == "apply" else launchopts.remove_vr_launch_options
        change = func(user.localconfig, steam_running=running, dry_run=args.dry_run, state_dir=user_config_dir())
        for note in change.notes:
            print("Note: " + note)
        if not change.changed:
            print(f"{label}: already set, nothing to change.")
        else:
            print(f"{label}:\n  before: {change.before or '(empty)'}\n  after:  {change.after or '(empty)'}")
            print("  Dry run: not written." if args.dry_run else f"  Saved (backup: {change.backup}).")
    if running and args.action == "show":
        print("Steam is running: close it completely before 'apply', or paste the options in Steam yourself.")
    return 0


def cmd_launch(args, saved) -> int:
    game = actions.require_game_dir(args.game_dir, saved.get("game_dir"))
    route = args.route or saved.get("route") or "steamvr"
    password = args.password or os.environ.get("L4D2VR_SERVER_PASSWORD")
    extra = args.extra[1:] if args.extra[:1] == ["--"] else args.extra
    if route == "steamvr" or IS_WINDOWS:
        if route == "monado":
            print("The Monado route is Linux-only; using SteamVR.")
        url = vrlaunch.steam_url(vrlaunch.connect_args(args.connect, password) + extra)
        print("Opening " + (url.split("//+password")[0] + "..." if password else url))
        vrlaunch.open_steam_url(url, dry_run=args.dry_run)
        if not IS_WINDOWS:
            users = []
            try:
                users = actions.launch_option_users(game)
            except InstallerError:
                pass
            if users and not launchopts.has_required_options(launchopts.read_launch_options(users[0].localconfig)):
                print("Warning: the Steam launch options for VR are not set; run 'launch-options apply'.")
        return 0
    config = _monado_config(args, saved, game)
    if args.save:
        stored = config.to_dict()
        stored.pop("game_dir", None)
        saved["monado"] = stored
        saved["route"] = "monado"
        settings.save(saved)
    if args.check:
        problems, warnings = vrlaunch.validate(config)
        for warning in warnings:
            print("Warning: " + warning)
        for problem in problems:
            print("ERROR: " + problem)
        print("Preflight passed." if not problems else "Preflight failed.")
        return 0 if not problems else 1
    return vrlaunch.launch_monado(config, args.connect, password, extra, dry_run=args.dry_run,
                                  wait=not args.no_wait, game_running=steam.is_game_running)


def cmd_shortcut(args, saved) -> int:
    game = actions.require_game_dir(args.game_dir, saved.get("game_dir"))
    route = args.route or saved.get("route") or "steamvr"
    if IS_WINDOWS:
        paths = shortcut.install_windows_shortcut(vrlaunch.steam_url(), game, dry_run=args.dry_run)
    else:
        if route == "monado":
            argv = shortcut.installer_command() + ["launch", "--route", "monado", "--game-dir", str(game)]
        else:
            argv = ["xdg-open", vrlaunch.steam_url()]
        paths = shortcut.install_linux_shortcut(argv, desktop=not args.no_desktop, dry_run=args.dry_run)
    for path in paths:
        print(("Would write " if args.dry_run else "Created ") + str(path))
    return 0


def cmd_install_server(args, saved) -> int:
    options = _source_options(args, saved)
    actions.install_server(Path(args.server_dir), platform=args.platform,
                           zip_path=Path(args.zip).expanduser() if args.zip else None,
                           options=options, dry_run=args.dry_run)
    return 0


def cmd_xrizer(args, saved) -> int:
    dest = Path(args.dest).expanduser() if args.dest else vrlaunch.default_xrizer_dir()
    if args.zip:
        zip_path = Path(args.zip).expanduser()
    else:
        release = actions.fetch_release(_source_options(args, saved))
        if release.xrizer is None:
            raise InstallerError(
                "The latest release has no xrizer-l4d2-runtime.zip. Point the installer at an XRizer "
                "runtime folder you built or received (launch --xrizer-dir ... --save).")
        zip_path = actions.download_asset(release.xrizer, print)
    if dest.exists() and any(dest.iterdir()) and not args.dry_run:
        raise InstallerError(f"{dest} is not empty; remove it first or choose --dest.")
    vrlaunch.install_xrizer_zip(zip_path, dest, dry_run=args.dry_run)
    if not args.dry_run:
        monado = dict(saved.get("monado") or {})
        monado["xrizer_dir"] = str(dest)
        saved["monado"] = monado
        settings.save(saved)
        print(f"XRizer runtime installed in {dest}")
    return 0


def cmd_self_update(args, saved) -> int:
    info = actions.check_installer_update(_source_options(args, saved))
    print(info.message)
    return 0


def cmd_make_manifest(args, saved) -> int:
    existing = read_json(Path(args.output)) if args.output else None
    manifest = actions.make_manifest(Path(args.zip), args.mod_version, args.url, args.tag, args.channel, existing)
    if args.output:
        atomic_write_json(Path(args.output), manifest)
        print(f"Wrote {args.output}")
    else:
        print(json.dumps(manifest, indent=2))
    if args.sha_file:
        entry = manifest if args.channel == "stable" else manifest["channels"][args.channel]
        Path(args.sha_file).write_text(f"{entry['sha256']}  {Path(args.zip).name}\n")
        print(f"Wrote {args.sha_file}")
    return 0


COMMANDS = {
    "detect": cmd_detect, "status": cmd_status, "check": cmd_check, "install": cmd_install,
    "update": cmd_install, "restore": cmd_restore, "uninstall": cmd_uninstall,
    "launch-options": cmd_launch_options, "launch": cmd_launch, "shortcut": cmd_shortcut,
    "install-server": cmd_install_server, "xrizer": cmd_xrizer, "self-update-check": cmd_self_update,
    "make-manifest": cmd_make_manifest,
}


def _run_gui() -> int:
    try:
        from . import gui
    except ImportError as error:  # tkinter missing (e.g. minimal Linux Python)
        print(f"The GUI is unavailable ({error}). Install python3-tk or use the commands below.\n", file=sys.stderr)
        build_parser().print_help()
        return 2
    return gui.main()


def main(argv: Optional[List[str]] = None) -> int:
    argv = sys.argv[1:] if argv is None else argv
    parser = build_parser()
    args = parser.parse_args(argv)
    if args.command in (None, "gui"):
        if args.command is None and not IS_WINDOWS and not (os.environ.get("DISPLAY") or os.environ.get("WAYLAND_DISPLAY")):
            parser.print_help()
            return 0
        return _run_gui()
    saved = settings.load()
    try:
        return COMMANDS[args.command](args, saved) or 0
    except InstallerError as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("Cancelled.", file=sys.stderr)
        return 130


if __name__ == "__main__":
    sys.exit(main())
