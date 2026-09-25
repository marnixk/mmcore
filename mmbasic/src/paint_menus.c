/*
 * PAINT menus (#638): the text menu bar, its dropdowns and the modal
 * confirmation / shortcut dialogs. Strong definitions replace the weak stubs
 * at the bottom of cmd_paint.c; cmd_paint.c owns the event loop and calls the
 * pt_menus_* hooks declared in paint.h.
 *
 * The bar is one 8x16 text row (PT_MENU_H). A dropdown hangs below its title
 * and is hit-tested in cells. Menu actions only call the frozen paint.h API -
 * undo/redo/clear, the file entry points and, for Quit, the Ctrl+X key path so
 * the existing lifecycle performs the teardown.
 */
#include "mmb_priv.h"
#include "tui.h"
#include "paint.h"

#define MENU_CW 8
#define MENU_CH 16

enum pt_action {
	PTA_NONE = 0,
	PTA_FILE_NEW,
	PTA_FILE_OPEN,
	PTA_FILE_SAVE,
	PTA_FILE_SAVE_AS,
	PTA_FILE_QUIT,
	PTA_EDIT_UNDO,
	PTA_EDIT_REDO,
	PTA_EDIT_CLEAR,
	PTA_EDIT_CUT,
	PTA_EDIT_COPY,
	PTA_EDIT_PASTE,
	PTA_EDIT_CLEAR_SEL,
	PTA_EDIT_SELECT_ALL,
	PTA_HELP_KEYS
};

#define PT_MENU_MAX_ITEMS 9

/* `key` is the accelerator: it is shown at the right of the dropdown row and
 * is the exact letter that activates that item, so the hint can never disagree
 * with what the keyboard does. */
typedef struct pt_menu_item {
	const char *label;
	char key;
	int action;
} pt_menu_item;

static const char *const s_titles[PT_MENU_COUNT] = { "File", "Edit", "Help" };
static const int s_title_col[PT_MENU_COUNT] = { 1, 7, 13 };

static const pt_menu_item s_items[PT_MENU_COUNT][PT_MENU_MAX_ITEMS] = {
	{ { "New", 'N', PTA_FILE_NEW }, { "Open", 'O', PTA_FILE_OPEN },
	  { "Save", 'S', PTA_FILE_SAVE }, { "Save as", 'A', PTA_FILE_SAVE_AS },
	  { "Quit", 'Q', PTA_FILE_QUIT }, { 0, 0, 0 }, { 0, 0, 0 },
	  { 0, 0, 0 }, { 0, 0, 0 } },
	{ { "Undo", 'U', PTA_EDIT_UNDO }, { "Redo", 'R', PTA_EDIT_REDO },
	  { "Clear", 'L', PTA_EDIT_CLEAR }, { "Cut", 'T', PTA_EDIT_CUT },
	  { "Copy", 'C', PTA_EDIT_COPY }, { "Paste", 'P', PTA_EDIT_PASTE },
	  { "Del sel", 'D', PTA_EDIT_CLEAR_SEL },
	  { "Select", 'S', PTA_EDIT_SELECT_ALL }, { 0, 0, 0 } },
	{ { "Keys", 'K', PTA_HELP_KEYS }, { 0, 0, 0 }, { 0, 0, 0 },
	  { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 },
	  { 0, 0, 0 } }
};

static const int s_item_count[PT_MENU_COUNT] = { 5, 8, 1 };

enum { DLG_NONE = 0, DLG_CONFIRM, DLG_KEYS };

static int s_dlg;
static int s_dlg_action;
static int s_dlg_yes;
static int s_alt;
static int s_hover;
static int s_btn;
/* The menu/dialog consumed the press in progress, so the poll keeps owning the
 * button until it is released (#710). Without this a press that ran an
 * immediate menu action (Undo/Redo/Open/Save) closes the menu and the next held
 * poll falls through to cmd_paint.c's palette/tool/canvas handling, starting a
 * stray stroke behind where the menu was. */
static int s_owned;
/* Last pointer position seen by pt_menus_mouse(). A poll that repeats the same
 * position is not a real move, so it must not clear a keyboard-set highlight
 * (#755). A keyboard menu open arms an "ignore until the pointer moves" state
 * (s_ptr_seen = 0) so the next idle poll only records the baseline. */
