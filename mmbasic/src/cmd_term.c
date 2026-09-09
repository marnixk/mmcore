#include "mmb_priv.h"
#include <string.h>

#define IAC   255
#define DONT  254
#define DO    253
#define WONT  252
#define WILL  251
#define SB    250
#define SE    240
#define TELOPT_ECHO  1
#define TELOPT_SGA   3
#define TELOPT_TTYPE 24
#define TELOPT_NAWS  31
#define TTYPE_IS     0
#define TTYPE_SEND   1

#define TM_COLS_BOXED 80
#define TM_MAX_COLS   256
#define TM_CH         16
#define TM_CW         8
#define TM_MAX_ROWS   80
#define TM_LINE     256
#define TM_ESC_BUF  16
#define TM_ANSI_ARGS 8

#define TM_SCROLL_MS    100
#define TM_DEMO_MIN_MS  120
#define TM_DEMO_MAX_MS  200
#define TM_ESC_IDLE_MS  60
#define TM_CONNECT_MS   25000
#define TM_RECV_MS      20
#define TM_BM_MAX       32
#define TM_BM_NAME      40
#define TM_DLG_NONE     0
#define TM_DLG_LIST     1
#define TM_DLG_EDIT     2
#define TM_DLG_DEL      3
#define TM_BTN_NEW      0
#define TM_BTN_EDIT     1
#define TM_BTN_DEL      2
#define TM_BTN_CONN     3
#define TM_LIST_VIEW    12

typedef struct {
	int active;
	int demo;
	int demo_burst;
	int tcp;
	int connecting;
	unsigned connect_at;
	int net_fail;
	char net_msg[64];
	char host[80];
	int port;
	int saved_mode;
	int saved_bits;
	int pane_left;
	int pane_rows;
	int pane_cols;
	int letterbox;
	int vid_rows;
	int vid_cols;
	int cur_row;
	int cur_col;
	char cell[TM_MAX_ROWS][TM_MAX_COLS];
	unsigned cell_fg[TM_MAX_ROWS][TM_MAX_COLS];
	unsigned cell_bg[TM_MAX_ROWS][TM_MAX_COLS];
	unsigned cur_fg;
	unsigned cur_bg;
	int ansi_bold;
	int ansi_inv;
	int ansi_st;
	int ansi_narg;
	int ansi_arg[TM_ANSI_ARGS];
	int ansi_priv;
	int sav_row;
	int sav_col;
	int char_mode;
	int no_echo;
	int echo_user;
	int iac;
	int iac_cmd;
	int sb;
	int last_eol;
	int sb_opt;
	int sb_cmd;
	char line[TM_LINE];
	int linelen;
	int esc;
	int csi_n;
	char esc_buf[TM_ESC_BUF];
	int esc_len;
	unsigned esc_at;
	int alt;
	int menu;
	int menu_sel;
	int dlg;
	int dlg_btn;
	int dlg_sel;
	int dlg_top;
	int dlg_focus;
	int dlg_edit_idx;
	char dlg_name[40];
	char dlg_host[80];
	int dlg_port;
	int dlg_echo;
	int dlg_letterbox;
	int demo_line;
	unsigned demo_next;
	int serial_gen;
	int need_draw;
	int dirty_full;
	int dirty_lo;
	int dirty_hi;
} tm_state;

static tm_state T;

typedef struct {
	char name[TM_BM_NAME];
	char host[80];
	int port;
	int echo;
	int letterboxed;
} term_bm;

static term_bm g_bm[TM_BM_MAX];
static int g_bm_n;

static const mmb_ed_theme *term_th(void)
{
	return mmb_editor_theme();
}

static unsigned term_vga_rgb(int idx)
{
	static const unsigned pal[16] = {
		0x000000u, 0xAA0000u, 0x00AA00u, 0xAA5500u,
		0x0000AAu, 0xAA00AAu, 0x00AAAAu, 0xAAAAAAu,
		0x555555u, 0xFF5555u, 0x55FF55u, 0xFFFF55u,
		0x5555FFu, 0xFF55FFu, 0x55FFFFu, 0xFFFFFFu
	};
	if (idx < 0)
		idx = 0;
	if (idx > 15)
		idx = 15;
	return pal[idx];
}

static unsigned term_rgb(unsigned char idx)
{
	const unsigned *pal = term_th()->pal;
	int i = (int)idx & 15;

	if (!pal)
		return term_vga_rgb(i);
	return pal[i] & 0xFFFFFFu;
}

#define TM_BG      term_rgb(term_th()->edit_bg)
#define TM_FG      term_rgb(term_th()->edit_fg)
#define TM_DIM     term_rgb(term_th()->cmt_fg)
#define TM_MENU_BG term_rgb(term_th()->menu_bg)
#define TM_MENU_FG term_rgb(term_th()->menu_fg)
#define TM_MENU_HI term_rgb(term_th()->sel_bg)
#define TM_SEL_FG  term_rgb(term_th()->sel_fg)
#define TM_SEL_BG  term_rgb(term_th()->sel_bg)
#define TM_DLG_FG  term_rgb(term_th()->dlg_fg)
#define TM_DLG_BG  term_rgb(term_th()->dlg_bg)
#define TM_SH_FG   term_rgb(term_th()->sh_fg)
#define TM_SH_BG   term_rgb(term_th()->sh_bg)

static void term_draw(void);
static void term_draw_dlg(void);
static void term_serial_dump(void);
static void term_serial_dump_dlg(void);
static void mark_dirty_full(void);
static void term_layout(void);
static void term_fill_pages(void);
static void pane_clear_row(int row);
static void pane_puts(const char *s);
static void pane_newline(void);
static void tcp_close_quiet(void);
static void telnet_announce(void);
static void term_reset_pen(void);
static void demo_emit_line(void);
static void esc_reset(void);
static int term_width(void);

static int term_want_echo(void);
static void pane_rubout(void);
static void term_echo_byte(unsigned char b);
static void send_naws(void);

static unsigned ansi_pal(int n)
{
	static const unsigned pal[16] = {
		0x000000u, 0xAA0000u, 0x00AA00u, 0xAA5500u,
		0x0000AAu, 0xAA00AAu, 0x00AAAAu, 0xAAAAAAu,
		0x555555u, 0xFF5555u, 0x55FF55u, 0xFFFF55u,
		0x5555FFu, 0xFF55FFu, 0x55FFFFu, 0xFFFFFFu
	};
	if (n < 0)
		n = 0;
	if (n > 15)
		n = 15;
	return pal[n];
}

static unsigned ansi_256(int n)
{
	int r, g, b, v;
	if (n < 16)
		return ansi_pal(n);
	if (n < 232)
	{
		n -= 16;
		b = n % 6;
		g = (n / 6) % 6;
		r = n / 36;
		r = r ? r * 40 + 55 : 0;
		g = g ? g * 40 + 55 : 0;
		b = b ? b * 40 + 55 : 0;
		return mmb_rgb_pack(r, g, b);
	}
	v = 8 + (n - 232) * 10;
	if (v > 255)
		v = 255;
	return mmb_rgb_pack(v, v, v);
}

static unsigned term_pen(void)
{
	unsigned fg = T.cur_fg;
	if (T.ansi_bold)
	{
		int r = (int)((fg >> 16) & 255);
		int g = (int)((fg >> 8) & 255);
		int b = (int)(fg & 255);
		r += (255 - r) / 3;
		g += (255 - g) / 3;
		b += (255 - b) / 3;
		fg = mmb_rgb_pack(r, g, b);
	}
	return T.ansi_inv ? T.cur_bg : fg;
}

static unsigned term_paper(void)
{
	return T.ansi_inv ? T.cur_fg : T.cur_bg;
}

static void term_reset_pen(void)
{
	T.cur_fg = TM_FG;
	T.cur_bg = TM_BG;
	T.ansi_bold = 0;
	T.ansi_inv = 0;
}

static void ser(const char *s)
{
	unsigned n;
	if (!s || !G.plat || !G.plat->write_serial)
		return;
	n = (unsigned)strlen(s);
	if (n)
		G.plat->write_serial(s, n);
}

static void fmt_hostport(char *dst, unsigned maxn)
{
	unsigned n = 0;
	const char *h = T.host;
	char pbuf[8];
	int pi = 0, pv = T.port;

	while (h[n] && n + 1 < maxn)
	{
		dst[n] = h[n];
		n++;
	}
	if (n + 1 < maxn)
		dst[n++] = ':';
	if (pv == 0)
		pbuf[pi++] = '0';
	else
	{
		while (pv && pi < 7)
		{
			pbuf[pi++] = (char)('0' + (pv % 10));
			pv /= 10;
		}
	}
	while (pi > 0 && n + 1 < maxn)
		dst[n++] = pbuf[--pi];
	dst[n] = 0;
}

static void fmt_line_num(char *dst, int n)
{
	dst[0] = 'l';
	dst[1] = 'i';
	dst[2] = 'n';
	dst[3] = 'e';
	dst[4] = ' ';
	dst[5] = (char)('0' + (n / 10) % 10);
	dst[6] = (char)('0' + n % 10);
	dst[7] = 0;
}

static int term_width(void)
{
	return T.pane_cols > 0 ? T.pane_cols : TM_COLS_BOXED;
}

static const char *term_width_label(void)
{
	return T.letterbox ? "Boxed" : "Full";
}

static void term_serial_dump(void)
{
	int r, c, cols;
	char line[TM_MAX_COLS + 1];

	if (!T.active)
		return;
	cols = term_width();
	for (r = 0; r < T.pane_rows; r++)
	{
		for (c = 0; c < cols; c++)
			line[c] = T.cell[r][c] ? T.cell[r][c] : ' ';
		line[cols] = 0;
		ser(line);
		ser("\r\n");
	}
	if (T.menu)
	{
		ser("File\r\n");
		ser("Exit\r\n");
		ser(term_want_echo() ? "Echo ON\r\n" : "Echo OFF\r\n");
		ser(term_width_label());
		ser("\r\n");
		ser("Bookmarks\r\n");
	}
	term_serial_dump_dlg();
	T.serial_gen++;
}

