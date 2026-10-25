"""App PATH (#520) and boot destination (#515).

Covers bare-token ``.APP`` commands on the PATH, the Ctrl+Space picker, the
APPS home launcher, OPTION PATH/BOOT persistence, and boot-to-app/launcher
from a FAT SD image.
"""

import os
import shutil
import subprocess
import tempfile

import pytest

from harness import MMBasicConsole
from ihelp_util import dump_topic


def _write_lines(con, path, lines):
    assert con.send_line(f'OPEN "{path}" FOR OUTPUT AS #1') == ""
    for line in lines:
        escaped = line.replace('"', '""')
        assert con.send_line(f'PRINT #1, "{escaped}"') == ""
    assert con.send_line("CLOSE #1") == ""


def _make_app(con, root, appname, text="APPOK"):
    """Create ``A:/<root>/<appname>.APP`` and return the PATH directory."""
    assert con.send_line('CHDIR "A:/"') == ""
    con.send_line(f'MKDIR "{root}"')
    con.send_line(f'MKDIR "{root}/SRC"')
    _write_lines(con, f"{root}/SRC/MAIN.BAS", [f'PRINT "{text}"'])
    assert con.send_line(f'CHDIR "A:/{root}"') == ""
    assert con.send_line(f'PACKAGE "{appname}.APP", "A:/{root}/SRC/"') == ""
    return f"A:/{root}/"


def _build_app(con, root, srcdir, appname, text, title=None):
    """Package ``A:/<root>/<appname>.APP`` from ``<root>/<srcdir>``.

    ``title`` is written into PACKAGE.INF before packaging, so the archive
    carries the same metadata the PACKAGE wizard writes (#1305).
    """
    assert con.send_line('CHDIR "A:/"') == ""
    con.send_line(f'MKDIR "{root}"')
    con.send_line(f'MKDIR "{root}/{srcdir}"')
    _write_lines(con, f"{root}/{srcdir}/MAIN.BAS", [f'PRINT "{text}"'])
    if title is not None:
        _write_lines(
            con,
            f"{root}/{srcdir}/PACKAGE.INF",
            [f"name={appname}.APP", "main=MAIN.BAS", f"title={title}"],
        )
    assert con.send_line(f'CHDIR "A:/{root}"') == ""
    assert con.send_line(f'PACKAGE "{appname}.APP", "A:/{root}/{srcdir}/"') == ""
    return f"A:/{root}/"


def test_default_path_is_apps_ramdisk(console):
    """#520: the default A:/APPS/ works with a bundled-style package."""
    con = console
    assert con.send_line("FACTORY_RESET") == "Factory defaults restored"
    assert con.send_line('CHDIR "A:/"') == ""
    con.send_line('MKDIR "APPS"')
    con.send_line('MKDIR "APPS/SRC"')
    _write_lines(con, "APPS/SRC/MAIN.BAS", ['PRINT "DEFAULTAPP"'])
    assert con.send_line('CHDIR "A:/APPS"') == ""
    assert con.send_line('PACKAGE "DFLT.APP", "A:/APPS/SRC/"') == ""
    assert con.send_line('CHDIR "A:/"') == ""
    out = con.send_line("DFLT")
    assert "DEFAULTAPP" in out


def test_bare_token_runs_app_on_path(console):
    root = _make_app(console, "PTHRUN", "MYGAME", "BAREAPP")
    assert console.send_line(f'OPTION PATH "{root}"') == ""
    assert "BAREAPP" in console.send_line("MYGAME")
    # A token that already carries the extension resolves too.
    assert "BAREAPP" in console.send_line("MYGAME.APP")


def test_builtin_wins_over_path_app(console):
    """#520: a PATH app must not shadow a real builtin."""
    root = _make_app(console, "PTHSHADOW", "PRINT", "SHADOW")
    assert console.send_line(f'OPTION PATH "{root}"') == ""
    out = console.send_line("PRINT")
    assert "SHADOW" not in out
    assert console.send_line("PRINT 6*7") == "42"


def test_unknown_bare_token_is_syntax_error(console):
    assert console.send_line("NO_SUCH_APP_ZZZ").upper().startswith("?")