static int s_ptr_x, s_ptr_y, s_ptr_seen;

/* ---- geometry ---------------------------------------------------------- */

static int menu_cells(int m)
{
	int i, n = 0;

	for (i = 0; i < s_item_count[m]; i++)
	{
		int len = (int)strlen(s_items[m][i].label);
		if (len > n)
			n = len;
	}
	/* Border, label, a separating blank and the accelerator cell. */
	return n + 4;
}

/* Which title is under a screen pixel, or PT_MENU_NONE. */
static int title_at(int sx, int sy)
{
	int m;

	if (sy < 0 || sy >= PT_MENU_H)
		return PT_MENU_NONE;
	for (m = 0; m < PT_MENU_COUNT; m++)
	{
		int x0 = s_title_col[m] * MENU_CW - 4;
		int x1 = (s_title_col[m] + (int)strlen(s_titles[m])) * MENU_CW + 4;
		if (sx >= x0 && sx < x1)
			return m;
	}
	return PT_MENU_NONE;
}

/* Dropdown row under a screen pixel, or -1. The panel starts at row 1. */
static int item_at(int m, int sx, int sy)
{
	int pw, x0, i;

	if (m < 0 || m >= PT_MENU_COUNT)
		return -1;
	pw = menu_cells(m);
	x0 = s_title_col[m] * MENU_CW;
	if (sx < x0 || sx >= x0 + pw * MENU_CW)
		return -1;
	if (sy < PT_MENU_H)
		return -1;
	i = (sy - PT_MENU_H) / MENU_CH;
	if (i < 0 || i >= s_item_count[m])
		return -1;
	return i;
}

static int cell_hit(int sx, int sy, int cx, int cy, int cw, int ch)
{
	return sx >= cx * MENU_CW && sx < (cx + cw) * MENU_CW &&
	       sy >= cy * MENU_CH && sy < (cy + ch) * MENU_CH;
}

static void confirm_geom(int *x, int *y, int *w, int *h)
{
	tui_dialog_geom(40, 7, x, y, w, h);
}

static void confirm_buttons(int x, int y, int w, int h, int *yx, int *yy,
			    int *nx, int *ny)
{
	int start = x + (w - 16) / 2;

	*yx = start;
	*yy = y + h - 2;
	*nx = start + 9;
	*ny = y + h - 2;
}

static void keys_geom(int *x, int *y, int *w, int *h)
{
	tui_dialog_geom(46, 11, x, y, w, h);
}

/* ---- damage + cell bookkeeping (#702) ---------------------------------- *
 * A menu open/hover/close or a dialog only changes a small rectangle. Mark
 * that rectangle with pt_damage() (screen pixels) so cmd_paint.c recomposites
 * and presents just those rows, instead of requesting a full redraw. The TUI
 * cell model is kept in step separately: a rectangle that closes is blanked
 * and "accepted" (raw canvas pixels win), and a rectangle whose content
 * changes is invalidated so its text re-blits over the canvas. */

/* Dropdown rectangle in text cells: title column, first row below the bar. */
static void dropdown_cells(int m, int *x, int *y, int *w, int *h)
{
	if (m < 0 || m >= PT_MENU_COUNT)
	{
		*x = *y = *w = *h = 0;
		return;
	}
	*x = s_title_col[m];
	*y = 1;
	*w = menu_cells(m);
	*h = s_item_count[m];
}

/* The overlay currently on screen: a dialog wins over a dropdown. */
static void overlay_cells(int *x, int *y, int *w, int *h)
{
	if (s_dlg == DLG_CONFIRM)
		confirm_geom(x, y, w, h);
	else if (s_dlg == DLG_KEYS)
		keys_geom(x, y, w, h);
	else
		dropdown_cells(PT.menu, x, y, w, h);
}

static void dmg_cells(int cx, int cy, int cw, int ch)
{
	if (cw > 0 && ch > 0)
		pt_damage(cx * MENU_CW, cy * MENU_CH, cw * MENU_CW,
			  ch * MENU_CH);
}

static void dmg_bar(void)
{
	pt_damage(0, 0, PT_W, PT_MENU_H);
}

