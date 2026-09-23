"""PAINT core drawing tools and primitives (#636).

Host-side pixel tests for ``mmbasic/src/paint_tools.c``. The module is compiled
against the frozen ``paint.h`` together with a small shim (written to a tmp dir
by this test) that stands in for cmd_paint.c's screen helpers, then driven
through the pt_tool_* hooks and inspected on PT.canvas directly.

Covered: pencil / line / rectangle / ellipse / circle / flood fill / eraser /
colour pick / grab with left=BG right=FG, exact-match fill bounds, the
rubber-band preview (committed on release, dropped on cancel) and the Shift
square / circle / 45-degree constraints. No QEMU required.
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
PT_MENU_H = 16
PT_TOOL_W = 32
PT_PAL_Y = 328
PT_CANVAS_Y = 16
PT_TOOL_COUNT = 13

# enum pt_tool.
PENCIL, LINE, RECT, ELLIPSE, CIRCLE, FILL, ERASER, PICK, GRAB, MAGNIFY = range(10)

LEFT, RIGHT = 1, 2

SHIM_C = r"""
/* Test shim: the pieces cmd_paint.c normally supplies. */
#include "paint.h"

#include <stdlib.h>
#include <string.h>

pt_state PT;

static mmb s_mmb;
mmb *g_cur = &s_mmb;

void pt_tool_modifiers(int shift);

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
	PT.tool = PT_TOOL_PENCIL;
	PT.menu = PT_MENU_NONE;
	PT.canvas = (unsigned char *)tst_alloc((unsigned)w * (unsigned)h);
	PT.scratch = (unsigned char *)tst_alloc((unsigned)w * (unsigned)h);
	memset(PT.canvas, 0, (unsigned)w * (unsigned)h);
	memset(PT.scratch, 0, (unsigned)w * (unsigned)h);
	pt_tools_init();
}

unsigned char *tst_canvas(void) { return PT.canvas; }
int tst_w(void) { return PT.width; }
int tst_h(void) { return PT.height; }

void tst_set(int x, int y, int v) { pt_canvas_set(x, y, v); }
int tst_get(int x, int y) { return pt_canvas_get(x, y); }

void tst_colors(int fg, int bg) { PT.fg = fg; PT.bg = bg; }
int tst_fg(void) { return PT.fg; }
int tst_bg(void) { return PT.bg; }
int tst_zoom(void) { return PT.zoom; }

void tst_tool_set(int t) { pt_tool_select(t); }
int tst_hit(int sx, int sy, int *tool) { return pt_tools_hit(sx, sy, tool); }
void tst_shift(int on) { pt_tool_modifiers((int)on); }

void tst_begin(int x, int y, int b) { pt_tool_begin(x, y, b); }
void tst_motion(int x, int y, int b) { pt_tool_motion(x, y, b); }
void tst_end(int x, int y, int b) { pt_tool_end(x, y, b); }
void tst_cancel(void) { pt_tool_cancel(); }

/* cmd_paint.c helpers, stubbed for the host. */
void pt_fill_rect(int x, int y, int w, int h, unsigned rgb)
{
	(void)x; (void)y; (void)w; (void)h; (void)rgb;
}

void pt_plot(int x, int y, unsigned rgb) { (void)x; (void)y; (void)rgb; }

unsigned pt_palette_rgb(int idx) { (void)idx; return 0; }

void pt_request_redraw(void) { PT.dirty = 1; }

