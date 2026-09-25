"""PAINT menu bar, dropdowns and confirmation dialogs (#638).

The menu module is compiled for the host with the real frozen ``paint.h`` and a
driver that stubs the drawing and file/undo hooks. The driver opens each menu,
walks the dropdowns and exercises every confirmation dialog, then reports one
``CHECK <name> PASS|FAIL`` line per assertion. This is the "native test opens
each menu and exercises a confirm dialog" acceptance check from the ticket; the
QEMU smoke test cannot inject clicks yet (#633).
"""
import os
import shutil
import subprocess

import pytest

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(REPO, "mmbasic", "src")
MENUS_C = os.path.join(SRC, "paint_menus.c")

DRIVER = r"""
#include "paint.h"

#include <stdio.h>
#include <string.h>

pt_state pt_console_state[MMB_MAX_CONSOLES];
int g_console;

/* ---- action/state recording ---- */
static int f_new, f_open, f_save, f_save_as;
static int u_undo, u_redo, u_clear;
static int quit_leave;
static int redraws;

void pt_request_redraw(void) { redraws++; }

/* ---- frame damage recording (#702) ---- */
static int dmg_valid, dmg_x0, dmg_y0, dmg_x1, dmg_y1;
void pt_damage(int x, int y, int w, int h)
{
	int x1 = x + w - 1, y1 = y + h - 1;

	if (w < 1 || h < 1)
		return;
	if (!dmg_valid)
	{
		dmg_x0 = x;
		dmg_y0 = y;
		dmg_x1 = x1;
		dmg_y1 = y1;
		dmg_valid = 1;
	}
	else
	{
		if (x < dmg_x0)
			dmg_x0 = x;
		if (y < dmg_y0)
			dmg_y0 = y;
		if (x1 > dmg_x1)
			dmg_x1 = x1;
		if (y1 > dmg_y1)
			dmg_y1 = y1;
	}
}
static void dmg_reset(void) { dmg_valid = 0; }

void pt_file_new(void) { f_new++; }
void pt_file_open(void) { f_open++; }
void pt_file_save(void) { f_save++; }
void pt_file_save_as(void) { f_save_as++; }
void pt_undo(void) { u_undo++; }
void pt_redo(void) { u_redo++; }
void pt_undo_clear(void) { u_clear++; }

/* #644 selection / clipboard actions. */
static int s_cut, s_copy, s_paste, s_clear_sel, s_select_all;
void pt_select_cut(void) { s_cut++; }
void pt_select_copy(void) { s_copy++; }
void pt_select_paste(void) { s_paste++; }
void pt_select_clear(void) { s_clear_sel++; }
void pt_select_all(void) { s_select_all++; }

/* ---- canvas stub (Edit > Clear wipes it via pt_canvas_set) ---- */
static unsigned char g_canvas[4 * 4];
void pt_canvas_set(int cx, int cy, int idx)
{
	if (cx < 0 || cy < 0 || cx >= PT.width || cy >= PT.height)
		return;
	g_canvas[(size_t)cy * PT.width + cx] = (unsigned char)idx;
}
static int canvas_wiped(void)
{
	int i;

	for (i = 0; i < (int)sizeof g_canvas; i++)
		if (g_canvas[i] != 0)
			return 0;
	return 1;
}
/* Quit menu / keyboard gate now tears down through the lifecycle directly. */
void pt_paint_leave(void) { quit_leave++; }

/* ---- minimal offscreen TUI ---- */
#define COLS 80
#define ROWS 25
static char cells[ROWS][COLS + 1];

static void scr_clear(void)
{
	int y;

	for (y = 0; y < ROWS; y++)
	{
		memset(cells[y], ' ', COLS);
		cells[y][COLS] = 0;
	}
}
static void put_cell(int x, int y, int ch)
{
	if (x >= 0 && x < COLS && y >= 0 && y < ROWS)
		cells[y][x] = (char)ch;
}
int tui_cols(void) { return COLS; }
int tui_rows(void) { return ROWS; }
void tui_put(int x, int y, int ch, int fg, int bg)
{
	(void)fg;
	(void)bg;
	put_cell(x, y, ch);
}
void tui_puts(int x, int y, const char *s, int fg, int bg)
{
	(void)fg;
	(void)bg;
	while (s && *s)
		put_cell(x++, y, *s++);
}
void tui_pad(int x, int y, const char *s, int w, int fg, int bg)
{
	int i = 0;
	(void)fg;
	(void)bg;
	while (s && s[i] && i < w)
	{
		put_cell(x + i, y, s[i]);
		i++;
	}
	while (i < w)
		put_cell(x + i++, y, ' ');
}
void tui_dialog_geom(int want_w, int want_h, int *x, int *y, int *w, int *h)
{
	if (want_w < 1)
		want_w = 1;
	if (want_h < 1)
		want_h = 1;
	*w = want_w;
	*h = want_h;
	*x = (COLS - want_w) / 2;
	*y = (ROWS - want_h) / 2;
}
void tui_dialog_panel(int x, int y, int w, int h, const char *title, int bf,
		      int bb, int brf, int brb, int tf, int tb)
{
	(void)bf;
	(void)bb;
	(void)brf;
	(void)brb;
	(void)tf;
	(void)tb;
	(void)w;
	(void)h;
	tui_puts(x + 1, y, title, 0, 0);
}
void tui_fill(int x, int y, int w, int h, int ch, int fg, int bg)
{
	int i, j;

	(void)fg;
	(void)bg;
	for (j = 0; j < h; j++)
		for (i = 0; i < w; i++)
			put_cell(x + i, y + j, ch);
}
void tui_accept_rect(int x, int y, int w, int h)
{
	(void)x;
	(void)y;
	(void)w;
	(void)h;
}
void tui_invalidate_rect(int x, int y, int w, int h)
{
	(void)x;
	(void)y;
	(void)w;
	(void)h;
}

static int screen_has(const char *needle)
{
	int y;

	for (y = 0; y < ROWS; y++)
		if (strstr(cells[y], needle))
			return 1;
	return 0;
}

/* #756: the row whose text contains ``label`` must end (before the right
 * border) with the accelerator ``key``. */
static int hint_at(const char *label, char key)
{
	int y, x, found = 0;

	for (y = 0; y < ROWS; y++) {
		for (x = 0; x + (int)strlen(label) <= COLS; x++)
			if (strncmp(&cells[y][x], label, strlen(label)) == 0) {
				found = 1;
				break;
			}
		if (found) {
			int end = COLS - 1;

			while (end >= 0 && cells[y][end] == ' ')
				end--;
			return end >= 0 && cells[y][end] == key;
		}
	}
	return 0;
}

/* ---- checks ---- */
static int fails;
static void check(int ok, const char *name)
{
	printf("CHECK %s %s\n", name, ok ? "PASS" : "FAIL");
	if (!ok)
		fails++;
}

/* Menu bar title centres, from the module's own layout. */
static const int kFile = 1 * 8 + 4;
static const int kEdit = 7 * 8 + 4;
static const int kHelp = 13 * 8 + 4;

static void press(int x, int y)
{
	pt_menus_mouse(x, y, PT_BTN_LEFT, 1);
}
static void release(int x, int y)
{
	pt_menus_mouse(x, y, PT_BTN_LEFT, 0);
}
static void click(int x, int y)
{
	press(x, y);
	release(x, y);
}
/* #708: motion with no button, and a held-button move. */
static void move_to(int x, int y)
{
	pt_menus_mouse(x, y, 0, 0);
}
static void drag_to(int x, int y)
{
	pt_menus_mouse(x, y, PT_BTN_LEFT, 1);
}

int main(void)
{
	scr_clear();
	memset(&PT, 0, sizeof PT);
	PT.active = 1;
	PT.menu = PT_MENU_NONE;
	PT.canvas = g_canvas;
	PT.width = 4;
	PT.height = 4;
	memset(g_canvas, 9, sizeof g_canvas);
	pt_menus_init();

	check(!pt_menus_active(), "idle_not_active");
	check(!PT.dialog, "idle_no_dialog");

	/* ---- open File, see its items ---- */
	pt_menus_draw();
	check(screen_has("File"), "bar_draws_file");
	check(screen_has("Edit"), "bar_draws_edit");
	check(screen_has("Help"), "bar_draws_help");

	click(kFile, 8);
	check(PT.menu == PT_MENU_FILE, "click_file_opens");
	check(pt_menus_active(), "file_active");
	scr_clear();
	pt_menus_draw();
	check(screen_has("New"), "file_new_visible");
	check(screen_has("Open"), "file_open_visible");
	check(screen_has("Save as"), "file_saveas_visible");
	check(screen_has("Quit"), "file_quit_visible");

	/* ---- hover switch to Edit / Help ---- */
	press(kEdit, 8);
	check(PT.menu == PT_MENU_EDIT, "hover_switch_edit");
	release(kEdit, 8);
	scr_clear();
	pt_menus_draw();
	check(screen_has("Undo"), "edit_undo_visible");
	check(screen_has("Redo"), "edit_redo_visible");
	check(screen_has("Clear"), "edit_clear_visible");

	press(kHelp, 8);
	check(PT.menu == PT_MENU_HELP, "hover_switch_help");
	release(kHelp, 8);

	/* ---- click away and Esc close ---- */
	press(200, 100);
	check(PT.menu == PT_MENU_NONE, "click_away_closes");
	release(200, 100);

	click(kFile, 8);
	check(PT.menu == PT_MENU_FILE, "reopen_file");
	check(pt_menus_key(27) == 1, "esc_consumed");
	check(PT.menu == PT_MENU_NONE, "esc_closes");

	/* ---- File > New with a clean canvas: no dialog ---- */
	f_new = 0;
	PT.undo_depth = 0;
	click(kFile, 8);
	click(kFile, 20);	/* first dropdown row: New */
	check(!pt_menus_active(), "new_clean_no_dialog");
	check(f_new == 1, "new_clean_dispatch");

	/* ---- File > New with unsaved changes: confirm, No keeps it ---- */
	f_new = 0;
	PT.undo_depth = 3;
	click(kFile, 8);
	click(kFile, 20);
	check(PT.dialog && pt_menus_active(), "new_dirty_dialog");
	check(f_new == 0, "new_dirty_not_run");
	check(pt_menus_key('n') == 1, "new_no_consumed");
	check(f_new == 0, "new_no_cancels");
	check(!pt_menus_active(), "new_no_closes");

	/* Yes runs it */
	click(kFile, 8);
	click(kFile, 20);
	check(PT.dialog, "new_dirty_dialog2");
	check(pt_menus_key('y') == 1, "new_yes_consumed");
	check(f_new == 1, "new_yes_dispatch");
	check(!PT.dialog, "new_yes_closes");

	/* ---- File > Open with unsaved changes (#728): confirm first ---- */
	f_open = 0;
	PT.undo_depth = 2;
	click(kFile, 8);
	click(kFile, 36);	/* second dropdown row: Open */
	check(PT.dialog && pt_menus_active(), "open_dirty_dialog");
	check(f_open == 0, "open_dirty_not_run");
	check(pt_menus_key('n') == 1, "open_no_consumed");
	check(f_open == 0, "open_no_keeps");
	check(!pt_menus_active(), "open_no_closes");
	/* Yes proceeds to the picker. */
	click(kFile, 8);
	click(kFile, 36);
	check(PT.dialog, "open_dirty_dialog2");
	check(pt_menus_key('y') == 1, "open_yes_consumed");
	check(f_open == 1, "open_yes_dispatch");
	check(!PT.dialog, "open_yes_closes");
	/* Clean canvas: Open goes straight to the picker, no dialog. */
	f_open = 0;
	PT.undo_depth = 0;
	click(kFile, 8);
	click(kFile, 36);
	check(!pt_menus_active(), "open_clean_no_dialog");
	check(f_open == 1, "open_clean_dispatch");

	/* ---- pt_menus_confirm_quit: clean no-op, dirty dialog (#728) ---- */
	PT.undo_depth = 0;
	check(pt_menus_confirm_quit() == 0, "confirm_quit_clean");
	PT.undo_depth = 1;
	check(pt_menus_confirm_quit() == 1, "confirm_quit_dirty");
	check(PT.dialog, "confirm_quit_dialog");
	check(pt_menus_key('n') == 1, "confirm_quit_cancel");
	check(!PT.dialog, "confirm_quit_cancelled");

	/* ---- File > Quit with unsaved: Yes tears down via the lifecycle ---- */
	quit_leave = 0;
	click(kFile, 8);
	click(kFile, 88);	/* fifth row: Quit */
	check(PT.dialog, "quit_dirty_dialog");
	check(pt_menus_key(13) == 1, "quit_enter_consumed");
	check(quit_leave == 0, "quit_default_no");
	/* default highlight is No; Tab flips to Yes, Enter confirms */
	click(kFile, 8);
	click(kFile, 88);
	pt_menus_key(9);
	pt_menus_key(13);
	check(quit_leave == 1, "quit_yes_leave");

	/* ---- Edit > Undo / Redo dispatch immediately ---- */
	u_undo = 0;
	click(kEdit, 8);
	click(kEdit, 20);
	check(u_undo == 1, "edit_undo_dispatch");
	check(!pt_menus_active(), "undo_no_dialog");

	u_redo = 0;
	click(kEdit, 8);
	click(kEdit, 36);
	check(u_redo == 1, "edit_redo_dispatch");

	/* ---- #644: Edit > Cut / Copy / Paste / Clear sel / Select all ---- */
	s_cut = s_copy = s_paste = s_clear_sel = s_select_all = 0;
	click(kEdit, 8);
	click(kEdit, 68);
	check(s_cut == 1, "edit_cut_dispatch");
	click(kEdit, 8);
	click(kEdit, 84);
	check(s_copy == 1, "edit_copy_dispatch");
	click(kEdit, 8);
	click(kEdit, 100);
	check(s_paste == 1, "edit_paste_dispatch");
	click(kEdit, 8);
	click(kEdit, 116);
	check(s_clear_sel == 1, "edit_clear_sel_dispatch");
	check(!PT.dialog, "clear_sel_no_dialog");
	click(kEdit, 8);
	click(kEdit, 132);
	check(s_select_all == 1, "edit_select_all_dispatch");
	check(!pt_menus_active(), "select_all_closes");

	/* ---- Edit > Clear always confirms ---- */
	u_clear = 0;
	PT.undo_depth = 0;
	click(kEdit, 8);
	click(kEdit, 52);
	check(PT.dialog, "clear_always_dialog");
	check(u_clear == 0, "clear_waits");
	check(pt_menus_key('y') == 1, "clear_yes");
	check(u_clear == 1, "clear_dispatch");
	check(canvas_wiped(), "clear_wipes_canvas");

	/* mouse Yes / No buttons */
	u_clear = 0;
	click(kEdit, 8);
	click(kEdit, 52);
	click(340, 228);	/* No button */
	check(u_clear == 0, "clear_mouse_no");
	click(kEdit, 8);
	click(kEdit, 52);
	click(260, 228);	/* Yes button */
	check(u_clear == 1, "clear_mouse_yes");

	/* ---- Help > Keys shows the shortcut list, Esc closes ---- */
	click(kHelp, 8);
	click(kHelp, 20);
	check(pt_menus_active() && PT.dialog, "keys_open");
	scr_clear();
	pt_menus_draw();
	check(screen_has("Alt+X"), "keys_altx");
	check(screen_has("Confirm"), "keys_confirm");
	check(screen_has("Leave PAINT"), "keys_leave");
	check(pt_menus_key(27) == 1, "keys_esc_consumed");
	check(!pt_menus_active(), "keys_esc_closes");

	/* ---- keyboard: Alt+F / Alt+E / Alt+H ---- */
	PT.menu = PT_MENU_NONE;
	check(pt_menus_key(1) == 0, "alt_prefix_passthrough");
	check(pt_menus_key('f') == 0, "alt_f_opens_file");
	check(PT.menu == PT_MENU_FILE, "alt_f_menu");
	check(pt_menus_key(27) == 1, "alt_f_esc");

	pt_menus_key(1);
	pt_menus_key('e');
	check(PT.menu == PT_MENU_EDIT, "alt_e_menu");
	pt_menus_key(27);

	pt_menus_key(1);
	pt_menus_key('h');
	check(PT.menu == PT_MENU_HELP, "alt_h_menu");
	pt_menus_key(27);

	/* Alt+X is left to the lifecycle */
	check(pt_menus_key(1) == 0, "altx_prefix");
	check(pt_menus_key('x') == 0, "altx_passthrough");
	check(PT.menu == PT_MENU_NONE, "altx_no_menu");

	/* Tab cycles menus, Enter chooses the hovered/first item */
	click(kFile, 8);
	check(PT.menu == PT_MENU_FILE, "tab_from_file");
	pt_menus_key(9);
	check(PT.menu == PT_MENU_EDIT, "tab_to_edit");
	pt_menus_key(9);
	check(PT.menu == PT_MENU_HELP, "tab_to_help");
	pt_menus_key(9);
	check(PT.menu == PT_MENU_FILE, "tab_wraps");

	/* letter accelerator: q on File opens the quit confirm when dirty */
	PT.undo_depth = 1;
	pt_menus_key('q');
	check(PT.dialog, "letter_accel_quit");
	pt_menus_key('n');

	/* ---- #755: arrow keys focus and activate menu items ---- */
	PT.undo_depth = 0;
	PT.redo_depth = 0;
	PT.menu = PT_MENU_NONE;
	pt_menus_key(1);
	pt_menus_key('f');
	check(PT.menu == PT_MENU_FILE, "arrow_open_file");
	/* Down moves the focus from the first row to Open. */
	check(pt_menus_key(PT_KEY_DOWN) == 1, "arrow_down_consumed");
	f_open = 0;
	check(pt_menus_key(13) == 1, "arrow_enter_consumed");
	check(f_open == 1, "arrow_down_activates_second");
	/* Up from the first row wraps to the last item (Quit), which leaves. */
	quit_leave = 0;
	PT.menu = PT_MENU_NONE;
	pt_menus_key(1);
	pt_menus_key('f');
	check(pt_menus_key(PT_KEY_UP) == 1, "arrow_up_consumed");
	check(pt_menus_key(13) == 1, "arrow_up_enter_consumed");
	check(quit_leave == 1, "arrow_up_wraps_to_quit");
	/* Right switches to Edit and resets the focus to the first item (Undo). */
	u_undo = 0;
	PT.menu = PT_MENU_NONE;
	pt_menus_key(1);
	pt_menus_key('f');
	check(pt_menus_key(PT_KEY_RIGHT) == 1, "arrow_right_consumed");
	check(PT.menu == PT_MENU_EDIT, "arrow_right_switches");
	check(pt_menus_key(13) == 1, "arrow_right_enter_consumed");
	check(u_undo == 1, "arrow_right_focuses_first");
	/* Left wraps File back to Help (the Keys dialog). */
	PT.menu = PT_MENU_NONE;
	pt_menus_key(1);
	pt_menus_key('f');
	check(pt_menus_key(PT_KEY_LEFT) == 1, "arrow_left_consumed");
	check(PT.menu == PT_MENU_HELP, "arrow_left_wraps");
	check(pt_menus_key(13) == 1, "arrow_left_enter_consumed");
	check(PT.dialog, "arrow_left_opens_keys");
	pt_menus_key(27);

	/* ---- #756: every item's hint is exactly the key that activates it ---- */
	/* Save as takes 'a' because Save owns its first letter. */
	f_save_as = 0;
	PT.menu = PT_MENU_NONE;
	pt_menus_key(1);
	pt_menus_key('f');
	pt_menus_key('a');
	check(f_save_as == 1, "accel_save_as");
	f_save = 0;
	pt_menus_key(1);
	pt_menus_key('f');
	pt_menus_key('s');
	check(f_save == 1, "accel_save");

	scr_clear();
	PT.menu = PT_MENU_FILE;
	pt_menus_draw();
	check(hint_at("New", 'N'), "hint_new");
	check(hint_at("Open", 'O'), "hint_open");
	check(hint_at("Save as", 'A'), "hint_save_as");
	check(hint_at("Quit", 'Q'), "hint_quit");
	scr_clear();
	PT.menu = PT_MENU_EDIT;
	pt_menus_draw();
	check(hint_at("Undo", 'U'), "hint_undo");
	check(hint_at("Select", 'S'), "hint_select");
	check(hint_at("Del sel", 'D'), "hint_del_sel");
	PT.menu = PT_MENU_NONE;

	/* ---- #702: menu damage stays inside the menu band ---- */
	/* Opening File damages the bar (row 0) and the File dropdown only; it
	 * never reaches the canvas below the dropdown, and never requests a full
	 * frame. */
	pt_menus_key(27);
	dmg_reset();
	redraws = 0;
	click(kFile, 8);
	check(dmg_valid && dmg_y0 == 0 && dmg_y1 <= 16 + 5 * 16,
	      "open_damage_bounded");
	check(redraws == 0, "menu_nav_no_full_redraw");
	/* Switching to Edit while open damages the old and new dropdowns + bar.
	 * Edit now has eight rows (#644), so the band reaches 16 + 8*16. */
	dmg_reset();
	press(kEdit, 8);
	check(dmg_valid && dmg_y0 == 0 && dmg_y1 <= 16 + 8 * 16,
	      "switch_damage_bounded");
	release(kEdit, 8);
	/* Closing damages the dropdown rectangle + bar, still not the canvas. */
	dmg_reset();
	press(300, 200);
	check(dmg_valid && dmg_y0 == 0 && dmg_y1 <= 16 + 8 * 16,
	      "close_damage_bounded");
	release(300, 200);

	/* ---- #708: hover with no button follows the pointer ---- */
	/* Open File, then move (no press) onto "New" (item 0): the highlight
	 * moves and only that dropdown row is damaged - never the canvas and
	 * never a full-frame redraw. */
	click(kFile, 8);
	f_new = 0;
	dmg_reset();
	redraws = 0;
	move_to(kFile, 20);
	check(dmg_valid && dmg_y0 == 16 && dmg_y1 == 31, "hover_row_damage");
	check(redraws == 0, "hover_no_full_redraw");
	check(f_new == 0 && PT.menu == PT_MENU_FILE, "hover_no_activate");

	/* Moving to the second row (Open) damages the old and new rows. */
	dmg_reset();
	move_to(kFile, 36);
	check(dmg_valid && dmg_y0 == 16 && dmg_y1 == 47, "hover_move_damage");

	/* Leaving the dropdown for the bar clears the highlight (row 2 only). */
	dmg_reset();
	move_to(kFile, 8);
	check(dmg_valid && dmg_y0 == 32 && dmg_y1 == 47, "hover_clear_damage");

	/* Hovering another title with no button switches menus. */
	move_to(kEdit, 8);
	check(PT.menu == PT_MENU_EDIT, "hover_switch_no_button");
	pt_menus_key(27);
	check(PT.menu == PT_MENU_NONE, "hover_cleanup");

	/* A held move highlights but never activates; after the button-up a
	 * later no-button move still drives the hover (the press state is not
	 * stuck). */
	f_open = 0;
	press(kFile, 8);
	drag_to(kFile, 36);
	check(PT.menu == PT_MENU_FILE && f_open == 0, "held_move_no_activate");
	release(kFile, 8);
	dmg_reset();
	move_to(kFile, 52);
	check(dmg_valid && dmg_y0 == 32, "up_then_hover_damage");
	pt_menus_key(27);

	/* ---- #710: a menu owns a held press after an immediate action ---- */
	/* The fresh press runs Undo and closes the dropdown; the button is still
	 * physically held. The poll must keep consuming it, so cmd_paint.c never
	 * treats the held position as a fresh canvas press. */
	u_undo = 0;
	click(kEdit, 8);
	press(kEdit, 20);	/* fresh press: Undo, menu closes */
	check(u_undo == 1, "held_undo_dispatch");
	check(!pt_menus_active(), "held_undo_closes");
	check(pt_menus_mouse(kEdit, 20, PT_BTN_LEFT, 1) == 1, "held_undo_owned");
	check(pt_menus_mouse(300, 200, PT_BTN_LEFT, 1) == 1, "held_move_owned");
	release(300, 200);
	/* Released: a fresh canvas press falls through to the canvas again. */
	check(pt_menus_mouse(300, 200, PT_BTN_LEFT, 1) == 0, "release_frees_canvas");
	release(300, 200);

	printf("FAILURES %d\n", fails);
	return fails ? 1 : 0;
}
"""