/* Damage whatever overlay (dropdown or dialog) is showing right now. */
static void dmg_overlay(void)
{
	int x, y, w, h;

	overlay_cells(&x, &y, &w, &h);
	dmg_cells(x, y, w, h);
}

/* Overlay rectangle painted on the previous frame, for cell bookkeeping. */
static int s_drawn_menu = PT_MENU_NONE;
static int s_drawn_hover = -1;
static int s_drawn_dlg = DLG_NONE;
static int s_drawn_yes = -1;

/* ---- drawing ----------------------------------------------------------- */

static void draw_bar(void)
{
	int m;

	tui_pad(0, 0, "", tui_cols(), TUI_BRWHITE, TUI_BRBLUE);
	for (m = 0; m < PT_MENU_COUNT; m++)
	{
		int open = (PT.menu == m);
		int fg = open ? TUI_BLACK : TUI_BRWHITE;
		int bg = open ? TUI_BRWHITE : TUI_BRBLUE;
		int len = (int)strlen(s_titles[m]);

		tui_put(s_title_col[m] - 1, 0, ' ', fg, bg);
		tui_puts(s_title_col[m], 0, s_titles[m], fg, bg);
		tui_put(s_title_col[m] + len, 0, ' ', fg, bg);
	}
}

static void draw_dropdown(int m)
{
	int i, pw = menu_cells(m);
	int px = s_title_col[m];

	for (i = 0; i < s_item_count[m]; i++)
	{
		int sel = (i == s_hover);
		int fg = sel ? TUI_BRWHITE : TUI_BLACK;
		int bg = sel ? TUI_BRBLUE : TUI_WHITE;
		int row = 1 + i;

		tui_put(px, row, ' ', fg, bg);
		tui_pad(px + 1, row, s_items[m][i].label, pw - 3, fg, bg);
		tui_put(px + pw - 2, row, s_items[m][i].key, fg, bg);
		tui_put(px + pw - 1, row, ' ', fg, bg);
	}
}

static void confirm_text(int act, const char **title, const char **msg)
{
	switch (act)
	{
	case PTA_FILE_NEW:
		*title = "New canvas";
		*msg = "Discard unsaved changes?";
		break;
	case PTA_FILE_OPEN:
		*title = "Open image";
		*msg = "Discard unsaved changes?";
		break;
	case PTA_FILE_QUIT:
		*title = "Quit PAINT";
		*msg = "Quit without saving?";
		break;
	default:
		*title = "Clear canvas";
		*msg = "Clear the whole canvas?";
		break;
	}
}

static void draw_confirm(void)
{
	const char *title, *msg;
	int x, y, w, h, yx, yy, nx, ny;

	confirm_text(s_dlg_action, &title, &msg);
	confirm_geom(&x, &y, &w, &h);
	confirm_buttons(x, y, w, h, &yx, &yy, &nx, &ny);
	tui_dialog_panel(x, y, w, h, title, TUI_BLACK, TUI_WHITE, TUI_WHITE,
			 TUI_BLUE, TUI_BRWHITE, TUI_BLUE);
	tui_puts(x + 2, y + 2, msg, TUI_BLACK, TUI_WHITE);
	tui_pad(yx, yy, "  Yes  ", 7, s_dlg_yes ? TUI_BRWHITE : TUI_BLACK,
		s_dlg_yes ? TUI_BRBLUE : TUI_WHITE);
	tui_pad(nx, ny, "  No   ", 7, !s_dlg_yes ? TUI_BRWHITE : TUI_BLACK,
		!s_dlg_yes ? TUI_BRBLUE : TUI_WHITE);
}

static const char *const s_key_lines[] = {
	"Esc / Alt+X   Leave PAINT",
	"Alt+F/E/H     Open a menu",
	"Up/Down       Move through items",
	"Left/Right    Switch menu",
	"Enter         Choose item",
	"Y / N         Confirm Yes/No",
	"Click         Choose item"
};

