"""PAINT app (#512) and its pointer path (#513).

Keyboard-only coverage runs against QEMU (no mouse attached, so the app must
degrade to the keyboard). The native SDL pointer path is compiled and run as a
host test so mouse motion/buttons reach the app without real hardware.
"""
import os
import re
import shutil
import subprocess

import pytest

from harness import MMBasicConsole
from ihelp_util import dump_topic

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Layout constants from mmbasic/src/cmd_paint.c.
PT_OX, PT_OY = 2, 4
CELL_W, CELL_H = 8, 16


def _plain(s):
    return re.sub(r"\x1b\[[0-9;?]*[A-Za-z]", "", s)


def _open(c, command):
    """Launch PAINT over raw serial (send_line hangs inside a full-screen app)."""
    assert c._ser is not None
    c.drain(quiet=0.15)
    c._ser.sendall(command.encode() + b"\r")
    return _plain(c.drain(quiet=0.8).decode(errors="replace"))


def _keys(c, data, quiet=0.35):
    assert c._ser is not None
    c._ser.sendall(data)
    return _plain(c.drain(quiet=quiet).decode(errors="replace"))


def _quit(c):
    return _keys(c, bytes([1]) + b"x", quiet=0.5)


def _pixel(console, x, y):
    out = console.send_line(f"PRINT PIXEL({x},{y})")
    return int(out.split()[0])


def _is_red(v):
    r, g, b = (v >> 16) & 255, (v >> 8) & 255, v & 255
    return r > 130 and g < 120 and b < 120


def _is_blue(v):
    r, g, b = (v >> 16) & 255, (v >> 8) & 255, v & 255
    return b > 130 and r < 120 and g < 120


def _is_white(v):
    r, g, b = (v >> 16) & 255, (v >> 8) & 255, v & 255
    return r > 200 and g > 200 and b > 200


def _pack(rgb):
    r, g, b = rgb
    return (r << 16) | (g << 8) | b


def _cell_pixel(console, cx, cy):
    """Packed RGB of the canvas pixel at (cx, cy) as rendered on the TUI."""
    x = (PT_OX + cx) * CELL_W + CELL_W // 2
    y = (PT_OY + cy) * CELL_H + CELL_H // 2
    return _pack(console.screen_pixel(x, y))


def test_paint_opens_and_shows_chrome(fresh_console):
    c = fresh_console
    seen = _open(c, 'PAINT "A:/PT0.PNG", 16, 16')
    assert "PAINT" in seen.upper()
    assert "PT0.PNG" in seen.upper()
    assert "16X16" in seen.upper() or "16x16" in seen.lower()
    _quit(c)
    # Back at the prompt the console is usable again (no-mouse degrade).
    assert c.send_line("PRINT 2+3") == "5"


def test_paint_draws_and_saves_loadable_png(fresh_console):
    c = fresh_console
    _open(c, 'PAINT "A:/PT1.PNG", 16, 16')
    _keys(c, b"4")          # colour 4 = red in the IBM palette
    _keys(c, b" ")          # pencil at (0,0)
    _keys(c, b"\x1b[C\x1b[C")  # right, right -> (2,0)
    _keys(c, b" ")          # pencil at (2,0)
    _keys(c, b"s", quiet=0.6)
    _quit(c)

    # The saved file is a real, pixel-accurate bitmap: load it as a sprite.
    assert c.send_line('SPRITE LOADPNG 1, "PT1.PNG"') == ""
    assert c.send_line("SPRITE SHOW 1, 0, 0, 1") == ""
    assert _is_red(_pixel(c, 0, 0))
    assert _is_red(_pixel(c, 2, 0))
    c.send_line("SPRITE CLOSE 1")


def test_paint_reloads_sketch_on_canvas(fresh_console):
    c = fresh_console
    _open(c, 'PAINT "A:/PT2.PNG", 16, 16')
    _keys(c, b"4 ")
    _keys(c, b"\x1b[C\x1b[C\x1b[C ")  # move to (3,0) and draw
    _keys(c, b"s", quiet=0.6)
    _quit(c)

    # No explicit size: the loaded image defines the canvas.
    seen = _open(c, 'PAINT "A:/PT2.PNG"')
    assert "16X16" in seen.upper() or "16x16" in seen.lower()
    _keys(c, b"\x1b[B")  # move the cursor off the checked cells
    assert _is_red(_cell_pixel(c, 0, 0))
    assert _is_red(_cell_pixel(c, 3, 0))
    assert _is_white(_cell_pixel(c, 1, 0))
    _quit(c)


def test_paint_fill_tool_floods_canvas(fresh_console):
    c = fresh_console
    _open(c, 'PAINT "A:/PT3.PNG", 16, 16')
    _keys(c, b"1")          # colour 1 = blue
    _keys(c, b"f")          # flood fill tool
    _keys(c, b" ")          # fill from (0,0)
    _keys(c, b"s", quiet=0.6)
    _quit(c)

    assert c.send_line('SPRITE LOADPNG 1, "PT3.PNG"') == ""
    assert c.send_line("SPRITE SHOW 1, 0, 0, 1") == ""
    assert _is_blue(_pixel(c, 0, 0))
    assert _is_blue(_pixel(c, 15, 15))
    c.send_line("SPRITE CLOSE 1")