@pytest.fixture(scope="module")
def checks(tmp_path_factory):
    if shutil.which("cc") is None:
        pytest.skip("needs a host C toolchain (cc)")
    tmp = tmp_path_factory.mktemp("paint_menus")
    driver = tmp / "driver.c"
    driver.write_text(DRIVER)
    exe = tmp / "driver"
    subprocess.run(
        [
            "cc", "-std=c11", "-O0", "-Wall", "-Wextra", "-Werror",
            "-DMMB_PLATFORM_POSIX",
            "-I", os.path.join(REPO, "mmbasic", "include"),
            "-I", SRC,
            "-o", str(exe), str(driver), MENUS_C,
        ],
        check=True,
        cwd=REPO,
    )
    out = subprocess.run([str(exe)], check=True, capture_output=True, text=True)
    parsed = {}
    for line in out.stdout.splitlines():
        parts = line.split()
        if parts and parts[0] == "CHECK":
            parsed[parts[1]] = parts[2] == "PASS"
    assert parsed, out.stdout + out.stderr
    return parsed


def test_menus_open_and_close(checks):
    for name in (
        "idle_not_active",
        "bar_draws_file",
        "bar_draws_edit",
        "bar_draws_help",
        "click_file_opens",
        "file_new_visible",
        "file_open_visible",
        "file_saveas_visible",
        "file_quit_visible",
        "hover_switch_edit",
        "edit_undo_visible",
        "edit_redo_visible",
        "edit_clear_visible",
        "hover_switch_help",
        "click_away_closes",
        "esc_closes",
    ):
        assert checks.get(name) is True, name


