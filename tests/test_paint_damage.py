"""PAINT frame damage + banded present (#700).

``mmbasic/src/cmd_paint.c`` now tracks one screen-space damage box per frame:
edits union their rectangle into it, ``pt_redraw()`` recomposites only that
box (canvas region blit, chrome only when requested) and ``pt_present()`` DMAs
only the affected rows. This host test compiles the real ``cmd_paint.c`` with
a driver that records every ``tui_fill_px`` / ``tui_present`` call, then
checks the union band, the banded present and that a small canvas edit never
touches the tool column, palette or menu bar.
"""
import os
import shutil
import subprocess

import pytest

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(REPO, "mmbasic", "src")
CMD_PAINT_C = os.path.join(SRC, "cmd_paint.c")

DRIVER = r"""
#include "paint.h"
#include "tui.h"

#include <stdio.h>
#include <string.h>

static mmb s_mmb;
mmb *g_cur = &s_mmb;
int g_console;

/* ---- present / fill recording ---- */
static int p_y0 = -1, p_y1 = -1, p_count;
static int fill_minx = 1 << 30, fill_miny = 1 << 30;
static int fill_maxx = -1, fill_maxy = -1, fill_count;
static int saw_tool_or_pal, saw_menu_row;

static void rec_fill(int x, int y, int w, int h, unsigned rgb)
{
	(void)rgb;
	if (w < 1 || h < 1)
		return;
	if (x < fill_minx) fill_minx = x;
	if (y < fill_miny) fill_miny = y;
	if (x + w - 1 > fill_maxx) fill_maxx = x + w - 1;
	if (y + h - 1 > fill_maxy) fill_maxy = y + h - 1;
	fill_count++;
	if (x < PT_TOOL_W && y + h > PT_CANVAS_Y && y < PT_PAL_Y)
		saw_tool_or_pal = 1;
	if (y < PT_MENU_H)
		saw_menu_row = 1;
}
static void rec_present(int y0, int y1) { p_y0 = y0; p_y1 = y1; p_count++; }

static void reset_rec(void)
{
	p_y0 = p_y1 = -1; p_count = 0;
	fill_minx = fill_miny = 1 << 30;
	fill_maxx = fill_maxy = -1; fill_count = 0;
	saw_tool_or_pal = saw_menu_row = 0;
}

static const mmb_platform g_plat = {
	.tui_fill_px = rec_fill,
	.tui_present = rec_present,
};

/* ---- stubs cmd_paint.c references (only mmb_cmd_paint does) ---- */
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
unsigned mmb_now_ms(void) { return 0; }
int pt_menus_confirm_quit(void) { return 0; }

/* ---- scripted pointer + menu poll forwarding (#708) ---- */
static mmb_mouse_state s_mouse;
static int menu_active;
static int menu_press, menu_release, menu_motion;
static int menu_prev_down;
static int tool_begins;

int mmb_mouse_read(mmb_mouse_state *o) { if (o) *o = s_mouse; return 1; }
int pt_menus_mouse(int sx, int sy, int button, int down)
{
	(void)sx; (void)sy; (void)button;
	if (down)
		menu_press++;
	else if (menu_prev_down)
		menu_release++;
	else
		menu_motion++;
	menu_prev_down = down;
	return menu_active;
}
int pt_menus_active(void) { return menu_active; }
void pt_tool_begin(int cx, int cy, int button)
{ (void)cx; (void)cy; (void)button; tool_begins++; }

/* ---- modal overlays that force the UI arrow (#791) ---- */
static int file_active, font_picker_active;
static int cursor_overlay = -1;

int pt_file_dialog_active(void) { return file_active; }
int pt_text_font_picker_active(void) { return font_picker_active; }

/* Strong override of the weak cmd_paint.c stub: record the overlay flag the
 * redraw passes to the cursor instead of stamping pixels. */
void pt_cursor_draw(int sx, int sy, int tool, int active, int overlay)
{
	(void)sx; (void)sy; (void)tool; (void)active;
	cursor_overlay = overlay;
}

/* tui shims used by cmd_paint.c and the weak module stubs */
void tui_begin(void) {}
void tui_end(void) {}
void tui_invalidate(void) {}
void tui_fill(int x, int y, int w, int h, int ch, int fg, int bg)
{ (void)x;(void)y;(void)w;(void)h;(void)ch;(void)fg;(void)bg; }
int tui_cols(void) { return 80; }
int tui_rows(void) { return 25; }
void tui_pad(int x, int y, const char *s, int w, int fg, int bg)
{ (void)x;(void)y;(void)s;(void)w;(void)fg;(void)bg; }
void tui_flush_no_present(void) {}
void tui_flush(void) {}

static unsigned char g_canvas[PT_CANVAS_W * PT_CANVAS_H];
static int fails;
static void check(int ok, const char *name)
{
	printf("CHECK %s %s\n", name, ok ? "PASS" : "FAIL");
	if (!ok) fails++;
}

int main(void)
{
	s_mmb.plat = &g_plat;
	PT.active = 1;
	PT.width = PT_CANVAS_W;
	PT.height = PT_CANVAS_H;
	PT.canvas = g_canvas;
	memset(g_canvas, 3, sizeof g_canvas);
	PT.cursor_sx = -1000;	/* keep the weak cursor off-screen */
	PT.cursor_sy = -1000;

	/* 1. pt_damage unions rectangles into one band, clamped to the screen. */
	reset_rec();
	pt_damage(100, 200, 50, 30);
	pt_damage(40, 10, 10, 10);	/* union: y 10..229 */
	pt_present();
	check(p_y0 == 10 && p_y1 == 229, "damage_union_band");
	pt_redraw();			/* consume the damage */

	/* 2. pt_present with no pending damage presents the full frame. */
	reset_rec();
	pt_present();
	check(p_y0 == 0 && p_y1 == PT_H - 1, "empty_present_full");

	/* 3. Full redraw composes chrome and canvas and presents full height. */
	reset_rec();
	PT.full_redraw = 1;
	pt_redraw();
	check(saw_tool_or_pal, "full_draws_chrome");
	check(p_y0 == 0 && p_y1 == PT_H - 1, "full_present_full");

	/* 4. A small canvas edit repaints only that region and presents only its
	 *    rows; chrome is untouched. */
	reset_rec();
	pt_canvas_set(10, 5, 1);
	pt_canvas_set(20, 15, 1);
	pt_redraw();
	check(!saw_tool_or_pal, "partial_keeps_chrome");
	check(!saw_menu_row, "partial_keeps_menu");
	check(p_y0 == PT_CANVAS_Y + 5 && p_y1 == PT_CANVAS_Y + 15, "partial_band");
	check(fill_minx >= PT_CANVAS_X, "partial_x_in_canvas");

	/* 5. A full-chrome request still happens through pt_request_redraw. */
	reset_rec();
	pt_request_redraw();
	pt_redraw();
	check(saw_tool_or_pal, "request_redraw_chrome");

	/* 6. Damage is clamped: a rect off the bottom/right never presents past
	 *    the screen. */
	reset_rec();
	pt_damage(PT_W - 2, PT_H - 2, 100, 100);
	pt_present();
	check(p_y0 == PT_H - 2 && p_y1 == PT_H - 1, "damage_clamped");
	pt_redraw();

	/* 7. Present-only damage (the cursor) must present its band but never
	 *    recomposite canvas or chrome. */
	reset_rec();
	pt_damage_present(100, 50, 40, 40);
	pt_redraw();
	check(fill_count == 0, "present_only_no_recompose");
	check(p_y0 == 50 && p_y1 == 89, "present_only_band");

	/* 8. #708: the poll forwards pointer motion (no button) and button-up to
	 *    the menu module, and an open menu consumes the pointer so no canvas
	 *    tool starts. */
	menu_active = 1;
	s_mouse.present = 1;
	s_mouse.x = 100;
	s_mouse.y = 200;
	s_mouse.buttons = 0;
	tool_begins = 0;
	mmb_paint_poll();		/* motion, no button */
	check(menu_motion > 0, "poll_forwards_motion");
	check(tool_begins == 0, "menu_motion_no_tool");

	s_mouse.buttons = 1;
	mmb_paint_poll();		/* press */
	check(menu_press > 0, "poll_forwards_press");
	check(PT.mouse_down == 0, "menu_press_no_canvas_drag");

	/* The press can close the menu (activate); the release must still be
	 * forwarded or the held-button state would stay stuck. */
	menu_active = 0;
	s_mouse.buttons = 0;
	mmb_paint_poll();		/* release with no menu open */
	check(menu_release > 0, "poll_forwards_release_after_close");
	menu_motion = 0;
	mmb_paint_poll();		/* later no-button motion is hover */
	check(menu_motion > 0, "poll_motion_after_release");

	/* 9. #791: the cursor is forced to the UI arrow whenever any modal
	 *    overlay that covers the canvas owns input, and stays a tool
	 *    otherwise. The cursor stub records the overlay flag per redraw. */
	file_active = font_picker_active = menu_active = 0;
	cursor_overlay = -1;
	PT.full_redraw = 1;
	pt_redraw();
	check(cursor_overlay == 0, "cursor_no_overlay_tool");

	file_active = 1;
	cursor_overlay = -1;
	PT.full_redraw = 1;
	pt_redraw();
	check(cursor_overlay == 1, "cursor_file_picker_arrow");

	file_active = 0;
	font_picker_active = 1;
	cursor_overlay = -1;
	PT.full_redraw = 1;
	pt_redraw();
	check(cursor_overlay == 1, "cursor_font_picker_arrow");

	font_picker_active = 0;
	menu_active = 1;
	cursor_overlay = -1;
	PT.full_redraw = 1;
	pt_redraw();
	check(cursor_overlay == 1, "cursor_menu_arrow");
	menu_active = 0;

	printf("FAILURES %d\n", fails);
	return fails ? 1 : 0;
}
"""


