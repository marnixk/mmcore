"""PAINT selection + clipboard (#644).

Host-side tests for ``mmbasic/src/paint_select.c``. The module is compiled with
the real ``paint_undo.c`` (so the single-undo-step checks are real) and a shim
that stands in for cmd_paint.c's screen helpers, then driven through the
pt_select_* hooks on a small canvas.

Covered: a rectangular selection drag, deselect-on-click, Select All, copy /
cut / paste with a background (transparent) index, clear-in-selection, moving a
selection, each edit as exactly one undo step, and the marching-ants boundary
being screen-only (never baked into the canvas).
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
PT_W, PT_H = 640, 360
PT_CANVAS_X, PT_CANVAS_Y = 64, 16

LEFT, RIGHT = 1, 2

SHIM_C = r"""
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

static unsigned g_screen[PT_W * PT_H];
#define TST_EMPTY 0x112233u

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
	PT.cursor_x = 0;
	PT.cursor_y = 0;
	PT.canvas = (unsigned char *)tst_alloc((unsigned)w * (unsigned)h);
	PT.scratch = (unsigned char *)tst_alloc((unsigned)w * (unsigned)h);
	memset(PT.canvas, 0, (unsigned)w * (unsigned)h);
	memset(PT.scratch, 0, (unsigned)w * (unsigned)h);
	{
		int i;
		for (i = 0; i < PT_W * PT_H; i++)
			g_screen[i] = TST_EMPTY;
	}
	pt_select_init();
	pt_undo_init();
}

void tst_clear_screen(void)
{
	int i;
	for (i = 0; i < PT_W * PT_H; i++)
		g_screen[i] = TST_EMPTY;
}

int tst_w(void) { return PT.width; }
int tst_h(void) { return PT.height; }
void tst_set(int x, int y, int v) { pt_canvas_set(x, y, v); }
int tst_get(int x, int y) { return pt_canvas_get(x, y); }
void tst_bg(int v) { PT.bg = v; }
void tst_cursor(int x, int y) { PT.cursor_x = x; PT.cursor_y = y; }
int tst_undo_depth(void) { return PT.undo_depth; }
void tst_undo(void) { pt_undo(); }

unsigned tst_screen(int x, int y)
{
	if (x < 0 || y < 0 || x >= PT_W || y >= PT_H)
		return 0;
	return g_screen[y * PT_W + x];
}

/* Time jumps 300ms per call so pt_select_tick() toggles every time. */
unsigned mmb_now_ms(void) { static unsigned t; t += 300; return t; }

void pt_fill_rect(int x, int y, int w, int h, unsigned rgb)
{
	int i, j;
	for (j = 0; j < h; j++)
		for (i = 0; i < w; i++)
			if (x + i >= 0 && y + j >= 0 && x + i < PT_W && y + j < PT_H)
				g_screen[(y + j) * PT_W + x + i] = rgb;
}

void pt_plot(int x, int y, unsigned rgb) { pt_fill_rect(x, y, 1, 1, rgb); }
unsigned pt_palette_rgb(int idx) { return (unsigned)idx; }
void pt_request_redraw(void) { PT.dirty = 1; }
void pt_damage(int x, int y, int w, int h) { (void)x; (void)y; (void)w; (void)h; }
void pt_damage_canvas(int x, int y, int w, int h)
{
	(void)x; (void)y; (void)w; (void)h;
}
void pt_damage_present(int x, int y, int w, int h)
{
	(void)x; (void)y; (void)w; (void)h;
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
    shim = os.path.join(tmp_path, "paint_select_shim.c")
    with open(shim, "w", encoding="utf-8") as fh:
        fh.write(SHIM_C)
    lib = os.path.join(tmp_path, "libpaint_select")
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
        os.path.join(SRC, "paint_select.c"),
        os.path.join(SRC, "paint_undo.c"),
        shim,
    ]
    subprocess.run(cmd, check=True, cwd=REPO)
    return ctypes.CDLL(lib)


@pytest.fixture(scope="module")
def _lib(tmp_path_factory):
    return _build(tmp_path_factory.mktemp("paint_select"))