def test_new_and_quit_confirmation(checks):
    for name in (
        "new_clean_no_dialog",
        "new_clean_dispatch",
        "new_dirty_dialog",
        "new_dirty_not_run",
        "new_no_cancels",
        "new_yes_dispatch",
        "quit_dirty_dialog",
        "quit_default_no",
        "quit_yes_leave",
        "open_dirty_dialog",
        "open_dirty_not_run",
        "open_no_consumed",
        "open_no_keeps",
        "open_no_closes",
        "open_yes_consumed",
        "open_yes_dispatch",
        "open_yes_closes",
        "open_clean_no_dialog",
        "open_clean_dispatch",
    ):
        assert checks.get(name) is True, name


def test_confirm_quit_helper(checks):
    """#728: pt_menus_confirm_quit only prompts on a dirty canvas."""
    for name in (
        "confirm_quit_clean",
        "confirm_quit_dirty",
        "confirm_quit_dialog",
        "confirm_quit_cancel",
        "confirm_quit_cancelled",
    ):
        assert checks.get(name) is True, name


def test_edit_actions_and_clear_dialog(checks):
    for name in (
        "edit_undo_dispatch",
        "undo_no_dialog",
        "edit_redo_dispatch",
        "clear_always_dialog",
        "clear_waits",
        "clear_dispatch",
        "clear_wipes_canvas",
        "clear_mouse_no",
        "clear_mouse_yes",
    ):
        assert checks.get(name) is True, name