def test_ctrl_space_picker_runs_and_cancels(console):
    root = _make_app(console, "PTHPICK", "PICKME", "PICKED")
    assert console.send_line(f'OPTION PATH "{root}"') == ""
    con = console
    con.drain(quiet=0.2, timeout=2.0)
    con._ser.sendall(b"\x00")  # Ctrl+Space
    seen = con.drain(quiet=0.9, timeout=3.0).decode(errors="replace")
    assert "PICKME" in seen.upper()
    assert "APPS" in seen.upper()
    assert "Move" in seen  # #569: hotkey tags render once, not "<Move>>"
    assert ">>" not in seen
    con._ser.sendall(b"\r")
    out = con.drain(quiet=1.2, timeout=8.0).decode(errors="replace")
    assert "PICKED" in out
    assert con.send_line("PRINT 3") == "3"

    # Esc cancels back to the prompt unchanged.
    con.drain(quiet=0.2, timeout=1.0)
    con._ser.sendall(b"\x00")
    con.drain(quiet=0.9, timeout=3.0)
    con._ser.sendall(b"\x1b")
    con.drain(quiet=0.9, timeout=3.0)
    assert con.send_line("PRINT 4") == "4"


def test_apps_command_launcher(console):
    root = _make_app(console, "PTHHOME", "HOMEGAME", "HOMEOK")
    assert console.send_line(f'OPTION PATH "{root}"') == ""
    con = console
    con.drain(quiet=0.2, timeout=2.0)
    con._ser.sendall(b"APPS\r")
    seen = con.drain(quiet=0.9, timeout=3.0).decode(errors="replace")
    assert "HOMEGAME" in seen.upper()
    assert "HOME" in seen.upper()
    con._ser.sendall(b"\x1b")
    con.drain(quiet=0.9, timeout=3.0)
    assert con.send_line("PRINT 5") == "5"


def test_option_path_and_boot_listing(console):
    assert console.send_line("FACTORY_RESET") == "Factory defaults restored"
    all_listed = console.send_line("OPTION LIST ALL")
    assert 'OPTION PATH "A:/APPS/"' in all_listed
    assert "OPTION BOOT REPL" in all_listed
    listed = console.send_line("OPTION LIST")
    assert "OPTION PATH" not in listed
    assert "OPTION BOOT" not in listed

    assert console.send_line('OPTION PATH "A:/CUSTOM/"') == ""
    assert console.send_line("OPTION BOOT LAUNCHER") == ""
    listed = console.send_line("OPTION LIST")
    assert 'OPTION PATH "A:/CUSTOM/"' in listed
    assert "OPTION BOOT LAUNCHER" in listed

    assert console.send_line('OPTION BOOT "MYAPP"') == ""
    listed = console.send_line("OPTION LIST")
    assert 'OPTION BOOT "MYAPP"' in listed

    assert console.send_line("OPTION BOOT REPL") == ""
    assert "OPTION BOOT" not in console.send_line("OPTION LIST")
    assert console.send_line('OPTION PATH "A:/APPS/"') == ""


def test_help_apps_path_boot(console):
    apps = dump_topic(console, "APPS")
    assert "CTRL+SPACE" in apps.upper()
    assert "MAIN.BAS" in apps.upper()
    path = dump_topic(console, "PATH")
    assert "A:/APPS/" in path
    boot = dump_topic(console, "BOOT")
    assert "LAUNCHER" in boot.upper()
    opt = dump_topic(console, "OPTION")
    assert "PATH" in opt.upper()
    assert "BOOT" in opt.upper()


# --- #1305: launcher labels use the package's stored Title -----------------


def test_launcher_shows_package_title(console):
    con = console
    root = _build_app(con, "TITLEPK", "SRC", "TITLED", "TITLEOK", "My Great Game")
    assert con.send_line(f'OPTION PATH "{root}"') == ""
    con.drain(quiet=0.2, timeout=2.0)
    con._ser.sendall(b"APPS\r")
    seen = con.drain(quiet=0.9, timeout=3.0).decode(errors="replace").upper()
    assert "MY GREAT GAME" in seen, seen
    assert "TITLED" not in seen, seen  # the Title replaces the stem
    # Enter still runs the same archive; only the label changed.
    con._ser.sendall(b"\r")
    out = con.drain(quiet=1.2, timeout=8.0).decode(errors="replace")
    assert "TITLEOK" in out, out
    assert con.send_line("PRINT 21") == "21"


def test_launcher_untitled_package_shows_stem(console):
    con = console
    root = _build_app(con, "NOTITLE", "SRC", "PLAINAPP", "PLAINOK")
    assert con.send_line(f'OPTION PATH "{root}"') == ""
    con.drain(quiet=0.2, timeout=2.0)
    con._ser.sendall(b"APPS\r")
    seen = con.drain(quiet=0.9, timeout=3.0).decode(errors="replace").upper()
    assert "PLAINAPP" in seen, seen
    con._ser.sendall(b"\x1b")
    con.drain(quiet=0.9, timeout=3.0)
    assert con.send_line("PRINT 22") == "22"