static void term_layout(void)
{
	T.vid_cols = G.plat && G.plat->video_cols ? G.plat->video_cols() : 80;
	T.vid_rows = G.plat && G.plat->video_rows ? G.plat->video_rows() : 33;
	if (T.letterbox)
		T.pane_cols = TM_COLS_BOXED;
	else
		T.pane_cols = T.vid_cols;
	if (T.pane_cols > TM_MAX_COLS)
		T.pane_cols = TM_MAX_COLS;
	if (T.pane_cols < 1)
		T.pane_cols = 1;
	T.pane_left = (T.vid_cols - T.pane_cols) / 2;
	if (T.pane_left < 0)
		T.pane_left = 0;
	T.pane_rows = T.vid_rows - 1;
	if (T.pane_rows < 1)
		T.pane_rows = 1;
	if (T.pane_rows > TM_MAX_ROWS)
		T.pane_rows = TM_MAX_ROWS;
}

static void pane_clear_row(int row)
{
	int c;
	if (row < 0 || row >= T.pane_rows)
		return;
	for (c = 0; c < term_width(); c++)
	{
		T.cell[row][c] = ' ';
		T.cell_fg[row][c] = TM_FG;
		T.cell_bg[row][c] = TM_BG;
	}
}

static void mark_dirty_row(int row)
{
	if (row < 0 || row >= T.pane_rows)
		return;
	T.need_draw = 1;
	if (T.dirty_full)
		return;
	if (T.dirty_lo < 0)
	{
		T.dirty_lo = row;
		T.dirty_hi = row;
		return;
	}
	if (row < T.dirty_lo)
		T.dirty_lo = row;
	if (row > T.dirty_hi)
		T.dirty_hi = row;
}

static void mark_dirty_full(void)
{
	T.dirty_full = 1;
	T.need_draw = 1;
}

static void term_draw(void);
static void term_draw_row(int r);

static void pane_flush_dirty_pixels(void)
{
	int r, saved, lo, hi;

	if (!T.need_draw)
		return;
	saved = G.gfx.write_page;
	G.gfx.write_page = 1;
	if (T.dirty_full)
	{
		lo = 0;
		hi = T.pane_rows - 1;
	}
	else
	{
		lo = T.dirty_lo;
		hi = T.dirty_hi;
	}
	if (lo >= 0 && hi >= lo)
	{
		for (r = lo; r <= hi; r++)
			term_draw_row(r);
	}
	G.gfx.write_page = saved;
	T.need_draw = 0;
	T.dirty_full = 0;
	T.dirty_lo = -1;
	T.dirty_hi = -1;
}

static void pane_scroll_up(void)
{
	int r, c;
	if (T.pane_rows <= 1)
		return;
	for (r = 0; r < T.pane_rows - 1; r++)
	{
		for (c = 0; c < term_width(); c++)
		{
			T.cell[r][c] = T.cell[r + 1][c];
			T.cell_fg[r][c] = T.cell_fg[r + 1][c];
			T.cell_bg[r][c] = T.cell_bg[r + 1][c];
		}
	}
	pane_clear_row(T.pane_rows - 1);
	T.cur_row = T.pane_rows - 1;
	T.cur_col = 0;
}

static void pane_scroll_smooth(void)
{
	uint32_t *pg;
	int x0, y, w, h, pw, ph, saved;
	unsigned fill = TM_BG;

	pane_flush_dirty_pixels();
	x0 = T.pane_left * TM_CW;
	pw = term_width() * TM_CW;
	ph = T.pane_rows * TM_CH;
	saved = G.gfx.write_page;
	G.gfx.write_page = 1;
	pg = mmb_gfx_buf_for(1, &w, &h);
	if (pg && ph > TM_CH)
	{
		for (y = 0; y < ph - TM_CH && y + TM_CH < h; y++)
			memmove(pg + y * w + x0, pg + (y + TM_CH) * w + x0,
				(unsigned)pw * sizeof(uint32_t));
		for (y = ph - TM_CH; y < ph && y < h; y++)
		{
			int x;
			for (x = 0; x < pw && x0 + x < w; x++)
				pg[y * w + x0 + x] = fill;
		}
	}
	G.gfx.write_page = saved;
	pane_scroll_up();
	if (pg && ph > TM_CH)
	{
		T.dirty_full = 0;
		T.dirty_lo = T.pane_rows - 1;
		T.dirty_hi = T.pane_rows - 1;
		T.need_draw = 1;
	}
	else
		mark_dirty_full();
}

static void pane_newline(void)
{
	if (T.cur_row >= T.pane_rows - 1)
	{
		pane_scroll_smooth();
		return;
	}
	T.cur_row++;
	T.cur_col = 0;
	mark_dirty_row(T.cur_row);
}

static void pane_put(char ch)
{
	if (T.cur_row < 0 || T.cur_row >= T.pane_rows)
		return;
	if (T.cur_col >= term_width())
		pane_newline();
	if (T.cur_col < term_width() && T.cur_row < T.pane_rows)
	{
		T.cell[T.cur_row][T.cur_col] = ch;
		T.cell_fg[T.cur_row][T.cur_col] = term_pen();
		T.cell_bg[T.cur_row][T.cur_col] = term_paper();
		T.cur_col++;
		mark_dirty_row(T.cur_row);
	}
}

static void pane_puts(const char *s)
{
	const char *p;
	if (!s)
		return;
	for (p = s; *p; p++)
	{
		if (*p == '\r')
			T.cur_col = 0;
		else if (*p == '\n')
			pane_newline();
		else if ((unsigned char)*p >= 32)
			pane_put(*p);
	}
}

static void term_put_str(int x, int y, const char *s, unsigned fg)
{
	int i;
	if (!s)
		return;
	for (i = 0; s[i]; i++)
		mmb_gfx_glyph_cp437(x + i * TM_CW, y, (unsigned char)s[i], fg);
}

static void term_draw_status(void)
{
	char left[32];
	char right[64];
	int y, n, x0;

	y = (T.vid_rows - 1) * TM_CH;
	x0 = T.pane_left * TM_CW;
	mmb_gfx_box(0, (T.vid_rows - 1) * TM_CH, T.vid_cols * TM_CW, TM_CH,
		    TM_BG, 1, (int)TM_BG);
	strcpy(left, "F10/Alt-X  Alt-F");
	if (T.demo)
	{
		n = (int)strlen(left);
		if (n + 4 < (int)sizeof(left))
		{
			left[n] = ' ';
			left[n + 1] = (char)176;
			left[n + 2] = (char)177;
			left[n + 3] = (char)178;
			left[n + 4] = 0;
		}
	}
	term_put_str(x0, y, left, TM_DIM);
	right[0] = 0;
	if (T.connecting)
	{
		if (mmb_net_tcp_cancelling())
			strncpy(right, "Cancelling...", sizeof(right) - 1);
		else
			strncpy(right, "Connecting...", sizeof(right) - 1);
	}
	else if (T.net_fail && T.net_msg[0])
		strncpy(right, T.net_msg, sizeof(right) - 1);
	else if (T.demo)
		strncpy(right, "demo", sizeof(right) - 1);
	else if (!T.host[0] && !T.tcp && !T.connecting)
		strncpy(right, "Disconnected", sizeof(right) - 1);
	else if (T.host[0])
		fmt_hostport(right, sizeof(right));
	{
		char echo[16];
		char mixed[80];
		strcpy(echo, term_want_echo() ? "Echo ON" : "Echo OFF");
		if (right[0])
		{
			strncpy(mixed, echo, sizeof(mixed) - 1);
			mixed[sizeof(mixed) - 1] = 0;
			n = (int)strlen(mixed);
			if (n + 2 < (int)sizeof(mixed))
			{
				mixed[n] = ' ';
				mixed[n + 1] = 0;
				strncat(mixed, right, sizeof(mixed) - 1 - (unsigned)n - 1);
			}
			strncpy(right, mixed, sizeof(right) - 1);
		}
		else
			strncpy(right, echo, sizeof(right) - 1);
	}
	right[sizeof(right) - 1] = 0;
	n = (int)strlen(right);
	if (n > term_width())
		n = term_width();
	term_put_str(x0 + (term_width() - n) * TM_CW, y, right, TM_DIM);
}

static void term_draw_menu(void)
{
	int x0, y0, w, i;
	const char *items[4];
	char echo[16];

	if (!T.menu)
		return;
	x0 = T.pane_left * TM_CW;
	y0 = 0;
	w = 12 * TM_CW;
	strcpy(echo, term_want_echo() ? "Echo ON" : "Echo OFF");
	items[0] = "Exit";
	items[1] = echo;
	items[2] = term_width_label();
	items[3] = "Bookmarks";
	mmb_gfx_box(x0, y0, w, 6 * TM_CH, TM_MENU_BG, 1, (int)TM_MENU_BG);
	term_put_str(x0 + TM_CW, y0, "File", TM_MENU_FG);
	for (i = 0; i < 4; i++)
	{
		int y = y0 + (i + 1) * TM_CH;
		if (i == T.menu_sel)
			mmb_gfx_box(x0, y, w, TM_CH, TM_MENU_HI, 1, (int)TM_MENU_HI);
		term_put_str(x0 + TM_CW, y, items[i],
			     i == T.menu_sel ? TM_SEL_FG : TM_MENU_FG);
	}
}

