#!/usr/bin/env python3
"""Select the test suites related to the changes since the previous release.

The full QEMU suite takes tens of minutes, and much of it exercises code that
rarely changes. ``plan`` maps the paths changed since the last release tag to
the test modules that exercise them, so a release re-runs only the related
suites. When a change is broad or cannot be mapped — the core interpreter, a
build file, Circle, an unknown source file — it falls back to the full suite,
so a release can never ship untested.

Subcommands
-----------
  plan     print the selected suites and why they were selected (no run)
  run      build and run the selected suites through scripts/test-watch.py
  notes    print the release-notes "Tests" section for the selection
  changed  print the paths changed since the previous release

Environment:
  PY       python interpreter used for the pytest run (default: this python)
"""
from __future__ import annotations

import argparse
import fnmatch
import glob
import os
import subprocess
import sys
from dataclasses import dataclass

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


@dataclass(frozen=True)
class Rule:
    """A changed-path glob mapped to the test suites that exercise it."""

    name: str
    paths: tuple[str, ...]
    suites: tuple[str, ...]
    why: str


@dataclass
class Selection:
    suites: list[str]
    reasons: list[str]
    full: bool = False
    fallback: str = ""


# A changed path matching any of these is too broad to scope: it can affect
# almost every suite, so the full suite runs. Keep this list conservative.
FULL_SUITE_TRIGGERS: tuple[str, ...] = (
    # Core interpreter: expression/parse/token/variable/util layers.
    "mmbasic/src/core.c",
    "mmbasic/src/expr.c",
    "mmbasic/src/tok.c",
    "mmbasic/src/vars.c",
    "mmbasic/src/util.c",
    "mmbasic/src/frontend.c",
    "mmbasic/src/session.c",
    "mmbasic/src/ramdisk.c",
    "mmbasic/src/libc_shims.c",
    "mmbasic/src/gfx.c",
    "mmbasic/src/blit*",
    # Headers wrap everything, regardless of the source that includes them.
    "mmbasic/src/*.h",
    "mmbasic/include/*",
    # Console/kernel bring-up and platform glue.
    "console/Makefile",
    "console/kernel.cpp",
    "console/kernel.h",
    "console/main.cpp",
    "console/platform.cpp",
    "console/storage.cpp",
    "console/storage.h",
    "native/Makefile",
    "native/main.c",
    "native/main_sdl.c",
    "native/cli.c",
    "native/platform_sdl.c",
    "native/platform_stdio.c",
    # Build system, generated build inputs, and the vendored runtime.
    "Makefile",
    "*/Makefile",
    "*/*/Makefile",
    "scripts/build.sh",
    "circle/**",
    "patches/**",
)