static void draw_keys(void)
{
	int x, y, w, h, i;

	keys_geom(&x, &y, &w, &h);
	tui_dialog_panel(x, y, w, h, "Keys", TUI_BLACK, TUI_WHITE, TUI_WHITE,
			 TUI_BLUE, TUI_BRWHITE, TUI_BLUE);
	for (i = 0; i < (int)(sizeof(s_key_lines) / sizeof(s_key_lines[0])); i++)
		tui_puts(x + 2, y + 2 + i, s_key_lines[i], TUI_BLACK, TUI_WHITE);
}

void pt_menus_draw(void)
{
	int cx, cy, cw, ch;	/* overlay this frame */
	int px, py, pw, ph;	/* overlay painted last frame */
	int changed = 0;

	if (!PT.active)
		return;

	overlay_cells(&cx, &cy, &cw, &ch);
	if (s_drawn_dlg == DLG_CONFIRM)
		confirm_geom(&px, &py, &pw, &ph);
	else if (s_drawn_dlg == DLG_KEYS)
		keys_geom(&px, &py, &pw, &ph);
	else
		dropdown_cells(s_drawn_menu, &px, &py, &pw, &ph);

	if (px != cx || py != cy || pw != cw || ph != ch)
	{
		/* The old rectangle is canvas (or a new overlay) now: blank the
		 * cells and accept them so the flush never repaints old text over
		 * the pixels cmd_paint.c just recomposited. */
		if (pw > 0 && ph > 0)
		{
			tui_fill(px, py, pw, ph, ' ', TUI_WHITE, TUI_BLACK);
			tui_accept_rect(px, py, pw, ph);
		}
		if (cw > 0 && ch > 0)
			tui_invalidate_rect(cx, cy, cw, ch);
		changed = 1;
	}

	draw_bar();
	if (PT.menu != PT_MENU_NONE)
		draw_dropdown(PT.menu);
	if (s_dlg == DLG_CONFIRM)
		draw_confirm();
	else if (s_dlg == DLG_KEYS)
		draw_keys();

	if (PT.menu != s_drawn_menu || s_hover != s_drawn_hover ||
	    s_dlg != s_drawn_dlg || s_dlg_yes != s_drawn_yes)
		changed = 1;

	if (changed && cw > 0 && ch > 0)
		tui_invalidate_rect(cx, cy, cw, ch);

	s_drawn_menu = PT.menu;
	s_drawn_hover = s_hover;
	s_drawn_dlg = s_dlg;
	s_drawn_yes = s_dlg_yes;
}

/* ---- actions ----------------------------------------------------------- */

static int unsaved(void)
{
	return PT.undo_depth > 0 || PT.redo_depth > 0;
}

static void open_confirm(int act);

/* Quit entry point for the lifecycle's keyboard quit paths (Esc, Alt+X,
 * Ctrl+X). Dirty canvas: open the discard confirmation and say so; clean:
 * let the caller tear down. */
int pt_menus_confirm_quit(void)
{
	if (!PT.active)
		return 0;
	if (!unsaved())
		return 0;
	open_confirm(PTA_FILE_QUIT);
	return 1;
}

/* Edit > Clear wipes the canvas to palette index 0 (black) through the frozen
 * canvas accessor, then drops the undo/redo history so the cleared image is
 * the new baseline. */
static void clear_canvas(void)
{
	int x, y;

	if (!PT.canvas || PT.width <= 0 || PT.height <= 0)
		return;
	for (y = 0; y < PT.height; y++)
		for (x = 0; x < PT.width; x++)
			pt_canvas_set(x, y, 0);
}

static void run_action(int act)
{
	switch (act)
	{
	case PTA_FILE_NEW:
		pt_file_new();
		break;
	case PTA_FILE_OPEN:
		pt_file_open();
		break;
	case PTA_FILE_SAVE:
		pt_file_save();
		break;
	case PTA_FILE_SAVE_AS:
		pt_file_save_as();
		break;
	case PTA_FILE_QUIT:
		/* The lifecycle owns teardown; the confirm gate already ran. */
		pt_paint_leave();
		return;
	case PTA_EDIT_UNDO:
		pt_undo();
		break;
	case PTA_EDIT_REDO:
		pt_redo();
		break;
	case PTA_EDIT_CLEAR:
		clear_canvas();
		pt_undo_clear();
		strncpy(PT.status, "Cleared", sizeof(PT.status) - 1);
		break;
	case PTA_EDIT_CUT:
		pt_select_cut();
		break;
	case PTA_EDIT_COPY:
		pt_select_copy();
		break;
	case PTA_EDIT_PASTE:
		pt_select_paste();
		break;
	case PTA_EDIT_CLEAR_SEL:
		pt_select_clear();
		break;
	case PTA_EDIT_SELECT_ALL:
		pt_select_all();
		break;
	default:
		break;
	}
	if (PT.active)
		pt_request_redraw();
}