static void term_draw_row(int r)
{
	int c, x, y;
	unsigned ch, fg, bg;

	if (r < 0 || r >= T.pane_rows)
		return;
	y = r * TM_CH;
	for (c = 0; c < term_width(); c++)
	{
		x = (T.pane_left + c) * TM_CW;
		ch = (unsigned char)T.cell[r][c];
		fg = T.cell_fg[r][c];
		bg = T.cell_bg[r][c];
		if (!ch)
			ch = ' ';
		if (bg != TM_BG)
			mmb_gfx_box(x, y, TM_CW, TM_CH, bg, 1, (int)bg);
		else
			mmb_gfx_box(x, y, TM_CW, TM_CH, TM_BG, 1, (int)TM_BG);
		mmb_gfx_glyph_cp437(x, y, ch, fg);
	}
}

static void term_copy_pane(void)
{
	uint32_t *s, *d;
	int w, h, y, x0, pw, ph;

	s = mmb_gfx_buf_for(1, &w, &h);
	d = mmb_gfx_buf_for(0, &w, &h);
	if (!s || !d)
		return;
	x0 = T.pane_left * TM_CW;
	pw = term_width() * TM_CW;
	ph = T.vid_rows * TM_CH;
	if (x0 < 0)
		x0 = 0;
	if (x0 + pw > w)
		pw = w - x0;
	if (pw <= 0)
		return;
	if (ph > h)
		ph = h;
	for (y = 0; y < ph; y++)
		memcpy(d + y * w + x0, s + y * w + x0, (unsigned)pw * sizeof(uint32_t));
}

static void term_present_pane(void)
{
	int x0 = T.pane_left * TM_CW;
	int pw = term_width() * TM_CW;
	int ph = T.vid_rows * TM_CH;

	mmb_gfx_present_rect(x0, 0, pw, ph);
}

static void term_draw(void)
{
	int r, saved, lo, hi;

	if (!T.need_draw)
		return;
	saved = G.gfx.write_page;
	G.gfx.write_page = 1;
	G.gfx.display_page = 0;
	if (T.dirty_full)
	{
		lo = 0;
		hi = T.pane_rows - 1;
	}
	else
	{
		lo = T.dirty_lo;
		hi = T.dirty_hi;
	}
	if (lo >= 0 && hi >= lo)
	{
		for (r = lo; r <= hi; r++)
			term_draw_row(r);
	}
	term_draw_status();
	term_draw_menu();
	term_draw_dlg();
	term_copy_pane();
	term_present_pane();
	G.gfx.write_page = saved;
	T.need_draw = 0;
	T.dirty_full = 0;
	T.dirty_lo = -1;
	T.dirty_hi = -1;
}

static void term_exit(void)
{
	mmb_net_tcp_close();
	T.tcp = 0;
	T.connecting = 0;
	if (T.saved_mode)
	{
		int bits = T.saved_bits;
		if (bits != 8 && bits != 12 && bits != 16 && bits != 32)
			bits = 16;
		mmb_gfx_set_mode(T.saved_mode, bits);
	}
	else if (G.plat && G.plat->fill_screen)
		G.plat->fill_screen(0);
	mmb_console_reset_prompt();
	memset(&T, 0, sizeof(T));
}

static void send_iac(int cmd, int opt)
{
	unsigned char b[3];
	b[0] = IAC;
	b[1] = (unsigned char)cmd;
	b[2] = (unsigned char)opt;
	mmb_net_tcp_send(b, 3);
}

static void send_ttype(void)
{
	static const unsigned char ttype[] = {
		IAC, SB, TELOPT_TTYPE, TTYPE_IS,
		'A', 'N', 'S', 'I',
		IAC, SE
	};
	mmb_net_tcp_send(ttype, (unsigned)sizeof(ttype));
}

static void send_naws(void)
{
	unsigned char b[9];
	int cols = term_width();
	int rows = T.pane_rows > 0 ? T.pane_rows : 24;
	b[0] = IAC;
	b[1] = SB;
	b[2] = TELOPT_NAWS;
	b[3] = (unsigned char)((cols >> 8) & 255);
	b[4] = (unsigned char)(cols & 255);
	b[5] = (unsigned char)((rows >> 8) & 255);
	b[6] = (unsigned char)(rows & 255);
	b[7] = IAC;
	b[8] = SE;
	mmb_net_tcp_send(b, 9);
}

static void telnet_announce(void)
{
	send_iac(WILL, TELOPT_TTYPE);
	send_iac(DO, TELOPT_SGA);
	send_iac(WILL, TELOPT_NAWS);
	send_naws();
	send_ttype();
}

static int term_want_echo(void)
{
	if (T.echo_user == 1)
		return 1;
	if (T.echo_user == 2)
		return 0;
	return !T.no_echo;
}

static void pane_rubout(void)
{
	if (T.cur_col > 0)
	{
		T.cur_col--;
		T.cell[T.cur_row][T.cur_col] = ' ';
		T.cell_fg[T.cur_row][T.cur_col] = TM_FG;
		T.cell_bg[T.cur_row][T.cur_col] = TM_BG;
		mark_dirty_row(T.cur_row);
	}
}

static void term_echo_byte(unsigned char b)
{
	if (!term_want_echo())
		return;
	if (b == 8 || b == 127)
		pane_rubout();
	else if (b == '\r' || b == '\n')
		pane_newline();
	else if (b >= 32)
		pane_put((char)b);
}

static void term_echo_flush(void)
{
	if (T.need_draw)
		term_draw();
	term_serial_dump();
}

static void term_menu_move(int dir)
{
	T.menu_sel += dir;
	if (T.menu_sel < 0)
		T.menu_sel = 3;
	if (T.menu_sel > 3)
		T.menu_sel = 0;
	mark_dirty_full();
	term_draw();
	term_serial_dump();
}

static void term_toggle_echo(void)
{
	if (term_want_echo())
		T.echo_user = 2;
	else
		T.echo_user = 1;
	mark_dirty_full();
	term_draw();
	term_serial_dump();
}

static void term_fill_pages(void)
{
	int saved = G.gfx.write_page;
	G.gfx.write_page = 1;
	mmb_gfx_cls(TM_BG);
	G.gfx.write_page = 0;
	mmb_gfx_cls(TM_BG);
	G.gfx.write_page = saved;
}

static void term_init_extra_cols(int old_cols)
{
	int r, c, cols = term_width();
	if (cols <= old_cols)
		return;
	for (r = 0; r < T.pane_rows; r++)
	{
		for (c = old_cols; c < cols; c++)
		{
			if (!T.cell[r][c])
			{
				T.cell[r][c] = ' ';
				T.cell_fg[r][c] = TM_FG;
				T.cell_bg[r][c] = TM_BG;
			}
		}
	}
}

static void term_clamp_cursor(void)
{
	if (T.cur_col >= term_width())
		T.cur_col = term_width() - 1;
	if (T.cur_col < 0)
		T.cur_col = 0;
	if (T.cur_row >= T.pane_rows)
		T.cur_row = T.pane_rows - 1;
	if (T.cur_row < 0)
		T.cur_row = 0;
}

static void term_apply_session_mode(void)
{
	int mode, bits;

	bits = T.saved_bits;
	if (bits != 8 && bits != 12 && bits != 16 && bits != 32)
		bits = 16;
	if (T.letterbox)
	{
		mode = 14;
		if (bits == 12)
			bits = 16;
	}
	else
	{
		mode = T.saved_mode;
		if (mode < 1 || mode > 17)
			mode = 14;
		if ((mode == 9 || mode == 11 || mode == 12 || mode == 14) &&
		    bits == 12)
			bits = 16;
	}
	if (G.gfx.mode != mode || G.gfx.bits != bits)
		mmb_gfx_set_mode(mode, bits);
	G.gfx.write_page = 1;
	G.gfx.display_page = 0;
}

static void term_toggle_letterbox(void)
{
	int old_cols = term_width();
	T.letterbox = T.letterbox ? 0 : 1;
	term_apply_session_mode();
	term_layout();
	term_init_extra_cols(old_cols);
	term_clamp_cursor();
	term_fill_pages();
	if (T.tcp)
		send_naws();
	mark_dirty_full();
	term_draw();
	mmb_gfx_present();
	term_serial_dump();
}

static void term_ui_refresh(void)
{
	mark_dirty_full();
	term_draw();
	term_serial_dump();
}

static int bm_name_cmp(const char *a, const char *b)
{
	while (*a && *b)
	{
		char ca = *a, cb = *b;
		if (ca >= 'a' && ca <= 'z')
			ca = (char)(ca - 32);
		if (cb >= 'a' && cb <= 'z')
			cb = (char)(cb - 32);
		if (ca != cb)
			return (unsigned char)ca - (unsigned char)cb;
		a++;
		b++;
	}
	return (unsigned char)*a - (unsigned char)*b;
}

static void bm_sort(void)
{
	int i, j;
	term_bm tmp;

	for (i = 1; i < g_bm_n; i++)
	{
		tmp = g_bm[i];
		j = i;
		while (j > 0 && bm_name_cmp(g_bm[j - 1].name, tmp.name) > 0)
		{
			g_bm[j] = g_bm[j - 1];
			j--;
		}
		g_bm[j] = tmp;
	}
}

static int bm_find_name(const char *name)
{
	int i;
	for (i = 0; i < g_bm_n; i++)
		if (bm_name_cmp(g_bm[i].name, name) == 0)
			return i;
	return 0;
}

static void bm_path(char *dst, unsigned n)
{
	if (mmb_fat_ready('C'))
		strncpy(dst, "C:/.termconfig", n - 1);
	else
		strncpy(dst, "A:/.termconfig", n - 1);
	dst[n - 1] = 0;
}

static void bm_append(char *buf, int sz, const char *s)
{
	int n = (int)strlen(buf);
	int i;
	for (i = 0; s && s[i] && n + 1 < sz; i++)
		buf[n++] = s[i];
	buf[n] = 0;
}

static void bm_append_int(char *buf, int sz, int v)
{
	char tmp[12];
	int i = 0, n;
	if (v < 0)
		v = 0;
	if (v == 0)
	{
		bm_append(buf, sz, "0");
		return;
	}
	n = v;
	while (n && i < 11)
	{
		tmp[i++] = (char)('0' + (n % 10));
		n /= 10;
	}
	while (i > 0)
	{
		char c[2];
		c[0] = tmp[--i];
		c[1] = 0;
		bm_append(buf, sz, c);
	}
}

