"""Virtual consoles (#510): Ctrl+Alt+1..4 switch between independent
interpreter sessions with their own screen and state.

The chord is driven through the real USB keyboard (``-device usb-kbd``) so
modifiers can be held, matching how it is pressed on hardware."""

import os
import re
import shutil
import subprocess
import tempfile
import time

import pytest

from harness import MMBasicConsole
from ihelp_util import dump_topic


def _usb_console(kernel_image) -> MMBasicConsole:
    return MMBasicConsole(kernel_image, extra_qemu=["-device", "usb-kbd"])


def _plain(s: str) -> str:
    return re.sub(r"\x1b\[[0-9;?]*[A-Za-z]", "", s)


def test_help_documents_consoles(console):
    out = dump_topic(console, "CONSOLES")
    assert "Ctrl+Alt+1" in out
    assert "RUN" in out


def _switch(con, n: int) -> None:
    # Hold the digit long enough for the guest's USB poll to see it; a fast
    # tap can fall between polls and be missed.
    con.key_down("ctrl")
    con.key_down("alt")
    time.sleep(0.15)
    con.key_down(str(n))
    time.sleep(0.25)
    con.key_up(str(n))
    time.sleep(0.15)
    con.key_up("alt")
    con.key_up("ctrl")
    time.sleep(0.5)


def test_switch_second_console_is_independent(kernel_image):
    con = _usb_console(kernel_image)
    con.start()
    try:
        assert con.send_line("PRINT 1+1") == "2"
        assert con.send_line("A = 111") == ""

        _switch(con, 2)
        banner = con.drain(quiet=0.3, timeout=3.0).decode(errors="ignore")
        assert "MMBasic" in banner

        # Console 2 is a fresh interpreter: it answers and has no leftovers.
        assert con.send_line("PRINT 10*10") == "100"
        assert con.send_line("PRINT A") == "0"
        assert con.send_line("A = 222") == ""

        # Switch back: console 1 keeps its own state and answers.
        _switch(con, 1)
        con.drain(quiet=0.3, timeout=2.0)
        assert con.send_line("PRINT A") == "111"

        # And console 2 still holds the variable set there.
        _switch(con, 2)
        con.drain(quiet=0.3, timeout=2.0)
        assert con.send_line("PRINT A") == "222"
        _switch(con, 1)
        con.drain(quiet=0.3, timeout=2.0)
        assert con.send_line("PRINT A") == "111"
    finally:
        con.stop()


def test_switch_restores_first_console_screen(kernel_image):
    con = _usb_console(kernel_image)
    con.start()
    try:
        assert con.send_line('PRINT "TOPSECRET"') == "TOPSECRET"
        _switch(con, 3)
        con.drain(quiet=0.3, timeout=2.0)
        assert con.send_line("PRINT 123") == "123"
        _switch(con, 1)
        con.drain(quiet=0.3, timeout=2.0)
        screen = con.wait_ocr("TOPSECRET", timeout=10.0, crop="1280x400+0+0")
        assert "TOPSECRET" in screen
    finally:
        con.stop()


def test_editor_survives_switch(kernel_image):
    """A full-screen TUI on one console keeps its buffer and screen while
    another console runs a REPL."""
    con = _usb_console(kernel_image)
    con.start()
    try:
        con.drain(quiet=0.3, timeout=2.0)
        con._ser.sendall(b'EDIT "VCSW.BAS"\r')
        time.sleep(1.2)
        con.drain(quiet=0.4)
        con._ser.sendall(b'PRINT 42')
        time.sleep(0.5)
        con.drain(quiet=0.3)

        _switch(con, 2)
        banner = con.drain(quiet=0.3, timeout=2.0).decode(errors="ignore")
        assert "MMBasic" in banner
        assert con.send_line("PRINT 7*6") == "42"

        _switch(con, 1)
        time.sleep(0.4)
        con.drain(quiet=0.3, timeout=2.0)
        screen = con.wait_ocr("Help", timeout=10.0, crop="1280x400+0+0")
        assert "Help" in screen
        assert "PRINT" in screen.upper()

        # The editor still owns console 1's keyboard: save and quit.
        con._ser.sendall(bytes([19]))  # Ctrl+S
        time.sleep(0.5)
        con.drain(quiet=0.3)
        con._ser.sendall(bytes([1]) + b"x")  # Alt+X
        time.sleep(0.6)
        con.drain(quiet=0.3)
        assert con.send_line("PRINT 100+1") == "101"
    finally:
        con.stop()