@pytest.fixture(scope="module")
def damage_driver(tmp_path_factory):
    if shutil.which("cc") is None:
        pytest.skip("needs a host C toolchain (cc)")
    tmp = tmp_path_factory.mktemp("paint_damage")
    driver = tmp / "driver.c"
    driver.write_text(DRIVER)
    exe = tmp / "driver"
    subprocess.run(
        [
            "cc", "-std=c11", "-O0", "-Wall", "-Wextra", "-Werror",
            "-DMMB_PLATFORM_POSIX",
            "-I", SRC,
            "-I", os.path.join(REPO, "mmbasic", "include"),
            "-I", os.path.join(REPO, "mmbasic", "third_party"),
            "-I", os.path.join(REPO, "console"),
            "-I", os.path.join(REPO, "native"),
            "-o", str(exe), str(driver), CMD_PAINT_C,
        ],
        check=True,
        cwd=REPO,
    )
    return exe


@pytest.fixture(scope="module")
def checks(damage_driver):
    out = subprocess.run(
        [str(damage_driver)], check=True, capture_output=True, text=True
    )
    parsed = {}
    for line in out.stdout.splitlines():
        parts = line.split()
        if parts and parts[0] == "CHECK":
            parsed[parts[1]] = parts[2] == "PASS"
    assert parsed, out.stdout + out.stderr
    return parsed


