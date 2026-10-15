#include "mmb_priv.h"
#include "frontend.h"
#include "tui.h"

#include <stdio.h>

/*
 * SETTINGS — system-wide settings hub (#509, #590, #591).
 *
 * A small modal dialog (see tui_dialog_panel) rather than a full-screen
 * takeover. The hub lists categories; Settings hosts the brightness/volume
 * sliders and status line (#1158), Appearance hosts the theme picker from
 * #509 (live preview, Enter persists via mmb_settings_save, Esc restores the
 * theme active on entry). Network is a peer slot so the product can grow
 * without another one-off UI. Esc backs a section to the hub and closes from
 * the hub, returning to the prompt.
 */

#define SET_ESC_IDLE_MS 60

#define ST_SEC_SETTINGS   0
#define ST_SEC_APPEARANCE 1
#define ST_SEC_NETWORK    2
#define ST_SEC_COUNT      3

static const char *const st_sec_name[ST_SEC_COUNT] = {
	"Settings", "Appearance", "Network"
};

static const char *const st_sec_desc[ST_SEC_COUNT] = {
	"Brightness, volume and status",
	"Theme and interface colours",
	"Wi-Fi, Ethernet and NTP"
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

/* Natural size of the live PREVIEW swatch: 6 content rows plus a border. */
#define ST_PREV_W      28
#define ST_PREV_ROWS   6
/* Below these the swatch is dropped rather than drawn clipped/overlapping. */
#define ST_PREV_MIN_W  22
#define ST_PREV_MIN_H  6

/* A compact preview of the active theme's roles. The rect is pre-clamped by
 * the caller; every line is guarded so nothing is written below the frame. */
static void st_preview(int x, int y, int w, int h)
{
	int row = y + 1;
	int last = y + h - 1; /* first row past the bottom border */

	if (w < ST_PREV_MIN_W || h < ST_PREV_MIN_H)
		return;
	tui_frame(x, y, w, h, ST_BRD_FG, ST_BRD_BG);
	tui_puts(x + 2, y, " PREVIEW ", ST_TITLE_FG, ST_TITLE_BG);
	if (row < last)
		tui_pad(x + 2, row++, "Menu", w - 4, ST_TITLE_FG, ST_TITLE_BG);
	if (row < last)
	{
		tui_puts(x + 2, row, "Text", ST_EDIT_FG, ST_EDIT_BG);
		tui_puts(x + 8, row++, "String", ST_STR, ST_EDIT_BG);
	}
	if (row < last)
	{
		tui_puts(x + 2, row, "Number", ST_NUM, ST_EDIT_BG);
		tui_puts(x + 11, row++, "'comment'", ST_CMT, ST_EDIT_BG);
	}
	if (row < last)
		tui_pad(x + 2, row++, "Selected line", w - 4, ST_SEL_FG, ST_SEL_BG);
	if (row < last)
	{
		tui_put(x + 2, row, '!', ST_ERR_FG, ST_ERR_BG);
		tui_puts(x + 4, row++, "error", ST_ERR_FG, ST_ERR_BG);
	}
	if (row < last)
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
	int hint_row = y + h - 2;
	int listh = hint_row - listy;
	int listw = 20;
	int lw;
	int preview_x = listw + 4; /* one blank column after the list */
	int avail_w, avail_h, pw, ph, i;

	if (listh < 1)
		listh = 1;
	if (S.sel < S.top)
		S.top = S.sel;
	if (S.sel >= S.top + listh)
		S.top = S.sel - listh + 1;
	if (S.top < 0)
		S.top = 0;

	lw = listw + 2;
	if (lw > w - 2)
		lw = w - 2;
	if (lw < 1)
		lw = 1;

	tui_puts(x + 2, y + 2, "Appearance - theme", ST_HOT, ST_BG);

	for (i = 0; i < listh; i++)
	{
		int idx = S.top + i;
		int ry = listy + i;
		int fg = ST_FG, bg = ST_BG;
		if (idx >= n)
		{
			tui_fill(x + 1, ry, lw, 1, ' ', ST_FG, ST_BG);
			continue;
		}
		if (idx == S.sel)
		{
			fg = ST_SEL_FG;
			bg = ST_SEL_BG;
		}
		tui_fill(x + 1, ry, lw, 1, ' ', fg, bg);
		tui_puts(x + 3, ry, idx == S.sel ? ">" : " ", fg, bg);
		if (lw > 6)
			tui_pad(x + 5, ry, mmb_editor_theme_name(idx), lw - 4,
				fg, bg);
	}

	/* Size the swatch from the panel interior, not a fixed guess. The
	 * preview may never share the bottom hint row or spill past the frame. */
	avail_w = (x + w - 2) - (x + preview_x);
	avail_h = (hint_row - 1) - (y + 4) + 1;
	pw = ST_PREV_W;
	if (pw > avail_w)
		pw = avail_w;
	ph = ST_PREV_ROWS + 2;
	if (ph > avail_h)
		ph = avail_h;
	if (pw >= ST_PREV_MIN_W && ph >= ST_PREV_MIN_H)
	{
		int cx = x + preview_x + (avail_w - pw) / 2;
		st_preview(cx, y + 4, pw, ph);
	}
	st_hint(x + 1, hint_row, w - 2,
		"<Up/Down> Preview  <Enter> Apply  <Esc> Back");
}

/* Current IP address from the same source IPCONFIG formats, without starting
 * a connection or waiting on DHCP: an unconnected link reports no address. */
static int st_network_ip(char *buf, int bufsize)
{
	buf[0] = 0;
	if (G.opt.ethernet_enabled || mmb_net_kind() == MMB_NET_ETH)
		return mmb_eth_ip(buf, bufsize) == 0 && buf[0];
	return mmb_wlan_ip(buf, bufsize) == 0 && buf[0];
}

static void st_draw_network(int x, int y, int w, int h)
{
	char line[160];
	char ip[64];
	int r = y + 2;
	const char *ssid = G.opt.wifi_ssid[0] ? G.opt.wifi_ssid : "(none)";

	(void)w;
	tui_puts(x + 2, r++, "Network", ST_HOT, ST_BG);
	tui_puts(x + 2, r++, mmb_net_available() ? "Link: up" : "Link: none",
		 mmb_net_available() ? ST_FG : ST_ERR_FG,
		 mmb_net_available() ? ST_BG : ST_ERR_BG);
	tui_puts(x + 2, r, "IP       :", ST_FG, ST_BG);
	if (st_network_ip(ip, (int)sizeof(ip)))
		tui_puts(x + 13, r++, ip, ST_FG, ST_BG);
	else
		tui_puts(x + 13, r++, "none", ST_ERR_FG, ST_ERR_BG);
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
	tui_puts(x + 2, r++, "Use OPTION WIFI / CONNECT / OPTION NTP at the prompt.",
		 ST_FG, ST_BG);
	st_hint(x + 1, y + h - 2, w - 2, "<Esc> Back");
}

/* ---- Settings tab (#1158): brightness/volume sliders, battery, clock ------ */

static const char *const st_months[12] = {
	"Jan", "Feb", "Mar", "Apr", "May", "Jun",
	"Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

/* Local date and time as "19:20 / 3 Oct 2026" from the clock strings the
 * DATE$/TIME$ functions maintain. */
static void st_clock_text(char *out, int outsz)
{
	const char *s;
	int d = 0, mo = 0, y = 0;

	mmb_clock_refresh();
	s = G.date_s;
	while (*s >= '0' && *s <= '9')
		d = d * 10 + (*s++ - '0');
	if (*s == '-')
		s++;
	while (*s >= '0' && *s <= '9')
		mo = mo * 10 + (*s++ - '0');
	if (*s == '-')
		s++;
	while (*s >= '0' && *s <= '9')
		y = y * 10 + (*s++ - '0');
	if (mo < 1 || mo > 12)
		mo = 1;
	snprintf(out, (size_t)outsz, "%.5s / %d %s %d",
		 G.time_s[0] ? G.time_s : "12:00", d,
		 st_months[mo - 1], 2000 + y);
}

/* Status line for the whole SETTINGS app: local date/time top-left and the
 * battery badge "[80%]" (coloured red / blue / green by charge band)
 * top-right, both on row 0 of the screen so they sit OUTSIDE the dialog
 * chrome on the backdrop, not inside the panel body. */
static void st_draw_status(void)
{
	static const int band[3] = { TUI_RED, TUI_BLUE, TUI_GREEN };
	char dt[64];
	char bat[8];
	int cols = tui_cols();
	int pct = mmb_battery_percent();
	int bx;

	if (cols < 1)
		return;
	st_clock_text(dt, (int)sizeof(dt));
	tui_pad(0, 0, dt, cols, ST_TITLE_FG, ST_TITLE_BG);
	snprintf(bat, sizeof(bat), "[%d%%]", pct);
	bx = cols - (int)strlen(bat);
	if (bx < 2)
		bx = 2;
	tui_puts(bx, 0, bat, band[mmb_battery_status_class(pct)], ST_TITLE_BG);
}

/* A [====------] slider; the focused one uses the selection surface. */
static void st_slider(int x, int y, int width, int pct, int focused)
{
	int fg = focused ? ST_SEL_FG : ST_FG;
	int bg = focused ? ST_SEL_BG : ST_BG;
	int fill = (pct * width + 50) / 100;

	if (fill < 0)
		fill = 0;
	if (fill > width)
		fill = width;
	tui_put(x, y, '[', fg, bg);
	tui_fill(x + 1, y, fill, 1, '=', fg, bg);
	tui_fill(x + 1 + fill, y, width - fill, 1, '-', fg, bg);
	tui_put(x + width + 1, y, ']', fg, bg);
}

static void st_draw_settings(int x, int y, int w, int h)
{
	static const char *const labels[2] = { "Brightness", "Volume" };
	int slider_x = x + 15;
	int slider_w = 30;
	int pct[2];
	int i;

	pct[0] = G.opt.brightness;
	pct[1] = g_audio.vol_l;

	tui_puts(x + 2, y + 2, "Brightness and volume", ST_HOT, ST_BG);

	for (i = 0; i < 2; i++)
	{
		int ry = y + 4 + i * 2;
		int fg = (S.sel == i) ? ST_SEL_FG : ST_FG;
		int bg = (S.sel == i) ? ST_SEL_BG : ST_BG;
		char val[8];

		tui_fill(x + 1, ry, w - 2, 1, ' ', fg, bg);
		tui_puts(x + 2, ry, S.sel == i ? ">" : " ", fg, bg);
		tui_pad(x + 4, ry, labels[i], 10, fg, bg);
		st_slider(slider_x, ry, slider_w, pct[i], S.sel == i);
		snprintf(val, sizeof(val), "%3d%%", pct[i]);
		tui_puts(slider_x + slider_w + 3, ry, val, fg, bg);
	}
	st_hint(x + 1, y + h - 2, w - 2,
		"<Up/Down> Select  <Left/Right> Adjust  <Esc> Back");
}

static void st_draw_section(void)
{
	int x, y, w, h;
	char title[48];

	title[0] = 0;
	strncat(title, "SETTINGS - ", sizeof(title) - 1);
	strncat(title, st_sec_name[S.sec], sizeof(title) - strlen(title) - 1);
	if (S.sec == ST_SEC_APPEARANCE)
		st_panel(title, 72, 18, &x, &y, &w, &h);
	else if (S.sec == ST_SEC_SETTINGS)
		st_panel(title, 60, 10, &x, &y, &w, &h);
	else
		st_panel(title, 68, 13, &x, &y, &w, &h);

	switch (S.sec)
	{
	case ST_SEC_SETTINGS:
		st_draw_settings(x, y, w, h);
		break;
	case ST_SEC_APPEARANCE:
		st_draw_theme(x, y, w, h);
		break;
	default:
		st_draw_network(x, y, w, h);
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
	st_draw_status();
	tui_cursor(0, 0, 0);
	tui_flush();
}

static void st_apply(int idx)
{
	if (idx < 0 || idx >= mmb_editor_theme_count())
		return;
	G.opt.theme = idx;
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
	else if (G.opt.theme != S.orig)
	{
		G.opt.theme = S.orig;
	}
	/* Put the REPL screen/scrollback back (overlay, #623); only add a line
	 * break when the platform has no snapshot to restore. */
	if (!tui_overlay_end())
		mmb_console_write("\r\n");
}

static void st_escape(void)
{
	if (S.level == 1)
	{
		if (S.sec == ST_SEC_APPEARANCE && G.opt.theme != S.orig)
		{
			G.opt.theme = S.orig;
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
		S.sel = G.opt.theme;
		S.top = 0;
		if (S.sel < 0 || S.sel >= mmb_editor_theme_count())
			S.sel = 0;
	}
	else if (sec == ST_SEC_SETTINGS)
		S.sel = 0;
}

/* #1158: Left/Right nudges the focused Settings slider, live. */
static void st_adjust(int dir)
{
	if (S.level != 1 || S.sec != ST_SEC_SETTINGS)
		return;
	if (S.sel == 0)
	{
		int pct = G.opt.brightness + dir * 5;
		if (pct < 0)
			pct = 0;
		if (pct > 100)
			pct = 100;
		G.opt.brightness = pct;
#if defined(MMB_PLATFORM_POSIX)
		(void)mmb_brightness_set(pct);
#endif
	}
	else
	{
		int vol = g_audio.vol_l + dir * 5;
		if (vol < 0)
			vol = 0;
		if (vol > 100)
			vol = 100;
		G.opt.volume_l = g_audio.vol_l = vol;
		G.opt.volume_r = g_audio.vol_r = vol;
	}
	mmb_settings_save();
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
	if (S.sec == ST_SEC_SETTINGS)
	{
		if (dir < 0)
		{
			if (S.sel > 0)
				S.sel--;
		}
		else if (S.sel < 1)
			S.sel++;
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
	S.hub_sel = ST_SEC_SETTINGS;
	S.sec = ST_SEC_SETTINGS;
	S.orig = G.opt.theme;
	S.sel = G.opt.theme;
	if (S.sel < 0 || S.sel >= n)
		S.sel = 0;
	tui_overlay_begin();
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
		else if (c == 'C')
			st_adjust(1);
		else if (c == 'D')
			st_adjust(-1);
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
			G.opt.theme = id;
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
			G.opt.theme = id;
		}
		mmb_settings_save();
		return;
	}
	mmb_settings_open();
}