# Changed-path glob -> test suites. Everything not listed here that is not a
# broad trigger above falls back to the full suite.
RULES: tuple[Rule, ...] = (
    Rule(
        "wordpad",
        ("mmbasic/src/cmd_wordpad.c",),
        ("tests/test_wordpad*.py",),
        "WORD PAD sources changed",
    ),
    Rule(
        "paint",
        ("mmbasic/src/cmd_paint.c", "mmbasic/src/paint_*.c"),
        ("tests/test_paint*.py",),
        "PAINT editor sources changed",
    ),
    Rule(
        "term",
        ("mmbasic/src/cmd_term.c", "harness/term*.py"),
        ("tests/test_term*.py",),
        "TERM terminal sources changed",
    ),
    Rule(
        "editor",
        ("mmbasic/src/editor.c", "mmbasic/src/tui.c"),
        ("tests/test_editor*.py", "tests/test_tui_prompt_cmm2.py"),
        "editor / TUI sources changed",
    ),
    Rule(
        "settings",
        ("mmbasic/src/settings.c", "mmbasic/src/cmd_settings.c"),
        ("tests/test_settings.py", "tests/test_settings_theme_contrast.py"),
        "settings sources changed",
    ),
    Rule(
        "package",
        ("mmbasic/src/cmd_package.c", "mmbasic/src/zip.c", "mmbasic/src/inflate.c"),
        ("tests/test_package.py", "tests/test_unpack.py"),
        "PACKAGE / archive sources changed",
    ),
    Rule(
        "tdf",
        ("mmbasic/src/cmd_tdf.c", "mmbasic/src/mmb_tdf.c"),
        ("tests/test_tdf_bas.py", "tests/test_tdf_ramdisk.py"),
        "TDF sources changed",
    ),
    Rule(
        "files",
        ("mmbasic/src/cmd_files.c", "mmbasic/src/cmd_files_ui.c"),
        ("tests/test_files_ansi.py", "tests/test_files_ui.py"),
        "FILES sources changed",
    ),
    Rule(
        "ftp",
        ("mmbasic/src/cmd_ftp.c",),
        ("tests/test_files_ftp.py", "tests/test_ftp_list.py", "tests/test_ftp_stor.py"),
        "FTP sources changed",
    ),
    Rule(
        "graphics",
        ("mmbasic/src/cmd_graphics.c",),
        ("tests/test_graphics.py",),
        "graphics command sources changed",
    ),
    Rule(
        "cmm2_gfx",
        ("mmbasic/src/gfx_cmm2.c",),
        ("tests/test_cmm2_gfx.py",),
        "CMM2 graphics sources changed",
    ),
    Rule(
        "sprite",
        ("mmbasic/src/cmd_sprite.c",),
        ("tests/test_sprite.py",),
        "SPRITE sources changed",
    ),
    Rule(
        "json",
        ("mmbasic/src/cmd_json.c",),
        ("tests/test_json.py",),
        "JSON sources changed",
    ),
    Rule(
        "struct",
        ("mmbasic/src/cmd_struct.c",),
        ("tests/test_struct.py",),
        "STRUCT sources changed",
    ),
    Rule(
        "math",
        ("mmbasic/src/cmd_math.c",),
        ("tests/test_math.py",),
        "math sources changed",
    ),
    Rule(
        "juke",
        ("mmbasic/src/cmd_juke.c",),
        ("tests/test_juke.py",),
        "JUKE sound sources changed",
    ),
    Rule(
        "afk",
        ("mmbasic/src/cmd_afk.c",),
        ("tests/test_afk.py",),
        "AFK sources changed",
    ),
    Rule(
        "apppath",
        ("mmbasic/src/cmd_apppath.c",),
        ("tests/test_apppath.py",),
        "APPPATH sources changed",
    ),
    Rule(
        "array",
        ("mmbasic/src/cmd_array.c",),
        ("tests/test_array.py",),
        "array command sources changed",
    ),
    Rule(
        "connect",
        ("mmbasic/src/cmd_connect.c",),
        ("tests/test_connect.py",),
        "CONNECT sources changed",
    ),
    Rule(
        "credits",
        ("mmbasic/src/cmd_credits.c",),
        ("tests/test_credits.py",),
        "CREDITS sources changed",
    ),
    Rule(
        "help",
        ("mmbasic/src/cmd_help.c", "mmbasic/src/cmd_ihelp.c"),
        ("tests/test_help.py", "tests/test_help_docs.py"),
        "HELP sources changed",
    ),
    Rule(
        "pcx",
        ("mmbasic/src/pcx.c",),
        ("tests/test_pcx.py",),
        "PCX codec sources changed",
    ),
    Rule(
        "zmodem",
        ("mmbasic/src/zmodem.c",),
        ("tests/test_zmodem.py",),
        "ZMODEM sources changed",
    ),
    Rule(
        "yield",
        ("mmbasic/src/yield.c",),
        ("tests/test_yield.py",),
        "YIELD sources changed",
    ),
    Rule(
        "ntp",
        ("mmbasic/src/ntp.c",),
        ("tests/test_ntp.py",),
        "NTP sources changed",
    ),
    Rule(
        "battery_brightness",
        ("mmbasic/src/battery.c", "mmbasic/src/brightness.c"),
        ("tests/test_battery.py", "tests/test_brightness.py"),
        "battery / brightness sources changed",
    ),
    Rule(
        "mouse",
        ("mmbasic/src/cmd_mouse.c", "mmbasic/src/mouse_cursor.c"),
        ("tests/test_mouse.py",),
        "mouse sources changed",
    ),
    Rule(
        "wlan",
        ("console/wlan.cpp", "console/wlan_escan.c", "console/wlan_escan.h"),
        ("tests/test_wlan_build.py", "tests/test_wlan_escan.py"),
        "WLAN sources changed",
    ),
    Rule(
        "ethernet",
        ("console/eth.cpp", "console/net.cpp"),
        (
            "tests/test_ethernet.py",
            "tests/test_qemu_ethernet.py",
            "tests/test_tcp.py",
            "tests/test_tcp_inuse.py",
        ),
        "Ethernet / TCP sources changed",
    ),
    Rule(
        "net_rxbuf",
        ("console/net_rxbuf.c", "console/net_rxbuf.h"),
        ("tests/test_net_rxbuf.py", "tests/test_pcap_send_holes.py"),
        "net receive-buffer sources changed",
    ),
    Rule(
        "netdrive",
        (
            "mmbasic/src/netdrive_unc.c",
            "mmbasic/src/netfs_smb2.c",
            "console/smb2_sock.cpp",
        ),
        ("tests/test_netdrive.py",),
        "network drive sources changed",
    ),
    Rule(
        "audio",
        ("console/audio.cpp", "native/sdl_audio.c"),
        ("tests/test_audio.py",),
        "audio sources changed",
    ),
    Rule(
        "vfs",
        ("mmbasic/src/vfs.c",),
        (
            "tests/test_vfs_stream.py",
            "tests/test_drives.py",
            "tests/test_storage_posix.py",
        ),
        "VFS sources changed",
    ),
    Rule(
        "native_sdl",
        ("native/sdl_*.c", "native/sdl_*.h"),
        (
            "tests/test_linux_native.py",
            "tests/test_sdl_vt.py",
            "tests/test_native_touchpad.py",
        ),
        "native SDL frontend sources changed",
    ),
    Rule(
        "gen_ramdisk",
        ("scripts/gen_ramdisk.py", "scripts/gen-mmb-version.sh"),
        ("tests/test_build_gen.py", "tests/test_gen_ramdisk.py"),
        "ramdisk / version generator changed",
    ),
    Rule(
        "gen_golden",
        ("scripts/gen_golden.py", "tests/golden/**"),
        ("tests/test_gen_golden.py",),
        "golden-fixture generator changed",
    ),
    Rule(
        "gen_help",
        ("scripts/gen_help.py",),
        ("tests/test_help.py", "tests/test_help_docs.py"),
        "HELP generator changed",
    ),
    Rule(
        "docs",
        ("docs/**", "README.md", "INSTALL.md"),
        ("tests/test_docs_framebuffer_iso.py", "tests/test_help_docs.py"),
        "documentation changed",
    ),
    Rule(
        "release_scripts",
        (
            "scripts/github-release.sh",
            "scripts/package-release.sh",
            "scripts/package-macos-app.sh",
            "scripts/package-windows.sh",
            "scripts/sign-windows-exe.sh",
            "scripts/mmcore-notary-env.sh",
            "scripts/install-sdcard.sh",
            "scripts/sdcard/**",
        ),
        (
            "tests/test_macos_release.py",
            "tests/test_pizero2_release.py",
            "tests/test_windows_release.py",
            "tests/test_framebuffer_release.py",
        ),
        "release / installer scripts changed",
    ),
    Rule(
        "framebuffer_iso",
        (
            "scripts/package-framebuffer.sh",
            "scripts/package-linux-appimage.sh",
            "scripts/build-iso.sh",
            "scripts/install-usb.sh",
            "scripts/iso/**",
        ),
        (
            "tests/test_framebuffer_release.py",
            "tests/test_iso_build.py",
            "tests/test_docs_framebuffer_iso.py",
        ),
        "framebuffer / ISO build scripts changed",
    ),
    Rule(
        "release_tooling",
        ("scripts/release_tests.py", "scripts/release-tests.sh"),
        ("tests/test_release_tests.py",),
        "release test-selection tooling changed",
    ),
    Rule(
        "process_docs",
        (
            ".cursor/skills/**",
            ".opencode/skill/**",
            "AGENTS.md",
        ),
        (
            "tests/test_loop_docs.py",
            "tests/test_release_tests.py",
            "tests/test_docs_framebuffer_iso.py",
        ),
        "process / skill documentation changed",
    ),
)