def test_selection_menu_actions(checks):
    """#644: the Edit menu dispatches Cut/Copy/Paste/Clear sel/Select all."""
    for name in (
        "edit_cut_dispatch",
        "edit_copy_dispatch",
        "edit_paste_dispatch",
        "edit_clear_sel_dispatch",
        "clear_sel_no_dialog",
        "edit_select_all_dispatch",
        "select_all_closes",
    ):
        assert checks.get(name) is True, name


def test_menu_damage_is_bounded(checks):
    """#702: open/switch/close damage only the menu band, never a full frame."""
    for name in (
        "open_damage_bounded",
        "menu_nav_no_full_redraw",
        "switch_damage_bounded",
        "close_damage_bounded",
    ):
        assert checks.get(name) is True, name


def test_hover_follows_pointer(checks):
    """#708: motion with no button moves the dropdown highlight; damage stays
    on the affected rows; a held drag never activates; button-up is handled."""
    for name in (
        "hover_row_damage",
        "hover_no_full_redraw",
        "hover_no_activate",
        "hover_move_damage",
        "hover_clear_damage",
        "hover_switch_no_button",
        "hover_cleanup",
        "held_move_no_activate",
        "up_then_hover_damage",
    ):
        assert checks.get(name) is True, name


def test_held_press_owned_after_menu_action(checks):
    """#710: a menu that runs an immediate action keeps owning the held press,
    so it cannot leak into cmd_paint.c's canvas handling."""
    for name in (
        "held_undo_dispatch",
        "held_undo_closes",
        "held_undo_owned",
        "held_move_owned",
        "release_frees_canvas",
    ):
        assert checks.get(name) is True, name


