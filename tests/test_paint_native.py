"""PAINT rewrite wave 0C (#633): native PAINT test harness.

Exercises the native macOS/Linux SDL build (``native/mmcore``) as a scriptable
target: synthetic mouse/keyboard input from ``MMB_SDL_HARNESS``, framebuffer
captures (``shot``) read back as PPM pixels, and the ``MMB_PAINT_FORCE_MOUSE``
test override that lets a headless run enter PAINT.

The reusable API lives in :mod:`native_harness`:

* ``NativeSession(workdir)`` - launch ``native/mmcore`` (SDL ``dummy`` driver)
  and script a session. Builders: ``feed``, ``move``, ``down``/``up``/``click``,
  ``key``, ``text``, ``shot``/``snapshot``, ``quit``. ``run()`` executes the
  buffered script to completion; the builders also work live after ``start()``.
* ``Ppm`` - a captured framebuffer with ``pixel(x, y)`` and ``nonblack()``;
  helpers ``lum``, ``is_black``, ``is_white``.
* ``build_native()`` / ``sdl_backend_available()`` - build/skip plumbing.

These tests are guarded: they skip without a host C toolchain and without the
SDL2 backend, so the QEMU suite is unaffected. The harness is opt-in (env vars
only); a normal run never reads a script or forces a mouse.
"""
import os
import shutil
import subprocess

import pytest

from native_harness import (
    NativeSession,
    Ppm,
    SDL_BIN,
    build_native,
    is_black,
    lum,
    sdl_backend_available,
)

# Layout constants mirrored from mmbasic/src/paint.h (#634).
PT_W, PT_H = 640, 360
PT_TOOL_W = 32
PT_CANVAS_X, PT_CANVAS_Y = 32, 16
PT_PAL_X, PT_PAL_Y = 32, 328
PT_PAL_SW = 8
PT_PAL_MID = 16
# Inner FG/BG indicator square: the foreground colour lives here.
IND_X, IND_Y = PT_TOOL_W // 2, PT_PAL_Y + PT_PAL_MID

pytestmark = pytest.mark.skipif(
    shutil.which("cc") is None or shutil.which("make") is None,
    reason="needs a host C toolchain (cc/make)",
)


@pytest.fixture(scope="module")
def native_mmcore():
    """Build the native tree once and return the SDL binary (or skip)."""
    build_native()
    if not sdl_backend_available():
        pytest.skip("SDL2 backend not built (pkg-config sdl2 missing)")
    return SDL_BIN


def _lit_near(img, x, y, r=8):
    """True when any pixel in a small window around (x, y) is lit.

    The cursor hotspot is not necessarily on an opaque art pixel (the pencil
    tool's tip sits one pixel right of its hotspot), so assert the sprite was
    drawn at the requested point by sampling a small neighbourhood instead of
    one exact pixel.
    """
    for yy in range(y - r, y + r + 1):
        for xx in range(x - r, x + r + 1):
            if 0 <= xx < img.width and 0 <= yy < img.height:
                if lum(img.pixel(xx, yy)) > 600:
                    return True
    return False


def _run_with_dump(binary, tmp_path, name, program, extra_env=None):
    """Run the binary headlessly; dump the final framebuffer as PPM on exit."""
    ppm = os.path.join(str(tmp_path), name + ".ppm")
    env = dict(os.environ, SDL_VIDEODRIVER="dummy", MMB_SDL_DUMP=ppm)
    if extra_env:
        env.update(extra_env)
    proc = subprocess.run(
        [binary],
        input=program,
        text=True,
        capture_output=True,
        timeout=60,
        env=env,
    )
    img = Ppm(ppm) if os.path.isfile(ppm) else None
    return proc.stdout + proc.stderr, img


def test_paint_reports_missing_mouse_by_default(native_mmcore, tmp_path):
    """No pointer and no override: PAINT declines and stays off the screen.

    This is the real-target behaviour (the SDL build wires no pointer), and it
    is exactly what would break headless CI without the test override.
    """
    out, img = _run_with_dump(native_mmcore, tmp_path, "nomouse", "PAINT\n")
    assert "needs a mouse" in out.lower(), out
    assert img is not None, "expected the framebuffer dump"
    # PAINT never retuned the display to its 640x360 screen.
    assert (img.width, img.height) != (PT_W, PT_H)


def test_force_mouse_override_enters_paint(native_mmcore, tmp_path):
    """#633: MMB_PAINT_FORCE_MOUSE makes the no-mouse gate proceed.

    With the override set PAINT opens its 640x360 screen and paints the
    palette strip; without it (previous test) it prints the message instead.
    """
    out, img = _run_with_dump(
        native_mmcore,
        tmp_path,
        "force",
        "PAINT\n",
        {"MMB_PAINT_FORCE_MOUSE": "1"},
    )
    assert "needs a mouse" not in out.lower(), out
    assert img is not None
    assert (img.width, img.height) == (PT_W, PT_H)
    # Palette swatch 15 is white, swatch 0 is black (fixed VGA palette).
    assert lum(img.pixel(PT_PAL_X + 15 * PT_PAL_SW + 4, PT_PAL_Y + 4)) > 600
    assert lum(img.pixel(PT_PAL_X + 0 * PT_PAL_SW + 4, PT_PAL_Y + 4)) < 30
    # Canvas starts black.
    assert is_black(img.pixel(PT_CANVAS_X + 80, PT_CANVAS_Y + 80))