def _count_colour(con: MMBasicConsole, name: str, step: int = 8) -> int:
    """Count framebuffer pixels that match a primary colour."""
    w, h = con.screen_size()
    coords = [(x, y) for y in range(0, h, step) for x in range(0, w, step)]
    preds = {
        "red": lambda p: p[0] > 130 and p[1] < 80 and p[2] < 80,
        "green": lambda p: p[1] > 130 and p[0] < 80 and p[2] < 80,
    }
    pred = preds[name]
    return sum(1 for p in con.screen_pixels(coords) if pred(p))


def test_switch_restores_console_mode(kernel_image):
    """#580: each console keeps its own MODE. Returning to a console must retune
    the HDMI framebuffer, not just its MM.INFO bookkeeping."""
    con = _usb_console(kernel_image)
    con.start()
    try:
        assert con.send_line("MODE 7,16") == ""
        assert con.screen_size() == (320, 240)

        _switch(con, 2)
        con.drain(quiet=0.3, timeout=2.0)
        assert con.send_line("MODE 8,16") == ""
        assert con.screen_size() == (640, 480)

        _switch(con, 1)
        con.drain(quiet=0.3, timeout=2.0)
        assert con.send_line("PRINT MM.INFO(MODE)") == "7.16"
        assert con.screen_size() == (320, 240)
    finally:
        con.stop()


def test_switch_restores_cursor_colour(kernel_image):
    """#580: the terminal pen travels with the console. After switching back the
    next characters must be drawn in that console's COLOUR."""
    con = _usb_console(kernel_image)
    con.start()
    try:
        assert con.send_line("MODE 8,16") == ""
        assert con.send_line("COLOUR RGB(255,0,0), RGB(0,0,0)") == ""
        con._ser.sendall(b'PRINT "HHHHHHHHHHHHHHHHHHHH"\r')
        time.sleep(0.6)
        con.drain(quiet=0.3)

        _switch(con, 2)
        con.drain(quiet=0.3, timeout=2.0)
        assert con.send_line("MODE 8,16") == ""
        assert con.send_line("COLOUR RGB(0,255,0), RGB(0,0,0)") == ""
        con._ser.sendall(b'PRINT "HHHHHHHHHHHHHHHHHHHH"\r')
        time.sleep(0.6)
        con.drain(quiet=0.3)

        _switch(con, 1)
        con.drain(quiet=0.3, timeout=2.0)
        # Re-draw enough text to dominate the screen: it must be red, not the
        # green pen the other console left behind.
        for _ in range(14):
            con._ser.sendall(b'PRINT "HHHHHHHHHHHHHHHHHHHHHHHHHHHHHHHHHHHHHHHH"\r')
        time.sleep(0.8)
        con.drain(quiet=0.4)
        assert _count_colour(con, "red") > _count_colour(con, "green")
    finally:
        con.stop()


def test_running_program_suspends_and_resumes(kernel_image):
    """Switching away from RUN stops it at a line boundary; switching back
    continues where it left off (the INKEY$ loop only exits after input on
    the original console)."""
    con = _usb_console(kernel_image)
    con.start()
    try:
        con.drain(quiet=0.3, timeout=2.0)
        con._ser.sendall(
            b'10 A$=""\r'
            b"20 DO\r"
            b"30 A$=INKEY$\r"
            b"40 LOOP UNTIL A$<>\"\"\r"
            b'50 PRINT "GOT:"; A$\r'
            b"RUN\r"
        )
        time.sleep(0.8)
        con.drain(quiet=0.3)

        _switch(con, 2)
        away = con.drain(quiet=0.3, timeout=2.0).decode(errors="ignore")
        assert "MMBasic" in away
        assert con.send_line("PRINT 7*6") == "42"
        # Suspended: no result line while the program is parked.
        assert "GOT" not in con.drain(quiet=0.4).decode(errors="ignore")

        _switch(con, 1)
        time.sleep(0.5)
        con._ser.sendall(b"x")
        deadline = time.time() + 10.0
        seen = ""
        while time.time() < deadline:
            seen += con.drain(quiet=0.3).decode(errors="ignore")
            if "GOT" in seen:
                break
        assert "GOT" in seen
    finally:
        con.stop()