/* Move the dropdown highlight to item `it`, wrapping, and damage the row that
 * loses it and the row that gains it. The keyboard arrows use this; the mouse
 * path has its own hover_item(). */
static void focus_item(int it)
{
	int m = PT.menu, x, y, w, h;

	if (m < 0 || m >= PT_MENU_COUNT)
		return;
	if (it < 0)
		it = s_item_count[m] - 1;
	else if (it >= s_item_count[m])
		it = 0;
	if (it == s_hover)
		return;
	dropdown_cells(m, &x, &y, &w, &h);
	if (s_hover >= 0)
		dmg_cells(x, y + s_hover, w, 1);
	dmg_cells(x, y + it, w, 1);
	s_hover = it;
}

/* Open dropdown `m`. `focus` puts the keyboard highlight on the first row (the
 * keyboard openers do); a mouse open leaves no row highlighted until a hover. */
static void open_menu(int m, int focus)
{
	/* A keyboard open ignores the pointer until it actually moves, so an
	 * idle poll cannot immediately clear the first-row highlight. */
	if (focus)
		s_ptr_seen = 0;
	if (PT.menu == m)
	{
		if (focus && s_hover < 0)
			focus_item(0);
		return;
	}
	dmg_overlay();
	PT.menu = m;
	s_hover = -1;
	dmg_overlay();
	dmg_bar();
	if (focus)
		focus_item(0);
}

static void close_menu(void)
{
	if (PT.menu != PT_MENU_NONE)
	{
		dmg_overlay();
		PT.menu = PT_MENU_NONE;
		s_hover = -1;
		dmg_bar();
	}
}

static void open_confirm(int act)
{
	dmg_overlay();
	PT.menu = PT_MENU_NONE;
	s_hover = -1;
	dmg_bar();
	s_dlg = DLG_CONFIRM;
	s_dlg_action = act;
	s_dlg_yes = 0;
	PT.dialog = 1;
	dmg_overlay();
}

static void close_dialog(void)
{
	if (s_dlg == DLG_NONE)
		return;
	dmg_overlay();
	s_dlg = DLG_NONE;
	PT.dialog = 0;
}

static void answer_confirm(int yes)
{
	int act = s_dlg_action;

	dmg_overlay();
	s_dlg = DLG_NONE;
	PT.dialog = 0;
	if (yes)
		run_action(act);
}

static void activate(int m, int i)
{
	int act = s_items[m][i].action;
	int x, y, w, h;

	/* The dropdown closes; damage its rectangle so the canvas under it is
	 * recomposited. */
	dropdown_cells(m, &x, &y, &w, &h);
	dmg_cells(x, y, w, h);
	PT.menu = PT_MENU_NONE;
	s_hover = -1;
	dmg_bar();

	if (act == PTA_HELP_KEYS)
	{
		s_dlg = DLG_KEYS;
		PT.dialog = 1;
		dmg_overlay();
		return;
	}
	if (act == PTA_FILE_NEW || act == PTA_FILE_QUIT ||
	    act == PTA_FILE_OPEN)
	{
		if (unsaved())
		{
			open_confirm(act);
			return;
		}
	}
	else if (act == PTA_EDIT_CLEAR)
	{
		open_confirm(act);
		return;
	}
	run_action(act);
}

/* ---- lifecycle hooks --------------------------------------------------- */

void pt_menus_init(void)
{
	s_dlg = DLG_NONE;
	s_dlg_action = PTA_NONE;
	s_dlg_yes = 0;
	s_alt = 0;
	s_hover = -1;
	s_btn = 0;
	s_owned = 0;
	s_ptr_x = 0;
	s_ptr_y = 0;
	s_ptr_seen = 0;
	s_drawn_menu = PT_MENU_NONE;
	s_drawn_hover = -1;
	s_drawn_dlg = DLG_NONE;
	s_drawn_yes = -1;
	PT.dialog = 0;
}