def test_launcher_corrupt_package_falls_back_to_stem(console):
    con = console
    assert con.send_line('CHDIR "A:/"') == ""
    con.send_line('MKDIR "BADPK"')
    _write_lines(con, "BADPK/BROKEN.APP", ["this is not a zip archive"])
    assert con.send_line('OPEN "BADPK/EMPTY.APP" FOR OUTPUT AS #1') == ""
    assert con.send_line("CLOSE #1") == ""
    assert con.send_line('OPTION PATH "A:/BADPK/"') == ""
    con.drain(quiet=0.2, timeout=2.0)
    con._ser.sendall(b"APPS\r")
    seen = con.drain(quiet=0.9, timeout=3.0).decode(errors="replace").upper()
    assert "BROKEN" in seen, seen
    assert "EMPTY" in seen, seen
    assert "?PACKAGE" not in seen and "?UNPACK" not in seen, seen
    con._ser.sendall(b"\x1b")
    con.drain(quiet=0.9, timeout=3.0)
    assert con.send_line("PRINT 23") == "23"


def test_launcher_duplicate_titles_keep_distinct_files(console):
    """Two packages sharing a Title must not collapse into one entry."""
    con = console
    _build_app(con, "DUPT", "ONE", "DUPONE", "ONE", "Shared Title")
    root = _build_app(con, "DUPT", "TWO", "DUPTWO", "TWO", "Shared Title")
    assert con.send_line(f'OPTION PATH "{root}"') == ""
    con.drain(quiet=0.2, timeout=2.0)
    con._ser.sendall(b"APPS\r")
    seen = con.drain(quiet=0.9, timeout=3.0).decode(errors="replace").upper()
    assert "SHARED TITLE" in seen, seen
    visible_stems = [s for s in ("DUPONE", "DUPTWO") if s in seen]
    assert len(visible_stems) == 1, (visible_stems, seen)
    con._ser.sendall(b"\x1b")
    con.drain(quiet=0.9, timeout=3.0)
    assert con.send_line("PRINT 24") == "24"


# --- #1320: type-to-filter in the launcher --------------------------------


def _launcher_type(con, keys):
    con._ser.sendall(keys)
    return con.drain(quiet=0.9, timeout=3.0).decode(errors="replace").upper()


def test_launcher_filters_as_you_type(console):
    """Typing narrows the list, Backspace widens it, Esc clears the filter."""
    con = console
    root = _build_app(con, "FILTER", "ONE", "ALPHA", "ALPHAOK", "Alpha App")
    root = _build_app(con, "FILTER", "TWO", "STORY", "FILTEROK", "Story Grove")
    assert con.send_line(f'OPTION PATH "{root}"') == ""
    con.drain(quiet=0.2, timeout=2.0)
    con._ser.sendall(b"APPS\r")
    seen = con.drain(quiet=0.9, timeout=3.0).decode(errors="replace").upper()
    assert "STORY GROVE" in seen, seen
    assert "ALPHA APP" in seen, seen

    # Case-insensitive substring: "gro" keeps Story Grove, drops Alpha/the
    # builtins that do not match.
    seen = _launcher_type(con, b"gro")
    assert "STORY GROVE" in seen, seen
    assert "ALPHA APP" not in seen, seen
    assert "EDITOR" not in seen, seen
    assert "FILTER: GRO" in seen, seen

    # Enter runs the filtered selection.
    con._ser.sendall(b"\r")
    out = con.drain(quiet=1.2, timeout=8.0).decode(errors="replace")
    assert "FILTEROK" in out, out
    assert con.send_line("PRINT 31") == "31"

    # Backspace removes characters and widens the list again. Three deletes
    # take the filter back to empty, restoring the whole list.
    con.drain(quiet=0.2, timeout=1.0)
    con._ser.sendall(b"APPS\r")
    con.drain(quiet=0.9, timeout=3.0)
    _launcher_type(con, b"gro")
    seen = _launcher_type(con, b"\x7f\x7f\x7f")
    assert "ALPHA APP" in seen, seen
    assert "EDITOR" in seen, seen
    assert "CHOOSE AN APP" in seen, seen

    # Esc clears a non-empty filter but leaves the launcher open; a second
    # Esc closes it.
    seen = _launcher_type(con, b"GRO")
    assert "STORY GROVE" in seen, seen
    assert "ALPHA APP" not in seen, seen
    seen = _launcher_type(con, b"\x1b")
    assert "ALPHA APP" in seen, seen
    assert "EDITOR" in seen, seen
    con._ser.sendall(b"\x1b")
    con.drain(quiet=0.9, timeout=3.0)
    assert con.send_line("PRINT 32") == "32"