def _matches(path: str, patterns: tuple[str, ...]) -> bool:
    return any(fnmatch.fnmatch(path, pat) for pat in patterns)


def _expand_suites(patterns: tuple[str, ...], root: str = ROOT) -> list[str]:
    """Resolve suite globs to existing repo-relative test files."""
    out: list[str] = []
    for pat in patterns:
        for match in sorted(glob.glob(os.path.join(root, pat))):
            rel = os.path.relpath(match, root).replace(os.sep, "/")
            if rel not in out:
                out.append(rel)
    return out


def select(changed: list[str], root: str = ROOT) -> Selection:
    """Map changed paths to a scoped selection, or the full suite."""
    changed = [p.strip() for p in changed if p.strip()]
    if not changed:
        return Selection(
            [],
            [],
            full=True,
            fallback="no changed paths since the previous release",
        )
    suites: list[str] = []
    reasons: list[str] = []
    for path in changed:
        trigger = next(
            (pat for pat in FULL_SUITE_TRIGGERS if fnmatch.fnmatch(path, pat)), None
        )
        if trigger is not None:
            return Selection(
                [],
                [],
                full=True,
                fallback=f"broad/core change: {path} matches {trigger}",
            )
        # A changed test module is exercised by itself. Test *helpers*
        # (conftest, harness shims) affect many suites, so they stay unmapped
        # and fall back to the full suite below.
        if fnmatch.fnmatch(path, "tests/test_*.py"):
            if path not in suites:
                suites.append(path)
            reason = f"test: {path} changed"
            if reason not in reasons:
                reasons.append(reason)
            continue
        matched = [rule for rule in RULES if _matches(path, rule.paths)]
        if not matched:
            return Selection(
                [],
                [],
                full=True,
                fallback=f"unmapped path: {path}",
            )
        for rule in matched:
            for suite in _expand_suites(rule.suites, root):
                if suite not in suites:
                    suites.append(suite)
            reason = f"{rule.name}: {rule.why} [{path}]"
            if reason not in reasons:
                reasons.append(reason)
    if not suites:
        return Selection(
            [],
            [],
            full=True,
            fallback="no suites resolved for the changed paths",
        )
    return Selection(suites, reasons, full=False)