def test_switch_keeps_audio_playing(kernel_image):
    """#620: the audio engine is one machine resource, so switching consoles
    must not silence it. PLAYING() is global, and PLAY STOP from any console
    stops the single engine for every console."""
    con = _usb_console(kernel_image)
    con.start()
    try:
        con.drain(quiet=0.3, timeout=2.0)
        assert con.send_line("PLAY TONE 440, 440") == ""
        assert con.send_line("PRINT PLAYING()") == "1"

        _switch(con, 2)
        con.drain(quiet=0.3, timeout=2.0)
        # A fresh console still sees the engine running.
        assert con.send_line("PRINT PLAYING()") == "1"

        # Stopping from the second console silences the one engine.
        assert con.send_line("PLAY STOP") == ""
        assert con.send_line("PRINT PLAYING()") == "0"

        _switch(con, 1)
        con.drain(quiet=0.3, timeout=2.0)
        assert con.send_line("PRINT PLAYING()") == "0"
    finally:
        con.stop()


def test_switch_keeps_per_console_ramdisk_cwd(kernel_image):
    """#618: the working directory is session state. CHDIR on one console must
    not move a fresh console, and switching back restores each console's own
    directory."""
    con = _usb_console(kernel_image)
    con.start()
    try:
        con.drain(quiet=0.3, timeout=2.0)
        assert con.send_line('CHDIR "A:/tests"') == ""
        assert con.send_line("PRINT CWD$") == "A:/tests"

        _switch(con, 2)
        con.drain(quiet=0.3, timeout=2.0)
        assert con.send_line("PRINT CWD$") == "A:/"
        assert con.send_line('CHDIR "A:/lib"') == ""
        assert con.send_line("PRINT CWD$") == "A:/lib"

        _switch(con, 1)
        con.drain(quiet=0.3, timeout=2.0)
        assert con.send_line("PRINT CWD$") == "A:/tests"

        # And the second console kept its own directory too.
        _switch(con, 2)
        con.drain(quiet=0.3, timeout=2.0)
        assert con.send_line("PRINT CWD$") == "A:/lib"
    finally:
        con.stop()


@pytest.mark.skipif(shutil.which("mkfs.vfat") is None, reason="mkfs.vfat not installed")
def test_switch_keeps_per_console_fat_cwd(kernel_image):
    """#618: a physical drive's current directory is per console; the mount
    itself stays visible to every console."""
    fd, img = tempfile.mkstemp(suffix=".img")
    os.close(fd)
    try:
        subprocess.run(
            ["dd", "if=/dev/zero", f"of={img}", "bs=1M", "count=64"],
            check=True, capture_output=True,
        )
        subprocess.run(
            ["mkfs.vfat", "-F", "32", "-n", "MMBCWD", img],
            check=True, capture_output=True,
        )
        os.sync()
        con = MMBasicConsole(
            kernel_image,
            extra_qemu=[
                "-device", "usb-kbd",
                "-drive", f"file={img},if=sd,format=raw",
            ],
            boot_timeout=30,
        )
        con.start()
        try:
            con.drain(quiet=0.3, timeout=2.0)
            assert con.send_line('CHDIR "C:"') == ""
            assert con.send_line('MKDIR "SUB"') == ""
            assert con.send_line('CHDIR "SUB"') == ""
            assert con.send_line("PRINT CWD$") == "C:/SUB"

            _switch(con, 2)
            con.drain(quiet=0.3, timeout=2.0)
            # The mount is machine-global, but the cwd is not.
            assert con.send_line("PRINT CWD$") == "A:/"
            assert con.send_line('CHDIR "C:"') == ""
            assert con.send_line("PRINT CWD$") == "C:/"

            _switch(con, 1)
            con.drain(quiet=0.3, timeout=2.0)
            assert con.send_line("PRINT CWD$") == "C:/SUB"
        finally:
            con.stop()
    finally:
        os.unlink(img)


def _edit(con, path: str) -> str:
    con.drain(quiet=0.1)
    con._ser.sendall(f'EDIT "{path}"\r'.encode())
    return _plain(con.drain(quiet=0.8).decode(errors="replace"))