static void term_bm_save(void)
{
	char path[24];
	char buf[4096];
	int i;

	bm_sort();
	bm_path(path, sizeof(path));
	buf[0] = 0;
	bm_append(buf, sizeof(buf), "# TERM bookmarks\n");
	for (i = 0; i < g_bm_n; i++)
	{
		bm_append(buf, sizeof(buf), "\n[bm.");
		bm_append_int(buf, sizeof(buf), i);
		bm_append(buf, sizeof(buf), "]\nname=");
		bm_append(buf, sizeof(buf), g_bm[i].name);
		bm_append(buf, sizeof(buf), "\nhost=");
		bm_append(buf, sizeof(buf), g_bm[i].host);
		bm_append(buf, sizeof(buf), "\nport=");
		bm_append_int(buf, sizeof(buf), g_bm[i].port);
		bm_append(buf, sizeof(buf), "\necho=");
		bm_append_int(buf, sizeof(buf), g_bm[i].echo ? 1 : 0);
		bm_append(buf, sizeof(buf), "\nletterboxed=");
		bm_append_int(buf, sizeof(buf), g_bm[i].letterboxed ? 1 : 0);
		bm_append(buf, sizeof(buf), "\n");
	}
	mmb_vfs_write(path, buf, (unsigned)strlen(buf), 0);
}

static void bm_trim(char *s)
{
	char *e;
	while (*s == ' ' || *s == '\t')
	{
		memmove(s, s + 1, strlen(s));
	}
	e = s + strlen(s);
	while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r'))
		*--e = 0;
}

static int bm_parse_int(const char *s)
{
	int n = 0;
	while (*s >= '0' && *s <= '9')
		n = n * 10 + (*s++ - '0');
	return n;
}

static void term_bm_load(void)
{
	char path[24];
	char buf[4096];
	unsigned got = 0;
	char *p, *nl;
	int cur = -1;

	g_bm_n = 0;
	memset(g_bm, 0, sizeof(g_bm));
	bm_path(path, sizeof(path));
	if (!mmb_vfs_exists(path))
		return;
	if (mmb_vfs_read(path, buf, sizeof(buf) - 1, &got) != 0)
		return;
	buf[got] = 0;
	p = buf;
	while (*p)
	{
		nl = p;
		while (*nl && *nl != '\n')
			nl++;
		if (*nl)
			*nl++ = 0;
		if (p[0] && p[strlen(p) - 1] == '\r')
			p[strlen(p) - 1] = 0;
		bm_trim(p);
		if (p[0] == 0 || p[0] == '#' || p[0] == ';')
		{
			p = nl;
			continue;
		}
		if (p[0] == '[')
		{
			if (p[1] == 'b' || p[1] == 'B')
			{
				if (g_bm_n < TM_BM_MAX)
				{
					cur = g_bm_n;
					memset(&g_bm[cur], 0, sizeof(g_bm[cur]));
					g_bm[cur].port = 23;
					g_bm[cur].echo = 1;
					g_bm[cur].letterboxed = 1;
					g_bm_n++;
				}
				else
					cur = -1;
			}
			else
				cur = -1;
			p = nl;
			continue;
		}
		if (cur >= 0)
		{
			char *eq = p;
			while (*eq && *eq != '=')
				eq++;
			if (*eq == '=')
			{
				*eq++ = 0;
				bm_trim(p);
				bm_trim(eq);
				if (mmb_keyword_eq(p, "name"))
					strncpy(g_bm[cur].name, eq, TM_BM_NAME - 1);
				else if (mmb_keyword_eq(p, "host"))
					strncpy(g_bm[cur].host, eq, sizeof(g_bm[cur].host) - 1);
				else if (mmb_keyword_eq(p, "port"))
				{
					g_bm[cur].port = bm_parse_int(eq);
					if (g_bm[cur].port < 1 || g_bm[cur].port > 65535)
						g_bm[cur].port = 23;
				}
				else if (mmb_keyword_eq(p, "echo"))
					g_bm[cur].echo = bm_parse_int(eq) ? 1 : 0;
				else if (mmb_keyword_eq(p, "letterboxed"))
					g_bm[cur].letterboxed = bm_parse_int(eq) ? 1 : 0;
			}
		}
		p = nl;
	}
	bm_sort();
}

static void term_dlg_hline(int x0, int y0, int cw, unsigned left, unsigned mid,
			   unsigned right)
{
	int i;
	unsigned brd_fg = TM_MENU_FG;

	mmb_gfx_glyph_cp437(x0, y0, left, brd_fg);
	for (i = 1; i < cw - 1; i++)
		mmb_gfx_glyph_cp437(x0 + i * TM_CW, y0, mid, brd_fg);
	mmb_gfx_glyph_cp437(x0 + (cw - 1) * TM_CW, y0, right, brd_fg);
}

static void term_dlg_frame(int col, int row, int cw, int ch, const char *title)
{
	int x0, y0, w, h, i, tw, tx;
	unsigned brd_fg = TM_MENU_FG;

	x0 = (T.pane_left + col) * TM_CW;
	y0 = row * TM_CH;
	w = cw * TM_CW;
	h = ch * TM_CH;
	mmb_gfx_box(x0, y0, w, h, TM_DLG_BG, 1, (int)TM_DLG_BG);
	term_dlg_hline(x0, y0, cw, 0xDAu, 0xC4u, 0xBFu);
	term_dlg_hline(x0, y0 + (ch - 1) * TM_CH, cw, 0xC0u, 0xC4u, 0xD9u);
	for (i = 1; i < ch - 1; i++)
	{
		mmb_gfx_glyph_cp437(x0, y0 + i * TM_CH, 0xB3u, brd_fg);
		mmb_gfx_glyph_cp437(x0 + (cw - 1) * TM_CW, y0 + i * TM_CH, 0xB3u,
				    brd_fg);
	}
	mmb_gfx_box(x0 + w, y0 + TM_CH, 2 * TM_CW, h - TM_CH, TM_SH_BG, 1,
		    (int)TM_SH_BG);
	mmb_gfx_box(x0 + TM_CW, y0 + h, w, TM_CH, TM_SH_BG, 1, (int)TM_SH_BG);
	if (title && title[0])
	{
		tw = (int)strlen(title);
		tx = col + (cw - tw) / 2;
		if (tx < col + 1)
			tx = col + 1;
		term_put_str((T.pane_left + tx) * TM_CW, y0, title, brd_fg);
	}
}

static void dlg_text(int col, int row, const char *s, int hi)
{
	int x, y, n;

	if (!s)
		return;
	n = (int)strlen(s);
	x = (T.pane_left + col) * TM_CW;
	y = row * TM_CH;
	if (hi && n > 0)
		mmb_gfx_box(x, y, n * TM_CW, TM_CH, TM_SEL_BG, 1, (int)TM_SEL_BG);
	term_put_str(x, y, s, hi ? TM_SEL_FG : TM_DLG_FG);
}

static void term_draw_dlg_list(void)
{
	int i, row;
	const char *btns[4];

	term_dlg_frame(8, 2, 48, 18, " Bookmarks ");
	if (T.dlg_sel < T.dlg_top)
		T.dlg_top = T.dlg_sel;
	if (T.dlg_sel >= T.dlg_top + TM_LIST_VIEW)
		T.dlg_top = T.dlg_sel - TM_LIST_VIEW + 1;
	if (T.dlg_top < 0)
		T.dlg_top = 0;
	if (g_bm_n == 0)
		dlg_text(10, 4, "(none)", 0);
	for (i = 0; i < TM_LIST_VIEW; i++)
	{
		int idx = T.dlg_top + i;
		if (idx >= g_bm_n)
			break;
		row = 4 + i;
		dlg_text(10, row, g_bm[idx].name[0] ? g_bm[idx].name : "(unnamed)",
			 idx == T.dlg_sel);
	}
	btns[0] = "New";
	btns[1] = "Edit";
	btns[2] = "Delete";
	btns[3] = "Connect";
	dlg_text(10, 17, btns[0], T.dlg_btn == 0);
	dlg_text(16, 17, btns[1], T.dlg_btn == 1);
	dlg_text(23, 17, btns[2], T.dlg_btn == 2);
	dlg_text(44, 17, btns[3], T.dlg_btn == 3);
}

static void term_draw_dlg_edit(void)
{
	char port[8];
	char echo[16];
	char box[16];
	const char *save;

	term_dlg_frame(8, 4, 48, 14,
		       T.dlg_edit_idx < 0 ? " New bookmark " : " Edit bookmark ");
	dlg_text(10, 6, "Name", 0);
	dlg_text(16, 6, T.dlg_name[0] ? T.dlg_name : "_", T.dlg_focus == 0);
	dlg_text(10, 8, "Host", 0);
	dlg_text(16, 8, T.dlg_host[0] ? T.dlg_host : "_", T.dlg_focus == 1);
	dlg_text(40, 8, "Port", 0);
	port[0] = 0;
	bm_append_int(port, sizeof(port), T.dlg_port);
	dlg_text(46, 8, port[0] ? port : "_", T.dlg_focus == 2);
	strcpy(echo, T.dlg_echo ? "[X] Echo ON" : "[ ] Echo ON");
	strcpy(box, T.dlg_letterbox ? "[X] Letterboxed" : "[ ] Letterboxed");
	dlg_text(10, 10, echo, T.dlg_focus == 3);
	dlg_text(10, 11, box, T.dlg_focus == 4);
	save = T.dlg_edit_idx < 0 ? "Save" : "Update";
	dlg_text(10, 15, "Cancel", T.dlg_focus == 5);
	dlg_text(44, 15, save, T.dlg_focus == 6);
}