void pt_undo_push(void) {}

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
    shim = os.path.join(tmp_path, "paint_tools_shim.c")
    with open(shim, "w", encoding="utf-8") as fh:
        fh.write(SHIM_C)
    lib = os.path.join(tmp_path, "libpaint_tools")
    cmd = [
        "cc", "-std=c11", "-O0", "-g", "-Wall", "-Wextra", "-fPIC",
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
    cmd += ["-o", lib, os.path.join(SRC, "paint_tools.c"), shim]
    subprocess.run(cmd, check=True, cwd=REPO)
    return ctypes.CDLL(lib)


@pytest.fixture(scope="module")
def _lib(tmp_path_factory):
    return _build(tmp_path_factory.mktemp("paint_tools"))


class Pad:
    """A (re)settable PT canvas driven through the pt_tool_* hooks."""

    def __init__(self, lib, w=48, h=32):
        self.lib = lib
        lib.tst_reset.argtypes = [ctypes.c_int, ctypes.c_int]
        lib.tst_get.argtypes = [ctypes.c_int, ctypes.c_int]
        lib.tst_get.restype = ctypes.c_int
        lib.tst_set.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_int]
        lib.tst_colors.argtypes = [ctypes.c_int, ctypes.c_int]
        lib.tst_fg.restype = ctypes.c_int
        lib.tst_bg.restype = ctypes.c_int
        lib.tst_zoom.restype = ctypes.c_int
        lib.tst_tool_set.argtypes = [ctypes.c_int]
        lib.tst_hit.argtypes = [ctypes.c_int, ctypes.c_int,
                                ctypes.POINTER(ctypes.c_int)]
        lib.tst_shift.argtypes = [ctypes.c_int]
        for fn in ("tst_begin", "tst_motion", "tst_end"):
            getattr(lib, fn).argtypes = [ctypes.c_int, ctypes.c_int,
                                         ctypes.c_int]
        self.reset(w, h)

    def reset(self, w=None, h=None):
        self.w = self.w if w is None else w
        self.h = self.h if h is None else h
        self.lib.tst_reset(self.w, self.h)

    def px(self, x, y):
        return self.lib.tst_get(x, y)

    def set(self, x, y, v):
        self.lib.tst_set(x, y, v)

    def colors(self, fg, bg):
        self.lib.tst_colors(fg, bg)

    def fg(self):
        return self.lib.tst_fg()

    def bg(self):
        return self.lib.tst_bg()

    def zoom(self):
        return self.lib.tst_zoom()

    def tool(self, t):
        self.lib.tst_tool_set(t)

    def shift(self, on):
        self.lib.tst_shift(1 if on else 0)

    def hit(self, sx, sy):
        got = ctypes.c_int(-1)
        ok = self.lib.tst_hit(sx, sy, ctypes.byref(got))
        return ok, got.value

    def begin(self, x, y, b=LEFT):
        self.lib.tst_begin(x, y, b)

    def motion(self, x, y, b=LEFT):
        self.lib.tst_motion(x, y, b)

    def end(self, x, y, b=LEFT):
        self.lib.tst_end(x, y, b)

    def cancel(self):
        self.lib.tst_cancel()


@pytest.fixture
def pt(_lib):
    return Pad(_lib)


# ---- pencil / line --------------------------------------------------------


def test_pencil_draws_fg_on_left_and_bg_on_right(pt):
    pt.colors(5, 9)
    pt.tool(PENCIL)
    pt.begin(10, 10, LEFT)
    pt.motion(15, 10, LEFT)
    pt.end(15, 10, LEFT)
    assert [pt.px(x, 10) for x in range(10, 16)] == [5] * 6

    pt.reset()
    pt.colors(5, 9)
    pt.tool(PENCIL)
    pt.begin(10, 10, RIGHT)
    pt.motion(10, 15, RIGHT)
    pt.end(10, 15, RIGHT)
    assert [pt.px(10, y) for y in range(10, 16)] == [9] * 6


def test_line_endpoints_and_diagonal(pt):
    pt.colors(7, 3)
    pt.tool(LINE)
    pt.begin(2, 2, LEFT)
    pt.end(20, 2, LEFT)
    assert pt.px(2, 2) == 7
    assert pt.px(11, 2) == 7
    assert pt.px(20, 2) == 7

    pt.reset()
    pt.colors(7, 3)
    pt.tool(LINE)
    pt.begin(0, 0, LEFT)
    pt.motion(9, 9, LEFT)
    pt.end(9, 9, LEFT)
    for i in range(10):
        assert pt.px(i, i) == 7


# ---- rectangle / ellipse / circle ----------------------------------------


def test_rectangle_is_a_hollow_outline_both_buttons(pt):
    pt.colors(6, 12)
    pt.tool(RECT)
    pt.begin(3, 3, LEFT)
    pt.motion(20, 15, LEFT)
    pt.end(20, 15, LEFT)
    for corner in ((3, 3), (20, 3), (3, 15), (20, 15)):
        assert pt.px(*corner) == 6
    assert pt.px(11, 3) == 6
    assert pt.px(3, 9) == 6
    assert pt.px(11, 9) == 0

    pt.reset()
    pt.colors(6, 12)
    pt.tool(RECT)
    pt.begin(3, 3, RIGHT)
    pt.end(20, 15, RIGHT)
    assert pt.px(3, 3) == 12
    assert pt.px(20, 15) == 12
    assert pt.px(11, 9) == 0