def _term_download_dialog(con) -> str:
    """Open the TERM download-folder browser and return its serial dump."""
    con._ser.sendall(b'TERM "demo", 23\r')
    con.drain(quiet=0.6, timeout=12.0)
    con._ser.sendall(bytes([1]) + b"t")
    con.drain(quiet=0.4, timeout=8.0)
    con._ser.sendall(b"\x1b[B\x1b[B\x1b[B\r")
    return _plain(con.drain(quiet=0.6, timeout=8.0).decode(errors="replace"))


def test_switch_keeps_per_console_term_download_dir(kernel_image):
    """#670: the ZMODEM download directory and folder-browser cursor are
    per-console. Browsing on one console must not move another console."""
    con = _usb_console(kernel_image)
    con.start()
    try:
        con.drain(quiet=0.3, timeout=2.0)
        assert con.send_line('CHDIR "A:/tests"') == ""
        seen = _term_download_dialog(con)
        assert "Download folder" in seen, seen
        assert "A:/TESTS" in seen.upper(), seen

        con._ser.sendall(b"\x1b")
        con.drain(quiet=0.4, timeout=5.0)
        con._ser.sendall(bytes([1]) + b"x")
        con.drain(quiet=0.8, timeout=15.0)

        _switch(con, 2)
        con.drain(quiet=0.3, timeout=2.0)
        assert con.send_line('CHDIR "A:/lib"') == ""
        seen2 = _term_download_dialog(con)
        assert "Download folder" in seen2, seen2
        # Starts in this console's own cwd, not the folder console 1 browsed.
        assert "A:/LIB" in seen2.upper(), seen2
        assert "A:/TESTS" not in seen2.upper(), seen2

        con._ser.sendall(b"\x1b")
        con.drain(quiet=0.4, timeout=5.0)
        con._ser.sendall(bytes([1]) + b"x")
        con.drain(quiet=0.8, timeout=15.0)
    finally:
        con.stop()


def test_switch_keeps_per_console_edit_pick_root(kernel_image):
    """#670: the EDIT file-picker root is per-console. Ctrl+P on a console
    whose editor is already open must use its own root, not the one another
    console's editor last set."""
    con = _usb_console(kernel_image)
    con.start()
    try:
        con.drain(quiet=0.3, timeout=2.0)
        assert con.send_line('CHDIR "A:/tests"') == ""
        _edit(con, "X.BAS")

        _switch(con, 2)
        con.drain(quiet=0.3, timeout=2.0)
        assert con.send_line('CHDIR "A:/lib"') == ""
        _edit(con, "Y.BAS")

        _switch(con, 1)
        con.drain(quiet=0.3, timeout=2.0)
        con._ser.sendall(bytes([16]))  # Ctrl+P quick open
        seen = _plain(con.drain(quiet=0.6, timeout=8.0).decode(errors="replace"))
        assert "Quick open" in seen, seen
        assert "A:/TESTS" in seen.upper(), seen
        assert "A:/LIB" not in seen.upper(), seen

        con._ser.sendall(b"\x1b")
        con.drain(quiet=0.4, timeout=4.0)
        con._ser.sendall(bytes([1]) + b"x")
        con.drain(quiet=0.6, timeout=8.0)
        _switch(con, 2)
        con.drain(quiet=0.3, timeout=2.0)
        con._ser.sendall(bytes([1]) + b"x")
        con.drain(quiet=0.6, timeout=8.0)
    finally:
        con.stop()


def _qemu_has_usb_mouse() -> bool:
    try:
        out = subprocess.run(
            ["qemu-system-aarch64", "-device", "help"],
            capture_output=True,
            text=True,
            timeout=20,
        )
    except (OSError, subprocess.SubprocessError):
        return False
    return "usb-mouse" in out.stdout