static void term_draw_dlg_del(void)
{
	const char *nm;

	term_dlg_frame(12, 8, 40, 7, " Delete bookmark? ");
	nm = (T.dlg_sel >= 0 && T.dlg_sel < g_bm_n && g_bm[T.dlg_sel].name[0])
		     ? g_bm[T.dlg_sel].name
		     : "";
	dlg_text(14, 10, nm, 0);
	dlg_text(14, 13, "Yes", T.dlg_btn == 0);
	dlg_text(40, 13, "No", T.dlg_btn == 1);
}

static void term_draw_dlg(void)
{
	if (T.dlg == TM_DLG_LIST)
		term_draw_dlg_list();
	else if (T.dlg == TM_DLG_EDIT)
		term_draw_dlg_edit();
	else if (T.dlg == TM_DLG_DEL)
		term_draw_dlg_del();
}

static void term_serial_dump_dlg(void)
{
	int i;
	char port[8];

	if (T.dlg == TM_DLG_NONE)
		return;
	if (T.dlg == TM_DLG_LIST)
	{
		ser("Bookmarks\r\n");
		for (i = 0; i < g_bm_n; i++)
		{
			ser(g_bm[i].name);
			ser("\r\n");
		}
		ser("New\r\n");
		ser("Edit\r\n");
		ser("Delete\r\n");
		ser("Connect\r\n");
		return;
	}
	if (T.dlg == TM_DLG_EDIT)
	{
		ser(T.dlg_edit_idx < 0 ? "New bookmark\r\n" : "Edit bookmark\r\n");
		ser("Name\r\n");
		ser(T.dlg_name);
		ser("\r\nHost\r\n");
		ser(T.dlg_host);
		ser("\r\nPort\r\n");
		port[0] = 0;
		bm_append_int(port, sizeof(port), T.dlg_port);
		ser(port);
		ser("\r\n");
		ser(T.dlg_echo ? "[X] Echo ON\r\n" : "[ ] Echo ON\r\n");
		ser(T.dlg_letterbox ? "[X] Letterboxed\r\n" : "[ ] Letterboxed\r\n");
		ser("Cancel\r\n");
		ser(T.dlg_edit_idx < 0 ? "Save\r\n" : "Update\r\n");
		return;
	}
	if (T.dlg == TM_DLG_DEL)
	{
		ser("Delete bookmark?\r\n");
		if (T.dlg_sel >= 0 && T.dlg_sel < g_bm_n)
		{
			ser(g_bm[T.dlg_sel].name);
			ser("\r\n");
		}
		ser("Yes\r\n");
		ser("No\r\n");
	}
}

static void term_bm_open_list(void)
{
	term_bm_load();
	T.menu = 0;
	T.dlg = TM_DLG_LIST;
	if (T.dlg_sel < 0 || T.dlg_sel >= g_bm_n)
		T.dlg_sel = 0;
	T.dlg_btn = TM_BTN_CONN;
	term_ui_refresh();
}

static void term_bm_open_edit(int is_new)
{
	if (!is_new && g_bm_n <= 0)
		return;
	T.dlg = TM_DLG_EDIT;
	T.dlg_focus = 0;
	if (is_new)
	{
		T.dlg_edit_idx = -1;
		T.dlg_name[0] = 0;
		T.dlg_host[0] = 0;
		T.dlg_port = 23;
		T.dlg_echo = 1;
		T.dlg_letterbox = 1;
	}
	else
	{
		term_bm *b = &g_bm[T.dlg_sel];
		T.dlg_edit_idx = T.dlg_sel;
		strncpy(T.dlg_name, b->name, sizeof(T.dlg_name) - 1);
		T.dlg_name[sizeof(T.dlg_name) - 1] = 0;
		strncpy(T.dlg_host, b->host, sizeof(T.dlg_host) - 1);
		T.dlg_host[sizeof(T.dlg_host) - 1] = 0;
		T.dlg_port = b->port;
		T.dlg_echo = b->echo ? 1 : 0;
		T.dlg_letterbox = b->letterboxed ? 1 : 0;
	}
	term_ui_refresh();
}

static void term_bm_save_edit(void)
{
	term_bm *b;
	char keep[TM_BM_NAME];

	if (!T.dlg_name[0] || !T.dlg_host[0])
		return;
	if (T.dlg_port < 1 || T.dlg_port > 65535)
		T.dlg_port = 23;
	if (T.dlg_edit_idx < 0)
	{
		if (g_bm_n >= TM_BM_MAX)
			return;
		b = &g_bm[g_bm_n++];
	}
	else
		b = &g_bm[T.dlg_edit_idx];
	memset(b, 0, sizeof(*b));
	strncpy(b->name, T.dlg_name, TM_BM_NAME - 1);
	strncpy(b->host, T.dlg_host, sizeof(b->host) - 1);
	b->port = T.dlg_port;
	b->echo = T.dlg_echo ? 1 : 0;
	b->letterboxed = T.dlg_letterbox ? 1 : 0;
	strncpy(keep, b->name, TM_BM_NAME - 1);
	keep[TM_BM_NAME - 1] = 0;
	term_bm_save();
	T.dlg_sel = bm_find_name(keep);
	T.dlg = TM_DLG_LIST;
	T.dlg_btn = TM_BTN_CONN;
	term_ui_refresh();
}

static void term_bm_delete(void)
{
	int i;
	if (T.dlg_sel < 0 || T.dlg_sel >= g_bm_n)
		return;
	for (i = T.dlg_sel; i + 1 < g_bm_n; i++)
		g_bm[i] = g_bm[i + 1];
	g_bm_n--;
	if (T.dlg_sel >= g_bm_n && g_bm_n > 0)
		T.dlg_sel = g_bm_n - 1;
	term_bm_save();
	T.dlg = TM_DLG_LIST;
	T.dlg_btn = TM_BTN_CONN;
	term_ui_refresh();
}

static void term_bm_connect(void)
{
	term_bm *b;
	int i, old_cols, n;

	if (T.dlg_sel < 0 || T.dlg_sel >= g_bm_n)
		return;
	b = &g_bm[T.dlg_sel];
	if (!b->host[0])
		return;
	tcp_close_quiet();
	T.connecting = 0;
	T.net_fail = 0;
	T.net_msg[0] = 0;
	T.char_mode = 0;
	T.no_echo = 0;
	T.echo_user = b->echo ? 1 : 2;
	T.iac = 0;
	T.iac_cmd = 0;
	T.sb = 0;
	T.linelen = 0;
	T.last_eol = 0;
	esc_reset();
	T.menu = 0;
	T.dlg = TM_DLG_NONE;
	T.cur_row = 0;
	T.cur_col = 0;
	T.demo_line = 0;
	old_cols = term_width();
	strncpy(T.host, b->host, sizeof(T.host) - 1);
	T.host[sizeof(T.host) - 1] = 0;
	T.port = b->port;
	T.demo = (strcasecmp(T.host, "demo") == 0 ||
		  strcasecmp(T.host, "demoburst") == 0);
	T.demo_burst = (strcasecmp(T.host, "demoburst") == 0);
	T.letterbox = b->letterboxed ? 1 : 0;
	term_reset_pen();
	term_apply_session_mode();
	term_layout();
	term_init_extra_cols(old_cols);
	term_clamp_cursor();
	for (i = 0; i < T.pane_rows; i++)
		pane_clear_row(i);
	term_fill_pages();
	if (!T.demo)
	{
		if (mmb_tcp_any_open())
		{
			T.net_fail = 1;
			strncpy(T.net_msg, "TCP file open", sizeof(T.net_msg) - 1);
			pane_puts(T.net_msg);
			pane_newline();
		}
		else if (mmb_net_tcp_begin(T.host, T.port) != 0)
		{
			T.net_fail = 1;
			strncpy(T.net_msg, mmb_net_tcp_errmsg(), sizeof(T.net_msg) - 1);
			pane_puts(T.net_msg);
			pane_newline();
		}
		else
		{
			T.connecting = 1;
			T.connect_at = mmb_now_ms();
			pane_puts("Connecting...");
			pane_newline();
		}
	}
	else
	{
		n = T.demo_burst ? 40 : 2;
		T.demo_next = mmb_now_ms();
		while (n > 0 && T.demo_line <= 43)
		{
			demo_emit_line();
			n--;
		}
	}
	if (T.tcp)
		telnet_announce();
	term_ui_refresh();
}

static void term_dlg_close(void)
{
	if (T.dlg == TM_DLG_EDIT || T.dlg == TM_DLG_DEL)
	{
		T.dlg = TM_DLG_LIST;
		T.dlg_btn = TM_BTN_CONN;
		term_ui_refresh();
		return;
	}
	T.dlg = TM_DLG_NONE;
	term_ui_refresh();
}

static void term_edit_add_char(char c)
{
	int n;
	if (T.dlg_focus == 0)
	{
		n = (int)strlen(T.dlg_name);
		if (n + 1 < (int)sizeof(T.dlg_name) && c >= 32 && c < 127)
		{
			T.dlg_name[n] = c;
			T.dlg_name[n + 1] = 0;
		}
	}
	else if (T.dlg_focus == 1)
	{
		n = (int)strlen(T.dlg_host);
		if (n + 1 < (int)sizeof(T.dlg_host) && c >= 32 && c < 127)
		{
			T.dlg_host[n] = c;
			T.dlg_host[n + 1] = 0;
		}
	}
	else if (T.dlg_focus == 2 && c >= '0' && c <= '9')
	{
		int v = T.dlg_port * 10 + (c - '0');
		if (T.dlg_port == 0 && c == '0')
			v = 0;
		if (v <= 65535)
			T.dlg_port = v;
	}
}

