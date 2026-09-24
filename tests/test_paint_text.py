"""PAINT text tool (#642).

Host-side pixel tests for ``mmbasic/src/paint_text.c``. The module is compiled
against the frozen ``paint.h`` together with the real ``paint_tools.c`` (so the
tool-registration hook is exercised) and ``paint_undo.c`` (so the single-undo-
step assertion is real), driven through a small ctypes shim.

Covered: clicking with the TEXT tool drops a caret, typing draws the built-in
CP437 8x8 glyphs in the foreground index (background on a right-button caret),
backspace edits the live buffer, Enter bakes the string into the canvas as one
undo step (and Ctrl-Z restores the pre-text pixels), Esc cancels, and keys are
ignored while no caret is open. No QEMU required.
"""
import ctypes
import os
import shutil
import subprocess
import sys

import pytest

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(REPO, "mmbasic", "src")

# Layout from mmbasic/src/paint.h.
PT_CANVAS_Y = 16
PT_TOOL_COUNT = 16
TEXT = 3

LEFT, RIGHT = 1, 2
ENTER, ESC, BACKSPACE = 13, 27, 8

# The two glyphs asserted below, from the built-in IBM CP437 8x8 font.
GLYPH_H = [0xCC, 0xCC, 0xCC, 0xFC, 0xCC, 0xCC, 0xCC, 0x00]
GLYPH_I = [0x78, 0x30, 0x30, 0x30, 0x30, 0x30, 0x78, 0x00]

SHIM_C = r"""
/* Test shim: the pieces cmd_paint.c normally supplies plus the text hooks. */
#include "paint.h"

#include <stdlib.h>
#include <string.h>

pt_state PT;

static mmb s_mmb;
mmb *g_cur = &s_mmb;

static void *tst_alloc(unsigned n) { return malloc(n ? n : 1); }
static void tst_free(void *p) { free(p); }

static mmb_platform g_plat;
static int g_bound;

static void bind_plat(void)
{
	if (g_bound)
		return;
	g_plat.alloc = tst_alloc;
	g_plat.free = tst_free;
	s_mmb.plat = &g_plat;
	g_bound = 1;
}

void tst_reset(int w, int h)
{
	bind_plat();
	if (PT.canvas)
		tst_free(PT.canvas);
	if (PT.scratch)
		tst_free(PT.scratch);
	memset(&PT, 0, sizeof PT);
	PT.width = w;
	PT.height = h;
	PT.fg = 15;
	PT.bg = 0;
	PT.zoom = 1;
	PT.tool = PT_TOOL_TEXT;
	PT.menu = PT_MENU_NONE;
	PT.canvas = (unsigned char *)tst_alloc((unsigned)w * (unsigned)h);
	PT.scratch = (unsigned char *)tst_alloc((unsigned)w * (unsigned)h);
	memset(PT.canvas, 0, (unsigned)w * (unsigned)h);
	memset(PT.scratch, 0, (unsigned)w * (unsigned)h);
	PT.scratch_valid = 0;
	pt_text_init();
	pt_tools_init();
	pt_undo_init();
}

unsigned char *tst_canvas(void) { return PT.canvas; }
int tst_w(void) { return PT.width; }
int tst_h(void) { return PT.height; }

void tst_set(int x, int y, int v) { pt_canvas_set(x, y, v); }
int tst_get(int x, int y) { return pt_canvas_get(x, y); }

void tst_colors(int fg, int bg) { PT.fg = fg; PT.bg = bg; }
void tst_tool(int t) { pt_tool_select(t); }
void tst_begin(int x, int y, int b) { pt_tool_begin(x, y, b); }
int tst_key(int k) { return pt_text_key(k); }
int tst_active(void) { return pt_text_active(); }
int tst_undo_depth(void) { return PT.undo_depth; }

void tst_undo(void) { pt_undo(); }

/* cmd_paint.c helpers, stubbed for the host. */
void pt_fill_rect(int x, int y, int w, int h, unsigned rgb)
{
	(void)x; (void)y; (void)w; (void)h; (void)rgb;
}

void pt_plot(int x, int y, unsigned rgb) { (void)x; (void)y; (void)rgb; }

unsigned pt_palette_rgb(int idx) { (void)idx; return 0; }

void pt_request_redraw(void) { PT.dirty = 1; }

/* paint_tools.c links here too; its preview marks the reverted rectangle. */
void pt_damage_canvas(int x, int y, int w, int h)
{
	(void)x; (void)y; (void)w; (void)h;
}

int pt_screen_to_canvas(int sx, int sy, int *cx, int *cy)
{
	(void)sx; (void)sy; (void)cx; (void)cy;
	return 0;
}

int pt_canvas_get(int cx, int cy)
{
	if (!PT.canvas || cx < 0 || cy < 0 || cx >= PT.width || cy >= PT.height)
		return 0;
	return PT.canvas[(size_t)cy * PT.width + cx];
}

void pt_canvas_set(int cx, int cy, int idx)
{
	if (!PT.canvas || cx < 0 || cy < 0 || cx >= PT.width || cy >= PT.height)
		return;
	PT.canvas[(size_t)cy * PT.width + cx] = (unsigned char)(idx & 255);
}
"""


