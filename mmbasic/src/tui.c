#include "mmb_priv.h"
#include "tui.h"

typedef struct {
	unsigned char ch;
	unsigned char fg;
	unsigned char bg;
} tui_cell;

/* TUI composition state is per console: each virtual console can host a
 * different full-screen app, and switching restores both the app state and
 * its pixel buffer. */
typedef struct {
	tui_cell front[TUI_MAX_ROWS][TUI_MAX_COLS];
	tui_cell shown[TUI_MAX_ROWS][TUI_MAX_COLS];
	unsigned char row_dirty[TUI_MAX_ROWS];
	int cols, rows;
	int inited;
	int cur_x, cur_y, cur_vis;
	int prev_cx, prev_cy;
	int pal_init;
	unsigned palette[16];
} tui_state;

static tui_state s_tui[MMB_MAX_CONSOLES];
#define TS        (s_tui[g_console])
#define front     (TS.front)
#define shown     (TS.shown)
#define row_dirty (TS.row_dirty)
#define cols      (TS.cols)
#define rows      (TS.rows)
#define inited    (TS.inited)
#define cur_x     (TS.cur_x)
#define cur_y     (TS.cur_y)
#define cur_vis   (TS.cur_vis)
#define prev_cx   (TS.prev_cx)
#define prev_cy   (TS.prev_cy)
#define pal_init  (TS.pal_init)
#define palette   (TS.palette)

#define TUI_VGA_PALETTE \
	0x000000, 0xAA0000, 0x00AA00, 0xAA5500, \
	0x0000AA, 0xAA00AA, 0x00AAAA, 0xAAAAAA, \
	0x555555, 0xFF5555, 0x55FF55, 0xFFFF55, \
	0x5555FF, 0xFF55FF, 0x55FFFF, 0xFFFFFF

static const unsigned k_vga[16] = { TUI_VGA_PALETTE };

static int clampi(int v, int lo, int hi)
{
	if (v < lo)
		return lo;
	if (v > hi)
		return hi;
	return v;
}

static int query_cols(void)
{
	int w, c;
	if (G.plat && G.plat->video_cols)
	{
		c = G.plat->video_cols();
		if (c > 0)
			return clampi(c, 20, TUI_MAX_COLS);
	}
	w = (G.plat && G.plat->hdmi_width) ? G.plat->hdmi_width() : 640;
	if (w < 8)
		w = 640;
	return clampi(w / 8, 20, TUI_MAX_COLS);
}

static int query_rows(void)
{
	int h, r;
	if (G.plat && G.plat->video_rows)
	{
		r = G.plat->video_rows();
		if (r > 0)
			return clampi(r, 10, TUI_MAX_ROWS);
	}
	h = (G.plat && G.plat->hdmi_height) ? G.plat->hdmi_height() : 480;
	if (h < 16)
		h = 480;
	return clampi(h / 16, 10, TUI_MAX_ROWS);
}

static char ascii_box(unsigned char ch)
{
	switch (ch)
	{
	case TUI_V:
		return '|';
	case TUI_H:
		return '-';
	case TUI_TL:
	case TUI_TR:
	case TUI_BL:
	case TUI_BR:
	case TUI_LT:
	case TUI_RT:
	case TUI_TT:
	case TUI_BT:
	case TUI_X:
		return '+';
	default:
		if (ch < 32 || ch > 126)
			return ' ';
		return (char)ch;
	}
}

static void hide_hw_cursor(int hide)
{
	const char *s = hide ? "\x1b[?25l" : "\x1b[?25h";
	if (G.plat && G.plat->write_screen)
		G.plat->write_screen(s, 6);
}

int tui_cols(void)
{
	return cols > 0 ? cols : query_cols();
}

int tui_rows(void)
{
	return rows > 0 ? rows : query_rows();
}

void tui_begin(void)
{
	int y, x;
	int c = query_cols();
	int r = query_rows();
	if (!pal_init)
	{
		for (y = 0; y < 16; y++)
			palette[y] = k_vga[y];
		pal_init = 1;
	}
	if (inited && c == cols && r == rows)
		return;
	cols = c;
	rows = r;
	for (y = 0; y < rows; y++)
	{
		row_dirty[y] = 1;
		for (x = 0; x < cols; x++)
		{
			front[y][x].ch = ' ';
			front[y][x].fg = TUI_WHITE;
			front[y][x].bg = TUI_BLACK;
			shown[y][x].ch = 0xFF;
			shown[y][x].fg = 0xFF;
			shown[y][x].bg = 0xFF;
		}
	}
	cur_x = cur_y = prev_cx = prev_cy = -1;
	cur_vis = 0;
	inited = 1;
	if (G.plat && G.plat->tui_prepare)
		G.plat->tui_prepare();
	hide_hw_cursor(1);
	if (G.plat && G.plat->write_screen)
		G.plat->write_screen("\x1b[H\x1b[J", 7);
}