static void term_edit_backspace(void)
{
	int n;
	if (T.dlg_focus == 0)
	{
		n = (int)strlen(T.dlg_name);
		if (n > 0)
			T.dlg_name[n - 1] = 0;
	}
	else if (T.dlg_focus == 1)
	{
		n = (int)strlen(T.dlg_host);
		if (n > 0)
			T.dlg_host[n - 1] = 0;
	}
	else if (T.dlg_focus == 2)
		T.dlg_port /= 10;
}

static void term_dlg_list_activate(void)
{
	if (T.dlg_btn == TM_BTN_NEW)
		term_bm_open_edit(1);
	else if (T.dlg_btn == TM_BTN_EDIT)
		term_bm_open_edit(0);
	else if (T.dlg_btn == TM_BTN_DEL)
	{
		if (g_bm_n <= 0)
			return;
		T.dlg = TM_DLG_DEL;
		T.dlg_btn = 0;
		term_ui_refresh();
	}
	else
		term_bm_connect();
}

static int term_dlg_key(char c)
{
	if (T.dlg == TM_DLG_LIST)
	{
		if (c == '\r' || c == '\n')
		{
			term_dlg_list_activate();
			return 1;
		}
		return 1;
	}
	if (T.dlg == TM_DLG_DEL)
	{
		if (c == '\r' || c == '\n')
		{
			if (T.dlg_btn == 0)
				term_bm_delete();
			else
			{
				T.dlg = TM_DLG_LIST;
				T.dlg_btn = TM_BTN_CONN;
				term_ui_refresh();
			}
		}
		if (c == 'y' || c == 'Y')
			term_bm_delete();
		if (c == 'n' || c == 'N')
		{
			T.dlg = TM_DLG_LIST;
			T.dlg_btn = TM_BTN_CONN;
			term_ui_refresh();
		}
		return 1;
	}
	if (T.dlg == TM_DLG_EDIT)
	{
		if (c == '\r' || c == '\n')
		{
			if (T.dlg_focus == 3)
			{
				T.dlg_echo = !T.dlg_echo;
				term_ui_refresh();
				return 1;
			}
			if (T.dlg_focus == 4)
			{
				T.dlg_letterbox = !T.dlg_letterbox;
				term_ui_refresh();
				return 1;
			}
			if (T.dlg_focus == 5)
			{
				term_dlg_close();
				return 1;
			}
			term_bm_save_edit();
			return 1;
		}
		if (c == 8 || c == 127)
		{
			term_edit_backspace();
			term_ui_refresh();
			return 1;
		}
		if (c == ' ' && (T.dlg_focus == 3 || T.dlg_focus == 4))
		{
			if (T.dlg_focus == 3)
				T.dlg_echo = !T.dlg_echo;
			else
				T.dlg_letterbox = !T.dlg_letterbox;
			term_ui_refresh();
			return 1;
		}
		if (c >= 32 && c < 127)
		{
			term_edit_add_char(c);
			term_ui_refresh();
			return 1;
		}
		return 1;
	}
	return 0;
}

static void term_dlg_arrow(int c)
{
	if (T.dlg == TM_DLG_LIST)
	{
		if (c == 'A' && g_bm_n > 0)
		{
			T.dlg_sel--;
			if (T.dlg_sel < 0)
				T.dlg_sel = g_bm_n - 1;
		}
		else if (c == 'B' && g_bm_n > 0)
		{
			T.dlg_sel++;
			if (T.dlg_sel >= g_bm_n)
				T.dlg_sel = 0;
		}
		else if (c == 'C')
		{
			T.dlg_btn++;
			if (T.dlg_btn > TM_BTN_CONN)
				T.dlg_btn = TM_BTN_NEW;
		}
		else if (c == 'D')
		{
			T.dlg_btn--;
			if (T.dlg_btn < TM_BTN_NEW)
				T.dlg_btn = TM_BTN_CONN;
		}
		term_ui_refresh();
		return;
	}
	if (T.dlg == TM_DLG_DEL)
	{
		if (c == 'C' || c == 'D')
			T.dlg_btn = T.dlg_btn ? 0 : 1;
		term_ui_refresh();
		return;
	}
	if (T.dlg == TM_DLG_EDIT)
	{
		if (c == 'A')
		{
			T.dlg_focus--;
			if (T.dlg_focus < 0)
				T.dlg_focus = 6;
		}
		else if (c == 'B')
		{
			T.dlg_focus++;
			if (T.dlg_focus > 6)
				T.dlg_focus = 0;
		}
		else if (c == 'C' || c == 'D')
		{
			if (T.dlg_focus == 1)
				T.dlg_focus = 2;
			else if (T.dlg_focus == 2)
				T.dlg_focus = 1;
			else if (T.dlg_focus == 5)
				T.dlg_focus = 6;
			else if (T.dlg_focus == 6)
				T.dlg_focus = 5;
		}
		term_ui_refresh();
	}
}

static void term_menu_activate(void)
{
	if (T.menu_sel == 1)
	{
		term_toggle_echo();
		return;
	}
	if (T.menu_sel == 2)
	{
		term_toggle_letterbox();
		return;
	}
	if (T.menu_sel == 3)
	{
		term_bm_open_list();
		return;
	}
	term_exit();
}

static void apply_option(int cmd, int opt)
{
	if (opt == TELOPT_ECHO)
	{
		T.echo_user = 0;
		if (cmd == WILL)
		{
			T.no_echo = 1;
			send_iac(DO, TELOPT_ECHO);
		}
		else if (cmd == WONT)
		{
			T.no_echo = 0;
			send_iac(DONT, TELOPT_ECHO);
		}
		else if (cmd == DO)
			send_iac(WONT, TELOPT_ECHO);
		else
			send_iac(WONT, TELOPT_ECHO);
		return;
	}
	if (opt == TELOPT_SGA)
	{
		if (cmd == WILL)
		{
			T.char_mode = 1;
			send_iac(DO, TELOPT_SGA);
		}
		else if (cmd == WONT)
		{
			T.char_mode = 0;
			send_iac(DONT, TELOPT_SGA);
		}
		else if (cmd == DO)
		{
			T.char_mode = 1;
			send_iac(WILL, TELOPT_SGA);
		}
		else
		{
			T.char_mode = 0;
			send_iac(WONT, TELOPT_SGA);
		}
		return;
	}
	if (opt == TELOPT_NAWS)
	{
		if (cmd == DO)
		{
			send_iac(WILL, TELOPT_NAWS);
			send_naws();
		}
		else if (cmd == WILL)
			send_iac(DONT, TELOPT_NAWS);
		else
			send_iac(WONT, TELOPT_NAWS);
		return;
	}
	if (opt == TELOPT_TTYPE)
	{
		if (cmd == DO)
		{
			send_iac(WILL, TELOPT_TTYPE);
			send_ttype();
		}
		else if (cmd == WILL)
			send_iac(DONT, TELOPT_TTYPE);
		else
			send_iac(WONT, TELOPT_TTYPE);
		return;
	}
	if (cmd == WILL)
		send_iac(DONT, opt);
	else if (cmd == DO)
		send_iac(WONT, opt);
}

static void ansi_cup(int row, int col)
{
	if (row < 1)
		row = 1;
	if (col < 1)
		col = 1;
	T.cur_row = row - 1;
	T.cur_col = col - 1;
	if (T.cur_row >= T.pane_rows)
		T.cur_row = T.pane_rows - 1;
	if (T.cur_col >= term_width())
		T.cur_col = term_width() - 1;
	if (T.cur_row < 0)
		T.cur_row = 0;
	if (T.cur_col < 0)
		T.cur_col = 0;
}

static void ansi_sgr(void)
{
	int i, n, v;
	n = T.ansi_narg > 0 ? T.ansi_narg : 1;
	if (T.ansi_narg == 0)
		T.ansi_arg[0] = 0;
	for (i = 0; i < n; i++)
	{
		v = T.ansi_arg[i];
		if (v == 0)
			term_reset_pen();
		else if (v == 1)
			T.ansi_bold = 1;
		else if (v == 7)
			T.ansi_inv = 1;
		else if (v == 22)
			T.ansi_bold = 0;
		else if (v == 27)
			T.ansi_inv = 0;
		else if (v >= 30 && v <= 37)
			T.cur_fg = ansi_pal(v - 30);
		else if (v >= 90 && v <= 97)
			T.cur_fg = ansi_pal(v - 90 + 8);
		else if (v >= 40 && v <= 47)
			T.cur_bg = ansi_pal(v - 40);
		else if (v >= 100 && v <= 107)
			T.cur_bg = ansi_pal(v - 100 + 8);
		else if (v == 39)
			T.cur_fg = TM_FG;
		else if (v == 49)
			T.cur_bg = TM_BG;
		else if ((v == 38 || v == 48) && i + 2 < n && T.ansi_arg[i + 1] == 5)
		{
			unsigned c = ansi_256(T.ansi_arg[i + 2]);
			if (v == 38)
				T.cur_fg = c;
			else
				T.cur_bg = c;
			i += 2;
		}
	}
}

static void ansi_reply(const char *s)
{
	if (T.tcp && s)
		mmb_net_tcp_send(s, (unsigned)strlen(s));
}

static void ansi_dsr(void)
{
	char buf[24];
	int n = 0, r = T.cur_row + 1, c = T.cur_col + 1;
	buf[n++] = 27;
	buf[n++] = '[';
	if (r >= 10)
		buf[n++] = (char)('0' + r / 10);
	buf[n++] = (char)('0' + r % 10);
	buf[n++] = ';';
	if (c >= 10)
		buf[n++] = (char)('0' + (c / 10) % 10);
	if (c >= 100)
		buf[n++] = (char)('0' + c / 100);
	buf[n++] = (char)('0' + c % 10);
	buf[n++] = 'R';
	buf[n] = 0;
	ansi_reply(buf);
}

static int ansi_arg(int i, int dflt)
{
	if (i < T.ansi_narg && T.ansi_arg[i] > 0)
		return T.ansi_arg[i];
	return dflt;
}

