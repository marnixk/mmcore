#include "mmb_priv.h"
#include "frontend.h"
#include "tui.h"

/*
 * SETTINGS — system-wide appearance settings (#509).
 *
 * A theme picker that lists every built-in TUI theme with a live preview.
 * Moving the selection re-applies the candidate palette immediately, Enter
 * persists it (mmb_settings_save), and Esc restores the theme that was active
 * on entry. The choice is the same G.opt.edit_theme that EDIT, FILES,
 * WORDPAD, TERM, HELP and the other themed apps read, so it takes effect
 * everywhere the moment the picker closes.
 */

#define SET_ESC_IDLE_MS 60

typedef struct settings_state {
	int active;
	int sel, top;
	int orig;
	int esc;
	unsigned esc_at;
} settings_state;

static settings_state S_s[MMB_MAX_CONSOLES];
#define S (S_s[g_console])

static const mmb_ed_theme *stth(void)
{
	return mmb_editor_theme();
}

#define ST_TITLE_FG ((int)stth()->menu_fg)
#define ST_TITLE_BG ((int)stth()->menu_bg)
#define ST_FG       ((int)stth()->edit_fg)
#define ST_BG       ((int)stth()->edit_bg)
#define ST_SEL_FG   ((int)stth()->sel_fg)
#define ST_SEL_BG   ((int)stth()->sel_bg)
#define ST_HOT      ((int)stth()->hot)
#define ST_DIM      ((int)stth()->cmt_fg)
#define ST_STR      ((int)stth()->str_fg)
#define ST_NUM      ((int)stth()->num_fg)
#define ST_BRD_FG   ((int)stth()->brd_fg)
#define ST_BRD_BG   ((int)stth()->brd_bg)
#define ST_DLG_FG   ((int)stth()->dlg_fg)
#define ST_DLG_BG   ((int)stth()->dlg_bg)
#define ST_ERR_FG   ((int)stth()->err_fg)
#define ST_ERR_BG   ((int)stth()->err_bg)

static void st_center(int y, const char *s, int fg, int bg)
{
	int w = tui_cols();
	int n = (int)strlen(s);
	int x = (w - n) / 2;
	if (x < 1)
		x = 1;
	tui_fill(0, y, w, 1, ' ', fg, bg);
	tui_puts(x, y, s, fg, bg);
}

static void st_status_bar(const char *hint)
{
	int w = tui_cols();
	int h = tui_rows();
	int i, p;
	tui_fill(0, h - 1, w, 1, ' ', ST_DIM, ST_BG);
	for (i = 0, p = 1; hint[i] && p < w - 1; i++)
	{
		if (hint[i] == '<')
		{
			tui_put(p++, h - 1, '<', ST_HOT, ST_BG);
			i++;
			while (hint[i] && hint[i] != '>' && p < w - 1)
			{
				tui_put(p++, h - 1, (unsigned char)hint[i], ST_HOT, ST_BG);
				i++;
			}
			if (hint[i] == '>' && p < w - 1)
				tui_put(p++, h - 1, '>', ST_HOT, ST_BG);
		}
		else if (p < w - 1)
			tui_put(p++, h - 1, (unsigned char)hint[i], ST_DIM, ST_BG);
	}
}

/* A compact preview of the active theme's roles. Drawn to the right of the
 * list when there is room. */
static void st_preview(int x, int y, int w, int h)
{
	int row = y + 1;
	if (w < 12 || h < 6)
		return;
	tui_frame(x, y, w, h, ST_BRD_FG, ST_BRD_BG);
	tui_puts(x + 2, y, " PREVIEW ", ST_TITLE_FG, ST_TITLE_BG);
	tui_pad(x + 2, row++, "Menu", w - 4, ST_TITLE_FG, ST_TITLE_BG);
	tui_puts(x + 2, row, "Text", ST_FG, ST_BG);
	tui_puts(x + 8, row++, "String", ST_STR, ST_BG);
	tui_puts(x + 2, row, "Number", ST_NUM, ST_BG);
	tui_puts(x + 11, row++, "'comment'", ST_DIM, ST_BG);
	tui_pad(x + 2, row++, "Selected line", w - 4, ST_SEL_FG, ST_SEL_BG);
	tui_put(x + 2, row, '!', ST_ERR_FG, ST_ERR_BG);
	tui_puts(x + 4, row++, "error", ST_ERR_FG, ST_ERR_BG);
	if (row < y + h - 1)
		tui_puts(x + 2, row, "1 2 ... 10", ST_DLG_FG, ST_DLG_BG);
}

