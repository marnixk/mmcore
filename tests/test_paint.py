"""PAINT rewrite scaffold (#634).

The Dr. Halo-style PAINT replaces the old Deluxe Paint app. This ticket lands
the shell: a 640x360 screen with the menu-bar row, left tool column, 4x64 VGA
palette strip and black canvas, plus the no-mouse gate. Tools, palette
behaviour, undo, menus and file I/O land in later tickets against the API
frozen in mmbasic/src/paint.h.

Keyboard coverage runs against QEMU with no pointer, proving the gate. Layout
assertions run with a USB mouse attached so PAINT starts.
"""
import os
import re
import subprocess
import time

import pytest

from harness import MMBasicConsole
from ihelp_util import dump_topic

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Layout constants from mmbasic/src/paint.h (#634).
PT_W, PT_H = 640, 360
PT_MENU_H = 16
PT_TOOL_W = 32
PT_PAL_X = 32
PT_PAL_Y = 328
PT_PAL_SW = 8
PT_PAL_COLS = 64
PT_CANVAS_X = 32
PT_CANVAS_Y = 16
PT_PAL_MID = 16  # middle of a 32px-tall indicator band


def _plain(s):
    return re.sub(r"\x1b\[[0-9;?]*[A-Za-z]", "", s)


def _open(c, command="PAINT"):
    """Launch PAINT over raw serial (send_line hangs inside a full-screen app)."""
    assert c._ser is not None
    c.drain(quiet=0.15)
    c._ser.sendall(command.encode() + b"\r")
    return _plain(c.drain(quiet=0.8).decode(errors="replace"))


def _quit(c):
    """Alt+X leaves PAINT."""
    assert c._ser is not None
    c._ser.sendall(bytes([1]) + b"x")
    return _plain(c.drain(quiet=0.6).decode(errors="replace"))


def _rgb(c, x, y):
    return c.screen_pixel(x, y)


def _lum(rgb):
    return sum(rgb)


def test_paint_requires_mouse_without_override(fresh_console):
    """No pointer and no override: a clear message, prompt untouched."""
    c = fresh_console
    seen = _open(c)
    assert "mouse" in seen.lower()
    assert "not implemented" not in seen.lower()
    # The console is usable again (the screen was not taken over).
    assert c.send_line("PRINT 2+3") == "5"


def test_help_paint_topic(console):
    out = dump_topic(console, "PAINT")
    low = out.lower()
    assert "not implemented" not in low
    assert "PAINT" in out
    assert "mouse" in low
    assert "palette" in low
    assert "foreground" in low
    assert "right" in low          # LMB/RMB rule
    assert "pcx" in low
    assert "640x360" in out


def _qemu_has_usb_mouse():
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


@pytest.fixture
def mouse_console(kernel_image):
    if not _qemu_has_usb_mouse():
        pytest.skip("qemu-system-aarch64 lacks usb-mouse")
    con = MMBasicConsole(
        kernel_image, extra_qemu=["-device", "usb-mouse"], boot_timeout=40.0
    )
    con.start()
    time.sleep(2.0)  # let the USB mouse enumerate and attach
    yield con
    con.stop()


def test_paint_layout_640x360(mouse_console):
    """#634: menu row, tool column, 4x64 palette strip and black canvas."""
    c = mouse_console
    vres_before = c.send_line("PRINT MM.VRES")
    hres_before = c.send_line("PRINT MM.HRES")
    _open(c)

    # PAINT retunes the display to a true 640x360.
    assert c.screen_size() == (PT_W, PT_H)

    # Canvas starts entirely black.
    assert _rgb(c, PT_CANVAS_X + 50, PT_CANVAS_Y + 50) == (0, 0, 0)
    assert _rgb(c, PT_CANVAS_X + 400, PT_CANVAS_Y + 200) == (0, 0, 0)

    # Palette swatch 0 is black, swatch 15 is bright.
    def swatch(i):
        return _rgb(
            c,
            PT_PAL_X + (i % PT_PAL_COLS) * PT_PAL_SW + 4,
            PT_PAL_Y + (i // PT_PAL_COLS) * PT_PAL_SW + 4,
        )

    assert _lum(swatch(0)) < 30
    assert _lum(swatch(15)) > 600
    assert swatch(0) != swatch(15)

    # FG/BG indicator at the left end of the strip: inner square is FG (white).
    assert _lum(_rgb(c, PT_TOOL_W // 2, PT_PAL_Y + PT_PAL_MID)) > 600

    # Menu-bar row 0 is painted (not black) and the tool column is drawn.
    assert _lum(_rgb(c, 400, PT_MENU_H // 2)) > 60
    assert _lum(_rgb(c, PT_TOOL_W // 2, PT_CANVAS_Y + 16)) > 60

    _quit(c)

    # The console is usable and the caller's display mode is restored.
    assert c.send_line("PRINT 2+3") == "5"
    assert c.send_line("PRINT MM.VRES") == vres_before
    assert c.send_line("PRINT MM.HRES") == hres_before


def test_paint_alt_x_exits(mouse_console):
    c = mouse_console
    _open(c)
    _quit(c)
    assert c.send_line("PRINT 4+4") == "8"
