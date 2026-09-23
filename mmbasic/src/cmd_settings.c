#include "mmb_priv.h"
#include "frontend.h"
#include "tui.h"

/*
 * SETTINGS — system-wide settings hub (#509, #590, #591).
 *
 * A small modal dialog (see tui_dialog_panel) rather than a full-screen
 * takeover. The hub lists categories; Appearance hosts the theme picker from
 * #509 (live preview, Enter persists via mmb_settings_save, Esc restores the
 * theme active on entry). Network, System and Sound are peer slots so the
 * product can grow without another one-off UI. Esc backs a section to the hub
 * and closes from the hub, returning to the prompt.
 */

#define SET_ESC_IDLE_MS 60

#define ST_SEC_APPEARANCE 0
#define ST_SEC_NETWORK    1
#define ST_SEC_SYSTEM     2
#define ST_SEC_SOUND      3
#define ST_SEC_COUNT      4

static const char *const st_sec_name[ST_SEC_COUNT] = {
	"Appearance", "Network", "System", "Sound"
};

static const char *const st_sec_desc[ST_SEC_COUNT] = {
	"Theme and interface colours",
	"Wi-Fi, Ethernet and NTP",
	"Boot destination and REPL options",
	"Audio output and mute"
};

typedef struct settings_state {
	int active;
	int level; /* 0 hub, 1 section body */
	int sec;   /* section shown while level == 1 */
	int hub_sel, hub_top;
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

/*
 * SETTINGS role map (#611). Every pair below is asserted legible by the
 * static audit in tests/test_settings_theme_contrast.py and by the QEMU
 * matrix in tests/test_settings.py, so a new surface cannot reintroduce the
 * Turbo wash-out (body text drawn edit_fg-on-dlg_bg, hot keys on edit_bg):
 *
 *   title   menu_fg / menu_bg   dialog title bar and backdrop
 *   body    dlg_fg  / dlg_bg    dialog body text, labels, values and rows
 *   select  sel_fg  / sel_bg    focused row
 *   hot     hot     / dlg_bg    section headings and <key> hints
 *   error   err_fg  / err_bg    error chip / down link
 *   border  brd_fg  / brd_bg    panel frame
 *
 * The editor-syntax roles (edit/str/num/cmt) are reserved for the live
 * PREVIEW swatch, which deliberately shows the editor's own surfaces.
 */
#define ST_TITLE_FG ((int)stth()->menu_fg)
#define ST_TITLE_BG ((int)stth()->menu_bg)
#define ST_FG       ((int)stth()->dlg_fg)
#define ST_BG       ((int)stth()->dlg_bg)
#define ST_SEL_FG   ((int)stth()->sel_fg)
#define ST_SEL_BG   ((int)stth()->sel_bg)
#define ST_HOT      ((int)stth()->hot)
#define ST_BRD_FG   ((int)stth()->brd_fg)
#define ST_BRD_BG   ((int)stth()->brd_bg)
#define ST_ERR_FG   ((int)stth()->err_fg)
#define ST_ERR_BG   ((int)stth()->err_bg)
/* Editor roles: only the PREVIEW swatch uses these. */
#define ST_EDIT_FG  ((int)stth()->edit_fg)
#define ST_EDIT_BG  ((int)stth()->edit_bg)
#define ST_STR      ((int)stth()->str_fg)
#define ST_NUM      ((int)stth()->num_fg)
#define ST_CMT      ((int)stth()->cmt_fg)

static void st_hint(int x, int row, int width, const char *hint)
{
	tui_status_hint_at(x, row, width, hint, ST_HOT, ST_FG, ST_BG);
}

/* Draw the modal panel backdrop and frame; returns the panel rect. */
static void st_panel(const char *title, int want_w, int want_h,
		     int *px, int *py, int *pw, int *ph)
{
	tui_clear(ST_TITLE_FG, ST_TITLE_BG);
	tui_dialog_geom(want_w, want_h, px, py, pw, ph);
	tui_dialog_panel(*px, *py, *pw, *ph, title, ST_FG, ST_BG,
			 ST_BRD_FG, ST_BRD_BG, ST_TITLE_FG, ST_TITLE_BG);
}

/* A compact preview of the active theme's roles. */
static void st_preview(int x, int y, int w, int h)
{
	int row = y + 1;
	if (w < 12 || h < 6)
		return;
	tui_frame(x, y, w, h, ST_BRD_FG, ST_BRD_BG);
	tui_puts(x + 2, y, " PREVIEW ", ST_TITLE_FG, ST_TITLE_BG);
	tui_pad(x + 2, row++, "Menu", w - 4, ST_TITLE_FG, ST_TITLE_BG);
	tui_puts(x + 2, row, "Text", ST_EDIT_FG, ST_EDIT_BG);
	tui_puts(x + 8, row++, "String", ST_STR, ST_EDIT_BG);
	tui_puts(x + 2, row, "Number", ST_NUM, ST_EDIT_BG);
	tui_puts(x + 11, row++, "'comment'", ST_CMT, ST_EDIT_BG);
	tui_pad(x + 2, row++, "Selected line", w - 4, ST_SEL_FG, ST_SEL_BG);
	tui_put(x + 2, row, '!', ST_ERR_FG, ST_ERR_BG);
	tui_puts(x + 4, row++, "error", ST_ERR_FG, ST_ERR_BG);
	if (row < y + h - 1)
		tui_puts(x + 2, row, "1 2 ... 10", ST_FG, ST_BG);
}

static void st_draw_hub(void)
{
	int x, y, w, h, listy, listh, i;

	st_panel("SETTINGS", 68, 13, &x, &y, &w, &h);
	tui_puts(x + 2, y + 2, "Settings hub - choose a category.", ST_HOT, ST_BG);

	listy = y + 4;
	listh = (y + h - 2) - listy;
	if (listh < 1)
		listh = 1;
	if (S.hub_sel < S.hub_top)
		S.hub_top = S.hub_sel;
	if (S.hub_sel >= S.hub_top + listh)
		S.hub_top = S.hub_sel - listh + 1;
	if (S.hub_top < 0)
		S.hub_top = 0;

	for (i = 0; i < listh; i++)
	{
		int idx = S.hub_top + i;
		int ry = listy + i;
		int fg = ST_FG, bg = ST_BG;
		if (idx >= ST_SEC_COUNT)
		{
			tui_fill(x + 1, ry, w - 2, 1, ' ', ST_FG, ST_BG);
			continue;
		}
		if (idx == S.hub_sel)
		{
			fg = ST_SEL_FG;
			bg = ST_SEL_BG;
		}
		tui_fill(x + 1, ry, w - 2, 1, ' ', fg, bg);
		tui_puts(x + 3, ry, idx == S.hub_sel ? ">" : " ", fg, bg);
		tui_puts(x + 5, ry, st_sec_name[idx], fg, bg);
		tui_puts(x + 22, ry, st_sec_desc[idx], fg, bg);
	}
	st_hint(x + 1, y + h - 2, w - 2,
		"<Up/Down> Move  <Enter> Open  <Esc> Close");
}

static void st_draw_theme(int x, int y, int w, int h)
{
	int n = mmb_editor_theme_count();
	int listy = y + 4;
	int listh = (y + h - 2) - listy;
	int listw = 20;
	int preview_x = listw + 2;
	int i;

	if (listh < 1)
		listh = 1;
	if (S.sel < S.top)
		S.top = S.sel;
	if (S.sel >= S.top + listh)
		S.top = S.sel - listh + 1;
	if (S.top < 0)
		S.top = 0;

	tui_puts(x + 2, y + 2, "Appearance - theme", ST_HOT, ST_BG);

	for (i = 0; i < listh; i++)
	{
		int idx = S.top + i;
		int ry = listy + i;
		int fg = ST_FG, bg = ST_BG;
		if (idx >= n)
		{
			tui_fill(x + 1, ry, listw + 2, 1, ' ', ST_FG, ST_BG);
			continue;
		}
		if (idx == S.sel)
		{
			fg = ST_SEL_FG;
			bg = ST_SEL_BG;
		}
		tui_fill(x + 1, ry, listw + 2, 1, ' ', fg, bg);
		tui_puts(x + 3, ry, idx == S.sel ? ">" : " ", fg, bg);
		tui_puts(x + 5, ry, mmb_editor_theme_name(idx), fg, bg);
	}

	if (preview_x + 28 <= w - 2)
		st_preview(x + preview_x, y + 4, 28, listh + 1);
	st_hint(x + 1, y + h - 2, w - 2,
		"<Up/Down> Preview  <Enter> Apply  <Esc> Back");
}

static void st_draw_network(int x, int y, int w, int h)
{
	char line[160];
	int r = y + 2;
	const char *ssid = G.opt.wifi_ssid[0] ? G.opt.wifi_ssid : "(none)";

	(void)w;
	tui_puts(x + 2, r++, "Network", ST_HOT, ST_BG);
	tui_puts(x + 2, r++, mmb_net_available() ? "Link: up" : "Link: none",
		 mmb_net_available() ? ST_FG : ST_ERR_FG,
		 mmb_net_available() ? ST_BG : ST_ERR_BG);
	r++;
	tui_puts(x + 2, r, "Wi-Fi    :", ST_FG, ST_BG);
	tui_puts(x + 13, r++, ssid, ST_FG, ST_BG);
	tui_puts(x + 2, r, "Ethernet :", ST_FG, ST_BG);
	tui_puts(x + 13, r++, G.opt.ethernet_enabled ? "ON" : "OFF", ST_FG, ST_BG);
	tui_puts(x + 2, r, "NTP      :", ST_FG, ST_BG);
	line[0] = 0;
	strncat(line, G.opt.ntp_enabled ? "ON  " : "OFF ", sizeof(line) - 1);
	strncat(line, G.opt.ntp_server[0] ? G.opt.ntp_server : MMB_NTP_DEFAULT_SERVER,
		sizeof(line) - strlen(line) - 1);
	tui_puts(x + 13, r++, line, ST_FG, ST_BG);
	r++;
	tui_puts(x + 2, r++, "Use OPTIONS WIFI / CONNECT / OPTION NTP at the prompt.",
		 ST_FG, ST_BG);
	st_hint(x + 1, y + h - 2, w - 2, "<Esc> Back");
}

static void st_draw_system(int x, int y, int w, int h)
{
	int r = y + 2;
	const char *boot = "REPL";

	(void)w;
	if (G.opt.boot_mode == 1)
		boot = "Launcher";
	else if (G.opt.boot_mode == 2)
		boot = "App";
	tui_puts(x + 2, r++, "System", ST_HOT, ST_BG);
	r++;
	tui_puts(x + 2, r, "Boot      :", ST_FG, ST_BG);
	tui_puts(x + 14, r++, boot, ST_FG, ST_BG);
	tui_puts(x + 2, r, "App path  :", ST_FG, ST_BG);
	tui_puts(x + 14, r++, G.opt.app_path, ST_FG, ST_BG);
	tui_puts(x + 2, r, "Prompt    :", ST_FG, ST_BG);
	tui_puts(x + 14, r++, G.opt.prompt ? "CWD" : "BARE", ST_FG, ST_BG);
	tui_puts(x + 2, r, "Theme     :", ST_FG, ST_BG);
	tui_puts(x + 14, r++, mmb_editor_theme_name(G.opt.edit_theme), ST_FG,
		 ST_BG);
	r++;
	tui_puts(x + 2, r++, "Change these with OPTION at the prompt.",
		 ST_FG, ST_BG);
	st_hint(x + 1, y + h - 2, w - 2, "<Esc> Back");
}

static void st_draw_sound(int x, int y, int w, int h)
{
	int r = y + 2;

	(void)w;
	tui_puts(x + 2, r++, "Sound", ST_HOT, ST_BG);
	r++;
	tui_puts(x + 2, r, "Audio     :", ST_FG, ST_BG);
	tui_puts(x + 14, r++, G.opt.audio_on ? "ON" : "OFF", ST_FG, ST_BG);
	tui_puts(x + 2, r, "Output    :", ST_FG, ST_BG);
	tui_puts(x + 14, r++, G.opt.audio_target ? "HDMI" : "JACK", ST_FG, ST_BG);
	r++;
	tui_puts(x + 2, r++, "Use OPTION AUDIO / AUDIO OUTPUT at the prompt.",
		 ST_FG, ST_BG);
	st_hint(x + 1, y + h - 2, w - 2, "<Esc> Back");
}

static void st_draw_section(void)
{
	int x, y, w, h;
	char title[48];

	title[0] = 0;
	strncat(title, "SETTINGS - ", sizeof(title) - 1);
	strncat(title, st_sec_name[S.sec], sizeof(title) - strlen(title) - 1);
	if (S.sec == ST_SEC_APPEARANCE)
		st_panel(title, 68, 16, &x, &y, &w, &h);
	else
		st_panel(title, 68, 13, &x, &y, &w, &h);

	switch (S.sec)
	{
	case ST_SEC_APPEARANCE:
		st_draw_theme(x, y, w, h);
		break;
	case ST_SEC_NETWORK:
		st_draw_network(x, y, w, h);
		break;
	case ST_SEC_SYSTEM:
		st_draw_system(x, y, w, h);
		break;
	default:
		st_draw_sound(x, y, w, h);
		break;
	}
}

static void st_draw(void)
{
	if (!S.active)
		return;
	tui_begin();
	mmb_editor_apply_tui_palette();
	if (S.level == 0)
		st_draw_hub();
	else
		st_draw_section();
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
	S.level = 0;
	tui_end();
	if (commit)
		mmb_settings_save();
	else if (G.opt.edit_theme != S.orig)
	{
		G.opt.edit_theme = S.orig;
	}
	mmb_console_write("\r\n");
}

static void st_escape(void)
{
	if (S.level == 1)
	{
		if (S.sec == ST_SEC_APPEARANCE && G.opt.edit_theme != S.orig)
		{
			G.opt.edit_theme = S.orig;
			mmb_editor_apply_tui_palette();
			tui_invalidate();
		}
		S.level = 0;
		return;
	}
	st_close(0);
}

static void st_open_section(int sec)
{
	if (sec < 0 || sec >= ST_SEC_COUNT)
		return;
	S.sec = sec;
	S.level = 1;
	if (sec == ST_SEC_APPEARANCE)
	{
		S.sel = G.opt.edit_theme;
		S.top = 0;
		if (S.sel < 0 || S.sel >= mmb_editor_theme_count())
			S.sel = 0;
	}
}

static void st_move(int dir)
{
	if (S.level == 0)
	{
		if (dir < 0)
		{
			if (S.hub_sel > 0)
				S.hub_sel--;
		}
		else if (S.hub_sel + 1 < ST_SEC_COUNT)
			S.hub_sel++;
		return;
	}
	if (S.sec != ST_SEC_APPEARANCE)
		return;
	if (dir < 0)
	{
		if (S.sel > 0)
			S.sel--;
	}
	else if (S.sel + 1 < mmb_editor_theme_count())
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
	S.level = 0;
	S.hub_sel = ST_SEC_APPEARANCE;
	S.sec = ST_SEC_APPEARANCE;
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
			st_escape();
			if (S.active)
				st_draw();
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
	{
		if (S.level == 0)
			st_open_section(S.hub_sel);
		else if (S.sec == ST_SEC_APPEARANCE)
			st_close(1);
		else
			S.level = 0;
	}
	else if (c == 'k' || c == 'K')
		st_move(-1);
	else if (c == 'j' || c == 'J')
		st_move(1);
	else if (S.level == 1 && S.sec == ST_SEC_APPEARANCE && c >= '1' && c <= '9')
		st_apply(c - '1');
	else if (S.level == 1 && S.sec == ST_SEC_APPEARANCE && c == '0')
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
	st_escape();
	if (S.active)
		st_draw();
	else
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
