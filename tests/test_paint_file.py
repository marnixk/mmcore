"""PAINT file Open / Save / Save As (#640).

Runs the real native SDL build (``native/mmcore``) with the #633 harness so the
whole path is exercised: the File menu dispatches into the frozen ``paint.h``
API, the reusable picker in ``cmd_files_ui.c`` browses and returns a path, and
``paint_file.c`` round-trips the canvas through the #631 PCX codec.

The native host maps ``C:`` under ``MMB_DRIVE_ROOT``, so a Save As targeted at
``C:/...`` can be checked on the real filesystem. Tests are guarded and skip
without a host C toolchain / SDL2, exactly like ``test_paint_native.py``.
"""
import os
import shutil
import subprocess
import tempfile

import pytest

from native_harness import (
    NativeSession,
    Ppm,
    SDL_BIN,
    build_native,
    sdl_backend_available,
)

pytestmark = pytest.mark.skipif(
    shutil.which("cc") is None or shutil.which("make") is None,
    reason="needs a host C toolchain (cc/make)",
)

# Canvas layout mirrored from paint.h (#634).
PT_CANVAS_X, PT_CANVAS_Y = 32, 16

# File dropdown item cell centres from paint_menus.c (#638).
FILE_TITLE = (12, 8)
ITEM_NEW = (12, 24)
ITEM_OPEN = (12, 40)
ITEM_SAVE = (12, 56)
ITEM_SAVE_AS = (12, 72)


@pytest.fixture(scope="module")
def native_mmcore():
    build_native()
    if not sdl_backend_available():
        pytest.skip("SDL2 backend not built (pkg-config sdl2 missing)")
    return SDL_BIN


@pytest.fixture
def root(tmp_path):
    """A writable drive root with an empty C: volume."""
    r = os.path.join(str(tmp_path), "drives")
    os.makedirs(os.path.join(r, "C"), exist_ok=True)
    return r


def _session(tmp_path, root):
    return NativeSession(
        tmp_path,
        env={"MMB_PAINT_FORCE_MOUSE": "1", "MMB_DRIVE_ROOT": root},
    )


def _menu(s, item):
    s.move(*FILE_TITLE)
    s.click("l")
    s.move(*item)
    s.click("l")


def _draw(s, x0, y0, x1, y1):
    s.move(x0, y0)
    s.down("l")
    s.move(x1, y1)
    s.up("l")


def _park(s):
    """Move the mouse off the checked pixels so the cursor cannot mask them."""
    s.move(560, 300)


def _px(path, x, y):
    return Ppm(path).pixel(x, y)


def _is_white(rgb):
    return all(c > 200 for c in rgb)


def test_save_as_then_open_round_trips(native_mmcore, tmp_path, root):
    """Acceptance: save a canvas, clear it, open it back, compare pixels."""
    s = _session(tmp_path, root)
    s.feed("PAINT")
    _draw(s, 100, 100, 160, 100)
    _park(s)
    before = s.shot("before.ppm")

    _menu(s, ITEM_SAVE_AS)
    s.text("C:/ROUND.PCX")
    s.key("enter")
    _park(s)
    s.shot("saved.ppm")

    _menu(s, ITEM_NEW)		# clean after the save: no confirm
    _park(s)
    s.shot("cleared.ppm")

    _menu(s, ITEM_OPEN)
    s.text("C:/ROUND.PCX")
    s.key("enter")
    _park(s)
    s.shot("reopened.ppm")
    s.quit()
    out = s.run()

    assert "C:/ROUND.PCX" in out, out
    assert os.path.exists(os.path.join(root, "C", "ROUND.PCX"))

    for x in (100, 120, 160):
        assert _is_white(_px(before, x, 100)), (x, _px(before, x, 100))
        assert not _is_white(_px(os.path.join(tmp_path, "cleared.ppm"), x, 100))
        assert _is_white(_px(os.path.join(tmp_path, "reopened.ppm"), x, 100)), x