void tui_invalidate(void)
{
	int y, x;
	if (!inited)
	{
		tui_begin();
		return;
	}
	for (y = 0; y < rows; y++)
		for (x = 0; x < cols; x++)
		{
			shown[y][x].ch = 0xFF;
			shown[y][x].fg = 0xFF;
			shown[y][x].bg = 0xFF;
		}
	prev_cx = prev_cy = -1;
}

void tui_set_palette(const unsigned *rgb16)
{
	const unsigned *src = rgb16 ? rgb16 : k_vga;
	int i, changed = 0;
	for (i = 0; i < 16; i++)
	{
		unsigned v = src[i] & 0xFFFFFFu;
		if (palette[i] != v)
		{
			palette[i] = v;
			changed = 1;
		}
	}
	if (changed && inited)
		tui_invalidate();
}

void tui_end(void)
{
	int h;
	inited = 0;
	tui_set_palette(0);
	cols = rows = 0;
	hide_hw_cursor(0);
	if (G.plat && G.plat->tui_prepare)
		G.plat->tui_prepare();
	h = (G.plat && G.plat->hdmi_height) ? G.plat->hdmi_height() : 480;
	if (h < 1)
		h = 480;
	if (G.plat && G.plat->tui_present)
		G.plat->tui_present(0, h - 1);
	if (G.plat && G.plat->write_screen)
		G.plat->write_screen("\x1b[0m\x1b[H\x1b[J", 11);
	mmb_console_reset_prompt();
}

void tui_clear(int fg, int bg)
{
	tui_fill(0, 0, tui_cols(), tui_rows(), ' ', fg, bg);
}

void tui_put(int x, int y, int ch, int fg, int bg)
{
	tui_cell *c;
	if (!inited)
		return;
	if (x < 0 || y < 0 || x >= cols || y >= rows)
		return;
	if (fg < 0)
		fg = 0;
	if (fg > 15)
		fg = 15;
	if (bg < 0)
		bg = 0;
	if (bg > 15)
		bg = 15;
	if (ch < 0)
		ch = ' ';
	c = &front[y][x];
	c->ch = (unsigned char)ch;
	c->fg = (unsigned char)fg;
	c->bg = (unsigned char)bg;
}

void tui_puts(int x, int y, const char *s, int fg, int bg)
{
	int i;
	if (!s)
		return;
	for (i = 0; s[i]; i++)
		tui_put(x + i, y, (unsigned char)s[i], fg, bg);
}

void tui_pad(int x, int y, const char *s, int width, int fg, int bg)
{
	int n = 0, i;
	if (width < 0)
		return;
	if (s)
	{
		while (s[n] && n < width)
			n++;
		for (i = 0; i < n; i++)
			tui_put(x + i, y, (unsigned char)s[i], fg, bg);
	}
	for (i = n; i < width; i++)
		tui_put(x + i, y, ' ', fg, bg);
}

/* Render a status-bar hint line clipped to [x, x+width).  ``<...>`` spans are
 * drawn in ``hot_fg`` so they read as key hints; everything else uses ``fg``.
 * Shared by the PACKAGE, FILES and app-PATH status bars, and by the modal
 * dialog shell, so the tag loop lives in one place (issue #569). */
void tui_status_hint_at(int x, int row, int width, const char *hint, int hot_fg,
			int fg, int bg)
{
	int end;
	int i, p;

	if (width < 1)
		return;
	end = x + width;
	tui_fill(x, row, width, 1, ' ', fg, bg);
	if (!hint)
		return;
	for (i = 0, p = x + 1; hint[i] && p < end - 1; i++)
	{
		if (hint[i] == '<')
		{
			tui_put(p++, row, '<', hot_fg, bg);
			i++;
			while (hint[i] && hint[i] != '>' && p < end - 1)
			{
				tui_put(p++, row, (unsigned char)hint[i], hot_fg, bg);
				i++;
			}
			if (hint[i] == '>' && p < end - 1)
				tui_put(p++, row, '>', hot_fg, bg);
		}
		else if (p < end - 1)
			tui_put(p++, row, (unsigned char)hint[i], fg, bg);
	}
}

void tui_status_hint(int row, const char *hint, int hot_fg, int fg, int bg)
{
	tui_status_hint_at(0, row, tui_cols(), hint, hot_fg, fg, bg);
}