int pt_menus_active(void)
{
	return (PT.menu != PT_MENU_NONE || s_dlg != DLG_NONE) ? 1 : 0;
}

void pt_menus_close(void)
{
	if (PT.menu != PT_MENU_NONE)
	{
		dmg_overlay();
		PT.menu = PT_MENU_NONE;
		s_hover = -1;
		dmg_bar();
	}
	if (s_dlg == DLG_KEYS)
	{
		dmg_overlay();
		s_dlg = DLG_NONE;
		PT.dialog = 0;
	}
}

/* ---- keyboard ---------------------------------------------------------- */

static int match_accel(char key, char accel)
{
	char c = accel;

	if (key == c)
		return 1;
	if (c >= 'a' && c <= 'z' && key == c - 'a' + 'A')
		return 1;
	if (c >= 'A' && c <= 'Z' && key == c - 'A' + 'a')
		return 1;
	return 0;
}

int pt_menus_key(int key)
{
	int i;

	if (!PT.active)
		return 0;
	/* Ctrl+X always leaves PAINT, even from a modal. */
	if (key == 24)
		return 0;

	if (s_dlg == DLG_KEYS)
	{
		close_dialog();
		return 1;
	}
	if (s_dlg == DLG_CONFIRM)
	{
		if (key == 'y' || key == 'Y')
			answer_confirm(1);
		else if (key == 'n' || key == 'N' || key == 27)
			answer_confirm(0);
		else if (key == 13)
			answer_confirm(s_dlg_yes);
		else if (key == 9 || key == ' ')
		{
			s_dlg_yes = !s_dlg_yes;
			pt_request_redraw();
		}
		return 1;
	}

	/* Alt prefix: report 0 so cmd_paint.c tracks its own chord state too. */
	if (key == 1)
	{
		s_alt = 1;
		return 0;
	}
	if (s_alt)
	{
		s_alt = 0;
		if (key == 'f' || key == 'F')
			open_menu(PT_MENU_FILE, 1);
		else if (key == 'e' || key == 'E')
			open_menu(PT_MENU_EDIT, 1);
		else if (key == 'h' || key == 'H')
			open_menu(PT_MENU_HELP, 1);
		/* Unhandled chords fall through so Alt+X still quits. */
		return 0;
	}

	if (PT.menu != PT_MENU_NONE)
	{
		int m = PT.menu;

		if (key == 27)
		{
			close_menu();
			return 1;
		}
		if (key == PT_KEY_UP)
		{
			focus_item(s_hover < 0 ? -1 : s_hover - 1);
			return 1;
		}
		if (key == PT_KEY_DOWN)
		{
			focus_item(s_hover < 0 ? 0 : s_hover + 1);
			return 1;
		}
		if (key == PT_KEY_LEFT)
		{
			open_menu((m + PT_MENU_COUNT - 1) % PT_MENU_COUNT, 1);
			return 1;
		}
		if (key == PT_KEY_RIGHT || key == 9)
		{
			open_menu((m + 1) % PT_MENU_COUNT, 1);
			return 1;
		}
		if (key == 13)
		{
			activate(m, s_hover >= 0 ? s_hover : 0);
			return 1;
		}
		for (i = 0; i < s_item_count[m]; i++)
			if (match_accel((char)key, s_items[m][i].key))
			{
				activate(m, i);
				return 1;
			}
		return 1;
	}
	return 0;
}

/* ---- mouse ------------------------------------------------------------- */

/* Move the dropdown highlight onto the row under the pointer, damaging the
 * row that loses it and the row that gains it. `dropdown_cells()` yields the
 * first dropdown row in cells (cell row 1) and item i is drawn at `1 + i`, so
 * the damage row is `y + i`. No full frame, no canvas touch (#702/#708). */
static void hover_item(int sx, int sy)
{
	int it, x, y, w, h;

	if (PT.menu == PT_MENU_NONE)
		return;
	it = item_at(PT.menu, sx, sy);
	if (it == s_hover)
		return;
	dropdown_cells(PT.menu, &x, &y, &w, &h);
	if (s_hover >= 0)
		dmg_cells(x, y + s_hover, w, 1);
	if (it >= 0)
		dmg_cells(x, y + it, w, 1);
	s_hover = it;
}