def test_circle_uses_the_longest_axis_as_radius(pt):
    pt.colors(4, 0)
    pt.tool(CIRCLE)
    # Drag mostly horizontally: radius is max(|dx|, |dy|) = 8.
    pt.begin(20, 16, LEFT)
    pt.motion(28, 20, LEFT)
    pt.end(28, 20, LEFT)
    assert pt.px(28, 16) == 4
    assert pt.px(12, 16) == 4
    assert pt.px(20, 8) == 4
    assert pt.px(20, 24) == 4


def test_ellipse_is_inscribed_in_the_drag_box(pt):
    pt.colors(4, 0)
    pt.tool(ELLIPSE)
    pt.begin(4, 6, LEFT)
    pt.motion(24, 22, LEFT)
    pt.end(24, 22, LEFT)
    assert pt.px(14, 6) == 4
    assert pt.px(14, 22) == 4
    assert pt.px(4, 14) == 4
    assert pt.px(24, 14) == 4
    assert pt.px(14, 14) == 0  # hollow


# ---- flood fill -----------------------------------------------------------


def test_flood_fill_is_exact_match_and_stops_at_boundaries(pt):
    # A 7-coloured box with a hollow 0 interior.
    for x in range(2, 18):
        pt.set(x, 2, 7)
        pt.set(x, 10, 7)
    for y in range(2, 11):
        pt.set(2, y, 7)
        pt.set(17, y, 7)

    pt.colors(3, 0)
    pt.tool(FILL)
    pt.begin(5, 5, LEFT)
    assert pt.px(5, 5) == 3
    assert pt.px(16, 9) == 3
    assert pt.px(10, 6) == 3
    assert pt.px(2, 2) == 7  # boundary survives
    assert pt.px(17, 10) == 7
    assert pt.px(1, 1) == 0  # outside the box is untouched
    assert pt.px(20, 12) == 0


def test_flood_fill_covers_the_whole_exterior_without_overrun(pt):
    for x in range(2, 8):
        pt.set(x, 2, 7)
    for y in range(2, 8):
        pt.set(2, y, 7)
    pt.colors(5, 0)
    pt.tool(FILL)
    pt.begin(pt.w - 1, pt.h - 1, LEFT)  # corner seed fills everything else
    assert pt.px(0, 0) == 5
    assert pt.px(pt.w - 1, 0) == 5
    assert pt.px(0, pt.h - 1) == 5
    assert pt.px(5, 5) == 5
    assert pt.px(2, 2) == 7  # the L-shaped boundary is untouched


# ---- eraser / pick --------------------------------------------------------


def test_eraser_wipes_to_the_background_both_buttons(pt):
    pt.colors(6, 0)
    for x in range(5, 15):
        pt.set(x, 5, 6)
    pt.tool(ERASER)
    pt.begin(5, 5, LEFT)
    pt.motion(14, 5, LEFT)
    pt.end(14, 5, LEFT)
    assert all(pt.px(x, 5) == 0 for x in range(5, 15))

    pt.reset()
    pt.colors(6, 2)
    pt.set(5, 5, 6)
    pt.tool(ERASER)
    pt.begin(5, 5, RIGHT)
    pt.end(5, 5, RIGHT)
    assert pt.px(5, 5) == 2


def test_pick_loads_fg_on_left_and_bg_on_right(pt):
    pt.colors(0, 0)
    pt.set(5, 5, 11)
    pt.set(6, 6, 12)
    pt.tool(PICK)
    pt.begin(5, 5, LEFT)
    pt.end(5, 5, LEFT)
    assert pt.fg() == 11
    pt.begin(6, 6, RIGHT)
    pt.end(6, 6, RIGHT)
    assert pt.bg() == 12


# ---- grab / custom brush --------------------------------------------------


def test_grab_captures_and_stamps_with_bg_transparent(pt):
    pt.colors(5, 0)
    # 3x3 block with a background hole in the middle.
    block = {
        (0, 0): 5, (1, 0): 5, (2, 0): 5,
        (0, 1): 5, (1, 1): 0, (2, 1): 5,
        (0, 2): 5, (1, 2): 5, (2, 2): 5,
    }
    for (dx, dy), v in block.items():
        pt.set(2 + dx, 2 + dy, v)

    pt.tool(GRAB)
    pt.begin(2, 2, LEFT)
    pt.motion(4, 4, LEFT)
    pt.end(4, 4, LEFT)

    # A marker under the transparent hole must survive the stamp.
    pt.set(11, 11, 8)
    pt.begin(10, 10, LEFT)
    pt.end(10, 10, LEFT)

    assert pt.px(10, 10) == 5
    assert pt.px(12, 10) == 5
    assert pt.px(10, 12) == 5
    assert pt.px(12, 12) == 5
    assert pt.px(11, 11) == 8  # brush BG pixel was transparent
    assert pt.px(11, 10) == 5  # opaque neighbour did stamp