@pytest.mark.skipif(
    not _qemu_has_usb_mouse(), reason="qemu-system-aarch64 lacks usb-mouse"
)
def test_paint_switch_leaves_other_console_usable(kernel_image):
    """#754: a full-screen PAINT session belongs to the console it started on.
    Switching away must release the keyboard and screen so the destination
    console runs its own prompt, and switching back must restore PAINT."""
    con = MMBasicConsole(
        kernel_image,
        extra_qemu=["-device", "usb-kbd", "-device", "usb-mouse"],
        boot_timeout=40.0,
    )
    con.start()
    try:
        time.sleep(2.0)  # let the USB mouse enumerate and attach
        con.drain(quiet=0.3, timeout=2.0)
        con._ser.sendall(b"PAINT\r")
        con.drain(quiet=0.8, timeout=8.0)
        assert con.screen_size() == (640, 360)

        _switch(con, 2)
        banner = con.drain(quiet=0.3, timeout=2.0).decode(errors="ignore")
        assert "MMBasic" in banner
        # The destination console accepts input; PAINT is not stealing keys.
        assert con.send_line("PRINT 3+4") == "7"

        _switch(con, 1)
        con.drain(quiet=0.3, timeout=2.0)
        # PAINT still owns console 1: its 640x360 mode and Alt+X handler.
        assert con.screen_size() == (640, 360)
        con._ser.sendall(bytes([1]) + b"x")  # Alt+X leaves PAINT
        con.drain(quiet=0.8, timeout=8.0)
        assert con.send_line("PRINT 2+3") == "5"
    finally:
        con.stop()


def test_switch_restores_term_screen(kernel_image):
    """#758: leaving a full-screen terminal for another console and returning
    restores its screen buffer, not just the destination prompt."""
    con = _usb_console(kernel_image)
    con.start()
    try:
        con.drain(quiet=0.3, timeout=2.0)
        con._ser.sendall(b'TERM "demo", 23\r')
        con.drain(quiet=0.8, timeout=12.0)
        before = con.wait_ocr("line 30", timeout=12.0, crop="1280x400+0+0")
        assert "line 30" in before, before

        _switch(con, 2)
        con.drain(quiet=0.3, timeout=2.0)
        assert con.send_line("PRINT 7*6") == "42"

        _switch(con, 1)
        con.drain(quiet=0.3, timeout=2.0)
        after = con.wait_ocr("line 30", timeout=8.0, crop="1280x400+0+0")
        assert "line 30" in after, after
    finally:
        con.stop()


def test_switch_restores_term_after_juke(kernel_image):
    """#758's repro: TERM on screen 1, JUKE on screen 2, then back to screen 1
    restores the terminal buffer (a graphics app on the other screen must not
    cost the first screen its pixels or text)."""
    con = _usb_console(kernel_image)
    con.start()
    try:
        con.drain(quiet=0.3, timeout=2.0)
        con._ser.sendall(b'TERM "demo", 23\r')
        con.drain(quiet=0.8, timeout=12.0)
        before = con.wait_ocr("line 30", timeout=12.0, crop="1280x400+0+0")
        assert "line 30" in before, before

        _switch(con, 2)
        con.drain(quiet=0.3, timeout=2.0)
        con._ser.sendall(b'JUKE "tests/TEST.MOD"\r')
        con.drain(quiet=0.9, timeout=10.0)

        _switch(con, 1)
        con.drain(quiet=0.3, timeout=2.0)
        after = con.wait_ocr("line 30", timeout=8.0, crop="1280x400+0+0")
        assert "line 30" in after, after
    finally:
        con.stop()