# --- #624: launcher overlay dialog with built-in app labels ---------------


def test_apps_launcher_lists_builtins_with_labels(console):
    """The launcher is a small dialog listing built-in apps by label."""
    con = console
    con.drain(quiet=0.2, timeout=2.0)
    con._ser.sendall(b"APPS\r")
    seen = con.drain(quiet=0.9, timeout=3.0).decode(errors="replace").upper()
    labels = (
        "EDITOR",
        "FILE MANAGER",
        "WORD PROCESSOR",
        "PAINT",
        "JUKEBOX",
        "TERMINAL",
        "HELP",
        "SETTINGS",
    )
    for label in labels:
        assert label in seen, (label, seen)
    # #793: exactly this order, Terminal immediately after Jukebox.
    positions = [seen.index(label) for label in labels]
    assert positions == sorted(positions), (labels, positions)
    # Package and Connect are no longer offered, but their commands remain.
    assert "PACKAGE" not in seen
    assert "CONNECT" not in seen
    # The overlay frame, not a full-width HOME/APPS title bar. ``APPS``
    # opens the boot-style launcher (title HOME); Ctrl+Space uses APPS.
    assert "HOME" in seen
    con._ser.sendall(b"\x1b")
    con.drain(quiet=0.9, timeout=3.0)
    assert con.send_line("PRINT 11") == "11"