def _build(tmp_path):
    if shutil.which("cc") is None:
        pytest.skip("needs a host C toolchain (cc)")
    shim = os.path.join(tmp_path, "paint_text_shim.c")
    with open(shim, "w", encoding="utf-8") as fh:
        fh.write(SHIM_C)
    lib = os.path.join(tmp_path, "libpaint_text")
    cmd = [
        "cc", "-std=c11", "-O0", "-g", "-Wall", "-Wextra", "-Werror",
        "-fPIC",
        "-DMMB_PLATFORM_POSIX",
        "-I", SRC,
        "-I", os.path.join(REPO, "mmbasic", "include"),
        "-I", os.path.join(REPO, "mmbasic", "third_party"),
        "-I", os.path.join(REPO, "console"),
        "-I", os.path.join(REPO, "native"),
    ]
    if sys.platform == "darwin":
        cmd += ["-dynamiclib"]
        lib += ".dylib"
    else:
        cmd += ["-shared"]
        lib += ".so"
    cmd += [
        "-o", lib,
        os.path.join(SRC, "paint_text.c"),
        os.path.join(SRC, "paint_tools.c"),
        os.path.join(SRC, "paint_undo.c"),
        shim,
    ]
    subprocess.run(cmd, check=True, cwd=REPO)
    return ctypes.CDLL(lib)


@pytest.fixture(scope="module")
def _lib(tmp_path_factory):
    return _build(tmp_path_factory.mktemp("paint_text"))


class Pad:
    """A (re)settable PT canvas driven through the text tool hooks."""

    def __init__(self, lib, w=64, h=32):
        self.lib = lib
        lib.tst_reset.argtypes = [ctypes.c_int, ctypes.c_int]
        lib.tst_get.argtypes = [ctypes.c_int, ctypes.c_int]
        lib.tst_get.restype = ctypes.c_int
        lib.tst_set.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_int]
        lib.tst_colors.argtypes = [ctypes.c_int, ctypes.c_int]
        lib.tst_tool.argtypes = [ctypes.c_int]
        lib.tst_begin.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_int]
        lib.tst_key.argtypes = [ctypes.c_int]
        lib.tst_key.restype = ctypes.c_int
        lib.tst_active.restype = ctypes.c_int
        lib.tst_undo_depth.restype = ctypes.c_int
        self.w, self.h = w, h
        self.reset(w, h)

    def reset(self, w=None, h=None):
        self.w = self.w if w is None else w
        self.h = self.h if h is None else h
        self.lib.tst_reset(self.w, self.h)

    def px(self, x, y):
        return self.lib.tst_get(x, y)

    def colors(self, fg, bg):
        self.lib.tst_colors(fg, bg)

    def tool(self, t):
        self.lib.tst_tool(t)

    def begin(self, x, y, b=LEFT):
        self.lib.tst_begin(x, y, b)

    def key(self, k):
        return self.lib.tst_key(k)

    def type(self, text):
        for ch in text:
            self.lib.tst_key(ord(ch))

    def active(self):
        return self.lib.tst_active()

    def undo_depth(self):
        return self.lib.tst_undo_depth()

    def undo(self):
        self.lib.tst_undo()