class Pad:
    def __init__(self, lib, w=32, h=24):
        self.lib = lib
        lib.tst_reset.argtypes = [ctypes.c_int, ctypes.c_int]
        lib.tst_get.argtypes = [ctypes.c_int, ctypes.c_int]
        lib.tst_get.restype = ctypes.c_int
        lib.tst_set.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_int]
        lib.tst_bg.argtypes = [ctypes.c_int]
        lib.tst_cursor.argtypes = [ctypes.c_int, ctypes.c_int]
        lib.tst_screen.argtypes = [ctypes.c_int, ctypes.c_int]
        lib.tst_screen.restype = ctypes.c_uint
        lib.tst_clear_screen.argtypes = []
        lib.tst_undo_depth.restype = ctypes.c_int
        lib.pt_select_has.restype = ctypes.c_int
        lib.pt_select_clip_has.restype = ctypes.c_int
        lib.pt_select_hit.argtypes = [ctypes.c_int, ctypes.c_int]
        lib.pt_select_tick.restype = ctypes.c_int
        lib.pt_select_begin.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_int]
        lib.pt_select_motion.argtypes = [ctypes.c_int, ctypes.c_int]
        lib.pt_select_end.argtypes = [ctypes.c_int, ctypes.c_int]
        for name in ("pt_select_cut", "pt_select_copy", "pt_select_paste",
                     "pt_select_clear", "pt_select_all", "pt_select_none",
                     "pt_select_draw", "pt_select_init"):
            getattr(lib, name).argtypes = []
        self.w, self.h = w, h
        self.reset(w, h)

    def reset(self, w=None, h=None):
        self.w = self.w if w is None else w
        self.h = self.h if h is None else h
        self.lib.tst_reset(self.w, self.h)

    def px(self, x, y):
        return self.lib.tst_get(x, y)

    def set(self, x, y, v):
        self.lib.tst_set(x, y, v)

    def bg(self, v):
        self.lib.tst_bg(v)

    def cursor(self, x, y):
        self.lib.tst_cursor(x, y)

    def begin(self, x, y, b=LEFT):
        self.lib.pt_select_begin(x, y, b)

    def motion(self, x, y):
        self.lib.pt_select_motion(x, y)

    def end(self, x, y):
        self.lib.pt_select_end(x, y)

    def rect(self):
        x = ctypes.c_int()
        y = ctypes.c_int()
        w = ctypes.c_int()
        h = ctypes.c_int()
        ok = self.lib.pt_select_rect(ctypes.byref(x), ctypes.byref(y),
                                     ctypes.byref(w), ctypes.byref(h))
        return ok, (x.value, y.value, w.value, h.value)

    def has(self):
        return self.lib.pt_select_has()

    def hit(self, x, y):
        return self.lib.pt_select_hit(x, y)

    def clip_has(self):
        return self.lib.pt_select_clip_has()

    def clip_size(self):
        w = ctypes.c_int()
        h = ctypes.c_int()
        ok = self.lib.pt_select_clip_size(ctypes.byref(w), ctypes.byref(h))
        return ok, (w.value, h.value)

    def undo_depth(self):
        return self.lib.tst_undo_depth()

    def undo(self):
        return self.lib.tst_undo()

    def tick(self):
        return self.lib.pt_select_tick()

    def draw(self):
        self.lib.pt_select_draw()

    def screen(self, x, y):
        return self.lib.tst_screen(x, y)

    def clear_screen(self):
        self.lib.tst_clear_screen()


SCREEN_EMPTY = 0x112233


@pytest.fixture
def sel(_lib):
    return Pad(_lib)


# ---- making a selection ---------------------------------------------------


def test_drag_makes_a_normalized_selection(sel):
    sel.begin(5, 6)
    sel.motion(2, 3)
    sel.end(2, 3)
    assert sel.has() == 1
    ok, (x, y, w, h) = sel.rect()
    assert ok and (x, y, w, h) == (2, 3, 4, 4)
    assert sel.hit(2, 3) == 1
    assert sel.hit(5, 6) == 1
    assert sel.hit(6, 6) == 0


def test_click_without_drag_deselects(sel):
    sel.begin(5, 5)
    sel.end(5, 5)
    assert sel.has() == 0


def test_select_all_covers_the_canvas(sel):
    sel.lib.pt_select_all()
    ok, (x, y, w, h) = sel.rect()
    assert ok and (x, y, w, h) == (0, 0, sel.w, sel.h)


# ---- clipboard ------------------------------------------------------------


def test_copy_paste_round_trips_and_skips_the_background(sel):
    sel.bg(0)
    # A 4x4 red block at (2,2) with one background pixel inside it.
    for y in range(2, 6):
        for x in range(2, 6):
            sel.set(x, y, 9)
    sel.set(3, 3, 0)			# background -> transparent

    sel.begin(1, 1)
    sel.motion(6, 6)
    sel.end(6, 6)
    assert sel.rect()[1] == (1, 1, 6, 6)
    sel.lib.pt_select_copy()
    assert sel.clip_has() == 1
    assert sel.clip_size() == (1, (6, 6))
    assert sel.undo_depth() == 0		# copy edits nothing

    # Paste at (12,12). The transparent pixels leave the target untouched.
    sel.set(12, 12, 6)			# clip (0,0) is transparent
    sel.set(14, 14, 6)			# clip (2,2) is transparent
    sel.cursor(12, 12)
    sel.lib.pt_select_paste()
    assert sel.undo_depth() == 1
    assert sel.px(13, 13) == 9		# clip (1,1) came from (2,2)
    assert sel.px(14, 14) == 6		# transparent kept the target
    assert sel.px(15, 15) == 9
    assert sel.px(12, 12) == 6		# transparent kept the target
    # The selection now frames the pasted image.
    assert sel.rect()[1] == (12, 12, 6, 6)