# ---- rubber-band preview --------------------------------------------------


def test_preview_is_live_and_committed_only_on_release(pt):
    pt.colors(4, 0)
    pt.tool(LINE)
    pt.begin(1, 1, LEFT)
    pt.motion(10, 1, LEFT)
    assert all(pt.px(x, 1) == 4 for x in range(1, 11))  # preview visible

    pt.motion(1, 10, LEFT)
    assert pt.px(10, 1) == 0  # previous preview was restored
    assert all(pt.px(1, y) == 4 for y in range(1, 11))

    pt.end(1, 10, LEFT)
    assert all(pt.px(1, y) == 4 for y in range(1, 11))  # committed


def test_cancel_drops_the_preview(pt):
    pt.colors(4, 0)
    pt.tool(RECT)
    pt.begin(5, 5, LEFT)
    pt.motion(20, 18, LEFT)
    assert pt.px(20, 5) == 4  # preview edge
    pt.cancel()
    assert all(pt.px(x, 5) == 0 for x in range(5, 21))
    assert all(pt.px(5, y) == 0 for y in range(5, 19))


# ---- Shift constraints ----------------------------------------------------


def test_shift_constrains_lines_to_axes_and_45_degrees(pt):
    pt.colors(4, 0)
    pt.shift(True)
    pt.tool(LINE)
    # Near-horizontal drag snaps to horizontal.
    pt.begin(5, 5, LEFT)
    pt.motion(20, 8, LEFT)
    pt.end(20, 8, LEFT)
    assert pt.px(20, 5) == 4
    assert pt.px(20, 8) == 0

    pt.reset()
    pt.colors(4, 0)
    pt.shift(True)
    pt.tool(LINE)
    # A ~45-degree drag stays diagonal.
    pt.begin(5, 5, LEFT)
    pt.motion(18, 17, LEFT)
    pt.end(18, 17, LEFT)
    assert pt.px(17, 17) == 4
    assert pt.px(5, 5) == 4


def test_shift_makes_rectangles_square(pt):
    pt.colors(4, 0)
    pt.shift(True)
    pt.tool(RECT)
    pt.begin(5, 5, LEFT)
    pt.motion(20, 10, LEFT)
    pt.end(20, 10, LEFT)
    assert pt.px(5, 20) == 4
    assert pt.px(20, 5) == 4
    assert pt.px(20, 20) == 4
    assert pt.px(10, 10) == 0  # interior stays hollow


def test_shift_makes_ellipses_circular(pt):
    pt.colors(4, 0)
    pt.shift(True)
    pt.tool(ELLIPSE)
    pt.begin(5, 5, LEFT)
    pt.motion(25, 15, LEFT)
    pt.end(25, 15, LEFT)
    assert pt.px(15, 5) == 4
    assert pt.px(5, 15) == 4
    assert pt.px(25, 15) == 4
    assert pt.px(15, 25) == 4


# ---- magnify / tool column ------------------------------------------------


def test_magnify_toggles_zoom(pt):
    pt.tool(MAGNIFY)
    assert pt.zoom() == 1
    pt.begin(10, 10, LEFT)
    pt.end(10, 10, LEFT)
    assert pt.zoom() == 2
    pt.begin(10, 10, LEFT)
    pt.end(10, 10, LEFT)
    assert pt.zoom() == 1


def test_tool_column_hit_maps_every_cell(pt):
    cell = (PT_PAL_Y - PT_CANVAS_Y) // PT_TOOL_COUNT
    for i in range(PT_TOOL_COUNT):
        sy = PT_CANVAS_Y + i * cell + cell // 2
        ok, tool = pt.hit(PT_TOOL_W // 2, sy)
        assert ok == 1 and tool == i
    # The palette strip and the menu bar are not tool hits.
    assert pt.hit(PT_TOOL_W // 2, PT_CANVAS_Y - 1)[0] == 0
    assert pt.hit(PT_TOOL_W // 2, PT_PAL_Y)[0] == 0
    assert pt.hit(PT_TOOL_W, PT_CANVAS_Y + cell // 2)[0] == 0