/* Pointer motion with no fresh press: follow the pointer. Hovering a title
 * switches menus; hovering an item moves the highlight; a modal ignores
 * motion. Returns 1 when a menu/dialog is open (the event is consumed). */
static int pointer_motion(int sx, int sy)
{
	if (s_dlg != DLG_NONE)
		return 1;
	if (PT.menu != PT_MENU_NONE)
	{
		int t = title_at(sx, sy);

		if (t >= 0 && t != PT.menu)
			open_menu(t, 0);
		else
			hover_item(sx, sy);
		return 1;
	}
	return 0;
}

int pt_menus_mouse(int sx, int sy, int button, int down)
{
	int fresh;

	(void)button;
	if (!PT.active)
		return 0;

	if (!down)
	{
		/* A button-up ends a press; once the button is up an event is
		 * pure motion and drives the hover highlight (#708). The
		 * release edge itself is also forwarded - even after the menu
		 * closed on the press - so `s_btn` cannot stay stuck set. */
		if (s_btn)
		{
			s_btn = 0;
			s_owned = 0;
			return pt_menus_active();
		}
		/* A poll that repeats the last position is not a real move:
		 * record the baseline and leave any keyboard highlight alone
		 * (#755). The first event after a keyboard open only arms the
		 * baseline, so an idle poll cannot clear it either. */
		if (!s_ptr_seen)
		{
			s_ptr_seen = 1;
			s_ptr_x = sx;
			s_ptr_y = sy;
			return pt_menus_active();
		}
		if (sx == s_ptr_x && sy == s_ptr_y)
			return pt_menus_active();
		s_ptr_x = sx;
		s_ptr_y = sy;
		return pointer_motion(sx, sy);
	}

	/* A press is a real pointer event: keep it as the baseline so the
	 * button-up and any held motion compare against it (#708). */
	s_ptr_seen = 1;
	s_ptr_x = sx;
	s_ptr_y = sy;

	/* `fresh` is a genuine new press: the loop now forwards the button-up
	 * too, so held motion is distinguished from a press. A press that lands
	 * while a menu/dialog is open belongs to it: if that press runs an
	 * immediate action and closes the overlay, the menu keeps owning the
	 * button so held polls do not reach the canvas (#710). */
	fresh = !s_btn;
	if (fresh)
		s_owned = pt_menus_active();
	s_btn = 1;

	if (s_dlg == DLG_CONFIRM)
	{
		if (fresh)
		{
			int x, y, w, h, yx, yy, nx, ny;

			confirm_geom(&x, &y, &w, &h);
			confirm_buttons(x, y, w, h, &yx, &yy, &nx, &ny);
			if (cell_hit(sx, sy, yx, yy, 7, 1))
				answer_confirm(1);
			else if (cell_hit(sx, sy, nx, ny, 7, 1))
				answer_confirm(0);
		}
		return 1;
	}
	if (s_dlg == DLG_KEYS)
	{
		if (fresh)
			close_dialog();
		return 1;
	}

	if (PT.menu != PT_MENU_NONE)
	{
		int t = title_at(sx, sy);

		if (!fresh)
		{
			/* Held: hovering a title switches, hovering an item
			 * highlights; a drag never activates an item. */
			if (t >= 0 && t != PT.menu)
				open_menu(t, 0);
			else
				hover_item(sx, sy);
			return 1;
		}
		{
			int it = item_at(PT.menu, sx, sy);

			if (it >= 0)
			{
				activate(PT.menu, it);
				return 1;
			}
			if (t >= 0 && t != PT.menu)
			{
				open_menu(t, 0);
				return 1;
			}
			close_menu();
			return 1;
		}
	}

	if (fresh)
	{
		int t = title_at(sx, sy);

		if (t >= 0)
		{
			open_menu(t, 0);
			return 1;
		}
	}

	/* A menu/dialog that is no longer on screen may still own this held
	 * press (it ran an immediate action and closed). Keep consuming the
	 * event until button-up so the press cannot leak into the canvas. */
	if (s_owned)
		return 1;
	return 0;
}