def test_paste_preserves_target_under_transparent_pixels(sel):
    sel.bg(0)
    sel.set(0, 0, 7)
    sel.set(1, 0, 0)
    sel.begin(0, 0)
    sel.motion(1, 0)
    sel.end(1, 0)
    sel.lib.pt_select_copy()
    sel.set(10, 10, 3)
    sel.set(11, 10, 4)
    sel.cursor(10, 10)
    sel.lib.pt_select_paste()
    assert sel.px(10, 10) == 7		# opaque overwrote
    assert sel.px(11, 10) == 4		# transparent kept the target


def test_cut_clears_to_background_as_one_undo(sel):
    sel.bg(0)
    for y in range(3):
        for x in range(3):
            sel.set(x, y, 5)
    sel.begin(0, 0)
    sel.motion(2, 2)
    sel.end(2, 2)
    sel.lib.pt_select_cut()
    assert sel.undo_depth() == 1
    assert sel.px(1, 1) == 0
    assert sel.clip_has() == 1
    sel.undo()
    assert sel.undo_depth() == 0
    assert sel.px(1, 1) == 5


def test_clear_in_selection_is_one_undo(sel):
    sel.bg(0)
    for y in range(3):
        for x in range(3):
            sel.set(x, y, 5)
    sel.set(9, 9, 5)
    sel.begin(0, 0)
    sel.motion(2, 2)
    sel.end(2, 2)
    sel.lib.pt_select_clear()
    assert sel.undo_depth() == 1
    assert sel.px(1, 1) == 0
    assert sel.px(9, 9) == 5		# outside the selection
    sel.undo()
    assert sel.px(1, 1) == 5


def test_paste_of_an_empty_clipboard_is_a_no_op(sel):
    sel.cursor(4, 4)
    sel.lib.pt_select_paste()
    assert sel.undo_depth() == 0


# ---- moving a selection ---------------------------------------------------


def test_move_lifts_the_selection_and_is_one_undo(sel):
    for y in range(3):
        for x in range(3):
            sel.set(x, y, 4)
    sel.begin(0, 0)
    sel.motion(2, 2)
    sel.end(2, 2)

    sel.begin(1, 1)		# inside the selection: start a move
    sel.motion(11, 11)		# grab point (1,1) -> (11,11): offset (10,10)
    sel.end(11, 11)
    assert sel.undo_depth() == 1
    assert sel.px(1, 1) == 0	# source cleared
    assert sel.px(11, 11) == 4	# moved pixel
    assert sel.rect()[1] == (10, 10, 3, 3)
    sel.undo()
    assert sel.px(1, 1) == 4
    assert sel.px(11, 11) == 0


# ---- marching ants --------------------------------------------------------


def test_ants_are_screen_only_and_tick_animates(sel):
    sel.begin(4, 4)
    sel.motion(10, 10)
    sel.end(10, 10)
    # The canvas holds no marquee pixels.
    for y in range(sel.h):
        for x in range(sel.w):
            assert sel.px(x, y) == 0
    sel.clear_screen()
    assert sel.tick() == 1
    sel.draw()
    # The top border is on screen at the canvas origin: 7 drawn pixels.
    drawn = sum(1 for x in range(4, 11)
                if sel.screen(PT_CANVAS_X + x, PT_CANVAS_Y + 4) != SCREEN_EMPTY)
    assert drawn == 7
    # Every screen pixel of the boundary is black or white only.
    for x in range(4, 11):
        c = sel.screen(PT_CANVAS_X + x, PT_CANVAS_Y + 4)
        assert c in (0xFFFFFF, 0x000000), c
    # Outside the boundary the sentinel is untouched.
    assert sel.screen(PT_CANVAS_X + 100, PT_CANVAS_Y + 100) == SCREEN_EMPTY


def test_no_ants_without_a_selection(sel):
    sel.clear_screen()
    assert sel.tick() == 0
    sel.draw()
    assert sel.screen(PT_CANVAS_X + 4, PT_CANVAS_Y + 4) == SCREEN_EMPTY