static void ansi_erase_line(int mode)
{
	int c, a, b;
	if (T.cur_row < 0 || T.cur_row >= T.pane_rows)
		return;
	a = 0;
	b = term_width();
	if (mode == 0)
		a = T.cur_col;
	else if (mode == 1)
		b = T.cur_col + 1;
	for (c = a; c < b; c++)
	{
		T.cell[T.cur_row][c] = ' ';
		T.cell_fg[T.cur_row][c] = term_pen();
		T.cell_bg[T.cur_row][c] = term_paper();
	}
	mark_dirty_row(T.cur_row);
}

static void ansi_erase_disp(int mode)
{
	int r, a, b;
	a = 0;
	b = T.pane_rows;
	if (mode == 0)
		a = T.cur_row;
	else if (mode == 1)
		b = T.cur_row + 1;
	for (r = a; r < b; r++)
		pane_clear_row(r);
	if (mode == 2 || mode == 3)
	{
		T.cur_row = 0;
		T.cur_col = 0;
		mark_dirty_full();
		return;
	}
	for (r = a; r < b; r++)
		mark_dirty_row(r);
}

static void ansi_exec_csi(char cmd)
{
	int n = ansi_arg(0, 1);
	if (cmd == 'A')
	{
		T.cur_row -= n;
		if (T.cur_row < 0)
			T.cur_row = 0;
	}
	else if (cmd == 'B')
	{
		T.cur_row += n;
		if (T.cur_row >= T.pane_rows)
			T.cur_row = T.pane_rows - 1;
	}
	else if (cmd == 'C')
	{
		T.cur_col += n;
		if (T.cur_col >= term_width())
			T.cur_col = term_width() - 1;
	}
	else if (cmd == 'D')
	{
		T.cur_col -= n;
		if (T.cur_col < 0)
			T.cur_col = 0;
	}
	else if (cmd == 'H' || cmd == 'f')
		ansi_cup(ansi_arg(0, 1), ansi_arg(1, 1));
	else if (cmd == 'J')
		ansi_erase_disp(T.ansi_narg ? T.ansi_arg[0] : 0);
	else if (cmd == 'K')
		ansi_erase_line(T.ansi_narg ? T.ansi_arg[0] : 0);
	else if (cmd == 'm')
		ansi_sgr();
	else if (cmd == 's')
	{
		T.sav_row = T.cur_row;
		T.sav_col = T.cur_col;
	}
	else if (cmd == 'u')
		ansi_cup(T.sav_row + 1, T.sav_col + 1);
	else if (cmd == 'n')
	{
		if (ansi_arg(0, 0) == 6)
			ansi_dsr();
		else if (ansi_arg(0, 0) == 5)
			ansi_reply("\x1b[0n");
	}
	else if (cmd == 'c')
		ansi_reply("\x1b[?1;2c");
}

static void ansi_reset_csi(void)
{
	int i;
	T.ansi_narg = 0;
	T.ansi_priv = 0;
	for (i = 0; i < TM_ANSI_ARGS; i++)
		T.ansi_arg[i] = 0;
}

static int ansi_feed(unsigned char b)
{
	if (T.ansi_st == 0)
	{
		if (b == 27)
		{
			T.ansi_st = 1;
			return 1;
		}
		return 0;
	}
	if (T.ansi_st == 1)
	{
		if (b == '[')
		{
			T.ansi_st = 2;
			ansi_reset_csi();
			return 1;
		}
		if (b == ']')
		{
			T.ansi_st = 4;
			return 1;
		}
		if (b == '7')
		{
			T.sav_row = T.cur_row;
			T.sav_col = T.cur_col;
			T.ansi_st = 0;
			return 1;
		}
		if (b == '8')
		{
			ansi_cup(T.sav_row + 1, T.sav_col + 1);
			T.ansi_st = 0;
			return 1;
		}
		if (b == 'c')
		{
			term_reset_pen();
			ansi_erase_disp(2);
			T.ansi_st = 0;
			return 1;
		}
		T.ansi_st = 0;
		return 1;
	}
	if (T.ansi_st == 4)
	{
		if (b == 7 || b == 27)
			T.ansi_st = (b == 27) ? 5 : 0;
		return 1;
	}
	if (T.ansi_st == 5)
	{
		T.ansi_st = 0;
		return 1;
	}
	if (b == '?')
	{
		T.ansi_priv = 1;
		return 1;
	}
	if (b >= '0' && b <= '9')
	{
		if (T.ansi_narg == 0)
			T.ansi_narg = 1;
		T.ansi_arg[T.ansi_narg - 1] =
			T.ansi_arg[T.ansi_narg - 1] * 10 + (b - '0');
		return 1;
	}
	if (b == ';')
	{
		if (T.ansi_narg < TM_ANSI_ARGS)
			T.ansi_narg++;
		return 1;
	}
	if ((b >= 'A' && b <= 'Z') || (b >= 'a' && b <= 'z'))
	{
		if (!T.ansi_priv)
			ansi_exec_csi((char)b);
		T.ansi_st = 0;
		return 1;
	}
	T.ansi_st = 0;
	return 1;
}

static void incoming_byte(unsigned char b)
{
	if (T.sb)
	{
		if (T.iac == 1)
		{
			T.iac = 0;
			if (b == SE)
			{
				if (T.sb_opt == TELOPT_TTYPE && T.sb_cmd == TTYPE_SEND)
					send_ttype();
				T.sb = 0;
			}
		}
		else if (b == IAC)
			T.iac = 1;
		else if (T.sb_opt < 0)
			T.sb_opt = (int)b;
		else if (T.sb_cmd < 0)
			T.sb_cmd = (int)b;
		return;
	}
	if (T.iac == 1)
	{
		if (b == IAC)
		{
			T.iac = 0;
			pane_put((char)b);
			return;
		}
		if (b == SB)
		{
			T.iac = 0;
			T.sb = 1;
			T.sb_opt = -1;
			T.sb_cmd = -1;
			return;
		}
		if (b == WILL || b == WONT || b == DO || b == DONT)
		{
			T.iac_cmd = b;
			T.iac = 2;
			return;
		}
		T.iac = 0;
		return;
	}
	if (T.iac == 2)
	{
		apply_option(T.iac_cmd, b);
		T.iac = 0;
		return;
	}
	if (b == IAC)
	{
		T.iac = 1;
		return;
	}
	if (ansi_feed(b))
		return;
	if (b == 0)
		return;
	if (b == '\r')
		T.cur_col = 0;
	else if (b == '\n')
		pane_newline();
	else if (b == 8 || b == 127)
		pane_rubout();
	else if (b >= 32)
		pane_put((char)b);
}

static void tcp_close_quiet(void)
{
	mmb_net_tcp_close();
	T.tcp = 0;
}

static int swallow_crlf_pair(char c)
{
	if (c != '\r' && c != '\n')
	{
		T.last_eol = 0;
		return 0;
	}
	if (T.last_eol && T.last_eol != (unsigned char)c)
	{
		T.last_eol = 0;
		return 1;
	}
	T.last_eol = (unsigned char)c;
	return 0;
}

static void send_enter(void)
{
	mmb_net_tcp_send("\r", 1);
}

static void send_line(void)
{
	T.line[T.linelen] = 0;
	if (T.linelen)
		mmb_net_tcp_send(T.line, (unsigned)T.linelen);
	mmb_net_tcp_send("\r\n", 2);
	T.linelen = 0;
}

static void esc_reset(void)
{
	T.esc = 0;
	T.csi_n = 0;
	T.esc_len = 0;
}

static void esc_send(void)
{
	if (T.tcp && T.esc_len > 0)
		mmb_net_tcp_send(T.esc_buf, (unsigned)T.esc_len);
	esc_reset();
}

static int esc_feed(char c)
{
	if (T.esc_len + 1 < TM_ESC_BUF)
		T.esc_buf[T.esc_len++] = c;
	if (T.esc == 1)
	{
		if (c == '[' || c == 'O')
		{
			T.esc = (c == '[') ? 2 : 5;
			T.csi_n = 0;
			return 1;
		}
		esc_send();
		return 1;
	}
	if (T.esc == 2)
	{
		if (c == '[')
		{
			T.esc = 6;
			return 1;
		}
		if (c == 'A' || c == 'B' || c == 'C' || c == 'D')
		{
			if (T.dlg)
			{
				term_dlg_arrow(c);
				esc_reset();
				return 1;
			}
			if (T.menu)
			{
				if (c == 'A' || c == 'B')
					term_menu_move(c == 'A' ? -1 : 1);
				esc_reset();
				return 1;
			}
			esc_send();
			return 1;
		}
		if (c >= '0' && c <= '9')
		{
			T.csi_n = c - '0';
			T.esc = 4;
			return 1;
		}
		if (T.dlg || T.menu)
		{
			esc_reset();
			return 1;
		}
		esc_send();
		return 1;
	}
	if (T.esc == 4)
	{
		if (c >= '0' && c <= '9')
		{
			T.csi_n = T.csi_n * 10 + (c - '0');
			return 1;
		}
		if (c == '~')
		{
			if (T.csi_n == 21)
			{
				esc_reset();
				term_exit();
				return 1;
			}
			if (T.dlg || T.menu)
			{
				esc_reset();
				return 1;
			}
			esc_send();
			return 1;
		}
		if (T.dlg || T.menu)
		{
			esc_reset();
			return 1;
		}
		esc_send();
		return 1;
	}
	if (T.esc == 5)
	{
		if (T.dlg || T.menu)
		{
			esc_reset();
			return 1;
		}
		esc_send();
		return 1;
	}
	if (T.esc == 6)
	{
		esc_reset();
		return 1;
	}
	return 0;
}

