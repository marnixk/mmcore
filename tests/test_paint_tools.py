"""PAINT core drawing tools and primitives (#636).

Host-side pixel tests for ``mmbasic/src/paint_tools.c``. The module is compiled
against the frozen ``paint.h`` together with a small shim (written to a tmp dir
by this test) that stands in for cmd_paint.c's screen helpers, then driven
through the pt_tool_* hooks and inspected on PT.canvas directly.

Covered: pencil / line / rectangle / ellipse / circle / flood fill / eraser /
colour pick / grab with left=BG right=FG, exact-match fill bounds, the
rubber-band preview (committed on release, dropped on cancel), the Shift
square / circle / 45-degree constraints, and the bonus airbrush / spray /
magnify tools (#641). No QEMU required.
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
PT_CELL_W = 32
PT_TOOL_W = 64
PT_CELL_H = (328 - 16) // 9
PT_PAL_Y = 328
PT_CANVAS_Y = 16
PT_TOOL_COUNT = 17

# enum pt_tool (#719 two-column order; #644 adds the selection tool).
(PENCIL, ERASER, LINE, TEXT, RECT, RECT_FILLED, ELLIPSE, ELLIPSE_FILLED,
 CIRCLE, CIRCLE_FILLED, FILL, PICK, AIRBRUSH, SPRAY, GRAB,
 MAGNIFY, SELECT) = range(PT_TOOL_COUNT)

# Bonus-tool radii from mmbasic/src/paint_tools.c.
AIR_R = 6
SPRAY_R = 8

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

/* Frame damage (#700): the tool preview marks the rectangle it reverts. The
 * tests also model the screen: tst_present() blits the damaged box from the
 * canvas into s_screen, exactly like cmd_paint.c's banded present. */
static unsigned char s_screen[PT_MAX_W * PT_MAX_H];
static int s_dv, s_dx0, s_dy0, s_dx1, s_dy1;

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
	memset(s_screen, 0, sizeof s_screen);
	s_dv = 0;
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
int tst_view_x(void) { return PT.view_x; }
int tst_view_y(void) { return PT.view_y; }

void tst_tool_set(int t) { pt_tool_select(t); }
int tst_hit(int sx, int sy, int *tool) { return pt_tools_hit(sx, sy, tool); }
void tst_shift(int on) { pt_tool_modifiers((int)on); }
void tst_draw(void) { pt_tools_draw(); }

void tst_width_set(int i) { PT.width_idx = i; }
int tst_width_hit(int sx, int sy, int *idx) { return pt_width_hit(sx, sy, idx); }
int tst_pen_width(void) { return pt_pen_width(); }

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

/* paint_select.c (#644) is not linked here; the tool dispatcher references
 * its hooks, so stub them out. */
void pt_select_begin(int x, int y, int b) { (void)x; (void)y; (void)b; }
void pt_select_motion(int x, int y) { (void)x; (void)y; }
void pt_select_end(int x, int y) { (void)x; (void)y; }
void pt_select_cancel(void) { }

void pt_damage_canvas(int x, int y, int w, int h)
{
	int x1 = x + w - 1, y1 = y + h - 1;

	if (w < 1 || h < 1)
		return;
	if (!s_dv) { s_dx0 = x; s_dy0 = y; s_dx1 = x1; s_dy1 = y1; s_dv = 1; }
	else {
		if (x < s_dx0) s_dx0 = x;
		if (y < s_dy0) s_dy0 = y;
		if (x1 > s_dx1) s_dx1 = x1;
		if (y1 > s_dy1) s_dy1 = y1;
	}
}

void tst_present(void)
{
	int x, y;

	if (!s_dv)
		return;
	if (s_dx0 < 0) s_dx0 = 0;
	if (s_dy0 < 0) s_dy0 = 0;
	if (s_dx1 > PT.width - 1) s_dx1 = PT.width - 1;
	if (s_dy1 > PT.height - 1) s_dy1 = PT.height - 1;
	for (y = s_dy0; y <= s_dy1; y++)
		for (x = s_dx0; x <= s_dx1; x++)
			s_screen[(size_t)y * PT.width + x] =
				PT.canvas[(size_t)y * PT.width + x];
	s_dv = 0;
}

int tst_screen_matches(void)
{
	return PT.canvas &&
	       memcmp(s_screen, PT.canvas, (size_t)PT.width * PT.height) == 0;
}

void pt_undo_push(void) {}

/* paint_text.c (#642) is not linked in this shim; the tool only forwards the
 * caret click to it. */
void pt_text_begin(int cx, int cy, int button)
{
	(void)cx; (void)cy; (void)button;
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
	pt_damage_canvas(cx, cy, 1, 1);
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
        lib.tst_view_x.restype = ctypes.c_int
        lib.tst_view_y.restype = ctypes.c_int
        lib.tst_tool_set.argtypes = [ctypes.c_int]
        lib.tst_hit.argtypes = [ctypes.c_int, ctypes.c_int,
                                ctypes.POINTER(ctypes.c_int)]
        lib.tst_shift.argtypes = [ctypes.c_int]
        lib.tst_width_set.argtypes = [ctypes.c_int]
        lib.tst_width_hit.argtypes = [ctypes.c_int, ctypes.c_int,
                                      ctypes.POINTER(ctypes.c_int)]
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

    def view(self):
        return self.lib.tst_view_x(), self.lib.tst_view_y()

    def tool(self, t):
        self.lib.tst_tool_set(t)

    def shift(self, on):
        self.lib.tst_shift(1 if on else 0)

    def width(self, i):
        self.lib.tst_width_set(i)

    def pen_width(self):
        self.lib.tst_pen_width.restype = ctypes.c_int
        return self.lib.tst_pen_width()

    def width_hit(self, sx, sy):
        idx = ctypes.c_int(-1)
        ok = self.lib.tst_width_hit(sx, sy, ctypes.byref(idx))
        return ok, idx.value

    def draw(self):
        self.lib.tst_draw()

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

    def present(self):
        self.lib.tst_present()

    def screen_matches(self):
        self.lib.tst_screen_matches.restype = ctypes.c_int
        return bool(self.lib.tst_screen_matches())


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


# ---- airbrush / spray (#641) ----------------------------------------------


def _painted(pt, want, cx, cy, r):
    """Pixels equal to ``want`` inside the disk of radius ``r`` about centre."""
    hits = []
    for dy in range(-r, r + 1):
        for dx in range(-r, r + 1):
            if dx * dx + dy * dy > r * r:
                continue
            if pt.px(cx + dx, cy + dy) == want:
                hits.append((dx, dy))
    return hits


def test_airbrush_dwells_and_stays_inside_its_radius(pt):
    pt.colors(5, 0)
    pt.tool(AIRBRUSH)
    pt.begin(24, 16, LEFT)
    pt.end(24, 16, LEFT)
    assert pt.px(24, 16) == 5  # the soft centre always takes ink
    assert _painted(pt, 5, 24, 16, AIR_R)  # at least one sample landed

    # Nothing escapes the brush radius (soft falloff only thins the edge).
    for y in range(16 - AIR_R - 1, 16 + AIR_R + 2):
        for x in range(24 - AIR_R - 1, 24 + AIR_R + 2):
            if (x - 24) ** 2 + (y - 16) ** 2 > AIR_R * AIR_R:
                assert pt.px(x, y) == 0

    # Dwelling multiplies the deposit: more stay events, more coverage.
    pt.reset()
    pt.colors(5, 0)
    pt.tool(AIRBRUSH)
    pt.begin(24, 16, LEFT)
    first = len(_painted(pt, 5, 24, 16, AIR_R))
    for _ in range(40):
        pt.motion(24, 16, LEFT)
    pt.end(24, 16, LEFT)
    assert len(_painted(pt, 5, 24, 16, AIR_R)) > first


def test_airbrush_right_button_uses_the_background(pt):
    pt.colors(5, 9)
    pt.tool(AIRBRUSH)
    pt.begin(24, 16, RIGHT)
    pt.end(24, 16, RIGHT)
    assert pt.px(24, 16) == 9
    assert not _painted(pt, 5, 24, 16, AIR_R)


def test_spray_scatters_dots_within_radius_both_buttons(pt):
    pt.colors(6, 0)
    pt.tool(SPRAY)
    pt.begin(24, 16, LEFT)
    for _ in range(40):
        pt.motion(24, 16, LEFT)
    pt.end(24, 16, LEFT)
    dots = _painted(pt, 6, 24, 16, SPRAY_R)
    assert dots
    # Scatter stays inside the spray radius.
    for y in range(16 - SPRAY_R - 1, 16 + SPRAY_R + 2):
        for x in range(24 - SPRAY_R - 1, 24 + SPRAY_R + 2):
            if (x - 24) ** 2 + (y - 16) ** 2 > SPRAY_R * SPRAY_R:
                assert pt.px(x, y) == 0

    pt.reset()
    pt.colors(6, 9)
    pt.tool(SPRAY)
    pt.begin(24, 16, RIGHT)
    for _ in range(40):
        pt.motion(24, 16, RIGHT)
    pt.end(24, 16, RIGHT)
    assert _painted(pt, 9, 24, 16, SPRAY_R)


def test_spray_covers_more_ground_than_a_single_airbrush_dab(pt):
    # The spray disk is wider than the airbrush disk by construction.
    assert SPRAY_R > AIR_R


# ---- magnify / tool column ------------------------------------------------


def test_magnify_zooms_in_steps_and_returns_cleanly(pt):
    pt.colors(4, 0)
    pt.set(10, 10, 4)
    pt.tool(MAGNIFY)
    assert pt.zoom() == 1

    pt.begin(24, 16, LEFT)
    pt.end(24, 16, LEFT)
    assert pt.zoom() == 2
    assert pt.view() == (12, 8)  # window centred on the click

    pt.begin(24, 16, LEFT)
    pt.end(24, 16, LEFT)
    assert pt.zoom() == 4

    # Right button steps back out, and 1:1 restores the view origin.
    pt.begin(24, 16, RIGHT)
    pt.end(24, 16, RIGHT)
    assert pt.zoom() == 2
    pt.begin(24, 16, RIGHT)
    pt.end(24, 16, RIGHT)
    assert pt.zoom() == 1
    assert pt.view() == (0, 0)

    assert pt.px(10, 10) == 4  # zooming never touched the canvas


def test_magnify_view_is_clamped_to_the_canvas(pt):
    pt.tool(MAGNIFY)
    pt.begin(pt.w - 1, pt.h - 1, LEFT)
    pt.end(pt.w - 1, pt.h - 1, LEFT)
    assert pt.zoom() == 2
    vx, vy = pt.view()
    assert 0 <= vx <= pt.w - pt.w // 2
    assert 0 <= vy <= pt.h - pt.h // 2
    assert vx + pt.w // 2 <= pt.w
    assert vy + pt.h // 2 <= pt.h


def test_magnify_wraps_to_one_at_the_top(pt):
    pt.tool(MAGNIFY)
    steps = []
    for _ in range(4):
        pt.begin(24, 16, LEFT)
        pt.end(24, 16, LEFT)
        steps.append(pt.zoom())
    assert steps == [2, 4, 8, 1]


def test_tool_column_hit_maps_every_cell(pt):
    for i in range(PT_TOOL_COUNT):
        col = i % 2
        row = i // 2
        sx = col * PT_CELL_W + PT_CELL_W // 2
        sy = PT_CANVAS_Y + row * PT_CELL_H + PT_CELL_H // 2
        ok, tool = pt.hit(sx, sy)
        assert ok == 1 and tool == i
    # The palette strip and the menu bar are not tool hits.
    assert pt.hit(PT_CELL_W // 2, PT_CANVAS_Y - 1)[0] == 0
    assert pt.hit(PT_CELL_W // 2, PT_PAL_Y)[0] == 0
    assert pt.hit(PT_TOOL_W, PT_CANVAS_Y + PT_CELL_H // 2)[0] == 0


def test_tool_column_draws_every_icon_including_the_bonus_tools(pt):
    # Exercises draw_tool_icon for all 16 tools (the filled shape variants and
    # the bonus airbrush / spray / magnify art included).
    for tool in (RECT_FILLED, ELLIPSE_FILLED, CIRCLE_FILLED, AIRBRUSH, SPRAY,
                 MAGNIFY):
        pt.tool(tool)
        pt.draw()
    pt.draw()


def _lit_count(pt):
    return sum(pt.px(x, y) == 15
               for x in range(pt.w) for y in range(pt.h))


def test_filled_rectangle_fills_but_the_outline_is_hollow(pt):
    pt.tool(RECT)
    pt.begin(6, 5, LEFT)
    pt.motion(26, 21, LEFT)
    pt.end(26, 21, LEFT)
    assert pt.px(16, 13) == 0        # outline leaves the centre empty
    assert pt.px(6, 13) == 15        # left edge drawn

    pt.reset()
    pt.tool(RECT_FILLED)
    pt.begin(6, 5, LEFT)
    pt.motion(26, 21, LEFT)
    pt.end(26, 21, LEFT)
    assert pt.px(16, 13) == 15       # interior filled
    assert pt.px(6, 5) == 15


def test_filled_ellipse_and_circle_are_solid(pt):
    pt.tool(ELLIPSE_FILLED)
    pt.begin(6, 6, LEFT)
    pt.motion(26, 20, LEFT)
    pt.end(26, 20, LEFT)
    assert pt.px(16, 13) == 15       # ellipse centre filled

    pt.reset()
    pt.tool(ELLIPSE)
    pt.begin(6, 6, LEFT)
    pt.motion(26, 20, LEFT)
    pt.end(26, 20, LEFT)
    assert pt.px(16, 13) == 0        # the outline sibling is hollow

    pt.reset()
    pt.tool(CIRCLE_FILLED)
    pt.begin(24, 16, LEFT)           # circle centre
    pt.motion(31, 16, LEFT)          # r = 7
    pt.end(31, 16, LEFT)
    assert pt.px(24, 16) == 15
    assert pt.px(24, 20) == 15       # inside the disc


def test_filled_ellipse_is_solid_on_every_scanline(pt):
    """A wide ellipse fills as spans, not concentric outlines (no dotted gaps)."""
    pt.colors(15, 0)
    pt.tool(ELLIPSE_FILLED)
    pt.begin(4, 8, LEFT)
    pt.motion(60, 28, LEFT)
    pt.end(60, 28, LEFT)

    rows = 0
    for y in range(pt.h):
        xs = [x for x in range(pt.w) if pt.px(x, y) == 15]
        if not xs:
            continue
        rows += 1
        assert xs == list(range(xs[0], xs[-1] + 1)), (y, xs)
    assert rows > 4


def test_filled_circle_is_solid_on_every_scanline(pt):
    pt.colors(15, 0)
    pt.tool(CIRCLE_FILLED)
    pt.begin(24, 16, LEFT)
    pt.motion(31, 16, LEFT)     # r = 7
    pt.end(31, 16, LEFT)
    for y in range(pt.h):
        xs = [x for x in range(pt.w) if pt.px(x, y) == 15]
        if xs:
            assert xs == list(range(xs[0], xs[-1] + 1)), (y, xs)


def _shrink_keeps_screen_in_sync(pt, tool, a, b, c):
    """Rubber-band a shape at a thick pen, then shrink it.

    The tool preview reverts from PT.scratch and marks only the box it thinks
    it covered. The pen overhangs that box, so without padding the present
    leaves the old, larger outline on screen: PT.canvas (correct) and the
    simulated screen diverge.
    """
    pt.reset(200, 120)
    pt.colors(15, 0)
    pt.tool(tool)
    pt.width(4)                 # 6 px pen
    pt.begin(*a, LEFT)
    pt.present()
    pt.motion(*b, LEFT)
    pt.present()
    pt.motion(*c, LEFT)         # shrink the rubber band
    pt.present()
    pt.end(*c, LEFT)
    pt.present()
    assert pt.screen_matches()


def test_shrinking_a_thick_rectangle_represents_the_old_overhang(pt):
    _shrink_keeps_screen_in_sync(pt, RECT, (20, 20), (180, 100), (60, 50))


def test_shrinking_a_thick_line_represents_the_old_overhang(pt):
    _shrink_keeps_screen_in_sync(pt, LINE, (20, 20), (180, 100), (60, 50))


def test_shrinking_a_thick_ellipse_represents_the_old_overhang(pt):
    _shrink_keeps_screen_in_sync(pt, ELLIPSE, (20, 20), (180, 100), (60, 50))


def test_pen_width_thickens_a_dot(pt):
    assert pt.pen_width() == 1
    pt.tool(PENCIL)
    pt.begin(10, 10, LEFT)
    pt.end(10, 10, LEFT)
    assert _lit_count(pt) == 1       # 1x1 dot

    pt.reset()
    pt.tool(PENCIL)
    pt.width(4)                      # 6 px
    assert pt.pen_width() == 6
    pt.begin(10, 10, LEFT)
    pt.end(10, 10, LEFT)
    assert _lit_count(pt) == 36      # 6x6 dot


def test_width_selector_maps_five_cells(pt):
    # PT_WB_X=32, PT_WB_W=60, five 12px cells in the bottom bar.
    for i in range(5):
        ok, idx = pt.width_hit(32 + i * 12 + 6, 328 + 16)
        assert ok == 1 and idx == i
    assert pt.width_hit(31, 344)[0] == 0
    assert pt.width_hit(92, 344)[0] == 0
    assert pt.width_hit(50, 327)[0] == 0
