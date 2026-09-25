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
PT_TOOL_W = 64
PT_CELL_W = 32
PT_CELL_H = (328 - 16) // 9
PT_CANVAS_X, PT_CANVAS_Y = 64, 16
PT_PAL_X, PT_PAL_Y = 92, 328
PT_PAL_SW = 8
PT_PAL_MID = 16
PT_WB_X, PT_WB_W, PT_WB_COUNT = 32, 60, 5
# Inner FG/BG indicator square: the foreground colour lives here.
IND_X, IND_Y = 16, PT_PAL_Y + PT_PAL_MID

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

    The hotspot names an opaque art pixel (#690), but the default pencil tool's
    hotspot is its graphite tip, which is black, so assert the sprite was drawn
    at the requested point by sampling a small neighbourhood (the white body)
    rather than one exact pixel.
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


def test_paint_starts_with_pointer_by_default(native_mmcore, tmp_path):
    """No override: the SDL backend always reports a pointer, so PAINT starts.

    Regression: sdl_input_init() used to run only from the test harness, so a
    normal native launch left s_mouse_present at 0 and PAINT wrongly declined
    with "needs a mouse" (sdl_input.h: present is non-zero whenever the SDL
    video backend runs).
    """
    out, img = _run_with_dump(native_mmcore, tmp_path, "defmouse", "PAINT\n")
    assert "needs a mouse" not in out.lower(), out
    assert img is not None, "expected the framebuffer dump"
    # PAINT retuned the display to its 640x360 screen.
    assert (img.width, img.height) == (PT_W, PT_H)


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
    # requested framebuffer point. The pencil hotspot is its black tip, so
    # sample the lit white body just inside instead of that one pixel.
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


def test_switching_menus_clears_the_old_dropdown(native_mmcore, tmp_path):
    """Switching File -> Help must erase the File dropdown from the canvas.

    The File dropdown starts at menu column 1 and so overlaps the tool column.
    pt_draw_canvas only repaints x >= PT_CANVAS_X, and the tool column is chrome
    that used to be repainted only on a full frame, so the part of File's
    dropdown over the canvas was left behind when the menu switched.
    """
    s = NativeSession(tmp_path)
    s.feed("PAINT")
    s.move(12, 8)
    s.click("l")           # open File
    s.move(110, 8)         # hover the Help title: switches the open menu
    s.move(400, 300)       # park the pointer clear of the checked pixels
    after = s.shot("after.ppm")
    s.quit()
    out = s.run()
    assert "needs a mouse" not in out.lower(), out

    # Help's panel hangs from column 13 (x >= 104). Everything canvas-side of
    # the old File dropdown must be the restored black canvas (the tool column
    # now reaches x < 64).
    img = Ppm(after)
    for y in range(20, 92):
        for x in range(66, 78):
            assert is_black(img.pixel(x, y)), (x, y, img.pixel(x, y))


def test_open_picker_is_visible_and_closes(native_mmcore, tmp_path):
    """#722: File > Open must leave the picker on screen, not flash and vanish.

    The picker used to be drawn once by the picker module and then wiped by
    pt_redraw()'s canvas pass. It is now composed as part of the frame.
    """
    s = NativeSession(tmp_path)
    s.feed("PAINT")
    s.move(12, 8)
    s.click("l")           # File
    s.move(12, 40)
    s.click("l")           # Open
    opened = s.shot("opened.ppm")
    s.key("esc")
    s.key("enter")         # resolves the pending Esc as a cancel
    closed = s.shot("closed.ppm")
    s.quit()
    out = s.run()
    assert "needs a mouse" not in out.lower(), out
    assert "OPEN ST=ACTIVE" in out, out

    def _panel_lit(path):
        img = Ppm(path)
        return sum(1 for y in range(34, 318, 4) for x in range(66, 574, 4)
                   if sum(img.pixel(x, y)) > 200)

    # The cursor stays on the tool column (x < 64), outside the sampled panel.
    assert _panel_lit(opened) > 500, "picker panel was not drawn"
    assert _panel_lit(closed) == 0, "picker panel did not clear"


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


def test_held_press_after_menu_action_stays_off_canvas(native_mmcore, tmp_path):
    """#710: a button still held after an immediate menu action must not paint.

    Draw an undoable stroke, open Edit, press-and-hold Undo. Undo runs and the
    dropdown closes, but the held press used to fall through to the canvas and
    start a pencil dot behind where the menu was. The pointer is then moved
    away after release, so any leftover ink at the Undo row is canvas pixels.
    """
    s = NativeSession(tmp_path)
    s.feed("PAINT")
    s.move(100, 100)
    s.down("l")
    s.move(160, 100)
    s.up("l")
    # Open Edit, press-and-hold Undo, and keep the button down for a frame.
    s.move(60, 8)          # Edit title
    s.click("l")
    s.move(70, 20)         # Undo row
    s.down("l")
    s.move(71, 20)         # held frame: the old code leaked to the canvas here
    held = s.shot("held.ppm")
    s.up("l")
    s.move(400, 300)       # park the pointer away from the row
    final = s.shot("final.ppm")
    s.quit()
    out = s.run()
    assert "needs a mouse" not in out.lower(), out

    # The undo ran: the stroke drawn at y=100 is gone.
    assert is_black(Ppm(held).pixel(130, 100))
    # No stray pencil dot behind the closed dropdown.
    img = Ppm(final)
    assert is_black(img.pixel(70, 20))
    assert is_black(img.pixel(71, 20))


def test_keyboard_injection_exits_paint(native_mmcore, tmp_path):
    """Synthetic keys reach the app: a lone Esc leaves PAINT and the REPL works.

    Esc is buffered while a possible CSI/SS3 sequence could still follow, so
    the idle window has to elapse before it resolves as a real Esc (#726).
    """
    s = NativeSession(tmp_path)
    s.feed("PAINT")
    s.shot("paint.ppm")
    s.key("esc")
    s.wait_ms(300)
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


# A dropdown pixel that only that menu covers: File's fifth row, Edit's third
# row past the File panel, and Help's first row past the Edit panel.
_MENU_MARK = ((30, 88), (90, 56), (130, 24))


def _menu_lit(img, menu):
    """True when the dropdown unique to ``menu`` is painted (white)."""
    x, y = _MENU_MARK[menu]
    return lum(img.pixel(x, y)) > 300


def test_arrow_keys_keep_paint_open(native_mmcore, tmp_path):
    """#726: CSI navigation keys must not be mistaken for the Esc quit.

    The front end replays a non-F12 escape byte-by-byte, so the leading 0x1b
    used to leave PAINT before the rest of the sequence arrived. After all four
    arrows (and Home/End) PAINT still owns its 640x360 screen; only a real lone
    Esc (after the idle window) returns to the prompt.
    """
    s = NativeSession(tmp_path)
    s.feed("PAINT")
    for name in ("left", "right", "up", "down", "home", "end"):
        s.key(name)
    alive = s.shot("alive.ppm")
    s.key("esc")
    s.wait_ms(300)
    s.feed("PRINT 2+3")
    s.quit()
    out = s.run()

    assert "needs a mouse" not in out.lower(), out
    img = Ppm(alive)
    assert (img.width, img.height) == (PT_W, PT_H), "PAINT left on an arrow key"
    assert lum(img.pixel(PT_PAL_X + 15 * PT_PAL_SW + 4, PT_PAL_Y + 4)) > 600
    assert "5" in out, out


def test_alt_letters_open_paint_menus(native_mmcore, tmp_path):
    """#727: Alt+F/E/H open the matching dropdown, and Alt+X quits.

    The chord is delivered as 0x01 + letter (the same bytes ``PollUsbAlt()``
    synthesises in the firmware build), so this covers the PAINT-side path.
    """
    s = NativeSession(tmp_path)
    s.feed("PAINT")
    s.key("alt+f")
    f = s.shot("file.ppm")
    s.key("alt+e")
    e = s.shot("edit.ppm")
    s.key("alt+h")
    h = s.shot("help.ppm")
    s.key("alt+x")
    s.feed("PRINT 5*5")
    s.quit()
    out = s.run()

    assert "needs a mouse" not in out.lower(), out
    assert "25" in out, out
    fi, ei, hi = Ppm(f), Ppm(e), Ppm(h)
    assert _menu_lit(fi, 0) and not _menu_lit(fi, 1) and not _menu_lit(fi, 2)
    assert _menu_lit(ei, 1) and not _menu_lit(ei, 0) and not _menu_lit(ei, 2)
    assert _menu_lit(hi, 2) and not _menu_lit(hi, 0) and not _menu_lit(hi, 1)


def _draw_stroke(s):
    s.move(100, 100)
    s.down("l")
    s.move(160, 100)
    s.up("l")


def test_quit_confirms_unsaved_canvas(native_mmcore, tmp_path):
    """#728: Esc on a dirty canvas prompts; No keeps it, Yes discards and quits."""
    s = NativeSession(tmp_path)
    s.feed("PAINT")
    _draw_stroke(s)
    s.key("esc")
    s.wait_ms(300)			# lone Esc -> discard confirmation
    dialog = s.shot("dialog.ppm")
    s.key("n")				# No: stay in PAINT with the drawing
    s.move(300, 200)
    kept = s.shot("kept.ppm")
    s.key("esc")			# dirty again -> prompt
    s.wait_ms(300)
    s.key("y")				# Yes: discard and tear down
    s.feed("PRINT 8+1")
    s.quit()
    out = s.run()

    assert "needs a mouse" not in out.lower(), out
    assert "9" in out, out
    # The centered confirm panel is white over the black canvas.
    assert lum(Ppm(dialog).pixel(300, 200)) > 300, "no discard dialog on Esc"
    # No kept PAINT and the stroke.
    img = Ppm(kept)
    assert lum(img.pixel(PT_PAL_X + 15 * PT_PAL_SW + 4, PT_PAL_Y + 4)) > 600
    assert lum(img.pixel(130, 100)) > 600, "No discarded the edit"


def test_open_confirms_unsaved_canvas(native_mmcore, tmp_path):
    """#728: File > Open prompts when dirty; No keeps, Yes opens the picker."""
    s = NativeSession(tmp_path)
    s.feed("PAINT")
    _draw_stroke(s)
    s.move(12, 8)
    s.click("l")			# File
    s.move(12, 40)
    s.click("l")			# Open
    dialog = s.shot("open_dialog.ppm")
    s.text("n")				# No: no picker, canvas intact
    s.move(300, 200)
    kept = s.shot("open_kept.ppm")
    s.move(12, 8)
    s.click("l")
    s.move(12, 40)
    s.click("l")			# Open again
    s.text("y")				# Yes: the picker opens
    s.key("esc")
    s.key("enter")			# cancel the picker
    s.quit()
    out = s.run()

    assert "needs a mouse" not in out.lower(), out
    assert "OPEN ST=ACTIVE" in out, out
    assert "OPEN ST=CHOSEN" not in out, out
    assert lum(Ppm(dialog).pixel(300, 200)) > 300, "no discard dialog on Open"
    assert is_black(Ppm(kept).pixel(300, 200)), "No opened the picker"



def _cell(col, row):
    return (col * PT_CELL_W + PT_CELL_W // 2,
            PT_CANVAS_Y + row * PT_CELL_H + PT_CELL_H // 2)


def test_filled_rectangle_cell_paints_a_solid_block(native_mmcore, tmp_path):
    """#719: the rectangle-filled tool fills the interior, unlike the outline."""
    s = NativeSession(tmp_path)
    s.feed("PAINT")
    s.move(*_cell(1, 2))       # RECT_FILLED is row 2, column 1
    s.click("l")
    s.move(120, 80)
    s.down("l")
    s.move(200, 140)
    s.up("l")
    s.move(430, 300)           # park the cursor clear of the check
    shot = s.shot("filled.ppm")
    s.quit()
    out = s.run()
    assert "needs a mouse" not in out.lower(), out

    img = Ppm(shot)
    assert lum(img.pixel(160, 110)) > 600    # interior is solid


def test_selection_tool_draws_marching_ants(native_mmcore, tmp_path):
    """#644: the SELECT tool drags a marching-ants boundary over the canvas."""
    s = NativeSession(tmp_path)
    s.feed("PAINT")
    s.move(*_cell(0, 8))       # SELECT tool
    s.click("l")
    s.move(200, 100)
    s.down("l")
    s.move(260, 150)
    s.up("l")
    s.move(430, 300)           # park the cursor clear of the boundary
    shot = s.shot("ants.ppm")
    s.quit()
    out = s.run()
    assert "needs a mouse" not in out.lower(), out

    img = Ppm(shot)
    # The top edge of the marquee alternates white and black dashes.
    white = sum(1 for x in range(206, 256, 2) if sum(img.pixel(x, 100)) > 600)
    black = sum(1 for x in range(206, 256, 2) if sum(img.pixel(x, 100)) < 40)
    assert white >= 3 and black >= 3, (white, black)


def test_text_font_picker_opens_and_closes(native_mmcore, tmp_path):
    """#643: Ctrl+F lists A:/fonts/gfx over the canvas; Esc closes it.

    The picker is drawn as ephemeral canvas preview pixels, so it must show up
    in a framebuffer shot and vanish on close without baking anything in.
    """
    s = NativeSession(tmp_path)
    s.feed("PAINT")
    s.move(*_cell(1, 1))       # TEXT tool
    s.click("l")
    s.move(200, 100)
    s.click("l")               # place the caret
    s.key("ctrl+f")            # open the font picker
    opened = s.shot("fontpick.ppm")
    s.key("esc")               # close the list (the caret stays)
    s.wait_ms(200)             # let the cleared frame present
    closed = s.shot("fontpick_closed.ppm")
    s.quit()
    out = s.run()
    assert "needs a mouse" not in out.lower(), out

    def lit(path):
        img = Ppm(path)
        return sum(1 for y in range(100, 190, 2)
                   for x in range(476, 638, 2)
                   if sum(img.pixel(x, y)) > 150)

    assert lit(opened) > 200, "font picker panel was not drawn"
    assert lit(closed) < 20, "font picker panel did not clear"


def test_width_selector_thickens_a_dot(native_mmcore, tmp_path):
    """#718: picking the 6px width makes the pencil lay a fat dot."""
    s = NativeSession(tmp_path)
    s.feed("PAINT")
    s.move(PT_WB_X + 4 * 12 + 6, PT_PAL_Y + PT_PAL_MID)   # 5th width cell
    s.click("l")
    s.move(150, 100)
    s.down("l")
    s.up("l")
    s.move(430, 300)           # park the cursor clear of the dot
    shot = s.shot("wide.ppm")
    s.quit()
    out = s.run()
    assert "needs a mouse" not in out.lower(), out

    img = Ppm(shot)
    lit = sum(1 for y in range(92, 109) if lum(img.pixel(150, y)) > 600)
    assert lit >= 5, lit