def test_help_keys_and_keyboard_navigation(checks):
    for name in (
        "keys_open",
        "keys_altx",
        "keys_confirm",
        "keys_leave",
        "keys_esc_closes",
        "alt_prefix_passthrough",
        "alt_f_opens_file",
        "alt_f_menu",
        "alt_e_menu",
        "alt_h_menu",
        "altx_passthrough",
        "altx_no_menu",
        "tab_from_file",
        "tab_to_edit",
        "tab_to_help",
        "tab_wraps",
        "letter_accel_quit",
    ):
        assert checks.get(name) is True, name


def test_keyboard_arrows_and_shortcut_hints(checks):
    """#755/#756: arrows move/activate items, and each item shows the exact key
    that activates it (Save as uses 'a' so Save can keep 's')."""
    for name in (
        "arrow_open_file",
        "arrow_down_consumed",
        "arrow_enter_consumed",
        "arrow_down_activates_second",
        "arrow_up_consumed",
        "arrow_up_enter_consumed",
        "arrow_up_wraps_to_quit",
        "arrow_right_consumed",
        "arrow_right_switches",
        "arrow_right_enter_consumed",
        "arrow_right_focuses_first",
        "arrow_left_consumed",
        "arrow_left_wraps",
        "arrow_left_enter_consumed",
        "arrow_left_opens_keys",
        "accel_save_as",
        "accel_save",
        "hint_new",
        "hint_open",
        "hint_save_as",
        "hint_quit",
        "hint_undo",
        "hint_select",
        "hint_del_sel",
    ):
        assert checks.get(name) is True, name