def test_paint_undo_restores_previous_pixels(fresh_console):
    c = fresh_console
    _open(c, 'PAINT "A:/PT4.PNG", 16, 16')
    _keys(c, b"4")
    _keys(c, b" ")          # red at (0,0)
    _keys(c, b"\x1b[C")     # move right -> (1,0)
    _keys(c, b"1")          # colour 1 = blue
    _keys(c, b" ")          # blue at (1,0)
    _keys(c, b"u")          # undo the blue
    _keys(c, b"s", quiet=0.6)
    _quit(c)

    assert c.send_line('SPRITE LOADPNG 1, "PT4.PNG"') == ""
    assert c.send_line("SPRITE SHOW 1, 0, 0, 1") == ""
    assert _is_red(_pixel(c, 0, 0))
    assert not _is_blue(_pixel(c, 1, 0))
    c.send_line("SPRITE CLOSE 1")


def test_help_paint_topic(console):
    out = dump_topic(console, "PAINT")
    assert "not implemented" not in out.lower()
    assert "pencil" in out.lower()
    assert "PAINT" in out


def _sdl_flags():
    sdl = subprocess.run(
        ["pkg-config", "--cflags", "--libs", "sdl2"],
        capture_output=True,
        text=True,
    )
    return sdl


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


def _pointer_event(con, events):
    resp = con._qmp_cmd(
        {"execute": "input-send-event", "arguments": {"events": events}}
    )
    if not resp or "error" in resp:
        pytest.skip(f"QEMU input-send-event unsupported: {resp}")


def _mouse_move(con, dx, dy):
    events = []
    if dx:
        events.append({"type": "rel", "data": {"axis": "x", "value": dx}})
    if dy:
        events.append({"type": "rel", "data": {"axis": "y", "value": dy}})
    if events:
        _pointer_event(con, events)


def _mouse_left(con, down):
    _pointer_event(
        con,
        [{"type": "btn", "data": {"down": down, "button": "left"}}],
    )


@pytest.fixture
def mouse_console(kernel_image):
    if not _qemu_has_usb_mouse():
        pytest.skip("qemu-system-aarch64 lacks usb-mouse")
    con = MMBasicConsole(
        kernel_image, extra_qemu=["-device", "usb-mouse"], boot_timeout=40.0
    )
    con.start()
    yield con
    con.stop()


def test_paint_mouse_draws_on_canvas(mouse_console):
    """#513: a USB mouse moves the paint pointer and clicks to draw."""
    import time

    c = mouse_console
    time.sleep(2.0)  # let the USB mouse enumerate and attach
    _open(c, 'PAINT "A:/PTM.PNG", 16, 16')

    # Slam the pointer to the top-left, then place it over canvas (0,0), which
    # is screen pixel (PT_OX*8+4, PT_OY*16+8) = (20, 72).
    for _ in range(6):
        _mouse_move(c, -127, -127)
    time.sleep(0.5)
    _mouse_move(c, 20, 72)
    time.sleep(0.4)
    _mouse_left(c, True)
    time.sleep(0.6)
    _mouse_left(c, False)
    time.sleep(0.4)

    _keys(c, b"s", quiet=0.6)
    _quit(c)

    assert c.send_line('SPRITE LOADPNG 1, "PTM.PNG"') == ""
    assert c.send_line("SPRITE SHOW 1, 0, 0, 1") == ""
    assert _is_red(_pixel(c, 0, 0))
    c.send_line("SPRITE CLOSE 1")


def test_native_pointer_path_reaches_apps(tmp_path):
    """#513: SDL mouse events become framebuffer-space pointer state."""
    sdl = _sdl_flags()
    if sdl.returncode != 0:
        pytest.skip("SDL2 not found (pkg-config sdl2 missing)")
    if shutil.which("cc") is None:
        pytest.skip("needs a host C toolchain (cc)")
    exe = os.path.join(str(tmp_path), "sdl_input_host")
    subprocess.run(
        [
            "cc",
            "-O0",
            "-Wall",
            "-Werror",
            "-DMMB_PLATFORM_POSIX",
            "-I",
            os.path.join(REPO, "native"),
            "-I",
            os.path.join(REPO, "mmbasic", "include"),
            "-I",
            os.path.join(REPO, "mmbasic", "third_party"),
            "-I",
            os.path.join(REPO, "console"),
            *sdl.stdout.split(),
            "-o",
            exe,
            os.path.join(REPO, "tests", "sdl_input_host.c"),
            os.path.join(REPO, "native", "sdl_input.c"),
        ],
        check=True,
        cwd=REPO,
    )
    env = dict(os.environ, SDL_VIDEODRIVER="dummy")
    out = subprocess.run([exe], check=True, capture_output=True, text=True, env=env)
    assert "all checks passed" in out.stdout
