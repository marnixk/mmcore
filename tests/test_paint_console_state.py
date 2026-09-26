"""PAINT per-console module state (#766).

Each virtual console runs an independent PAINT instance: the undo/redo
history, the selection + clipboard, the grab brush and the sprite-restore
cursor background are keyed by ``g_console`` and must not leak between
consoles. This host test compiles the real ``paint_undo.c``, ``paint_select.c``,
``paint_tools.c`` and ``paint_cursors.c`` against one shim, drives two
consoles by moving ``g_console`` and checks that state set on one is invisible
on the other.
"""
import ctypes
import os
import shutil
import subprocess
import sys

import pytest

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(REPO, "mmbasic", "src")

PT_W, PT_H = 640, 360
PT_TOOL_GRAB = 14
PT_TOOL_PENCIL = 0
LEFT = 1

SHIM_C = r"""
#include "paint.h"
#include "paint_cursor_art.h"

#include <stdlib.h>
#include <string.h>

pt_state pt_console_state[MMB_MAX_CONSOLES];
int g_console;

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

void tst_set_console(int n) { g_console = n; }
int tst_console(void) { return g_console; }

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
	pt_tools_init();
	pt_cursor_init();
}

int tst_w(void) { return PT.width; }
int tst_h(void) { return PT.height; }
void tst_set(int x, int y, int v) { pt_canvas_set(x, y, v); }
int tst_get(int x, int y) { return pt_canvas_get(x, y); }
void tst_bg(int v) { PT.bg = v; }
void tst_cursor(int x, int y) { PT.cursor_x = x; PT.cursor_y = y; }
void tst_tool(int t) { PT.tool = t; }

void tst_edit(int idx, int val)
{
	pt_undo_push();
	PT.canvas[idx] = (unsigned char)val;
}

int tst_undo_depth(void) { return PT.undo_depth; }
int tst_redo_depth(void) { return PT.redo_depth; }
void tst_undo(void) { pt_undo(); }
void tst_redo(void) { pt_redo(); }

void tst_screen_set(int x, int y, unsigned v)
{
	if (x >= 0 && y >= 0 && x < PT_W && y < PT_H)
		g_screen[y * PT_W + x] = v;
}

unsigned tst_screen(int x, int y)
{
	if (x < 0 || y < 0 || x >= PT_W || y >= PT_H)
		return 0;
	return g_screen[y * PT_W + x];
}

unsigned tui_get_px(int x, int y) { return tst_screen(x, y); }

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

int pt_screen_to_canvas(int sx, int sy, int *cx, int *cy)
{
	(void)sx; (void)sy; (void)cx; (void)cy;
	return 0;
}

/* paint_tools.c hands a TEXT click to paint_text.c, which is not linked. */
void pt_text_begin(int cx, int cy, int button)
{
	(void)cx; (void)cy; (void)button;
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
    shim = os.path.join(tmp_path, "paint_console_shim.c")
    with open(shim, "w", encoding="utf-8") as fh:
        fh.write(SHIM_C)
    lib = os.path.join(tmp_path, "libpaint_console")
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
        os.path.join(SRC, "paint_undo.c"),
        os.path.join(SRC, "paint_select.c"),
        os.path.join(SRC, "paint_tools.c"),
        os.path.join(SRC, "paint_cursors.c"),
        os.path.join(SRC, "paint_cursor_art.c"),
        shim,
    ]
    subprocess.run(cmd, check=True, cwd=REPO)
    return ctypes.CDLL(lib)


@pytest.fixture(scope="module")
def lib(tmp_path_factory):
    return _build(tmp_path_factory.mktemp("paint_console"))


def _bind(lib):
    lib.tst_set_console.argtypes = [ctypes.c_int]
    lib.tst_console.restype = ctypes.c_int
    lib.tst_reset.argtypes = [ctypes.c_int, ctypes.c_int]
    lib.tst_set.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_int]
    lib.tst_get.argtypes = [ctypes.c_int, ctypes.c_int]
    lib.tst_get.restype = ctypes.c_int
    lib.tst_bg.argtypes = [ctypes.c_int]
    lib.tst_cursor.argtypes = [ctypes.c_int, ctypes.c_int]
    lib.tst_tool.argtypes = [ctypes.c_int]
    lib.tst_edit.argtypes = [ctypes.c_int, ctypes.c_int]
    lib.tst_undo_depth.restype = ctypes.c_int
    lib.tst_redo_depth.restype = ctypes.c_int
    lib.tst_screen_set.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_uint]
    lib.tst_screen.argtypes = [ctypes.c_int, ctypes.c_int]
    lib.tst_screen.restype = ctypes.c_uint
    lib.pt_select_has.restype = ctypes.c_int
    lib.pt_select_clip_has.restype = ctypes.c_int
    lib.pt_select_begin.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_int]
    lib.pt_select_motion.argtypes = [ctypes.c_int, ctypes.c_int]
    lib.pt_select_end.argtypes = [ctypes.c_int, ctypes.c_int]
    lib.pt_select_copy.argtypes = []
    lib.pt_tool_begin.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_int]
    lib.pt_tool_motion.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_int]
    lib.pt_tool_end.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_int]
    lib.pt_cursor_draw.argtypes = [
        ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int
    ]
    lib.pt_cursor_restore.argtypes = []


def test_undo_history_is_per_console(lib):
    """Edits and undo on one console never appear on another (#766)."""
    _bind(lib)

    lib.tst_set_console(0)
    lib.tst_reset(16, 16)
    lib.tst_edit(0, 7)
    lib.tst_edit(1, 9)
    assert lib.tst_undo_depth() == 2

    # A fresh console has no history of its own.
    lib.tst_set_console(1)
    lib.tst_reset(16, 16)
    assert lib.tst_undo_depth() == 0
    assert lib.tst_redo_depth() == 0
    lib.tst_undo()			# "Nothing to undo"
    assert lib.tst_undo_depth() == 0
    assert lib.tst_get(0, 0) == 0

    # Console 0's stack is untouched by the other console's undo.
    lib.tst_set_console(0)
    assert lib.tst_undo_depth() == 2
    lib.tst_undo()
    assert lib.tst_undo_depth() == 1
    assert lib.tst_redo_depth() == 1
    assert lib.tst_get(1, 0) == 0		# restore of the pre-edit canvas

    # Console 1 still has nothing, even though console 0 now has a redo.
    lib.tst_set_console(1)
    assert lib.tst_undo_depth() == 0
    assert lib.tst_redo_depth() == 0


def test_selection_and_clipboard_are_per_console(lib):
    """A selection and its clipboard do not leak across consoles (#766)."""
    _bind(lib)

    lib.tst_set_console(0)
    lib.tst_reset(32, 24)
    lib.tst_set(1, 1, 5)
    lib.pt_select_begin(0, 0, LEFT)
    lib.pt_select_motion(3, 3)
    lib.pt_select_end(3, 3)
    lib.pt_select_copy()
    assert lib.pt_select_has() == 1
    assert lib.pt_select_clip_has() == 1

    lib.tst_set_console(1)
    lib.tst_reset(32, 24)
    assert lib.pt_select_has() == 0
    assert lib.pt_select_clip_has() == 0

    # Back on console 0 both are still there.
    lib.tst_set_console(0)
    assert lib.pt_select_has() == 1
    assert lib.pt_select_clip_has() == 1


def test_grab_brush_is_per_console(lib):
    """A brush captured on one console is not stamped by another (#766)."""
    _bind(lib)

    lib.tst_set_console(0)
    lib.tst_reset(32, 24)
    for y in range(4):
        for x in range(4):
            lib.tst_set(x, y, 9)
    lib.tst_tool(PT_TOOL_GRAB)
    lib.pt_tool_begin(1, 1, LEFT)
    lib.pt_tool_motion(4, 4, LEFT)		# drag creates the custom brush
    lib.pt_tool_end(4, 4, LEFT)

    # A fresh console has no brush: a click neither stamps nor records undo.
    lib.tst_set_console(1)
    lib.tst_reset(32, 24)
    lib.tst_tool(PT_TOOL_GRAB)
    lib.pt_tool_begin(10, 10, LEFT)
    lib.pt_tool_end(10, 10, LEFT)
    assert lib.tst_undo_depth() == 0
    assert lib.tst_get(10, 10) == 0
    assert lib.tst_get(13, 13) == 0


def test_cursor_background_is_per_console(lib):
    """The saved sprite background is not restored onto another console."""
    _bind(lib)

    # Fresh state for both consoles before the screen is used.
    lib.tst_set_console(1)
    lib.tst_reset(32, 24)
    lib.tst_set_console(0)
    lib.tst_reset(32, 24)

    lib.tst_screen_set(100, 100, 0x123456)
    lib.pt_cursor_draw(100, 100, PT_TOOL_PENCIL, 0, 0)

    # Poke the pixel after the capture: a restore of console 0's buffer would
    # put 0x123456 back.
    lib.tst_screen_set(100, 100, 0xABCDEF)

    lib.tst_set_console(1)
    lib.pt_cursor_restore()		# console 1 has no saved background
    assert lib.tst_screen(100, 100) == 0xABCDEF

    # Console 0 still holds its own saved background.
    lib.tst_set_console(0)
    lib.tst_screen_set(100, 100, 0xABCDEF)
    lib.pt_cursor_restore()
    assert lib.tst_screen(100, 100) == 0x123456
