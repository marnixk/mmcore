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
	PTA_HELP_KEYS
};

typedef struct pt_menu_item {
	const char *label;
	int action;
} pt_menu_item;

static const char *const s_titles[PT_MENU_COUNT] = { "File", "Edit", "Help" };
static const int s_title_col[PT_MENU_COUNT] = { 1, 7, 13 };

static const pt_menu_item s_items[PT_MENU_COUNT][6] = {
	{ { "New", PTA_FILE_NEW }, { "Open", PTA_FILE_OPEN },
	  { "Save", PTA_FILE_SAVE }, { "Save as", PTA_FILE_SAVE_AS },
	  { "Quit", PTA_FILE_QUIT }, { 0, 0 } },
	{ { "Undo", PTA_EDIT_UNDO }, { "Redo", PTA_EDIT_REDO },
	  { "Clear", PTA_EDIT_CLEAR }, { 0, 0 }, { 0, 0 }, { 0, 0 } },
	{ { "Keys", PTA_HELP_KEYS }, { 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 },
	  { 0, 0 } }
};

static const int s_item_count[PT_MENU_COUNT] = { 5, 3, 1 };

enum { DLG_NONE = 0, DLG_CONFIRM, DLG_KEYS };

static int s_dlg;
static int s_dlg_action;
static int s_dlg_yes;
static int s_alt;
static int s_hover;
static int s_btn;
static int s_last_sx = -1, s_last_sy = -1;

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
	return n + 2;
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
		tui_pad(px + 1, row, s_items[m][i].label, pw - 2, fg, bg);
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
	"Enter         Choose item",
	"Y / N         Confirm Yes/No",
	"Esc           Close a menu",
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
	if (!PT.active)
		return;
	draw_bar();
	if (PT.menu != PT_MENU_NONE)
		draw_dropdown(PT.menu);
	if (s_dlg == DLG_CONFIRM)
		draw_confirm();
	else if (s_dlg == DLG_KEYS)
		draw_keys();
}

/* ---- actions ----------------------------------------------------------- */

static int unsaved(void)
{
	return PT.undo_depth > 0 || PT.redo_depth > 0;
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
		/* Reuse the lifecycle's Ctrl+X path so cmd_paint.c tears down. */
		mmb_paint_key(24);
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
	default:
		break;
	}
	if (PT.active)
		pt_request_redraw();
}

static void open_menu(int m)
{
	PT.menu = m;
	s_hover = -1;
	pt_request_redraw();
}

static void close_menu(void)
{
	if (PT.menu != PT_MENU_NONE)
	{
		PT.menu = PT_MENU_NONE;
		s_hover = -1;
		pt_request_redraw();
	}
}

static void open_confirm(int act)
{
	s_dlg = DLG_CONFIRM;
	s_dlg_action = act;
	s_dlg_yes = 0;
	PT.menu = PT_MENU_NONE;
	s_hover = -1;
	PT.dialog = 1;
	pt_request_redraw();
}

static void close_dialog(void)
{
	s_dlg = DLG_NONE;
	PT.dialog = 0;
	pt_request_redraw();
}

static void answer_confirm(int yes)
{
	int act = s_dlg_action;

	s_dlg = DLG_NONE;
	PT.dialog = 0;
	if (yes)
		run_action(act);
	else
		pt_request_redraw();
}

static void activate(int m, int i)
{
	int act = s_items[m][i].action;

	PT.menu = PT_MENU_NONE;
	s_hover = -1;

	if (act == PTA_HELP_KEYS)
	{
		s_dlg = DLG_KEYS;
		PT.dialog = 1;
		pt_request_redraw();
		return;
	}
	if (act == PTA_FILE_NEW || act == PTA_FILE_QUIT)
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
	s_last_sx = s_last_sy = -1;
	PT.dialog = 0;
}

int pt_menus_active(void)
{
	return (PT.menu != PT_MENU_NONE || s_dlg != DLG_NONE) ? 1 : 0;
}

void pt_menus_close(void)
{
	int changed = 0;

	if (PT.menu != PT_MENU_NONE)
	{
		PT.menu = PT_MENU_NONE;
		s_hover = -1;
		changed = 1;
	}
	if (s_dlg == DLG_KEYS)
	{
		s_dlg = DLG_NONE;
		PT.dialog = 0;
		changed = 1;
	}
	if (changed)
		pt_request_redraw();
}

/* ---- keyboard ---------------------------------------------------------- */

static int match_label(char key, const char *label)
{
	char c = label[0];

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
			open_menu(PT_MENU_FILE);
		else if (key == 'e' || key == 'E')
			open_menu(PT_MENU_EDIT);
		else if (key == 'h' || key == 'H')
			open_menu(PT_MENU_HELP);
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
		if (key == 9)
		{
			open_menu((m + 1) % PT_MENU_COUNT);
			return 1;
		}
		if (key == 13)
		{
			activate(m, s_hover >= 0 ? s_hover : 0);
			return 1;
		}
		for (i = 0; i < s_item_count[m]; i++)
			if (match_label((char)key, s_items[m][i].label))
			{
				activate(m, i);
				return 1;
			}
		return 1;
	}
	return 0;
}

/* ---- mouse ------------------------------------------------------------- */

int pt_menus_mouse(int sx, int sy, int button, int down)
{
	int fresh;

	(void)button;
	if (!PT.active)
		return 0;
	if (!down)
	{
		s_btn = 0;
		s_last_sx = s_last_sy = -1;
		return pt_menus_active();
	}

	/* A press is "fresh" at a new position even without a release event: the
	 * frozen loop only forwards button-down events. */
	fresh = !s_btn || sx != s_last_sx || sy != s_last_sy;
	s_btn = 1;
	s_last_sx = sx;
	s_last_sy = sy;

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

		if (fresh)
		{
			int it = item_at(PT.menu, sx, sy);

			if (it >= 0)
			{
				activate(PT.menu, it);
				return 1;
			}
			if (t >= 0 && t != PT.menu)
			{
				open_menu(t);
				return 1;
			}
			close_menu();
			return 1;
		}
		/* Held: hovering a title switches, hovering an item highlights. */
		if (t >= 0 && t != PT.menu)
		{
			open_menu(t);
			return 1;
		}
		{
			int it = item_at(PT.menu, sx, sy);

			if (it >= 0 && it != s_hover)
			{
				s_hover = it;
				pt_request_redraw();
			}
		}
		return 1;
	}

	if (fresh)
	{
		int t = title_at(sx, sy);

		if (t >= 0)
		{
			open_menu(t);
			return 1;
		}
	}
	return 0;
}