def _since_ref(since: str | None) -> str | None:
    """Normalise a release version to a git ref, or ``None`` for no baseline."""
    if not since:
        return None
    since = since.strip()
    if since in ("", "0.0.0"):
        return None
    if since.isdigit() or _is_semver(since.lstrip("v")):
        return "v" + since.lstrip("v")
    return since


def _is_semver(text: str) -> bool:
    parts = text.split(".")
    return len(parts) == 3 and all(part.isdigit() for part in parts)


def last_release_tag(root: str = ROOT) -> str | None:
    """The ref of the latest published release, or ``None`` when there is none."""
    try:
        out = subprocess.run(
            ["bash", os.path.join(root, "scripts", "github-release.sh"), "last-version"],
            cwd=root,
            capture_output=True,
            text=True,
            check=True,
        ).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return None
    return _since_ref(out)


def changed_paths(since: str | None, root: str = ROOT) -> list[str]:
    ref = _since_ref(since)
    if ref is None:
        return []
    try:
        out = subprocess.run(
            ["git", "diff", "--name-only", f"{ref}..HEAD"],
            cwd=root,
            capture_output=True,
            text=True,
            check=True,
        ).stdout
    except (OSError, subprocess.CalledProcessError) as exc:
        raise SystemExit(f"release_tests: cannot diff {ref}..HEAD: {exc}")
    return [line for line in out.splitlines() if line.strip()]