@pytest.fixture
def pt(_lib):
    return Pad(_lib)


def _on_pixels(rows):
    """The (col, row) cells that a glyph bit pattern lights up."""
    return {
        (col, row)
        for row, bits in enumerate(rows)
        for col in range(8)
        if bits & (0x80 >> col)
    }


def assert_glyph(pt, rows, ox, oy, color):
    """Every cell of an 8x8 glyph is exactly ``color`` on and 0 off."""
    on = _on_pixels(rows)
    for row in range(8):
        for col in range(8):
            want = color if (col, row) in on else 0
            assert pt.px(ox + col, oy + row) == want, (ox + col, oy + row)


# ---- caret + typing -------------------------------------------------------


def test_click_places_caret_and_enter_bakes_one_undo_step(pt):
    pt.colors(7, 0)
    pt.begin(2, 2, LEFT)
    assert pt.active() == 1
    # An empty buffer shows just the caret in the text colour.
    assert pt.px(2, 2) == 7
    assert pt.px(2, 3) == 7

    pt.type("HI")
    assert pt.active() == 1
    # The caret trails the last glyph while editing.
    assert pt.px(2 + 2 * 8, 2) == 7

    assert pt.key(ENTER) == 1
    assert pt.active() == 0
    assert_glyph(pt, GLYPH_H, 2, 2, 7)
    assert_glyph(pt, GLYPH_I, 10, 2, 7)

    # The whole string is one undo step; undoing restores the blank canvas.
    assert pt.undo_depth() == 1
    pt.undo()
    for y in range(pt.h):
        for x in range(pt.w):
            assert pt.px(x, y) == 0


def test_right_button_caret_draws_with_the_background(pt):
    pt.colors(7, 9)
    pt.begin(4, 5, RIGHT)
    pt.type("H")
    pt.key(ENTER)
    assert_glyph(pt, GLYPH_H, 4, 5, 9)


# ---- editing before commit ------------------------------------------------


def test_backspace_edits_the_live_buffer(pt):
    pt.colors(5, 0)
    pt.begin(2, 2, LEFT)
    pt.type("HI")
    pt.key(BACKSPACE)
    assert pt.active() == 1
    pt.key(ENTER)

    assert_glyph(pt, GLYPH_H, 2, 2, 5)
    for y in range(2, 10):
        for x in range(10, 18):
            assert pt.px(x, y) == 0


def test_esc_cancels_and_leaves_no_history(pt):
    pt.colors(5, 0)
    pt.begin(2, 2, LEFT)
    pt.type("H")
    assert pt.px(3, 2) == 5        # live preview
    assert pt.key(ESC) == 1
    assert pt.active() == 0
    assert pt.undo_depth() == 0
    for y in range(pt.h):
        for x in range(pt.w):
            assert pt.px(x, y) == 0


# ---- gating ---------------------------------------------------------------


def test_keys_are_ignored_without_a_caret(pt):
    assert pt.active() == 0
    assert pt.key(ord("X")) == 0
    assert pt.undo_depth() == 0


def test_tool_registration_routes_the_click_into_the_text_module(pt):
    # pt_tool_begin with PT_TOOL_TEXT is what opens the caret.
    pt.colors(6, 0)
    pt.tool(TEXT)
    pt.begin(1, 1, LEFT)
    assert pt.active() == 1
    pt.type("I")
    pt.key(ENTER)
    assert_glyph(pt, GLYPH_I, 1, 1, 6)