def test_switch_keeps_per_console_wordpad_pick_root(kernel_image):
    """#670: the WORDPAD file-picker root is per-console. Ctrl+P on a console
    whose WORDPAD is already open must list its own directory, not another
    console's.

    The picker's root line is screen-only, so observe the root through the
    files it walks: each console's cwd holds a distinct .MD marker."""
    con = _usb_console(kernel_image)
    con.start()
    try:
        con.drain(quiet=0.3, timeout=2.0)
        assert con.send_line('OPEN "A:/tests/ALPHA.MD" FOR OUTPUT AS #1') == ""
        assert con.send_line("CLOSE #1") == ""
        assert con.send_line('CHDIR "A:/tests"') == ""
        con._ser.sendall(b"WORDPAD\r")
        con.drain(quiet=0.8, timeout=10.0)

        _switch(con, 2)
        con.drain(quiet=0.3, timeout=2.0)
        assert con.send_line('OPEN "A:/lib/BETA.MD" FOR OUTPUT AS #1') == ""
        assert con.send_line("CLOSE #1") == ""
        assert con.send_line('CHDIR "A:/lib"') == ""
        con._ser.sendall(b"WORDPAD\r")
        con.drain(quiet=0.8, timeout=10.0)
        con._ser.sendall(bytes([16]))  # sets the shared root to A:/lib
        setroot = _plain(con.drain(quiet=0.7, timeout=8.0).decode(errors="replace"))
        assert "BETA.MD" in setroot.upper(), setroot
        con._ser.sendall(b"\x1b")
        con.drain(quiet=0.4, timeout=4.0)

        _switch(con, 1)
        con.drain(quiet=0.3, timeout=2.0)
        con._ser.sendall(bytes([16]))
        seen = _plain(con.drain(quiet=0.7, timeout=8.0).decode(errors="replace"))
        up = seen.upper()
        assert "QUICK OPEN" in up, seen
        assert "ALPHA.MD" in up, seen
        assert "BETA.MD" not in up, seen

        con._ser.sendall(b"\x1b")
        con.drain(quiet=0.4, timeout=4.0)
        con._ser.sendall(bytes([24]))  # Ctrl+X quits WORDPAD
        con.drain(quiet=0.6, timeout=8.0)
        _switch(con, 2)
        con.drain(quiet=0.3, timeout=2.0)
        con._ser.sendall(bytes([24]))
        con.drain(quiet=0.6, timeout=8.0)
    finally:
        con.stop()


def _open_term_download(con) -> str:
    """Open the TERM download-folder browser and leave it open."""
    con._ser.sendall(b'TERM "demo", 23\r')
    con.drain(quiet=0.6, timeout=12.0)
    con._ser.sendall(bytes([1]) + b"t")
    con.drain(quiet=0.4, timeout=8.0)
    con._ser.sendall(b"\x1b[B\x1b[B\x1b[B\r")
    return _plain(con.drain(quiet=0.6, timeout=8.0).decode(errors="replace"))


def _quit_term(con) -> None:
    con._ser.sendall(b"\x1b")
    con.drain(quiet=0.4, timeout=5.0)
    con._ser.sendall(bytes([1]) + b"x")
    con.drain(quiet=0.8, timeout=15.0)


def test_switch_term_download_default_is_per_console(kernel_image):
    """#679: Use on one console persists only that console's download default,
    so another console is not seeded with it and the shared default is not
    overwritten for every future boot."""
    con = _usb_console(kernel_image)
    con.start()
    try:
        con.drain(quiet=0.3, timeout=2.0)
        assert con.send_line('CHDIR "A:/tests"') == ""
        seen = _open_term_download(con)
        assert "A:/TESTS" in seen.upper(), seen
        con._ser.sendall(b"\t\r")  # focus Use, persist A:/tests
        con.drain(quiet=0.6, timeout=8.0)
        _quit_term(con)

        _switch(con, 2)
        con.drain(quiet=0.3, timeout=2.0)
        assert con.send_line('CHDIR "A:/lib"') == ""
        seen2 = _open_term_download(con)
        # Not seeded by console 1's Use; starts at this console's own cwd.
        assert "A:/LIB" in seen2.upper(), seen2
        assert "A:/TESTS" not in seen2.upper(), seen2
        con._ser.sendall(b"\t\r")  # persist A:/lib for this console
        con.drain(quiet=0.6, timeout=8.0)
        _quit_term(con)

        _switch(con, 1)
        con.drain(quiet=0.3, timeout=2.0)
        seen3 = _open_term_download(con)
        # Console 2's later Use must not clobber console 1's default.
        assert "A:/TESTS" in seen3.upper(), seen3
        assert "A:/LIB" not in seen3.upper(), seen3
        _quit_term(con)
        assert con.send_line("PRINT 3+4") == "7"
    finally:
        con.stop()