def test_save_reuses_the_last_path(native_mmcore, tmp_path, root):
    """Save with a known path must not reopen the picker."""
    s = _session(tmp_path, root)
    s.feed("PAINT")
    _draw(s, 100, 100, 140, 100)
    _menu(s, ITEM_SAVE_AS)
    s.text("C:/REUSE.PCX")
    s.key("enter")
    # A second stroke, then plain Save: no picker, writes the same file.
    _draw(s, 100, 130, 140, 130)
    _menu(s, ITEM_SAVE)
    _park(s)
    s.shot("after.ppm")
    s.quit()
    out = s.run()

    # Exactly one Save As session happened; Save reused the path silently.
    assert out.count("SAVE ST=CHOSEN") == 1, out
    assert os.path.exists(os.path.join(root, "C", "REUSE.PCX"))
    # Both strokes are in the saved file.
    assert _is_white(_px(os.path.join(tmp_path, "after.ppm"), 120, 100))
    assert _is_white(_px(os.path.join(tmp_path, "after.ppm"), 120, 130))


def test_save_as_adds_the_pcx_extension(native_mmcore, tmp_path, root):
    """A typed name without an extension gets .PCX."""
    s = _session(tmp_path, root)
    s.feed("PAINT")
    _draw(s, 100, 100, 140, 100)
    _menu(s, ITEM_SAVE_AS)
    s.text("C:/NODOT")
    s.key("enter")
    s.quit()
    s.run()

    files = os.listdir(os.path.join(root, "C"))
    assert files == ["NODOT.PCX"], files


def test_open_cancel_leaves_the_canvas_unchanged(native_mmcore, tmp_path, root):
    """Esc from the picker keeps the canvas exactly as it was."""
    s = _session(tmp_path, root)
    s.feed("PAINT")
    _draw(s, 100, 100, 160, 100)
    _park(s)
    before = s.shot("before.ppm")

    _menu(s, ITEM_OPEN)
    s.key("esc")
    s.key("enter")		# resolves the pending Esc as a cancel
    _park(s)
    s.shot("after.ppm")
    s.quit()
    out = s.run()

    assert "OPEN ST=CANCEL" in out, out
    assert "OPEN ST=CHOSEN" not in out, out
    for x in (100, 120, 160):
        assert _px(before, x, 100) == _px(os.path.join(tmp_path, "after.ppm"), x, 100), x


def test_picker_browses_into_a_subdirectory(native_mmcore, tmp_path, root):
    """The list navigates with the arrow keys: enter a folder, pick a file."""
    os.makedirs(os.path.join(root, "C", "ART"))
    s = _session(tmp_path, root)
    s.feed("PAINT")
    _draw(s, 100, 100, 160, 100)
    _menu(s, ITEM_SAVE_AS)
    s.text("C:/ART/PIC.PCX")
    s.key("enter")
    # A second stroke that is not in the file; Open must drop it.
    _draw(s, 100, 200, 160, 200)
    # Open starts in C:/ART (the last path); Down selects PIC.PCX.
    _menu(s, ITEM_OPEN)
    s.key("down")
    s.key("enter")
    _park(s)
    s.shot("open.ppm")
    s.quit()
    out = s.run()

    assert "OPEN ST=CHOSEN" in out, out
    assert "SEL=PIC.PCX" in out, out
    assert _is_white(_px(os.path.join(tmp_path, "open.ppm"), 120, 100))
    assert not _is_white(_px(os.path.join(tmp_path, "open.ppm"), 120, 200))


def test_files_browser_survives_a_picker_session(native_mmcore, tmp_path, root):
    """The picker owns its own state; the FILES browser still opens after."""
    s = _session(tmp_path, root)
    s.feed("PAINT")
    _draw(s, 100, 100, 140, 100)
    _menu(s, ITEM_SAVE_AS)
    s.text("C:/SHARED.PCX")
    s.key("enter")
    s.key("ctrl+x")		# leave PAINT without the Alt+TEXTINPUT swallow
    s.feed('FILES "C:/"')
    s.text("q")			# plain letters arrive as text, not named keys
    s.feed("PRINT 6*7")
    s.quit()
    out = s.run()

    assert "[FILES]" in out, out
    assert "SHARED.PCX" in out.upper(), out
    assert "42" in out, out