def test_mouse_move_and_click_change_pixels(native_mmcore, tmp_path):
    """Acceptance: move the mouse, click, and assert pixels changed.

    One scripted session: enter PAINT, move the pointer onto the canvas (the
    cursor sprite paints), then left-click palette swatch 0 (the FG indicator
    flips from white to black). Every step is separated by a captured frame.
    """
    s = NativeSession(tmp_path)
    s.feed("PAINT")
    before = s.shot("before.ppm")
    s.move(100, 100)
    moved = s.shot("moved.ppm")
    s.move(PT_PAL_X + 4, PT_PAL_Y + 4)  # swatch 0
    s.click("l")
    clicked = s.shot("clicked.ppm")
    s.quit()
    out = s.run()

    assert "needs a mouse" not in out.lower(), out
    img0, img1, img2 = Ppm(before), Ppm(moved), Ppm(clicked)
    assert (img0.width, img0.height) == (PT_W, PT_H)

    # The pointer moved onto the canvas: the cursor sprite is drawn at the
    # requested framebuffer point (sampled, since the hotspot need not be an
    # opaque art pixel and window placement must not shift it).
    assert is_black(img0.pixel(100, 100))
    assert _lit_near(img1, 100, 100)

    # The click selected a foreground colour: the indicator swatch changed.
    assert lum(img0.pixel(IND_X, IND_Y)) > 600
    assert lum(img2.pixel(IND_X, IND_Y)) < 30


def test_open_dropdown_is_not_overpainted(native_mmcore, tmp_path):
    """#661: an open dropdown draws over the canvas and closes cleanly.

    The menu bar lives in text row 0; the dropdown hangs below it over the
    canvas. Before the redraw-order fix the canvas/tools/palette passes
    overpainted the overlay. The right padding cell of the first File row is a
    solid fill, so it reads light grey while the menu is open and black again
    once the canvas is redrawn.
    """
    s = NativeSession(tmp_path)
    s.feed("PAINT")
    base = s.shot("base.ppm")
    s.move(12, 8)          # File title on the menu bar
    s.click("l")
    opened = s.shot("opened.ppm")
    s.move(300, 300)       # click the canvas: closes the menu
    s.click("l")
    closed = s.shot("closed.ppm")
    s.quit()
    out = s.run()
    assert "needs a mouse" not in out.lower(), out

    # Right padding of the first File dropdown row, below the menu bar.
    px, py = 76, 24
    img0, img1, img2 = Ppm(base), Ppm(opened), Ppm(closed)
    assert is_black(img0.pixel(px, py))
    assert lum(img1.pixel(px, py)) > 300       # dropdown on top of the canvas
    assert is_black(img2.pixel(px, py))        # closed: canvas restored


def test_edit_clear_wipes_canvas(native_mmcore, tmp_path):
    """#669: Edit > Clear wipes the canvas, not just the undo history."""
    s = NativeSession(tmp_path)
    s.feed("PAINT")
    s.move(100, 100)
    s.down("l")
    s.move(150, 100)
    s.move(200, 100)
    s.up("l")
    drawn = s.shot("drawn.ppm")
    s.move(60, 8)          # Edit title
    s.click("l")
    s.move(70, 56)         # Clear (third dropdown row)
    s.click("l")
    s.text("y")            # confirm the dialog
    cleared = s.shot("cleared.ppm")
    s.quit()
    out = s.run()
    assert "needs a mouse" not in out.lower(), out

    img0, img1 = Ppm(drawn), Ppm(cleared)
    assert lum(img0.pixel(150, 100)) > 600     # the pencil stroke is drawn
    assert is_black(img1.pixel(150, 100))      # and Clear wipes it
    assert is_black(img1.pixel(200, 100))


def test_keyboard_injection_exits_paint(native_mmcore, tmp_path):
    """Synthetic keys reach the app: Esc leaves PAINT and the REPL works."""
    s = NativeSession(tmp_path)
    s.feed("PAINT")
    s.shot("paint.ppm")
    s.key("esc")
    s.feed("PRINT 2+3")
    s.quit()
    out = s.run()
    assert "needs a mouse" not in out.lower(), out
    assert "5" in out, out


def test_keyboard_alt_x_exits_paint(native_mmcore, tmp_path):
    """Alt+X (the app's documented quit chord) is injectable as `key alt+x`."""
    s = NativeSession(tmp_path)
    s.feed("PAINT")
    s.shot("paint.ppm")
    s.key("alt+x")
    s.feed("PRINT 7*6")
    s.quit()
    out = s.run()
    assert "42" in out, out


def test_text_input_injection(native_mmcore, tmp_path):
    """`text` + `key enter` types a line at the prompt."""
    s = NativeSession(tmp_path)
    s.text("PRINT 9*9")
    s.key("enter")
    s.quit()
    out = s.run()
    assert "81" in out, out