static void demo_emit_line(void)
{
	char buf[64];
	const char *s;

	if (T.demo_line == 0)
		s = "TERM demo";
	else if (T.demo_line == 1)
		s = "Luxurious terminal";
	else if (T.demo_line == 2)
		s = "F10 to leave";
	else if (T.demo_line == 3)
		s = "CP437 shades";
	else if (T.demo_line <= 43)
	{
		fmt_line_num(buf, T.demo_line - 3);
		s = buf;
	}
	else
		return;
	pane_puts(s);
	pane_newline();
	term_serial_dump();
	T.demo_line++;
	T.demo_next = mmb_now_ms() + TM_DEMO_MIN_MS +
		(mmb_now_ms() % (TM_DEMO_MAX_MS - TM_DEMO_MIN_MS + 1));
}

void mmb_cmd_term(void)
{
	mmb_val host, portv;
	int port, i, no_args;

	mmb_skip_sp();
	no_args = (*G.p == 0 || *G.p == ':' || *G.p == '\'');
	if (!no_args)
	{
		host = mmb_expr();
		if (host.type != T_STR)
			mmb_syntax();
		mmb_skip_sp();
		if (*G.p == ',')
			G.p++;
		mmb_skip_sp();
		if (*G.p == 0 || *G.p == ':' || *G.p == '\'')
			mmb_syntax();
		portv = mmb_expr();
		if (portv.type != T_INT && portv.type != T_NUM)
			mmb_syntax();
		port = (int)mmb_as_int(portv);
		if (port < 1 || port > 65535)
			mmb_error("?SYNTAX ERROR");
	}

	memset(&T, 0, sizeof(T));
	if (!no_args)
	{
		strncpy(T.host, host.s, sizeof(T.host) - 1);
		T.host[sizeof(T.host) - 1] = 0;
		T.port = port;
	}
	T.saved_mode = G.gfx.mode;
	T.saved_bits = G.gfx.bits;
	T.demo = (strcasecmp(T.host, "demo") == 0 ||
		  strcasecmp(T.host, "demoburst") == 0);
	T.demo_burst = (strcasecmp(T.host, "demoburst") == 0);
	T.letterbox = 1;
	term_reset_pen();

	ser("TERM\r\n");
	if (!T.demo && mmb_tcp_any_open())
		mmb_error("?FILE");
	if (!T.demo && T.host[0])
	{
		if (mmb_net_tcp_begin(T.host, T.port) != 0)
		{
			T.net_fail = 1;
			strncpy(T.net_msg, mmb_net_tcp_errmsg(), sizeof(T.net_msg) - 1);
		}
		else
		{
			T.connecting = 1;
			T.connect_at = mmb_now_ms();
		}
	}

	term_apply_session_mode();
	G.gfx.write_page = 1;
	G.gfx.display_page = 0;
	mmb_gfx_cls(TM_BG);
	G.gfx.write_page = 0;
	mmb_gfx_cls(TM_BG);
	G.gfx.write_page = 1;

	term_layout();
	for (i = 0; i < T.pane_rows; i++)
		pane_clear_row(i);
	T.cur_row = 0;
	T.cur_col = 0;
	T.active = 1;
	T.dirty_lo = -1;
	T.dirty_hi = -1;
	mark_dirty_full();

	if (T.tcp)
		telnet_announce();
	if (T.net_fail && T.net_msg[0])
	{
		pane_puts(T.net_msg);
		pane_newline();
	}
	else if (T.connecting)
	{
		pane_puts("Connecting...");
		pane_newline();
	}
	else if (no_args)
	{
		pane_puts("Disconnected - Alt+F for Bookmarks");
		pane_newline();
	}
	else if (T.demo)
	{
		int n = T.demo_burst ? 40 : 2;
		T.demo_next = mmb_now_ms();
		while (n > 0 && T.demo_line <= 43)
		{
			demo_emit_line();
			n--;
		}
	}

	term_draw();
	term_serial_dump();
}

int mmb_in_term(void)
{
	return T.active;
}

const char *mmb_term_key(char c)
{
	unsigned char b;

	if (!T.active)
		return "";
	if (T.alt)
	{
		T.alt = 0;
		if (c >= 'A' && c <= 'Z')
			c = (char)(c - 'A' + 'a');
		if (c == 'x')
		{
			term_exit();
			return "";
		}
		if (c == 'f')
		{
			T.menu = 1;
			T.menu_sel = 0;
			T.dlg = TM_DLG_NONE;
			mark_dirty_full();
			term_draw();
			term_serial_dump();
			return "";
		}
		if (T.dlg == TM_DLG_EDIT)
		{
			if (c == 'c')
			{
				term_dlg_close();
				return "";
			}
			if (c == 's')
			{
				term_bm_save_edit();
				return "";
			}
		}
		return "";
	}
	if ((unsigned char)c == 1)
	{
		T.alt = 1;
		return "";
	}
	if (c == 27)
	{
		if (T.esc == 1)
		{
			if (T.dlg)
			{
				term_dlg_close();
				esc_reset();
				return "";
			}
			if (T.menu)
			{
				T.menu = 0;
				esc_reset();
				mark_dirty_full();
				term_draw();
				term_serial_dump();
				return "";
			}
			if (T.esc_len <= 0)
			{
				T.esc_buf[0] = 27;
				T.esc_len = 1;
			}
			esc_send();
		}
		T.esc = 1;
		T.esc_len = 0;
		T.esc_buf[0] = 27;
		T.esc_len = 1;
		T.esc_at = mmb_now_ms();
		return "";
	}
	if (T.esc)
	{
		if (esc_feed(c))
		{
			if (!T.active)
				return "";
			return "";
		}
	}
	if (T.dlg)
	{
		term_dlg_key(c);
		return "";
	}
	if (T.menu)
	{
		if (c == '\r' || c == '\n')
		{
			term_menu_activate();
			return "";
		}
		if (c == 'x' || c == 'X')
		{
			term_exit();
			return "";
		}
		if (c == 'e' || c == 'E')
		{
			term_toggle_echo();
			return "";
		}
		if (c == 'b' || c == 'B' || c == 'w' || c == 'W')
		{
			term_toggle_letterbox();
			return "";
		}
		if (c == 'k' || c == 'K')
		{
			term_bm_open_list();
			return "";
		}
		return "";
	}
	if (!T.tcp && !T.demo)
		return "";
	if (swallow_crlf_pair(c))
		return "";
	if (T.char_mode)
	{
		if (c == '\r' || c == '\n')
		{
			if (T.tcp)
				send_enter();
			term_echo_byte('\n');
			term_echo_flush();
			return "";
		}
		b = (unsigned char)c;
		if (b == 127)
			b = 8;
		if (T.tcp)
		{
			if (b == IAC)
			{
				unsigned char esc[2] = { IAC, IAC };
				mmb_net_tcp_send(esc, 2);
			}
			else
				mmb_net_tcp_send(&b, 1);
		}
		term_echo_byte(b);
		term_echo_flush();
		return "";
	}
	if (c == '\r' || c == '\n')
	{
		if (T.tcp)
			send_line();
		else
			T.linelen = 0;
		term_echo_byte('\n');
		term_echo_flush();
		return "";
	}
	if (c == 8 || c == 127)
	{
		if (T.linelen > 0)
			T.linelen--;
		term_echo_byte(8);
		term_echo_flush();
		return "";
	}
	if ((unsigned char)c < 32)
		return "";
	if (T.linelen + 1 < TM_LINE)
	{
		T.line[T.linelen++] = c;
		term_echo_byte((unsigned char)c);
		term_echo_flush();
	}
	return "";
}

void mmb_term_poll(void)
{
	unsigned char buf[512];
	int n, i, loops, got;
	unsigned t0;

	if (!T.active)
		return;
	if (T.esc == 1 && T.esc_at &&
	    mmb_now_ms() - T.esc_at >= TM_ESC_IDLE_MS)
	{
		if (T.dlg)
		{
			term_dlg_close();
			esc_reset();
		}
		else if (T.menu)
		{
			T.menu = 0;
			esc_reset();
			mark_dirty_full();
			term_draw();
			term_serial_dump();
		}
		else
		{
			if (T.esc_len <= 0)
			{
				T.esc_buf[0] = 27;
				T.esc_len = 1;
			}
			esc_send();
		}
	}
	if (T.demo && T.demo_line <= 43 && mmb_now_ms() >= T.demo_next)
		demo_emit_line();
	if (T.need_draw)
		term_draw();
	if (T.connecting)
	{
		int st = mmb_net_tcp_status();
		if (st == 0 && mmb_now_ms() - T.connect_at < TM_CONNECT_MS)
		{
			if (mmb_net_tcp_cancelling() &&
			    mmb_now_ms() - T.connect_at >= 400)
			{
				mark_dirty_full();
				term_draw();
			}
			return;
		}
		T.connecting = 0;
		if (st == 1)
		{
			T.tcp = 1;
			telnet_announce();
			pane_puts("Connected");
			pane_newline();
			term_draw();
			term_serial_dump();
		}
		else
		{
			if (mmb_now_ms() - T.connect_at >= TM_CONNECT_MS && st == 0)
				strncpy(T.net_msg, "TCP timeout", sizeof(T.net_msg) - 1);
			else
				strncpy(T.net_msg, mmb_net_tcp_errmsg(),
					sizeof(T.net_msg) - 1);
			mmb_net_tcp_close();
			T.net_fail = 1;
			pane_puts(T.net_msg);
			pane_newline();
			term_draw();
			term_serial_dump();
			return;
		}
	}
	if (!T.tcp)
		return;
	got = 0;
	t0 = mmb_now_ms();
	for (loops = 0; loops < 32; loops++)
	{
		n = mmb_net_tcp_recv(buf, sizeof(buf));
		if (n < 0)
		{
			tcp_close_quiet();
			return;
		}
		if (n == 0)
			break;
		for (i = 0; i < n; i++)
			incoming_byte(buf[i]);
		got = 1;
		if (mmb_now_ms() - t0 >= TM_RECV_MS)
			break;
	}
	if (got)
		term_serial_dump();
	if (T.need_draw)
		term_draw();
	mmb_net_yield();
}