void tui_fill(int x, int y, int w, int h, int ch, int fg, int bg)
{
	int i, j;
	for (j = 0; j < h; j++)
		for (i = 0; i < w; i++)
			tui_put(x + i, y + j, ch, fg, bg);
}

void tui_hline(int x, int y, int w, int left, int mid, int right, int fg, int bg)
{
	int i;
	if (w <= 0)
		return;
	if (w == 1)
	{
		tui_put(x, y, mid, fg, bg);
		return;
	}
	tui_put(x, y, left, fg, bg);
	for (i = 1; i < w - 1; i++)
		tui_put(x + i, y, mid, fg, bg);
	tui_put(x + w - 1, y, right, fg, bg);
}

void tui_vline(int x, int y, int h, int ch, int fg, int bg)
{
	int j;
	for (j = 0; j < h; j++)
		tui_put(x, y + j, ch, fg, bg);
}

void tui_frame(int x, int y, int w, int h, int fg, int bg)
{
	int j;
	if (w < 2 || h < 2)
		return;
	tui_hline(x, y, w, TUI_TL, TUI_H, TUI_TR, fg, bg);
	for (j = 1; j < h - 1; j++)
	{
		tui_put(x, y + j, TUI_V, fg, bg);
		tui_put(x + w - 1, y + j, TUI_V, fg, bg);
	}
	tui_hline(x, y + h - 1, w, TUI_BL, TUI_H, TUI_BR, fg, bg);
}

void tui_cursor(int x, int y, int vis)
{
	cur_vis = vis ? 1 : 0;
	cur_x = x;
	cur_y = y;
}

static void blit_cell(int x, int y, const tui_cell *c, int invert)
{
	unsigned fg = palette[c->fg & 15];
	unsigned bg = palette[c->bg & 15];
	if (invert)
	{
		unsigned t = fg;
		fg = bg;
		bg = t;
	}
	if (G.plat && G.plat->tui_glyph)
		G.plat->tui_glyph(x, y, c->ch, fg, bg);
}

static void ser_row(int y)
{
	char line[TUI_MAX_COLS + 4];
	int x, n = 0;
	if (!G.plat || !G.plat->write_serial)
		return;
	for (x = 0; x < cols; x++)
		line[n++] = ascii_box(front[y][x].ch);
	line[n++] = '\r';
	line[n++] = '\n';
	G.plat->write_serial(line, (unsigned)n);
}

static void tui_flush_impl(int present)
{
	int x, y;
	int pix0 = -1, pix1 = -1;
	int any_serial = 0;
	if (!inited)
		return;
	for (y = 0; y < rows; y++)
	{
		int rd = 0;
		for (x = 0; x < cols; x++)
		{
			tui_cell *a = &front[y][x];
			tui_cell *b = &shown[y][x];
			if (a->ch != b->ch || a->fg != b->fg || a->bg != b->bg)
			{
				blit_cell(x, y, a, 0);
				*b = *a;
				rd = 1;
				if (pix0 < 0)
					pix0 = y * 16;
				pix1 = y * 16 + 15;
			}
		}
		row_dirty[y] = (unsigned char)rd;
		if (rd)
			any_serial = 1;
	}
	if (prev_cx >= 0 && prev_cy >= 0 && prev_cx < cols && prev_cy < rows)
	{
		int py0 = prev_cy * 16, py1 = py0 + 15;
		blit_cell(prev_cx, prev_cy, &shown[prev_cy][prev_cx], 0);
		if (pix0 < 0 || py0 < pix0)
			pix0 = py0;
		if (pix1 < py1)
			pix1 = py1;
	}
	if (cur_vis && cur_x >= 0 && cur_y >= 0 && cur_x < cols && cur_y < rows)
	{
		int py0 = cur_y * 16, py1 = py0 + 15;
		blit_cell(cur_x, cur_y, &shown[cur_y][cur_x], 1);
		if (pix0 < 0 || py0 < pix0)
			pix0 = py0;
		if (pix1 < py1)
			pix1 = py1;
		prev_cx = cur_x;
		prev_cy = cur_y;
	}
	else
		prev_cx = prev_cy = -1;
	if (present && G.plat && G.plat->tui_present && pix0 >= 0)
		G.plat->tui_present(pix0, pix1);
	if (any_serial)
	{
		for (y = 0; y < rows; y++)
			if (row_dirty[y])
				ser_row(y);
	}
}

void tui_flush(void)
{
	tui_flush_impl(1);
}

void tui_flush_no_present(void)
{
	tui_flush_impl(0);
}

void tui_invalidate_rect(int x, int y, int w, int h)
{
	int i, j;

	if (!inited)
		return;
	for (j = y; j < y + h; j++)
	{
		if (j < 0 || j >= rows)
			continue;
		for (i = x; i < x + w; i++)
		{
			if (i < 0 || i >= cols)
				continue;
			shown[j][i].ch = 0xFF;
			shown[j][i].fg = 0xFF;
			shown[j][i].bg = 0xFF;
		}
	}
}