def format_plan(selection: Selection, changed: list[str], since: str | None) -> str:
    baseline = since or "the previous release"
    lines: list[str] = []
    if selection.full:
        lines.append(f"release-tests: FULL SUITE (fallback: {selection.fallback})")
    else:
        lines.append(f"release-tests: {len(selection.suites)} suite file(s) (scoped)")
    lines.append(f"changed: {len(changed)} path(s) since {baseline}")
    if not selection.full:
        for suite in selection.suites:
            lines.append(f"  - {suite}")
    lines.append("reasons:")
    for reason in selection.reasons or [selection.fallback]:
        lines.append(f"  - {reason}")
    return "\n".join(lines)


def format_notes(selection: Selection, since: str | None) -> str:
    baseline = since or "the previous release"
    if selection.full:
        return (
            "## Tests\n\n"
            f"Full test suite. {selection.fallback}, so the release could not be "
            "scoped to related suites."
        )
    lines = [f"Test suites related to the changes since `{baseline}`:"]
    for suite in selection.suites:
        lines.append(f"- `{suite}`")
    lines.append("")
    lines.append("Selected because:")
    for reason in selection.reasons:
        lines.append(f"- {reason}")
    return "## Tests\n\n" + "\n".join(lines)


def _resolve(args: argparse.Namespace) -> tuple[Selection, list[str], str | None]:
    if args.files:
        changed = list(args.files)
        since = args.since
    else:
        since = args.since or last_release_tag()
        changed = changed_paths(since)
    return select(changed), changed, since


def cmd_plan(args: argparse.Namespace) -> int:
    selection, changed, since = _resolve(args)
    print(format_plan(selection, changed, since))
    return 0


def cmd_changed(args: argparse.Namespace) -> int:
    since = args.since or last_release_tag()
    for path in changed_paths(since):
        print(path)
    return 0


def cmd_notes(args: argparse.Namespace) -> int:
    selection, _changed, since = _resolve(args)
    print(format_notes(selection, since))
    return 0


def cmd_run(args: argparse.Namespace) -> int:
    selection, changed, since = _resolve(args)
    print(format_plan(selection, changed, since), flush=True)
    if not args.no_build:
        subprocess.run(
            ["bash", os.path.join(ROOT, "scripts", "build.sh")],
            cwd=ROOT,
            check=True,
        )
    py = os.environ.get("PY", sys.executable)
    targets = [] if selection.full else selection.suites
    subprocess.run(
        [py, os.path.join(ROOT, "scripts", "test-watch.py"), "start", "--", *targets],
        cwd=ROOT,
        check=True,
    )
    print(
        "release-tests: started via test-watch.py; poll with "
        "`scripts/test-watch.py status --sleep 60`",
        flush=True,
    )
    return 0


def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)

    def add_common(p: argparse.ArgumentParser) -> None:
        p.add_argument(
            "--since",
            help="baseline release version/ref (default: last published release)",
        )
        p.add_argument(
            "--files",
            nargs="+",
            default=None,
            help="changed paths, bypassing git (for tests/planning)",
        )

    p = sub.add_parser("plan", help="print the selected suites and why")
    add_common(p)
    p.set_defaults(func=cmd_plan)

    p = sub.add_parser("run", help="build and run the selected suites")
    add_common(p)
    p.add_argument(
        "--no-build",
        action="store_true",
        help="skip scripts/build.sh (the image must already exist)",
    )
    p.set_defaults(func=cmd_run)

    p = sub.add_parser("notes", help="print the release-notes Tests section")
    add_common(p)
    p.set_defaults(func=cmd_notes)

    p = sub.add_parser("changed", help="print paths changed since the release")
    p.add_argument("--since", help="baseline release version/ref")
    p.set_defaults(func=cmd_changed)

    return ap


def main(argv: list[str] | None = None) -> int:
    ns = build_parser().parse_args(argv)
    return ns.func(ns)


if __name__ == "__main__":
    sys.exit(main())