def test_switch_keeps_per_console_term_download_list(kernel_image):
    """#680: the download browser's list/selection state is per-console.
    Rescanning on a second console must not replace the entries the first is
    showing when it redraws."""
    con = _usb_console(kernel_image)
    con.start()
    try:
        con.drain(quiet=0.3, timeout=2.0)
        assert con.send_line('CHDIR "A:/tests"') == ""
        assert con.send_line('MKDIR "CONONE"') == ""
        seen = _open_term_download(con)
        assert "CONONE" in seen.upper(), seen

        _switch(con, 2)
        con.drain(quiet=0.3, timeout=2.0)
        assert con.send_line('CHDIR "A:/lib"') == ""
        assert con.send_line('MKDIR "CONTWO"') == ""
        seen2 = _open_term_download(con)
        assert "CONTWO" in seen2.upper(), seen2

        _switch(con, 1)
        con.drain(quiet=0.3, timeout=2.0)
        con._ser.sendall(b"\x1b[B")  # redraw this console's browser
        redraw = _plain(con.drain(quiet=0.6, timeout=8.0).decode(errors="replace"))
        assert "CONONE" in redraw.upper(), redraw
        assert "CONTWO" not in redraw.upper(), redraw

        _quit_term(con)
        _switch(con, 2)
        con.drain(quiet=0.3, timeout=2.0)
        _quit_term(con)
    finally:
        con.stop()


def test_switch_keeps_per_console_edit_pick_list(kernel_image):
    """#680: EDIT's quick-open list/selection state is per-console. After a
    rescan on another console, Enter here must still open this console's own
    selected entry, not one from the other console's list."""
    con = _usb_console(kernel_image)
    con.start()
    try:
        con.drain(quiet=0.3, timeout=2.0)
        assert con.send_line('OPEN "A:/tests/ZLONE.BAS" FOR OUTPUT AS #1') == ""
        assert con.send_line("CLOSE #1") == ""
        assert con.send_line('CHDIR "A:/tests"') == ""
        _edit(con, "X.BAS")
        con._ser.sendall(bytes([16]))  # Ctrl+P quick open
        seen = _plain(con.drain(quiet=0.6, timeout=8.0).decode(errors="replace"))
        assert "ZLONE.BAS" in seen.upper(), seen

        _switch(con, 2)
        con.drain(quiet=0.3, timeout=2.0)
        assert con.send_line('OPEN "A:/lib/ZLTWO.BAS" FOR OUTPUT AS #1') == ""
        assert con.send_line("CLOSE #1") == ""
        assert con.send_line('CHDIR "A:/lib"') == ""
        _edit(con, "Y.BAS")
        con._ser.sendall(bytes([16]))
        seen2 = _plain(con.drain(quiet=0.6, timeout=8.0).decode(errors="replace"))
        assert "ZLTWO.BAS" in seen2.upper(), seen2

        _switch(con, 1)
        con.drain(quiet=0.3, timeout=2.0)
        con._ser.sendall(b"\r")  # open the still-selected entry
        opened = _plain(con.drain(quiet=0.8, timeout=8.0).decode(errors="replace"))
        # The status bar names the file this console actually opened.
        assert "ZLONE.BAS" in opened.upper(), opened
        assert "EXAMPLE.INC" not in opened.upper(), opened
        assert "TDF.BAS" not in opened.upper(), opened

        con._ser.sendall(bytes([1]) + b"x")
        con.drain(quiet=0.6, timeout=8.0)
        _switch(con, 2)
        con.drain(quiet=0.3, timeout=2.0)
        con._ser.sendall(bytes([1]) + b"x")
        con.drain(quiet=0.6, timeout=8.0)
    finally:
        con.stop()


