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

/* paint_select.c (#644) is not linked here; the tool dispatcher references
 * its hooks, so stub them out. */
void pt_select_begin(int x, int y, int b) { (void)x; (void)y; (void)b; }
void pt_select_motion(int x, int y) { (void)x; (void)y; }
void pt_select_end(int x, int y) { (void)x; (void)y; }
void pt_select_cancel(void) { }

/* paint_tools.c links here too; its preview marks the reverted rectangle. */
void pt_damage_canvas(int x, int y, int w, int h)
{
	(void)x; (void)y; (void)w; (void)h;
}

/* ---- fake A:/fonts/gfx for the font-picker tests (#643) ---------------- */

static const char *TST_JSON =
	"{\"source\":\"tst.png\",\"charset\":\"AB\",\"offsetX\":0,"
	"\"offsetY\":0,\"charWidth\":8,\"charHeight\":8,\"charsPerRow\":16,"
	"\"bgColour\":0}";

static int s_fonts_off;

void tst_fonts(int off) { s_fonts_off = off ? 1 : 0; }

int mmb_vfs_list_entries(const char *spec, mmb_dirent *out, int max,
			 int *trunc)
{
	(void)spec;
	(void)out;
	if (trunc)
		*trunc = 0;
	if (s_fonts_off)
		return 0;
	if (max < 1)
		return -1;
	memset(out, 0, sizeof(*out));
	strcpy(out[0].name, "tst.json");
	out[0].is_dir = 0;
	out[0].size = (int)strlen(TST_JSON);
	return 1;
}

int mmb_vfs_size(const char *path)
{
	if (path && strstr(path, ".json"))
		return (int)strlen(TST_JSON);
	if (path && strstr(path, ".png"))
		return 1;
	return -1;
}

int mmb_vfs_read(const char *path, void *data, unsigned maxn, unsigned *n)
{
	if (path && strstr(path, ".json"))
	{
		unsigned L = (unsigned)strlen(TST_JSON);
		if (L > maxn)
			L = maxn;
		memcpy(data, TST_JSON, L);
		if (n)
			*n = L;
		return 0;
	}
	if (path && strstr(path, ".png"))
	{
		if (n)
			*n = 0;
		((unsigned char *)data)[0] = 0;
		return 0;
	}
	return -1;
}

/* 128x8 sheet: glyph 'A' is a solid block at x 0..7; glyph 'B' is one red
 * pixel. 0xAARRGGBB, alpha 0 is transparent. */
int mmb_png_decode_rgba(const unsigned char *file, unsigned n,
			uint32_t **out, int *w, int *h)
{
	uint32_t *p = malloc(128u * 8u * sizeof(uint32_t));
	int x, y;

	(void)file;
	(void)n;
	if (!p)
		return -1;
	memset(p, 0, 128u * 8u * sizeof(uint32_t));
	for (y = 0; y < 8; y++)
		for (x = 0; x < 8; x++)
			p[y * 128 + x] = 0xFFFFFFFFu;
	p[8] = 0xFFFF0000u;
	*out = p;
	*w = 128;
	*h = 8;
	return 0;
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
        lib.tst_fonts.argtypes = [ctypes.c_int]
        lib.pt_text_font_count.restype = ctypes.c_int
        lib.pt_text_font_name.argtypes = [ctypes.c_int]
        lib.pt_text_font_name.restype = ctypes.c_char_p
        lib.pt_text_font_load.argtypes = [ctypes.c_int]
        lib.pt_text_font_current.restype = ctypes.c_char_p
        lib.pt_text_font_w.restype = ctypes.c_int
        lib.pt_text_font_h.restype = ctypes.c_int
        lib.pt_text_font_picker_active.restype = ctypes.c_int
        self.w, self.h = w, h
        self.reset(w, h)

    def reset(self, w=None, h=None):
        self.w = self.w if w is None else w
        self.h = self.h if h is None else h
        self.lib.tst_fonts(0)
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

    def fonts_off(self, off):
        self.lib.tst_fonts(1 if off else 0)

    def font_count(self):
        return self.lib.pt_text_font_count()

    def font_name(self, i):
        return self.lib.pt_text_font_name(i).decode()

    def font_load(self, i):
        return self.lib.pt_text_font_load(i)

    def font_builtin(self):
        self.lib.pt_text_font_builtin()

    def font_current(self):
        return self.lib.pt_text_font_current().decode()

    def font_w(self):
        return self.lib.pt_text_font_w()

    def font_h(self):
        return self.lib.pt_text_font_h()

    def picker_open(self):
        self.lib.pt_text_font_picker_open()

    def picker_active(self):
        return self.lib.pt_text_font_picker_active()


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


# ---- font picker from A:/fonts/gfx (#643) ---------------------------------


def test_font_catalog_lists_the_folder(pt):
    assert pt.font_count() == 1
    assert pt.font_name(0) == "tst"
    assert pt.font_current() == ""


def test_load_and_render_with_the_chosen_bitmap_font(pt):
    assert pt.font_load(0) == 0
    assert pt.font_current() == "tst"
    assert (pt.font_w(), pt.font_h()) == (8, 8)

    pt.colors(5, 0)
    pt.begin(2, 2, LEFT)
    pt.type("AB")
    pt.key(ENTER)

    # 'A' is a solid 8x8 block in the chosen foreground index.
    for y in range(8):
        for x in range(8):
            assert pt.px(2 + x, 2 + y) == 5, (x, y)
    # 'B' is a single red pixel at its top-left (the rest is background).
    assert pt.px(10, 2) == 5
    assert pt.px(11, 2) == 0
    assert pt.px(10, 3) == 0


def test_picker_open_navigate_and_choose(pt):
    pt.begin(2, 2, LEFT)
    pt.picker_open()
    assert pt.picker_active() == 1
    # The first entry is the built-in; 'n' moves to the first folder font.
    assert pt.key(ord("n")) == 1
    assert pt.key(ENTER) == 1
    assert pt.picker_active() == 0
    assert pt.font_current() == "tst"


def test_builtin_fallback_when_no_fonts_are_available(pt):
    pt.fonts_off(True)
    assert pt.font_count() == 0
    assert pt.font_load(0) == -1
    assert pt.font_current() == ""
    assert (pt.font_w(), pt.font_h()) == (8, 8)

    pt.colors(7, 0)
    pt.begin(2, 2, LEFT)
    pt.type("H")
    pt.key(ENTER)
    assert_glyph(pt, GLYPH_H, 2, 2, 7)


def test_switching_back_to_builtin(pt):
    assert pt.font_load(0) == 0
    assert pt.font_current() == "tst"
    pt.font_builtin()
    assert pt.font_current() == ""
    pt.colors(4, 0)
    pt.begin(2, 2, LEFT)
    pt.type("H")
    pt.key(ENTER)
    assert_glyph(pt, GLYPH_H, 2, 2, 4)