static void st_draw(void)
{
	int w = tui_cols();
	int h = tui_rows();
	int n = mmb_editor_theme_count();
	int listy = 3;
	int listw, listh, preview_x, i;

	if (!S.active)
		return;
	tui_begin();
	mmb_editor_apply_tui_palette();
	tui_clear(ST_FG, ST_BG);
	st_center(0, "SETTINGS", ST_TITLE_FG, ST_TITLE_BG);
	tui_puts(2, 1, "Theme (applies to EDIT, FILES, WORDPAD, TERM, HELP)", ST_STR, ST_BG);

	listw = 18;
	preview_x = listw + 4;
	if (preview_x + 26 > w)
		preview_x = -1; /* too narrow: list only */

	listh = h - listy - 2;
	if (listh < 1)
		listh = 1;
	if (S.sel < S.top)
		S.top = S.sel;
	if (S.sel >= S.top + listh)
		S.top = S.sel - listh + 1;
	if (S.top < 0)
		S.top = 0;

	for (i = 0; i < listh; i++)
	{
		int idx = S.top + i;
		int y = listy + i;
		int fg = ST_FG, bg = ST_BG;
		if (idx >= n)
		{
			tui_fill(1, y, w - 2, 1, ' ', ST_FG, ST_BG);
			continue;
		}
		if (idx == S.sel)
		{
			fg = ST_SEL_FG;
			bg = ST_SEL_BG;
		}
		tui_fill(1, y, listw + 2, 1, ' ', fg, bg);
		tui_puts(2, y, idx == S.sel ? ">" : " ", fg, bg);
		tui_puts(4, y, mmb_editor_theme_name(idx), fg, bg);
	}

	if (preview_x >= 0)
		st_preview(preview_x, listy, w - preview_x - 2, h - listy - 2);

	st_status_bar("<Up/Down> Preview  <Enter> Apply  <Esc> Cancel");
	tui_cursor(0, 0, 0);
	tui_flush();
}

static void st_apply(int idx)
{
	if (idx < 0 || idx >= mmb_editor_theme_count())
		return;
	G.opt.edit_theme = idx;
	mmb_editor_apply_tui_palette();
	tui_invalidate();
}

static void st_close(int commit)
{
	S.active = 0;
	S.esc = 0;
	tui_end();
	if (commit)
		mmb_settings_save();
	else if (G.opt.edit_theme != S.orig)
	{
		G.opt.edit_theme = S.orig;
	}
	mmb_console_write("\r\n");
}

static void st_move(int dir)
{
	int n = mmb_editor_theme_count();
	if (dir < 0)
	{
		if (S.sel > 0)
			S.sel--;
	}
	else if (S.sel + 1 < n)
		S.sel++;
	st_apply(S.sel);
}

int mmb_settings_active(void)
{
	return S.active;
}

void mmb_settings_open(void)
{
	int n = mmb_editor_theme_count();
	if (S.active || n <= 0)
		return;
	memset(&S, 0, sizeof(S));
	S.active = 1;
	S.orig = G.opt.edit_theme;
	S.sel = G.opt.edit_theme;
	if (S.sel < 0 || S.sel >= n)
		S.sel = 0;
	tui_begin();
	mmb_editor_apply_tui_palette();
	tui_invalidate();
	st_draw();
}

const char *mmb_settings_key(char c)
{
	G.outn = 0;
	G.out[0] = 0;
	if (!S.active)
		return G.out;
	if (S.esc)
	{
		if (S.esc == 1)
		{
			if (c == '[' || c == 'O')
			{
				S.esc = 2;
				return G.out;
			}
			S.esc = 0;
			st_close(0);
			return G.out;
		}
		S.esc = 0;
		if (c == 'A')
			st_move(-1);
		else if (c == 'B')
			st_move(1);
		if (S.active)
			st_draw();
		return G.out;
	}
	if (c == 27)
	{
		S.esc = 1;
		S.esc_at = mmb_now_ms();
		return G.out;
	}
	if (c == '\r' || c == '\n')
		st_close(1);
	else if (c == 'k' || c == 'K')
		st_move(-1);
	else if (c == 'j' || c == 'J')
		st_move(1);
	else if (c >= '1' && c <= '9')
		st_apply(c - '1');
	else if (c == '0')
		st_apply(9);
	if (S.active)
		st_draw();
	return G.out;
}

void mmb_settings_poll(void)
{
	if (!S.active || S.esc != 1)
		return;
	if (mmb_now_ms() - S.esc_at < SET_ESC_IDLE_MS)
		return;
	S.esc = 0;
	st_close(0);
	if (!S.active)
		mmb_front_prompt();
}

void mmb_cmd_settings(void)
{
	mmb_skip_sp();
	if (*G.p && *G.p != '\'' && *G.p != 0)
	{
		/* SETTINGS THEME <name|n> sets it directly, no UI. */
		if (mmb_match("THEME"))
			mmb_skip_sp();
		if (*G.p == '"')
		{
			int id;
			mmb_val v = mmb_expr();
			if (v.type != T_STR)
				mmb_syntax();
			id = mmb_editor_theme_lookup(v.s);
			if (id < 0)
				mmb_error("?THEME");
			G.opt.edit_theme = id;
		}
		else
		{
			int i, n = mmb_editor_theme_count(), id = -1;
			for (i = 0; i < n; i++)
			{
				if (mmb_match(mmb_editor_theme_name(i)))
				{
					id = i;
					break;
				}
			}
			if (id < 0)
				id = (int)mmb_as_int(mmb_expr());
			if (id < 0 || id >= n)
				mmb_error("?THEME");
			G.opt.edit_theme = id;
		}
		mmb_settings_save();
		return;
	}
	mmb_settings_open();
}