def test_switch_keeps_per_console_wordpad_pick_list(kernel_image):
    """#680: WORDPAD's quick-open list/selection state is per-console. A rescan
    on another console must not replace this console's entries on redraw."""
    con = _usb_console(kernel_image)
    con.start()
    try:
        con.drain(quiet=0.3, timeout=2.0)
        assert con.send_line('OPEN "A:/tests/ALPHA.MD" FOR OUTPUT AS #1') == ""
        assert con.send_line("CLOSE #1") == ""
        assert con.send_line('CHDIR "A:/tests"') == ""
        con._ser.sendall(b"WORDPAD\r")
        con.drain(quiet=0.8, timeout=10.0)
        con._ser.sendall(bytes([16]))  # Ctrl+P quick open
        seen = _plain(con.drain(quiet=0.7, timeout=8.0).decode(errors="replace"))
        assert "ALPHA.MD" in seen.upper(), seen

        _switch(con, 2)
        con.drain(quiet=0.3, timeout=2.0)
        assert con.send_line('OPEN "A:/lib/BETA.MD" FOR OUTPUT AS #1') == ""
        assert con.send_line("CLOSE #1") == ""
        assert con.send_line('CHDIR "A:/lib"') == ""
        con._ser.sendall(b"WORDPAD\r")
        con.drain(quiet=0.8, timeout=10.0)
        con._ser.sendall(bytes([16]))
        seen2 = _plain(con.drain(quiet=0.7, timeout=8.0).decode(errors="replace"))
        assert "BETA.MD" in seen2.upper(), seen2

        _switch(con, 1)
        con.drain(quiet=0.3, timeout=2.0)
        con._ser.sendall(b"\x1b[B")  # redraw the still-open picker
        redraw = _plain(con.drain(quiet=0.7, timeout=8.0).decode(errors="replace"))
        assert "ALPHA.MD" in redraw.upper(), redraw
        assert "BETA.MD" not in redraw.upper(), redraw

        con._ser.sendall(b"\x1b")
        con.drain(quiet=0.4, timeout=4.0)
        con._ser.sendall(bytes([24]))  # Ctrl+X quits WORDPAD
        con.drain(quiet=0.6, timeout=8.0)
        _switch(con, 2)
        con.drain(quiet=0.3, timeout=2.0)
        con._ser.sendall(bytes([24]))
        con.drain(quiet=0.6, timeout=8.0)
    finally:
        con.stop()


def test_switch_keeps_per_console_edit_kill_buffer(kernel_image):
    """#769: the kill buffer belongs to the console that cut into it. A paste
    on another console must not insert the first console's text."""
    con = _usb_console(kernel_image)
    con.start()
    try:
        con.drain(quiet=0.3, timeout=2.0)
        _edit(con, "KB1.BAS")
        con._ser.sendall(b"HELLO")
        con._ser.sendall(b"\x1b[H")      # Home
        con._ser.sendall(b"\x1b[1;2F")   # Shift+End selects the line
        con._ser.sendall(b"\x1b[3;2~")   # Shift+Del cuts it to the kill buffer
        con.drain(quiet=0.5, timeout=4.0)

        _switch(con, 2)
        con.drain(quiet=0.3, timeout=2.0)
        _edit(con, "KB2.BAS")
        con._ser.sendall(b"X")
        con._ser.sendall(b"\x1b[2;2~")   # Shift+Ins paste
        con.drain(quiet=0.5, timeout=4.0)
        con._ser.sendall(bytes([19]))    # Ctrl+S save
        con.drain(quiet=0.5, timeout=4.0)
        con._ser.sendall(bytes([1]) + b"x")  # Alt+X quit
        con.drain(quiet=0.6, timeout=8.0)

        assert con.send_line('OPEN "KB2.BAS" FOR INPUT AS #1') == ""
        assert con.send_line("LINE INPUT #1, A$") == ""
        assert con.send_line("PRINT A$") == "X"
        con.send_line("CLOSE #1")
    finally:
        con.stop()


def test_switch_keeps_per_console_edit_find_bar(kernel_image):
    """#769: an open find bar is per console. Opening the editor on another
    console must not clear the first console's bar or borrow its query."""
    con = _usb_console(kernel_image)
    con.start()
    try:
        con.drain(quiet=0.3, timeout=2.0)
        _edit(con, "FND1.BAS")
        con._ser.sendall(b"HELLO")
        con._ser.sendall(b"\x1b[H")
        con._ser.sendall(bytes([6]))  # Ctrl+F find
        con._ser.sendall(b"ZZTOP")
        time.sleep(0.4)
        opened = _plain(con.drain(quiet=0.4, timeout=4.0).decode(errors="replace"))
        assert "Find: ZZTOP" in opened, opened

        _switch(con, 2)
        con.drain(quiet=0.3, timeout=2.0)
        _edit(con, "FND2.BAS")
        con.drain(quiet=0.3, timeout=2.0)

        _switch(con, 1)
        con.drain(quiet=0.3, timeout=2.0)
        con._ser.sendall(b"Q")  # this console's find bar still owns the keyboard
        time.sleep(0.4)
        back = _plain(con.drain(quiet=0.4, timeout=4.0).decode(errors="replace"))
        assert "Find: ZZTOPQ" in back, back
    finally:
        con.stop()

