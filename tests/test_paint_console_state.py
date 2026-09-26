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
CMD_PAINT_C = os.path.join(SRC, "cmd_paint.c")

PT_W, PT_H = 640, 360
PT_TOOL_GRAB = 14
PT_TOOL_PENCIL = 0
PT_TOOL_LINE = 2
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

static unsigned g_screen[PT_W * PT_H];
#define TST_EMPTY 0x112233u

static void tst_fill_px(int x, int y, int w, int h, unsigned rgb)
{
	int i, j;
	for (j = 0; j < h; j++)
		for (i = 0; i < w; i++)
			if (x + i >= 0 && y + j >= 0 && x + i < PT_W && y + j < PT_H)
				g_screen[(y + j) * PT_W + x + i] = rgb;
}

static mmb_platform g_plat;
static int g_bound;

static void bind_plat(void)
{
	if (g_bound)
		return;
	g_plat.alloc = tst_alloc;
	g_plat.free = tst_free;
	g_plat.tui_fill_px = tst_fill_px;
	s_mmb.plat = &g_plat;
	g_bound = 1;
}

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

unsigned pt_palette_rgb(int idx) { return (unsigned)idx; }

/* paint_tools.c hands a TEXT click to paint_text.c, which is not linked. */
void pt_text_begin(int cx, int cy, int button)
{
	(void)cx; (void)cy; (void)button;
}

/* ---- scripted global pointer (the device is machine-wide, #811) -------- */
static mmb_mouse_state s_mouse;

int mmb_mouse_read(mmb_mouse_state *out)
{
	if (out)
		*out = s_mouse;
	return 1;
}

void tst_mouse(int present, int x, int y, int buttons)
{
	s_mouse.present = present;
	s_mouse.x = x;
	s_mouse.y = y;
	s_mouse.buttons = buttons;
}

void tst_active(int on) { PT.active = on; }
int tst_mouse_down(void) { return PT.mouse_down; }

int tst_canvas_nonzero(void)
{
	int i, n = 0;

	for (i = 0; i < PT.width * PT.height; i++)
		if (PT.canvas[i])
			n++;
	return n;
}

/* Console-indexed views: after a switch PT names the destination, so inspect
 * the screen we left by index (#811). */
int tst_mouse_down_for(int n) { return pt_console_state[n].mouse_down; }

int tst_canvas_nonzero_for(int n)
{
	pt_state *st = &pt_console_state[n];
	int i, c = 0;

	for (i = 0; i < st->width * st->height; i++)
		if (st->canvas[i])
			c++;
	return c;
}

/* Mirror console_do_switch()'s paint hooks: cancel the departing screen's
 * stroke (and pending keys), then hand the screen to the destination console
 * and seed its pointer-edge tracker (#811, #814, #815). */
void tst_switch(int n)
{
	if (pt_console_state[g_console].active)
		mmb_paint_console_deactivated(g_console);
	g_console = n;
	if (pt_console_state[n].active)
		mmb_paint_console_activated(n);
}

/* ---- stubs cmd_paint.c references (only its command/teardown paths) ----- */
void mmb_error(const char *m) { (void)m; }
void mmb_syntax(void) {}
void mmb_skip_sp(void) {}
mmb_val mmb_expr(void) { mmb_val v; memset(&v, 0, sizeof v); v.type = T_STR; return v; }
int64_t mmb_as_int(mmb_val v) { (void)v; return 0; }
void mmb_gfx_set_mode(int m, int b) { (void)m; (void)b; }
void mmb_gfx_reset_console(int w) { (void)w; }
int mmb_vfs_resolve(const char *p, char *o, int n) { (void)p; (void)o; (void)n; return 0; }
void mmb_console_write(const char *s) { (void)s; }
void mmb_hw_cursor(int s) { (void)s; }
const char *mmb_prompt(void) { return ""; }
int pt_menus_confirm_quit(void) { return 0; }
void tui_begin(void) {}
void tui_end(void) {}
void tui_invalidate(void) {}
void tui_fill(int x, int y, int w, int h, int ch, int fg, int bg)
{
	(void)x; (void)y; (void)w; (void)h; (void)ch; (void)fg; (void)bg;
}
int tui_cols(void) { return 80; }
int tui_rows(void) { return 25; }
void tui_pad(int x, int y, const char *s, int w, int fg, int bg)
{
	(void)x; (void)y; (void)s; (void)w; (void)fg; (void)bg;
}
void tui_flush_no_present(void) {}
void tui_flush(void) {}
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
        CMD_PAINT_C,
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
    lib.tst_mouse.argtypes = [
        ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int
    ]
    lib.tst_active.argtypes = [ctypes.c_int]
    lib.tst_mouse_down.restype = ctypes.c_int
    lib.tst_canvas_nonzero.restype = ctypes.c_int
    lib.tst_mouse_down_for.argtypes = [ctypes.c_int]
    lib.tst_mouse_down_for.restype = ctypes.c_int
    lib.tst_canvas_nonzero_for.argtypes = [ctypes.c_int]
    lib.tst_canvas_nonzero_for.restype = ctypes.c_int
    lib.tst_switch.argtypes = [ctypes.c_int]
    lib.mmb_paint_poll.argtypes = []
    lib.mmb_paint_console_deactivated.argtypes = [ctypes.c_int]
    lib.mmb_paint_console_activated.argtypes = [ctypes.c_int]
    lib.mmb_paint_key.argtypes = [ctypes.c_char]
    lib.mmb_paint_key.restype = ctypes.c_char_p
    lib.mmb_in_paint.restype = ctypes.c_int


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


