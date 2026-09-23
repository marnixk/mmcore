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

pt_state PT;

/* ---- action/state recording ---- */
static int f_new, f_open, f_save, f_save_as;
static int u_undo, u_redo, u_clear;
static int quit_key = -1;
static int redraws;

void pt_request_redraw(void) { redraws++; }
void pt_file_new(void) { f_new++; }
void pt_file_open(void) { f_open++; }
void pt_file_save(void) { f_save++; }
void pt_file_save_as(void) { f_save_as++; }
void pt_undo(void) { u_undo++; }
void pt_redo(void) { u_redo++; }
void pt_undo_clear(void) { u_clear++; }
const char *mmb_paint_key(char c)
{
	if ((unsigned char)c == 24)
		quit_key = 24;
	return "";
}

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

static int screen_has(const char *needle)
{
	int y;

	for (y = 0; y < ROWS; y++)
		if (strstr(cells[y], needle))
			return 1;
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

int main(void)
{
	scr_clear();
	memset(&PT, 0, sizeof PT);
	PT.active = 1;
	PT.menu = PT_MENU_NONE;
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

	/* ---- File > Quit with unsaved: Yes routes through Ctrl+X ---- */
	quit_key = -1;
	click(kFile, 8);
	click(kFile, 88);	/* fifth row: Quit */
	check(PT.dialog, "quit_dirty_dialog");
	check(pt_menus_key(13) == 1, "quit_enter_consumed");
	check(quit_key == -1, "quit_default_no");
	/* default highlight is No; Tab flips to Yes, Enter confirms */
	click(kFile, 8);
	click(kFile, 88);
	pt_menus_key(9);
	pt_menus_key(13);
	check(quit_key == 24, "quit_yes_ctl_x");

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

	/* ---- Edit > Clear always confirms ---- */
	u_clear = 0;
	PT.undo_depth = 0;
	click(kEdit, 8);
	click(kEdit, 52);
	check(PT.dialog, "clear_always_dialog");
	check(u_clear == 0, "clear_waits");
	check(pt_menus_key('y') == 1, "clear_yes");
	check(u_clear == 1, "clear_dispatch");

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
        "quit_yes_ctl_x",
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
        "clear_mouse_no",
        "clear_mouse_yes",
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