def test_damage_band_tracks_the_changed_rows(checks):
    assert checks.get("damage_union_band") is True
    assert checks.get("damage_clamped") is True
    assert checks.get("partial_band") is True


def test_full_frame_presents_everything(checks):
    assert checks.get("full_draws_chrome") is True
    assert checks.get("full_present_full") is True
    assert checks.get("empty_present_full") is True
    assert checks.get("request_redraw_chrome") is True


def test_partial_edit_leaves_chrome_alone(checks):
    assert checks.get("partial_keeps_chrome") is True
    assert checks.get("partial_keeps_menu") is True
    assert checks.get("partial_x_in_canvas") is True


def test_present_only_damage_does_not_recompose(checks):
    """Cursor motion marks a present band, not a canvas recomposite (#700/#701)."""
    assert checks.get("present_only_no_recompose") is True
    assert checks.get("present_only_band") is True


def test_poll_forwards_hover_and_button_up(checks):
    """#708: mmb_paint_poll forwards no-button motion and button-up to the menu
    module, so hover follows the pointer and the held state cannot stick."""
    for name in (
        "poll_forwards_motion",
        "menu_motion_no_tool",
        "poll_forwards_press",
        "menu_press_no_canvas_drag",
        "poll_forwards_release_after_close",
        "poll_motion_after_release",
    ):
        assert checks.get(name) is True, name


def test_overlays_force_ui_arrow_cursor(checks):
    """#791: the redraw passes the overlay flag to pt_cursor_draw() while the
    file picker, the text font picker or a menu owns input, so no tool sprite
    is stamped over them; with no overlay the tool cursor is used."""
    for name in (
        "cursor_no_overlay_tool",
        "cursor_file_picker_arrow",
        "cursor_font_picker_arrow",
        "cursor_menu_arrow",
    ):
        assert checks.get(name) is True, name