def test_paint_stroke_does_not_commit_across_console_switch(lib):
    """#811: a button held on PAINT's screen must not commit a stroke when
    focus returns from another console.

    The pointer is a machine-wide device whose poll only runs on the active
    screen. Pressing on console 0 starts a line preview; switching away while
    held must cancel it, so releasing on console 1 and switching back cannot
    commit a ghost line from the stale press point."""
    _bind(lib)

    # Console 0 runs PAINT with the line tool; the canvas is 64x48 at the
    # usual (64, 16) screen origin, so screen = canvas + (64, 16).
    lib.tst_set_console(0)
    lib.tst_reset(64, 48)
    lib.tst_active(1)
    lib.tst_tool(PT_TOOL_LINE)

    # Press on canvas (10,10) and drag to (36,34) while still held.
    lib.tst_mouse(1, 64 + 10, 16 + 10, LEFT)
    lib.mmb_paint_poll()
    assert lib.tst_mouse_down() == 1

    lib.tst_mouse(1, 64 + 36, 16 + 34, LEFT)
    lib.mmb_paint_poll()
    assert lib.tst_canvas_nonzero() > 0	# live line preview

    # Focus leaves console 0 with the button still held (as console_do_switch
    # does): the in-progress stroke is dropped and its preview reverted.
    lib.tst_switch(1)
    assert lib.tst_mouse_down_for(0) == 0
    assert lib.tst_canvas_nonzero_for(0) == 0

    # The release happens on console 1.
    lib.tst_mouse(1, 64 + 36, 16 + 34, 0)

    # Switching back must not fire a tool action from the stale point.
    lib.tst_switch(0)
    lib.mmb_paint_poll()
    assert lib.tst_mouse_down() == 0
    assert lib.tst_canvas_nonzero() == 0


def test_held_button_does_not_begin_stroke_on_switch_in(lib):
    """#814: a button already held as a PAINT console becomes active is not a
    fresh press on that screen, so it must not begin a stroke.

    The pointer is machine-wide: holding the button on console 0 and switching
    to console 1 (also PAINT) must not call pt_tool_begin() on console 1 from
    the carried-over hold. Only an up->down edge seen here is a press."""
    _bind(lib)

    # Console 1 is a background PAINT session that never saw a press.
    lib.tst_set_console(1)
    lib.tst_reset(64, 48)
    lib.tst_active(1)

    # Console 0 runs PAINT and observes a real press on its canvas.
    lib.tst_set_console(0)
    lib.tst_reset(64, 48)
    lib.tst_active(1)
    lib.tst_mouse(1, 64 + 20, 16 + 20, LEFT)
    lib.mmb_paint_poll()
    assert lib.tst_mouse_down() == 1		# stroke live on console 0

    # Focus moves to console 1 with the button still held.
    lib.tst_switch(1)
    assert lib.tst_console() == 1
    assert lib.tst_mouse_down_for(1) == 0	# dest had no press of its own

    lib.mmb_paint_poll()
    assert lib.tst_mouse_down() == 0		# held button is not an edge
    assert lib.tst_canvas_nonzero() == 0

    # Releasing and pressing again on this screen is a fresh edge.
    lib.tst_mouse(1, 64 + 20, 16 + 20, 0)
    lib.mmb_paint_poll()
    lib.tst_mouse(1, 64 + 20, 16 + 20, LEFT)
    lib.mmb_paint_poll()
    assert lib.tst_mouse_down() == 1


def test_buffered_escape_is_dropped_across_console_switch(lib):
    """#815: a lone Esc buffered on one screen must not resolve after a switch.

    PAINT holds a lone Esc for PT_ESC_IDLE_MS so it can tell it from the lead
    byte of a CSI/SS3 sequence. The clock is per console; time that passes on a
    background screen must not cash the stale Esc in as a real one that closes
    a menu or quits."""
    _bind(lib)

    lib.tst_set_console(0)
    lib.tst_reset(64, 48)
    lib.tst_active(1)

    lib.mmb_paint_key(b"\x1b")		# lone Esc buffered on console 0
    lib.tst_switch(1)			# leave before the idle window elapses
    lib.tst_switch(0)			# and come back

    lib.mmb_paint_poll()
    assert lib.mmb_in_paint() == 1	# the stale Esc must not quit PAINT


def test_armed_alt_prefix_is_dropped_across_console_switch(lib):
    """#815: an Alt prefix armed on one screen must not survive a switch, or an
    ``x`` typed on return would quit PAINT as Alt+X."""
    _bind(lib)

    lib.tst_set_console(0)
    lib.tst_reset(64, 48)
    lib.tst_active(1)

    lib.mmb_paint_key(b"\x01")		# Alt prefix armed on console 0
    lib.tst_switch(1)
    lib.tst_switch(0)
    lib.mmb_paint_key(b"x")		# would be Alt+X if the prefix survived
    assert lib.mmb_in_paint() == 1