void tui_accept_rect(int x, int y, int w, int h)
{
	int i, j;

	if (!inited)
		return;
	for (j = y; j < y + h; j++)
	{
		if (j < 0 || j >= rows)
			continue;
		for (i = x; i < x + w; i++)
		{
			if (i < 0 || i >= cols)
				continue;
			shown[j][i] = front[j][i];
		}
	}
}

unsigned tui_get_px(int x, int y)
{
	if (G.plat && G.plat->tui_get_px)
		return G.plat->tui_get_px(x, y);
	if (G.plat && G.plat->get_pixel)
		return G.plat->get_pixel(x, y);
	return 0;
}

/* ---- modal dialog shell (#590) -------------------------------------- */

/* Centre a panel of want_w x want_h cells, leaving a one-cell margin on every
 * side. Zero or negative requests fill the available screen minus the margin.
 * The result is clamped so the panel always fits the current text grid. */
void tui_dialog_geom(int want_w, int want_h, int *x, int *y, int *w, int *h)
{
	int c = tui_cols();
	int r = tui_rows();
	int ww = want_w > 0 ? want_w : c - 4;
	int hh = want_h > 0 ? want_h : r - 2;

	if (ww > c - 2)
		ww = c - 2;
	if (hh > r - 2)
		hh = r - 2;
	if (ww < 6)
		ww = c < 6 ? c : 6;
	if (hh < 3)
		hh = r < 3 ? r : 3;
	*x = (c - ww) / 2;
	*y = (r - hh) / 2;
	if (*x < 1)
		*x = 1;
	if (*y < 1)
		*y = 1;
	if (*x + ww > c)
		ww = c - *x;
	if (*y + hh > r)
		hh = r - *y;
	if (ww < 0)
		ww = 0;
	if (hh < 0)
		hh = 0;
	*w = ww;
	*h = hh;
}

/* Draw a framed dialog panel with a title bar on its first interior row. The
 * interior below the title is cleared to body_bg; the caller composes content
 * at (x+2, y+2) and flushes with tui_flush(). */
void tui_dialog_panel(int x, int y, int w, int h, const char *title,
		      int body_fg, int body_bg, int brd_fg, int brd_bg,
		      int title_fg, int title_bg)
{
	int n, tx;
	if (w < 4 || h < 3)
		return;
	tui_fill(x + 1, y + 1, w - 2, h - 2, ' ', body_fg, body_bg);
	tui_fill(x + 1, y + 1, w - 2, 1, ' ', title_fg, title_bg);
	tui_frame(x, y, w, h, brd_fg, brd_bg);
	if (!title || !title[0])
		return;
	n = (int)strlen(title);
	tx = x + (w - n) / 2;
	if (tx < x + 2)
		tx = x + 2;
	if (tx + n > x + w - 1)
		tx = x + w - 1 - n;
	if (tx < x + 1)
		tx = x + 1;
	tui_puts(tx, y + 1, title, title_fg, title_bg);
}

/* ---- overlay dialogs (#623, #624) ------------------------------------ */

static int s_overlay_active[MMB_MAX_CONSOLES];
static int s_overlay_saved[MMB_MAX_CONSOLES];

void tui_overlay_begin(void)
{
	if (g_console < 0 || g_console >= MMB_MAX_CONSOLES)
		return;
	s_overlay_active[g_console] = 1;
	s_overlay_saved[g_console] = 0;
	if (G.plat && G.plat->console_save)
		s_overlay_saved[g_console] = G.plat->console_save(g_console, 0);
}

int tui_overlay_end(void)
{
	int restored = 0;
	if (g_console < 0 || g_console >= MMB_MAX_CONSOLES)
		return 0;
	if (s_overlay_active[g_console] && s_overlay_saved[g_console] &&
	    G.plat && G.plat->console_restore)
		restored = G.plat->console_restore(g_console, 0);
	s_overlay_active[g_console] = 0;
	s_overlay_saved[g_console] = 0;
	return restored ? 1 : 0;
}

/* Cold-boot the TUI layer on a warm reset (#763): every console's cell grid
 * and any in-flight overlay snapshot is dropped, not just the active one. */
void mmb_tui_reset_all(void)
{
	int i;

	for (i = 0; i < MMB_MAX_CONSOLES; i++)
	{
		memset(&s_tui[i], 0, sizeof(s_tui[i]));
		s_overlay_active[i] = 0;
		s_overlay_saved[i] = 0;
	}
}