def _panel_rect(con, want_w, want_h):
    """Mirror tui_dialog_geom for a dialog of want_w x want_h text cells."""
    w, h = con.screen_size()
    cols, rows = w // 8, h // 16
    pw = min(want_w, cols - 2)
    ph = min(want_h, rows - 2)
    px = max(1, (cols - pw) // 2)
    py = max(1, (rows - ph) // 2)
    return px, py, pw, ph


def _cell(col, row):
    """A pixel at the vertical middle of a text cell (8x16 font)."""
    return (col * 8 + 2, row * 16 + 8)


def test_apps_launcher_chrome_matches_settings(console):
    """#793: the APPS dialog backdrop, title bar and frame match SETTINGS.

    The picker previously drew its body on the editor surface (edit_bg) and
    its frame in the title colour, so it did not match the SETTINGS hub's
    dialog surfaces. Sample the same roles in both dialogs.
    """
    con = console
    ax, ay, aw, _ = _panel_rect(con, 52, 16)
    sx, sy, sw, _ = _panel_rect(con, 68, 13)

    con.drain(quiet=0.2, timeout=2.0)
    con._ser.sendall(b"APPS\r")
    con.drain(quiet=0.9, timeout=3.0)
    apps = con.screen_pixels([
        _cell(2, 2),               # backdrop margin, left of the panel
        _cell(ax + 2, ay + 1),     # title bar interior
        _cell(ax + aw // 2, ay),   # top frame
        _cell(ax + aw - 3, ay + 2),  # body row, right of the intro text
    ])
    con._ser.sendall(b"\x1b")
    con.drain(quiet=0.9, timeout=3.0)
    assert con.send_line("PRINT 12") == "12"

    con.drain(quiet=0.2, timeout=2.0)
    con._ser.sendall(b"SETTINGS\r")
    con.drain(quiet=0.9, timeout=3.0)
    settings = con.screen_pixels([
        _cell(2, 2),
        _cell(sx + 2, sy + 1),
        _cell(sx + sw // 2, sy),
        _cell(sx + sw - 3, sy + 2),
    ])
    con._ser.sendall(b"\x1b")
    con.drain(quiet=0.9, timeout=3.0)
    assert con.send_line("PRINT 13") == "13"

    for role, a, s in zip(
        ("backdrop", "title bar", "frame", "body"), apps, settings
    ):
        assert a == s, (role, apps, settings)


def _launcher_rows(con, opener):
    """Open the picker/launcher and return the raw serial frame as rows."""
    con.drain(quiet=0.2, timeout=2.0)
    con._ser.sendall(opener)
    text = con.drain(quiet=0.9, timeout=3.0).decode(errors="replace")
    return [ln.rstrip() for ln in text.replace("\r", "\n").split("\n")]


def test_apps_launcher_status_header(console):
    """#1217: the Ctrl+Space picker and the HOME launcher show the same row-0
    status line as SETTINGS -- local date/time top-left and the battery badge
    top-right, on the backdrop above the dialog chrome."""
    import re

    con = console
    for opener in (b"\x00", b"APPS\r"):  # Ctrl+Space picker, HOME launcher
        rows = _launcher_rows(con, opener)
        clock_row = next(
            i
            for i, ln in enumerate(rows)
            if re.search(r"\d{2}:\d{2} / \d{1,2} [A-Z][a-z]{2} \d{4}", ln)
        )
        assert "[100%]" in rows[clock_row], (opener, rows[clock_row])
        panel_top = next(
            i for i, ln in enumerate(rows) if ln.strip().startswith("+")
        )
        assert clock_row < panel_top, (opener, clock_row, panel_top, rows[:4])
        assert (
            "+" not in rows[clock_row] and "|" not in rows[clock_row]
        ), (opener, rows[clock_row])
        con._ser.sendall(b"\x1b")
        con.drain(quiet=0.9, timeout=3.0)
    assert con.send_line("PRINT 7") == "7"


def test_apps_launcher_restores_screen_on_close(fresh_console):
    """Esc from the launcher restores the REPL screen behind the overlay."""
    con = fresh_console
    con.send_line('PRINT "OVERLAYMARK"')
    con.drain(quiet=0.2, timeout=2.0)
    con._ser.sendall(b"APPS\r")
    con.drain(quiet=0.9, timeout=3.0)
    con._ser.sendall(b"\x1b")
    con.drain(quiet=0.9, timeout=3.0)
    text = "".join(con.ocr_screen(crop="640x400+0+0").split())
    assert "OVERLAYMARK" in text
    assert con.send_line("PRINT 3") == "3"



# ---- boot destination from a FAT SD image --------------------------------

SD_ARGS = None


def _fat_image():
    fd, img = tempfile.mkstemp(suffix=".img")
    os.close(fd)
    subprocess.run(
        ["dd", "if=/dev/zero", f"of={img}", "bs=1M", "count=64"],
        check=True,
        capture_output=True,
    )
    subprocess.run(
        ["mkfs.vfat", "-F", "32", "-n", "APPTEST", img],
        check=True,
        capture_output=True,
    )
    os.sync()
    return img


def _sd_console(kernel_image, img, **kwargs):
    return MMBasicConsole(
        kernel_image,
        extra_qemu=["-drive", f"file={img},if=sd,format=raw"],
        boot_timeout=40,
        **kwargs,
    )


def _seed_sd_app(kernel_image, img, boot):
    """First boot: create C:/APPS/GAME.APP and persist the boot setting."""
    con = _sd_console(kernel_image, img)
    con.start()
    try:
        assert con.send_line('CHDIR "C:"') == ""
        con.send_line('MKDIR "APPS"')
        con.send_line('MKDIR "APPS/SRC"')
        _write_lines(con, "C:/APPS/SRC/MAIN.BAS", ['PRINT "BOOTAPP"'])
        assert con.send_line('CHDIR "C:/APPS"') == ""
        assert con.send_line('PACKAGE "GAME.APP", "C:/APPS/SRC/"') == ""
        assert con.send_line('OPTION PATH "C:/APPS/"') == ""
        if boot == "LAUNCHER":
            assert con.send_line("OPTION BOOT LAUNCHER") == ""
        else:
            assert con.send_line('OPTION BOOT "GAME"') == ""
    finally:
        con.stop()


@pytest.mark.skipif(shutil.which("mkfs.vfat") is None, reason="mkfs.vfat not installed")
def test_boot_to_app_from_sd(kernel_image):
    img = _fat_image()
    try:
        _seed_sd_app(kernel_image, img, boot="APP")
        con = _sd_console(kernel_image, img)
        con.start()
        try:
            log = con.boot_log.decode(errors="replace")
            assert "BOOTAPP" in log, log
            assert con.send_line("PRINT 9") == "9"
        finally:
            con.stop()
    finally:
        os.unlink(img)


@pytest.mark.skipif(shutil.which("mkfs.vfat") is None, reason="mkfs.vfat not installed")
def test_boot_to_launcher_from_sd(kernel_image):
    img = _fat_image()
    try:
        _seed_sd_app(kernel_image, img, boot="LAUNCHER")
        # The launcher never prints the REPL prompt. Wait on the status bar
        # hint (not just "REPL", which the header text also contains) so the
        # app list has been flushed before start() returns.
        con = _sd_console(kernel_image, img, prompt=b"Move")
        con.start()
        try:
            log = con.boot_log.decode(errors="replace")
            assert "GAME" in log.upper(), log
            con.prompt = b"> "
            con._ser.sendall(b"\r")
            out = con.drain(quiet=1.2, timeout=8.0).decode(errors="replace")
            assert "BOOTAPP" in out, out
            assert con.send_line("PRINT 7") == "7"
        finally:
            con.stop()
    finally:
        os.unlink(img)
