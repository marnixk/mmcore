#include "mmb_priv.h"
#include "net_rxbuf.h"
#include <string.h>

#define IAC   255
#define DONT  254
#define DO    253
#define WONT  252
#define WILL  251
#define SB    250
#define GA    249
#define EL    248
#define EC    247
#define AYT   246
#define AO    245
#define IP    244
#define BRK   243
#define DM    242
#define NOP   241
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
#define TM_RECV_MS      16
#define TM_RECV_LOOPS   256
#define TM_RECV_IDLE    8
#define TM_RECV_BUF     8192
#define TM_INTERP_YIELD 256
#define TM_DUMP_MS      250
#define TM_SB_MAX       512
#define TM_SB_MS        2000
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
#define TM_RS          0x1e
#define TM_BOX_H       0xC4u
#define TM_BOX_V       0xB3u
#define TM_BOX_TL      0xDAu
#define TM_BOX_TR      0xBFu
#define TM_BOX_BL      0xC0u
#define TM_BOX_BR      0xD9u
#define TM_ANSI_OSC_MS  2000
#define TM_REPLAY_HEX   512
#define TM_FILE_LINE    1280
#define TM_FILE_PEND    16
#define TM_LOG_BUF      32768
#define TM_LOG_FLUSH_MS 10000
#define TM_LOG_WATER    24576

typedef struct {
	int active;
	int demo;
	int demo_burst;
	int demo_iac;
	int demo_iac_step;
	int tcp;
	int connecting;
	unsigned connect_at;
	int net_fail;
	int net_lost;
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
	int sb_n;
	unsigned sb_at;
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
	int alt_pend;
	int menu;
	int menu_sel;
	int replay;
	int replay_st;
	char replay_hex[TM_REPLAY_HEX + 4];
	int replay_hex_n;
	int file_replay;
	char file_path[88];
	int file_sz;
	unsigned file_pos;
	char file_line[TM_FILE_LINE];
	int file_line_n;
	int file_wait;
	int file_done;
	unsigned char file_pend[TM_FILE_PEND];
	int file_pend_n;
	unsigned dump_at;
	int present_full;
	unsigned ansi_at;
	int mon_ansi;
	int mon_iac;
	int mon_sb;
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
	int echo_typed;
} tm_state;

static tm_state T;

static unsigned char term_rx_store[MMB_NET_RX_CAP];
static mmb_net_rxbuf term_rx;

static struct {
	unsigned in_n;
	unsigned out_n;
	unsigned rendered;
	unsigned flush_at;
	unsigned start_at;
	int buf_n;
	char buf[TM_LOG_BUF];
} L;

static int host_is_demo(void)
{
	return strcasecmp(T.host, "demo") == 0 ||
	       strcasecmp(T.host, "demoburst") == 0 ||
	       strcasecmp(T.host, "demoiac") == 0;
}

static int host_is_replay(void)
{
	return strcasecmp(T.host, "replay") == 0;
}

typedef struct {
	char name[TM_BM_NAME];
	char host[80];
	int port;
	int echo;
	int letterboxed;
} term_bm;

static term_bm g_bm[TM_BM_MAX];
static int g_bm_n;
static int dlg_c0, dlg_r0, dlg_cw, dlg_ch;

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

static unsigned term_default_fg(void)
{
	unsigned fg = term_rgb(term_th()->edit_fg);
	int r = (int)((fg >> 16) & 255);
	int g = (int)((fg >> 8) & 255);
	int b = (int)(fg & 255);
	int lum = (299 * r + 587 * g + 114 * b) / 1000;

	if (lum >= 80)
		return fg;
	return term_rgb(7);
}

#define TM_BG      0x000000u
#define TM_FG      term_default_fg()
#define TM_DIM     term_rgb(term_th()->cmt_fg)
#define TM_MENU_BG term_rgb(term_th()->menu_bg)
#define TM_MENU_FG term_rgb(term_th()->menu_fg)
#define TM_HOT     term_rgb(term_th()->hot)
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
static void tcp_lost(const char *why);
static void replay_closed(const char *why);
static void telnet_announce(void);
static void term_reset_pen(void);
static void demo_emit_line(void);
static void esc_reset(void);
static int term_width(void);
static int term_tcp_drain(int idle_max);
static void incoming_feed(const unsigned char *src, int n);
static int term_rx_interpret(void);
static void file_replay_poll(void);
static void term_open_menu(void);
static void term_close_menu(void);
static void term_dlg_refresh(void);
static void term_overlay_chrome(void);
static void term_log_mark(void);
static void term_log_rx(const unsigned char *p, int n);
static void term_log_tx(const unsigned char *p, int n);
static void term_log_event(const char *s);
static void term_log_flush(void);
static void term_log_poll(void);
static void term_log_path(char *dst, unsigned n);

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
	if (T.file_replay)
		return;
	if (!mmb_opt_console_serial())
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
	if (T.menu || T.alt_pend)
	{
		ser("Terminal\r\n");
		ser("Bookmarks\r\n");
		ser(term_want_echo() ? "Echo ON\r\n" : "Echo OFF\r\n");
		ser(term_width_label());
		ser("\r\n");
		ser("Exit\r\n");
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

static int term_fb_h(void)
{
	int h = G.gfx.h;
	int hh;

	if (h < 1)
		h = T.vid_rows * TM_CH;
	if (G.plat && G.plat->hdmi_height)
	{
		hh = G.plat->hdmi_height();
		if (hh > 0 && hh < h)
			h = hh;
	}
	if (h < TM_CH)
		h = TM_CH;
	return h;
}

static int term_status_y(void)
{
	return term_fb_h() - TM_CH;
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

static int term_full_present(void)
{
	return T.tcp && !T.demo;
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
		T.present_full = 1;
	}
	else
		mark_dirty_full();
}

static void pane_newline(void)
{
	if (T.cur_row >= T.pane_rows - 1)
	{
		pane_scroll_smooth();
		term_log_mark();
		return;
	}
	T.cur_row++;
	T.cur_col = 0;
	mark_dirty_row(T.cur_row);
	term_log_mark();
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
		term_log_mark();
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

static void term_cell(int x, int y, unsigned ch, unsigned fg, unsigned bg)
{
	mmb_gfx_box(x, y, TM_CW, TM_CH, bg, 1, (int)bg);
	mmb_gfx_glyph_cp437(x, y, ch, fg);
}

static void term_put_str_bg(int x, int y, const char *s, unsigned fg, unsigned bg)
{
	int i;
	if (!s)
		return;
	for (i = 0; s[i]; i++)
		term_cell(x + i * TM_CW, y, (unsigned char)s[i], fg, bg);
}

static void term_fill_cells(int x, int y, int n, unsigned bg)
{
	if (n > 0)
		mmb_gfx_box(x, y, n * TM_CW, TM_CH, bg, 1, (int)bg);
}

static void term_put_hot(int x, int y, const char *s, char hot, unsigned fg,
			 unsigned hot_fg, unsigned bg)
{
	int i, used = 0;

	if (!s)
		return;
	for (i = 0; s[i]; i++)
	{
		unsigned c_fg = fg;
		char ch = s[i];

		if (!used && (ch == hot || ch == hot + 32 || ch == hot - 32))
		{
			c_fg = hot_fg;
			used = 1;
		}
		term_cell(x + i * TM_CW, y, (unsigned char)ch, c_fg, bg);
	}
}

static void term_draw_status(void)
{
	char right[64];
	int y, n, x0;
	unsigned bg = TM_MENU_BG;
	unsigned fg = TM_MENU_FG;

	if (!T.menu && !T.alt_pend)
		return;
	y = term_status_y();
	x0 = 0;
	mmb_gfx_box(0, y, T.vid_cols * TM_CW, TM_CH, bg, 1, (int)bg);
	term_put_str_bg(x0, y, "Alt-X", TM_HOT, bg);
	term_put_str_bg(x0 + 5 * TM_CW, y, "  ", fg, bg);
	term_put_str_bg(x0 + 7 * TM_CW, y, "Alt-T", TM_HOT, bg);
	if (T.demo)
	{
		term_cell(x0 + 13 * TM_CW, y, 176, fg, bg);
		term_cell(x0 + 14 * TM_CW, y, 177, fg, bg);
		term_cell(x0 + 15 * TM_CW, y, 178, fg, bg);
	}
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
	if (n > T.vid_cols)
		n = T.vid_cols;
	term_put_str_bg((T.vid_cols - n) * TM_CW, y, right, fg, bg);
}

static void term_draw_menu(void)
{
	int x0, y0, w, i, n, L, drop_h, bar_w;
	const char *items[4];
	char echo[16];
	unsigned brd = TM_DLG_FG;
	unsigned title_fg, title_bg;

	if (!T.menu && !T.alt_pend)
		return;
	strcpy(echo, term_want_echo() ? "Echo ON" : "Echo OFF");
	items[0] = "Bookmarks";
	items[1] = echo;
	items[2] = term_width_label();
	items[3] = "Exit";
	n = 4;
	w = 10;
	for (i = 0; i < n; i++)
	{
		L = (int)strlen(items[i]);
		if (L + 2 > w)
			w = L + 2;
	}
	w += 2;
	bar_w = T.vid_cols * TM_CW;
	mmb_gfx_box(0, 0, bar_w, TM_CH, TM_MENU_BG, 1, (int)TM_MENU_BG);
	title_fg = T.menu ? TM_SEL_FG : TM_MENU_FG;
	title_bg = T.menu ? TM_SEL_BG : TM_MENU_BG;
	term_cell(0, 0, ' ', title_fg, title_bg);
	term_put_hot(TM_CW, 0, "Terminal", 'T', title_fg, TM_HOT, title_bg);
	term_cell(9 * TM_CW, 0, ' ', title_fg, title_bg);
	if (!T.menu)
		return;
	x0 = TM_CW;
	y0 = TM_CH;
	drop_h = n + 2;
	mmb_gfx_box(x0, y0, w * TM_CW, drop_h * TM_CH, TM_DLG_BG, 1,
		    (int)TM_DLG_BG);
	term_cell(x0, y0, TM_BOX_TL, brd, TM_DLG_BG);
	for (i = 1; i < w - 1; i++)
		term_cell(x0 + i * TM_CW, y0, TM_BOX_H, brd, TM_DLG_BG);
	term_cell(x0 + (w - 1) * TM_CW, y0, TM_BOX_TR, brd, TM_DLG_BG);
	for (i = 0; i < n; i++)
	{
		int y = y0 + (1 + i) * TM_CH;
		unsigned fg = (i == T.menu_sel) ? TM_SEL_FG : TM_DLG_FG;
		unsigned bg = (i == T.menu_sel) ? TM_SEL_BG : TM_DLG_BG;

		term_cell(x0, y, TM_BOX_V, brd, TM_DLG_BG);
		term_fill_cells(x0 + TM_CW, y, w - 2, bg);
		term_cell(x0 + TM_CW, y, ' ', fg, bg);
		term_put_str_bg(x0 + 2 * TM_CW, y, items[i], fg, bg);
		term_cell(x0 + (w - 2) * TM_CW, y, ' ', fg, bg);
		term_cell(x0 + (w - 1) * TM_CW, y, TM_BOX_V, brd, TM_DLG_BG);
	}
	{
		int y = y0 + (n + 1) * TM_CH;
		term_cell(x0, y, TM_BOX_BL, brd, TM_DLG_BG);
		for (i = 1; i < w - 1; i++)
			term_cell(x0 + i * TM_CW, y, TM_BOX_H, brd, TM_DLG_BG);
		term_cell(x0 + (w - 1) * TM_CW, y, TM_BOX_BR, brd, TM_DLG_BG);
	}
	mmb_gfx_box(x0 + w * TM_CW, y0, 2 * TM_CW, drop_h * TM_CH, TM_SH_BG, 1,
		    (int)TM_SH_BG);
	mmb_gfx_box(x0 + 2 * TM_CW, y0 + drop_h * TM_CH, w * TM_CW, TM_CH,
		    TM_SH_BG, 1, (int)TM_SH_BG);
}

static void term_draw_row(int r)
{
	int c, x, y, wpx;
	unsigned ch, fg, bg;

	if (r < 0 || r >= T.pane_rows)
		return;
	y = r * TM_CH;
	x = T.pane_left * TM_CW;
	wpx = term_width() * TM_CW;
	mmb_gfx_box(x, y, wpx, TM_CH, TM_BG, 1, (int)TM_BG);
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
		mmb_gfx_glyph_cp437(x, y, ch, fg);
	}
}

static void term_copy_rect(int x, int y, int pw, int ph)
{
	uint32_t *s, *d;
	int w, h, row;

	s = mmb_gfx_buf_for(1, &w, &h);
	d = mmb_gfx_buf_for(0, &w, &h);
	if (!s || !d || pw <= 0 || ph <= 0)
		return;
	if (x < 0)
	{
		pw += x;
		x = 0;
	}
	if (y < 0)
	{
		ph += y;
		y = 0;
	}
	if (x + pw > w)
		pw = w - x;
	if (y + ph > h)
		ph = h - y;
	if (pw <= 0 || ph <= 0)
		return;
	for (row = 0; row < ph; row++)
	{
		memcpy(d + (y + row) * w + x, s + (y + row) * w + x,
		       (unsigned)pw * sizeof(uint32_t));
		if ((row & 15) == 15)
			mmb_net_yield();
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
	ph = term_fb_h();
	if (x0 < 0)
		x0 = 0;
	if (x0 + pw > w)
		pw = w - x0;
	if (pw <= 0)
		return;
	if (ph > h)
		ph = h;
	for (y = 0; y < ph; y++)
	{
		memcpy(d + y * w + x0, s + y * w + x0, (unsigned)pw * sizeof(uint32_t));
		if ((y & 15) == 15)
			mmb_net_yield();
	}
}

static void term_copy_rows(int lo, int hi)
{
	int x0, pw, y, ph;

	if (lo < 0)
		lo = 0;
	if (hi >= T.pane_rows)
		hi = T.pane_rows - 1;
	if (hi < lo)
		return;
	x0 = T.pane_left * TM_CW;
	pw = term_width() * TM_CW;
	y = lo * TM_CH;
	ph = (hi - lo + 1) * TM_CH;
	term_copy_rect(x0, y, pw, ph);
}

static void term_present_pane(void)
{
	int x0 = T.pane_left * TM_CW;
	int pw = term_width() * TM_CW;
	int ph = term_fb_h();

	mmb_gfx_present_rect(x0, 0, pw, ph);
}

static void term_present_rows(int lo, int hi)
{
	int x0 = T.pane_left * TM_CW;
	int pw = term_width() * TM_CW;
	int y, ph;

	if (lo < 0)
		lo = 0;
	if (hi >= T.pane_rows)
		hi = T.pane_rows - 1;
	if (hi < lo)
		return;
	y = lo * TM_CH;
	ph = (hi - lo + 1) * TM_CH;
	mmb_gfx_present_rect(x0, y, pw, ph);
}

static void term_copy_screen(void)
{
	uint32_t *s, *d;
	int w, h;

	s = mmb_gfx_buf_for(1, &w, &h);
	d = mmb_gfx_buf_for(0, &w, &h);
	if (!s || !d)
		return;
	memcpy(d, s, (unsigned)w * (unsigned)h * sizeof(uint32_t));
}

static void term_draw(void)
{
	int r, saved, lo, hi, full_screen;

	if (!T.need_draw)
		return;
	saved = G.gfx.write_page;
	G.gfx.write_page = 1;
	G.gfx.display_page = 0;
	full_screen = T.dirty_full;
	if (T.dirty_full)
	{
		mmb_gfx_cls(TM_BG);
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
		{
			term_draw_row(r);
			if ((r & 3) == 0)
				mmb_net_yield();
		}
	}
	if (T.menu || T.alt_pend)
		term_draw_status();
	else
		mmb_gfx_box(0, term_status_y(), T.vid_cols * TM_CW, TM_CH,
			    TM_BG, 1, (int)TM_BG);
	term_draw_menu();
	term_draw_dlg();
	if (full_screen)
	{
		term_copy_screen();
		mmb_gfx_present();
	}
	else if (term_full_present() || T.present_full || T.menu || T.alt_pend)
	{
		term_copy_pane();
		if (T.menu || T.alt_pend)
		{
			int top_h = T.menu ? 8 * TM_CH : TM_CH;
			term_copy_rect(0, 0, T.vid_cols * TM_CW, top_h);
			term_copy_rect(0, term_status_y(), T.vid_cols * TM_CW,
				    TM_CH);
		}
		term_present_pane();
		if (T.menu || T.alt_pend)
		{
			int top_h = T.menu ? 8 * TM_CH : TM_CH;
			mmb_gfx_present_rect(0, 0, T.vid_cols * TM_CW, top_h);
			mmb_gfx_present_rect(0, term_status_y(),
					    T.vid_cols * TM_CW, TM_CH);
		}
	}
	else if (lo >= 0 && hi >= lo)
	{
		term_copy_rows(lo, hi);
		term_copy_rect(0, term_status_y(), T.vid_cols * TM_CW, TM_CH);
		term_present_rows(lo, hi);
	}
	else
	{
		term_copy_pane();
		term_present_pane();
	}
	G.gfx.write_page = saved;
	T.need_draw = 0;
	T.dirty_full = 0;
	T.present_full = 0;
	T.dirty_lo = -1;
	T.dirty_hi = -1;
}

static void term_exit(void)
{
	term_log_flush();
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
	if (!term_rx.data)
		mmb_net_rxbuf_init(&term_rx, term_rx_store, MMB_NET_RX_CAP);
	mmb_net_rxbuf_reset(&term_rx);
	memset(&T, 0, sizeof(T));
}

static void replay_tx(const unsigned char *p, unsigned n)
{
	static const char hexdig[] = "0123456789ABCDEF";
	char line[8 + TM_REPLAY_HEX + 4];
	unsigned i, o, chunk;

	while (n > 0)
	{
		chunk = n > (TM_REPLAY_HEX / 2) ? (TM_REPLAY_HEX / 2) : n;
		o = 0;
		line[o++] = '!';
		line[o++] = 'T';
		line[o++] = 'X';
		line[o++] = ' ';
		for (i = 0; i < chunk; i++)
		{
			line[o++] = hexdig[p[i] >> 4];
			line[o++] = hexdig[p[i] & 15];
		}
		line[o++] = '\r';
		line[o++] = '\n';
		line[o] = 0;
		ser(line);
		p += chunk;
		n -= chunk;
	}
}

static void term_net_send(const void *data, unsigned n)
{
	const unsigned char *pb;

	if (!data || n == 0)
		return;
	pb = (const unsigned char *)data;
	if (pb[0] == IAC)
		term_log_tx(pb, (int)n);
	if (T.replay)
		replay_tx((const unsigned char *)data, n);
	else if (T.tcp && !T.file_replay)
	{
		const unsigned char *p = (const unsigned char *)data;
		int left = (int)n, rc, idle = 0;

		mmb_net_yield();
		while (left > 0)
		{
			rc = mmb_net_tcp_send(p, (unsigned)left);
			if (rc == left)
				return;
			if (rc > 0)
			{
				p += rc;
				left -= rc;
				idle = 0;
				continue;
			}
			if (rc < 0)
				return;
			mmb_net_yield();
			if (T.tcp && !T.replay && !T.file_replay)
				term_tcp_drain(1);
			if (++idle > 500)
				return;
		}
	}
}

static int hexval(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	return -1;
}

static void replay_flush_hex(void)
{
	unsigned char buf[TM_REPLAY_HEX / 2];
	int n = 0, i, hi, lo;

	if (T.replay_hex_n < 2)
	{
		T.replay_hex_n = 0;
		return;
	}
	for (i = 0; i + 1 < T.replay_hex_n && n < (int)sizeof(buf); i += 2)
	{
		hi = hexval(T.replay_hex[i]);
		lo = hexval(T.replay_hex[i + 1]);
		if (hi < 0 || lo < 0)
			break;
		buf[n++] = (unsigned char)((hi << 4) | lo);
	}
	T.replay_hex_n = 0;
	if (n > 0)
	{
		incoming_feed(buf, n);
		if (T.need_draw)
			term_draw();
		if (n < 80 || !T.dump_at ||
		    mmb_now_ms() - T.dump_at >= TM_DUMP_MS)
		{
			term_serial_dump();
			T.dump_at = mmb_now_ms();
		}
	}
}

static int replay_key(char c)
{
	unsigned char b = (unsigned char)c;

	if (T.replay_st == 0)
	{
		if (b == TM_RS)
		{
			T.replay_st = 1;
			T.replay_hex_n = 0;
			return 1;
		}
		return 0;
	}
	if (T.replay_st == 1)
	{
		if (c == 'R' || c == 'r')
		{
			T.replay_st = 2;
			return 1;
		}
		T.replay_st = 0;
		return 0;
	}
	if (T.replay_st == 2)
	{
		if (c == 'X' || c == 'x')
		{
			T.replay_st = 3;
			T.replay_hex_n = 0;
			return 1;
		}
		if (c == 'C' || c == 'c')
		{
			T.replay_st = 4;
			T.replay_hex_n = 0;
			return 1;
		}
		T.replay_st = 0;
		return 0;
	}
	if (T.replay_st == 4)
	{
		/* RS "RC" [reason] NL: the harness hangs up like a remote host */
		if (c == '\n' || c == '\r')
		{
			int i = 0;

			T.replay_st = 0;
			while (i < T.replay_hex_n && T.replay_hex[i] == ' ')
				i++;
			T.replay_hex[T.replay_hex_n] = 0;
			replay_closed(T.replay_hex + i);
			T.replay_hex_n = 0;
			return 1;
		}
		if (c >= ' ' && c < 127 && T.replay_hex_n + 1 < TM_REPLAY_HEX)
			T.replay_hex[T.replay_hex_n++] = c;
		return 1;
	}
	if (c == '\n' || c == '\r')
	{
		T.replay_st = 0;
		replay_flush_hex();
		return 1;
	}
	if (hexval(c) >= 0 && T.replay_hex_n < TM_REPLAY_HEX)
		T.replay_hex[T.replay_hex_n++] = c;
	return 1;
}

static void replay_mon(void)
{
	char line[96];
	int n = 0;

	if (!T.replay)
		return;
	if (T.mon_ansi == T.ansi_st && T.mon_iac == T.iac && T.mon_sb == T.sb)
		return;
	T.mon_ansi = T.ansi_st;
	T.mon_iac = T.iac;
	T.mon_sb = T.sb;
	line[n++] = '!';
	line[n++] = 'M';
	line[n++] = 'O';
	line[n++] = 'N';
	line[n++] = ' ';
	line[n++] = 'a';
	line[n++] = '=';
	line[n++] = (char)('0' + (T.ansi_st % 10));
	line[n++] = ' ';
	line[n++] = 'i';
	line[n++] = '=';
	line[n++] = (char)('0' + (T.iac % 10));
	line[n++] = ' ';
	line[n++] = 's';
	line[n++] = '=';
	line[n++] = (char)('0' + (T.sb ? 1 : 0));
	line[n++] = ' ';
	line[n++] = 'c';
	line[n++] = '=';
	line[n++] = (char)('0' + (T.char_mode ? 1 : 0));
	line[n++] = ' ';
	line[n++] = 'e';
	line[n++] = '=';
	line[n++] = (char)('0' + (term_want_echo() ? 1 : 0));
	line[n++] = '\r';
	line[n++] = '\n';
	line[n] = 0;
	ser(line);
}

static void send_iac(int cmd, int opt)
{
	unsigned char b[3];
	b[0] = IAC;
	b[1] = (unsigned char)cmd;
	b[2] = (unsigned char)opt;
	term_net_send(b, 3);
}

static void send_ttype(void)
{
	static const unsigned char ttype[] = {
		IAC, SB, TELOPT_TTYPE, TTYPE_IS,
		'A', 'N', 'S', 'I',
		IAC, SE
	};
	term_net_send(ttype, (unsigned)sizeof(ttype));
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
	term_net_send(b, 9);
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
		term_log_mark();
	}
}

static void term_echo_byte(unsigned char b)
{
	if (!term_want_echo())
		return;
	if (b == 8 || b == 127)
	{
		if (T.echo_typed > 0)
		{
			T.echo_typed--;
			pane_rubout();
		}
	}
	else if (b == '\r' || b == '\n')
	{
		T.echo_typed = 0;
		pane_newline();
	}
	else if (b >= 32)
	{
		T.echo_typed++;
		pane_put((char)b);
	}
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
	term_overlay_chrome();
}

static void term_toggle_echo(void)
{
	if (term_want_echo())
		T.echo_user = 2;
	else
		T.echo_user = 1;
	if (T.menu || T.alt_pend)
		term_overlay_chrome();
	else
	{
		mark_dirty_full();
		term_draw();
		term_serial_dump();
	}
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
	if (bits < 16)
		bits = 16;
	if (T.letterbox)
		mode = 14;
	else
	{
		mode = T.saved_mode;
		if (mode < 1 || mode > 17)
			mode = 14;
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

static void term_present_overlay(int extra_w, int extra_h)
{
	int x, y, pw, ph;

	if (dlg_cw <= 0 || dlg_ch <= 0)
		return;
	x = dlg_c0 * TM_CW;
	y = dlg_r0 * TM_CH;
	pw = (dlg_cw + extra_w) * TM_CW;
	ph = (dlg_ch + extra_h) * TM_CH;
	term_copy_rect(x, y, pw, ph);
	mmb_gfx_present_rect(x, y, pw, ph);
}

static void term_dlg_refresh(void)
{
	int saved;

	saved = G.gfx.write_page;
	G.gfx.write_page = 1;
	term_draw_dlg();
	term_present_overlay(2, 1);
	G.gfx.write_page = saved;
	term_serial_dump_dlg();
}

static void term_overlay_chrome(void)
{
	int saved;

	saved = G.gfx.write_page;
	G.gfx.write_page = 1;
	term_draw_status();
	term_draw_menu();
	term_copy_rect(0, 0, T.vid_cols * TM_CW, 8 * TM_CH);
	mmb_gfx_present_rect(0, 0, T.vid_cols * TM_CW, 8 * TM_CH);
	term_copy_rect(0, term_status_y(), T.vid_cols * TM_CW, TM_CH);
	mmb_gfx_present_rect(0, term_status_y(), T.vid_cols * TM_CW, TM_CH);
	G.gfx.write_page = saved;
	term_serial_dump();
}

static void term_open_menu(void)
{
	T.menu = 1;
	T.menu_sel = 0;
	T.alt_pend = 0;
	T.dlg = TM_DLG_NONE;
	term_ui_refresh();
}

static void term_close_menu(void)
{
	T.menu = 0;
	T.alt_pend = 0;
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

static void dlg_center(int cw, int ch)
{
	int shw = 2, shh = 1;

	dlg_cw = cw;
	dlg_ch = ch;
	dlg_c0 = (T.vid_cols - cw) / 2;
	dlg_r0 = (T.vid_rows - ch) / 2;
	if (dlg_c0 < 0)
		dlg_c0 = 0;
	if (dlg_r0 < 0)
		dlg_r0 = 0;
	if (dlg_c0 + cw + shw > T.vid_cols && T.vid_cols > cw + shw)
		dlg_c0 = T.vid_cols - cw - shw;
	if (dlg_r0 + ch + shh > T.vid_rows && T.vid_rows > ch + shh)
		dlg_r0 = T.vid_rows - ch - shh;
	if (dlg_c0 < 0)
		dlg_c0 = 0;
	if (dlg_r0 < 0)
		dlg_r0 = 0;
}

static void term_dlg_hline_bg(int x0, int y0, int cw, unsigned left, unsigned mid,
			      unsigned right)
{
	int i;
	unsigned brd = TM_DLG_FG;

	term_cell(x0, y0, left, brd, TM_DLG_BG);
	for (i = 1; i < cw - 1; i++)
		term_cell(x0 + i * TM_CW, y0, mid, brd, TM_DLG_BG);
	term_cell(x0 + (cw - 1) * TM_CW, y0, right, brd, TM_DLG_BG);
}

static void term_dlg_frame(int cw, int ch, const char *title)
{
	int x0, y0, w, h, i, tw, tx;
	unsigned brd = TM_DLG_FG;

	dlg_center(cw, ch);
	x0 = dlg_c0 * TM_CW;
	y0 = dlg_r0 * TM_CH;
	w = cw * TM_CW;
	h = ch * TM_CH;
	mmb_gfx_box(x0, y0, w, h, TM_DLG_BG, 1, (int)TM_DLG_BG);
	term_dlg_hline_bg(x0, y0, cw, TM_BOX_TL, TM_BOX_H, TM_BOX_TR);
	term_dlg_hline_bg(x0, y0 + (ch - 1) * TM_CH, cw, TM_BOX_BL, TM_BOX_H,
			 TM_BOX_BR);
	for (i = 1; i < ch - 1; i++)
	{
		term_cell(x0, y0 + i * TM_CH, TM_BOX_V, brd, TM_DLG_BG);
		term_fill_cells(x0 + TM_CW, y0 + i * TM_CH, cw - 2, TM_DLG_BG);
		term_cell(x0 + (cw - 1) * TM_CW, y0 + i * TM_CH, TM_BOX_V, brd,
			  TM_DLG_BG);
	}
	mmb_gfx_box(x0 + w, y0 + TM_CH, 2 * TM_CW, h - TM_CH, TM_SH_BG, 1,
		    (int)TM_SH_BG);
	mmb_gfx_box(x0 + TM_CW, y0 + h, w, TM_CH, TM_SH_BG, 1, (int)TM_SH_BG);
	if (title && title[0])
	{
		tw = (int)strlen(title);
		tx = dlg_c0 + (cw - tw) / 2;
		if (tx < dlg_c0 + 1)
			tx = dlg_c0 + 1;
		term_put_str_bg(tx * TM_CW, y0, title, brd, TM_DLG_BG);
	}
}

static void dlg_text_at(int dcol, int drow, const char *s, int hi)
{
	int x, y, n;
	unsigned fg, bg;

	if (!s)
		return;
	n = (int)strlen(s);
	x = (dlg_c0 + dcol) * TM_CW;
	y = (dlg_r0 + drow) * TM_CH;
	fg = hi ? TM_SEL_FG : TM_DLG_FG;
	bg = hi ? TM_SEL_BG : TM_DLG_BG;
	if (n > 0)
		term_put_str_bg(x, y, s, fg, bg);
}

static void dlg_field(int dcol, int drow, int width, const char *s, int hi)
{
	int x, y, i, n;
	unsigned fg, bg;

	x = (dlg_c0 + dcol) * TM_CW;
	y = (dlg_r0 + drow) * TM_CH;
	fg = hi ? TM_SEL_FG : TM_DLG_FG;
	bg = hi ? TM_SEL_BG : TM_SH_BG;
	if (width < 1)
		width = 1;
	term_fill_cells(x, y, width, bg);
	s = s ? s : "";
	n = (int)strlen(s);
	if (n > width)
		n = width;
	for (i = 0; i < n; i++)
		term_cell(x + i * TM_CW, y, (unsigned char)s[i], fg, bg);
}

static void dlg_btn(int dcol, int drow, const char *s, int hi)
{
	char buf[24];
	int n;

	if (!s)
		return;
	n = (int)strlen(s);
	if (n > 20)
		n = 20;
	buf[0] = ' ';
	memcpy(buf + 1, s, (unsigned)n);
	buf[n + 1] = ' ';
	buf[n + 2] = 0;
	dlg_text_at(dcol, drow, buf, hi);
}

static void term_draw_dlg_list(void)
{
	int i, row;
	const char *btns[4];

	term_dlg_frame(48, 18, " Bookmarks ");
	if (T.dlg_sel < T.dlg_top)
		T.dlg_top = T.dlg_sel;
	if (T.dlg_sel >= T.dlg_top + TM_LIST_VIEW)
		T.dlg_top = T.dlg_sel - TM_LIST_VIEW + 1;
	if (T.dlg_top < 0)
		T.dlg_top = 0;
	if (g_bm_n == 0)
		dlg_text_at(2, 2, "(none)", 0);
	for (i = 0; i < TM_LIST_VIEW; i++)
	{
		int idx = T.dlg_top + i;
		char name[40];
		if (idx >= g_bm_n)
			break;
		row = 2 + i;
		strncpy(name, g_bm[idx].name[0] ? g_bm[idx].name : "(unnamed)",
			sizeof(name) - 1);
		name[sizeof(name) - 1] = 0;
		dlg_field(2, row, 44, name, idx == T.dlg_sel);
	}
	btns[0] = "New";
	btns[1] = "Edit";
	btns[2] = "Delete";
	btns[3] = "Connect";
	dlg_btn(2, 16, btns[0], T.dlg_btn == 0);
	dlg_btn(8, 16, btns[1], T.dlg_btn == 1);
	dlg_btn(15, 16, btns[2], T.dlg_btn == 2);
	dlg_btn(36, 16, btns[3], T.dlg_btn == 3);
}

static void term_draw_dlg_edit(void)
{
	char port[8];
	char echo[16];
	char box[16];
	const char *save;

	term_dlg_frame(48, 14,
		       T.dlg_edit_idx < 0 ? " New bookmark " : " Edit bookmark ");
	dlg_text_at(2, 2, "Name", 0);
	dlg_field(8, 2, 36, T.dlg_name, T.dlg_focus == 0);
	dlg_text_at(2, 4, "Host", 0);
	dlg_field(8, 4, 22, T.dlg_host, T.dlg_focus == 1);
	dlg_text_at(32, 4, "Port", 0);
	port[0] = 0;
	bm_append_int(port, sizeof(port), T.dlg_port);
	dlg_field(38, 4, 6, port[0] ? port : "", T.dlg_focus == 2);
	strcpy(echo, T.dlg_echo ? "[X] Echo ON" : "[ ] Echo ON");
	strcpy(box, T.dlg_letterbox ? "[X] Letterboxed" : "[ ] Letterboxed");
	dlg_text_at(2, 6, echo, T.dlg_focus == 3);
	dlg_text_at(2, 7, box, T.dlg_focus == 4);
	save = T.dlg_edit_idx < 0 ? "Save" : "Update";
	dlg_btn(2, 12, "Cancel", T.dlg_focus == 5);
	dlg_btn(36, 12, save, T.dlg_focus == 6);
}

static void term_draw_dlg_del(void)
{
	const char *nm;

	term_dlg_frame(40, 7, " Delete bookmark? ");
	nm = (T.dlg_sel >= 0 && T.dlg_sel < g_bm_n && g_bm[T.dlg_sel].name[0])
		     ? g_bm[T.dlg_sel].name
		     : "";
	dlg_text_at(2, 2, nm, 0);
	dlg_btn(2, 5, "Yes", T.dlg_btn == 0);
	dlg_btn(28, 5, "No", T.dlg_btn == 1);
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
	T.demo = host_is_demo();
	T.demo_burst = (strcasecmp(T.host, "demoburst") == 0);
	T.demo_iac = (strcasecmp(T.host, "demoiac") == 0);
	T.replay = host_is_replay();
	T.letterbox = b->letterboxed ? 1 : 0;
	term_reset_pen();
	term_apply_session_mode();
	term_layout();
	term_init_extra_cols(old_cols);
	term_clamp_cursor();
	for (i = 0; i < T.pane_rows; i++)
		pane_clear_row(i);
	term_fill_pages();
	if (T.replay)
	{
		T.tcp = 1;
		T.mon_ansi = -1;
		T.mon_iac = -1;
		T.mon_sb = -1;
		pane_puts("Connected");
		pane_newline();
	}
	else if (!T.demo)
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
		if (!T.demo_iac)
		{
			n = T.demo_burst ? 40 : 2;
			T.demo_next = mmb_now_ms();
			while (n > 0 && T.demo_line <= 43)
			{
				demo_emit_line();
				n--;
			}
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
				term_dlg_refresh();
				return 1;
			}
			if (T.dlg_focus == 4)
			{
				T.dlg_letterbox = !T.dlg_letterbox;
				term_dlg_refresh();
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
			term_dlg_refresh();
			return 1;
		}
		if (c == ' ' && (T.dlg_focus == 3 || T.dlg_focus == 4))
		{
			if (T.dlg_focus == 3)
				T.dlg_echo = !T.dlg_echo;
			else
				T.dlg_letterbox = !T.dlg_letterbox;
			term_dlg_refresh();
			return 1;
		}
		if (c >= 32 && c < 127)
		{
			term_edit_add_char(c);
			term_dlg_refresh();
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
		term_dlg_refresh();
		return;
	}
	if (T.dlg == TM_DLG_DEL)
	{
		if (c == 'C' || c == 'D')
			T.dlg_btn = T.dlg_btn ? 0 : 1;
		term_dlg_refresh();
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
		term_dlg_refresh();
	}
}

static void term_menu_activate(void)
{
	if (T.menu_sel == 0)
	{
		term_bm_open_list();
		return;
	}
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
		term_net_send(s, (unsigned)strlen(s));
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
	if (c >= 100)
		buf[n++] = (char)('0' + c / 100);
	if (c >= 10)
		buf[n++] = (char)('0' + (c / 10) % 10);
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
		if (n > 0 && T.cur_row >= 0 && T.cur_row < T.pane_rows)
		{
			mark_dirty_row(T.cur_row);
			T.present_full = 1;
		}
	}
	else if (cmd == 'G')
		ansi_cup(T.cur_row + 1, ansi_arg(0, 1));
	else if (cmd == 'd')
		ansi_cup(ansi_arg(0, 1), T.cur_col + 1);
	else if (cmd == 'X')
	{
		int c, cnt = n < 1 ? 1 : n;
		int w = term_width();
		if (T.cur_row >= 0 && T.cur_row < T.pane_rows)
		{
			for (c = T.cur_col; c < T.cur_col + cnt && c < w; c++)
			{
				T.cell[T.cur_row][c] = ' ';
				T.cell_fg[T.cur_row][c] = term_pen();
				T.cell_bg[T.cur_row][c] = term_paper();
			}
			mark_dirty_row(T.cur_row);
		}
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
			T.ansi_at = mmb_now_ms();
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
		if (b == '(' || b == ')' || b == '*' || b == '+' || b == '-')
		{
			T.ansi_st = 7;
			return 1;
		}
		T.ansi_st = 0;
		return 1;
	}
	if (T.ansi_st == 7)
	{
		T.ansi_st = 0;
		return 1;
	}
	if (T.ansi_st == 4)
	{
		if (T.ansi_at && mmb_now_ms() - T.ansi_at > TM_ANSI_OSC_MS)
		{
			T.ansi_st = 0;
			return ansi_feed(b);
		}
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

static int telopt_known(unsigned char o)
{
	return o == TELOPT_ECHO || o == TELOPT_SGA ||
	       o == TELOPT_TTYPE || o == TELOPT_NAWS;
}

static int iac_cmd_byte(unsigned char b)
{
	return b == IAC || b == WILL || b == WONT || b == DO || b == DONT ||
	       (b >= NOP && b <= GA);
}

static void ansi_watchdog(void)
{
	if (T.ansi_st != 4 && T.ansi_st != 5)
		return;
	if (T.ansi_at && mmb_now_ms() - T.ansi_at > TM_ANSI_OSC_MS)
		T.ansi_st = 0;
}

static void sb_watchdog(void)
{
	if (!T.sb)
		return;
	if (T.sb_n > TM_SB_MAX)
	{
		T.sb = 0;
		T.iac = 0;
		return;
	}
	if (T.sb_at && mmb_now_ms() - T.sb_at > TM_SB_MS)
	{
		T.sb = 0;
		T.iac = 0;
	}
}

static void incoming_byte(unsigned char b)
{
	sb_watchdog();
	ansi_watchdog();
	if (T.sb)
	{
		T.sb_n++;
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
			T.sb_n = 0;
			T.sb_at = mmb_now_ms();
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
	if (b == 12)
	{
		ansi_erase_disp(2);
		return;
	}
	if (b == '\r')
		T.cur_col = 0;
	else if (b == '\n')
		pane_newline();
	else if (b == 8 || b == 127)
		pane_rubout();
	else if (b >= 32)
		pane_put((char)b);
}

static void term_log_path(char *dst, unsigned n)
{
	if (mmb_fat_ready('C'))
		strncpy(dst, "C:/.termlog", n - 1);
	else
		strncpy(dst, "A:/.termlog", n - 1);
	dst[n - 1] = 0;
}

static void term_log_flush(void)
{
	char path[24];

	if (L.buf_n <= 0)
		return;
	term_log_path(path, sizeof(path));
	mmb_vfs_write(path, L.buf, (unsigned)L.buf_n, 1);
	L.buf_n = 0;
	L.flush_at = mmb_now_ms();
}

static void term_log_poll(void)
{
	if (!G.opt.term_log || L.buf_n <= 0)
		return;
	if (mmb_now_ms() - L.flush_at >= TM_LOG_FLUSH_MS)
		term_log_flush();
}

static void term_log_putc(char c)
{
	if (L.buf_n >= TM_LOG_BUF)
		term_log_flush();
	if (L.buf_n < TM_LOG_BUF)
		L.buf[L.buf_n++] = c;
}

static void term_log_u(unsigned v)
{
	char tmp[12];
	int i = 0;

	if (v == 0)
	{
		term_log_putc('0');
		return;
	}
	while (v && i < 11)
	{
		tmp[i++] = (char)('0' + (v % 10));
		v /= 10;
	}
	while (i > 0)
		term_log_putc(tmp[--i]);
}

static void term_log_line(char kind, const unsigned char *p, int n)
{
	static const char hexdig[] = "0123456789ABCDEF";
	int i, need;

	if (!G.opt.term_log || n <= 0)
		return;
	need = 36 + n * 2;
	if (need > TM_LOG_BUF)
		n = (TM_LOG_BUF - 36) / 2;
	if (L.buf_n + need > TM_LOG_BUF)
		term_log_flush();
	term_log_putc(kind);
	term_log_putc(' ');
	term_log_u(mmb_now_ms() - L.start_at);
	term_log_putc(' ');
	term_log_u(L.in_n);
	term_log_putc(' ');
	term_log_u(L.rendered);
	term_log_putc(' ');
	for (i = 0; i < n; i++)
	{
		term_log_putc(hexdig[p[i] >> 4]);
		term_log_putc(hexdig[p[i] & 15]);
	}
	term_log_putc('\n');
	if (L.buf_n >= TM_LOG_WATER)
		term_log_flush();
}

static void term_log_mark(void)
{
	if (G.opt.term_log)
		L.rendered = L.in_n;
}

static void term_log_rx(const unsigned char *p, int n)
{
	if (!G.opt.term_log || host_is_demo() || !p || n <= 0)
		return;
	L.in_n += (unsigned)n;
	term_log_line('R', p, n);
}

static void term_log_tx(const unsigned char *p, int n)
{
	if (!G.opt.term_log || host_is_demo() || !p || n <= 0)
		return;
	L.out_n += (unsigned)n;
	term_log_line('T', p, n);
}

static void term_log_event(const char *s)
{
	if (!G.opt.term_log || !s || !s[0])
		return;
	term_log_line('E', (const unsigned char *)s, (int)strlen(s));
}

void mmb_term_log_enable(int on)
{
	char path[24];

	if (!on)
	{
		term_log_flush();
		mmb_out("TERM log off in=");
		mmb_outf(0, (int64_t)L.in_n);
		mmb_out(" out=");
		mmb_outf(0, (int64_t)L.out_n);
		mmb_out(" rendered=");
		mmb_outf(0, (int64_t)L.rendered);
		return;
	}
	L.in_n = 0;
	L.out_n = 0;
	L.rendered = 0;
	L.buf_n = 0;
	L.start_at = mmb_now_ms();
	L.flush_at = L.start_at;
	term_log_path(path, sizeof(path));
	mmb_vfs_write(path, "# TERMLOG 2\n", 12, 0);
	mmb_out("TERM log ");
	mmb_out(path);
}

static void term_rx_consume(unsigned n)
{
	unsigned char tmp[256];
	unsigned got;

	while (n > 0)
	{
		got = n;
		if (got > sizeof tmp)
			got = sizeof tmp;
		got = mmb_net_rxbuf_pop(&term_rx, tmp, got);
		if (!got)
			return;
		term_log_rx(tmp, (int)got);
		n -= got;
	}
}

static int term_tcp_drain(int idle_max)
{
	static unsigned char buf[TM_RECV_BUF];
	static int draining;
	int n, loops, idle, got;
	unsigned space, want;

	if (!T.tcp || T.replay || T.file_replay)
		return 0;
	if (idle_max < 1)
		idle_max = 1;
	if (!term_rx.data)
		mmb_net_rxbuf_init(&term_rx, term_rx_store, MMB_NET_RX_CAP);
	if (draining)
		return 0;
	draining = 1;
	idle = 0;
	got = 0;
	for (loops = 0; loops < TM_RECV_LOOPS; loops++)
	{
		space = mmb_net_rxbuf_free(&term_rx);
		if (!space)
			break;
		want = sizeof(buf);
		if (want > space)
			want = space;
		n = mmb_net_tcp_recv(buf, want);
		if (n < 0)
		{
			draining = 0;
			tcp_lost(mmb_net_tcp_close_reason());
			return -1;
		}
		if (n == 0)
		{
			mmb_net_yield();
			if (++idle >= idle_max)
				break;
			continue;
		}
		idle = 0;
		mmb_net_rxbuf_push(&term_rx, buf, (unsigned)n);
		got += n;
		mmb_net_yield();
	}
	draining = 0;
	return got;
}

static int term_tcp_ingest(int idle_max)
{
	int got;

	got = term_tcp_drain(idle_max);
	if (got < 0)
		return -1;
	if (got || mmb_net_rxbuf_used(&term_rx))
		term_rx_interpret();
	return got;
}

static void term_maybe_serial_dump(int got)
{
	if (got <= 0)
		return;
	if (got < 80 || !T.dump_at ||
	    mmb_now_ms() - T.dump_at >= TM_DUMP_MS)
	{
		term_serial_dump();
		T.dump_at = mmb_now_ms();
	}
}

static int term_rx_interpret(void)
{
	int got = 0;

	if (!term_rx.data)
		mmb_net_rxbuf_init(&term_rx, term_rx_store, MMB_NET_RX_CAP);
	while (mmb_net_rxbuf_used(&term_rx) > 0)
	{
		unsigned char b = mmb_net_rxbuf_at(&term_rx, 0);
		unsigned avail = mmb_net_rxbuf_used(&term_rx);

		sb_watchdog();
		ansi_watchdog();
		if (T.sb || T.iac)
		{
			incoming_byte(b);
			term_rx_consume(1);
			got++;
			if ((got & (TM_INTERP_YIELD - 1)) == 0)
			{
				mmb_net_yield();
				if (T.tcp && !T.replay && !T.file_replay &&
				    term_tcp_drain(1) < 0)
					return got;
			}
			continue;
		}
		if (b != IAC)
		{
			incoming_byte(b);
			term_rx_consume(1);
			got++;
			if ((got & (TM_INTERP_YIELD - 1)) == 0)
			{
				mmb_net_yield();
				if (T.tcp && !T.replay && !T.file_replay &&
				    term_tcp_drain(1) < 0)
					return got;
			}
			continue;
		}
		if (avail < 2)
			break;
		{
			unsigned char b1 = mmb_net_rxbuf_at(&term_rx, 1);

			if (b1 == SB)
			{
				if (avail < 3)
					break;
				if (telopt_known(mmb_net_rxbuf_at(&term_rx, 2)))
				{
					incoming_byte(IAC);
					term_rx_consume(1);
					got++;
					continue;
				}
				pane_put((char)IAC);
				term_rx_consume(1);
				got++;
				continue;
			}
			if (iac_cmd_byte(b1))
			{
				incoming_byte(IAC);
				term_rx_consume(1);
				got++;
				continue;
			}
			pane_put((char)IAC);
			term_rx_consume(1);
			got++;
		}
	}
	return got;
}

static void incoming_feed(const unsigned char *src, int n)
{
	if (!src || n <= 0)
		return;
	if (!term_rx.data)
		mmb_net_rxbuf_init(&term_rx, term_rx_store, MMB_NET_RX_CAP);
	while (n > 0)
	{
		unsigned w = mmb_net_rxbuf_push(&term_rx, src, (unsigned)n);

		if (!w)
		{
			if (!term_rx_interpret())
				return;
			continue;
		}
		src += w;
		n -= (int)w;
	}
	term_rx_interpret();
}

static int file_hex(const char *s, unsigned char *dst, int maxn)
{
	int n = 0, hi, lo;

	while (s[0] && s[1] && n < maxn)
	{
		if (s[0] == ' ' || s[0] == '\t')
		{
			s++;
			continue;
		}
		hi = hexval(s[0]);
		lo = hexval(s[1]);
		if (hi < 0 || lo < 0)
			break;
		dst[n++] = (unsigned char)((hi << 4) | lo);
		s += 2;
	}
	return n;
}

static int file_fill(void)
{
	unsigned got, room;

	if (T.file_line_n < 0)
		T.file_line_n = 0;
	room = (unsigned)(TM_FILE_LINE - 1 - T.file_line_n);
	if (!room)
		return 1;
	if ((int)T.file_pos >= T.file_sz)
		return T.file_line_n > 0 ? 1 : 0;
	if (mmb_vfs_read_at(T.file_path, T.file_pos, T.file_line + T.file_line_n,
			    room, &got) != 0)
		return -1;
	if (!got)
		return T.file_line_n > 0 ? 1 : 0;
	T.file_pos += got;
	T.file_line_n += (int)got;
	T.file_line[T.file_line_n] = 0;
	return 1;
}

static int file_next_line(char *dst, int dstsz)
{
	char *nl;
	int n, skip;

	for (;;)
	{
		T.file_line[T.file_line_n] = 0;
		nl = strchr(T.file_line, '\n');
		if (nl)
			break;
		skip = file_fill();
		if (skip < 0)
			return -1;
		if (skip == 0)
		{
			if (T.file_line_n <= 0)
				return 0;
			n = T.file_line_n;
			if (n >= dstsz)
				n = dstsz - 1;
			memcpy(dst, T.file_line, (unsigned)n);
			dst[n] = 0;
			T.file_line_n = 0;
			return 1;
		}
		if (!strchr(T.file_line, '\n') && T.file_line_n >= TM_FILE_LINE - 1)
		{
			n = T.file_line_n;
			if (n >= dstsz)
				n = dstsz - 1;
			memcpy(dst, T.file_line, (unsigned)n);
			dst[n] = 0;
			T.file_line_n = 0;
			return 1;
		}
	}
	n = (int)(nl - T.file_line);
	skip = n;
	if (n > 0 && T.file_line[n - 1] == '\r')
		n--;
	if (n >= dstsz)
		n = dstsz - 1;
	memcpy(dst, T.file_line, (unsigned)n);
	dst[n] = 0;
	skip++;
	T.file_line_n -= skip;
	if (T.file_line_n > 0)
		memmove(T.file_line, T.file_line + skip, (unsigned)T.file_line_n);
	T.file_line[T.file_line_n] = 0;
	return 1;
}

static const char *file_after_ints(const char *p, int want)
{
	int i;

	for (i = 0; i < want; i++)
	{
		while (*p == ' ')
			p++;
		if (*p < '0' || *p > '9')
			return 0;
		while (*p >= '0' && *p <= '9')
			p++;
		if (*p && *p != ' ')
			return 0;
	}
	while (*p == ' ')
		p++;
	return p;
}

static void file_replay_poll(void)
{
	char line[TM_FILE_LINE];
	unsigned char buf[512];
	int loops, rc, n, kind_t;
	char *p;
	const char *hex;

	if (T.file_done || T.file_wait)
		return;
	for (loops = 0; loops < 128; loops++)
	{
		rc = file_next_line(line, (int)sizeof(line));
		if (rc < 0)
		{
			T.file_done = 1;
			ser("!REPLAY DONE\r\n");
			return;
		}
		if (rc == 0)
		{
			T.file_done = 1;
			term_rx_interpret();
			if (T.need_draw)
				term_draw();
			ser("!REPLAY DONE\r\n");
			return;
		}
		p = line;
		while (*p == ' ' || *p == '\t')
			p++;
		if (!p[0] || p[0] == '#')
			continue;
		if (p[0] != 'R' && p[0] != 'T')
			continue;
		kind_t = (p[0] == 'T');
		p++;
		hex = file_after_ints(p, 3);
		if (!hex)
			hex = file_after_ints(p, 2);
		if (!hex)
			continue;
		n = file_hex(hex, buf, (int)sizeof(buf));
		if (!kind_t)
		{
			if (n > 0)
				incoming_feed(buf, n);
			continue;
		}
		if (n <= 0 || buf[0] == IAC)
			continue;
		if (n > TM_FILE_PEND)
			n = TM_FILE_PEND;
		memcpy(T.file_pend, buf, (unsigned)n);
		T.file_pend_n = n;
		T.file_wait = 1;
		term_rx_interpret();
		if (T.need_draw)
			term_draw();
		ser("!REPLAY WAIT\r\n");
		return;
	}
	term_rx_interpret();
	if (T.need_draw)
		term_draw();
}

static void demo_iac_tick(void)
{
	static const unsigned char g1[] = {
		'G', '1', ':', IAC, SB, 'K', 'E', 'E', 'P',
		IAC, SE, 'A', IAC, 0x80, 'B', '\r', '\n'
	};
	static const unsigned char g2[] = {
		'G', '2', ':', IAC, WILL, TELOPT_ECHO,
		IAC, DO, TELOPT_SGA, 'O', 'K', '\r', '\n'
	};
	static const unsigned char g3[] = {
		'G', '3', ':', IAC, IAC, 'X', '\r', '\n'
	};
	static const unsigned char g4[] = {
		'G', '4', ':', IAC, SB, TELOPT_TTYPE, TTYPE_SEND, IAC, SE,
		'T', 'Y', '\r', '\n'
	};
	static const unsigned char g5a[] = { 'G', '5', ':', IAC };
	static const unsigned char g5b[] = { 0x80, 'G', 'O', '\r', '\n' };

	if (T.demo_iac_step == 0)
		incoming_feed(g1, (int)sizeof(g1));
	else if (T.demo_iac_step == 1)
		incoming_feed(g2, (int)sizeof(g2));
	else if (T.demo_iac_step == 2)
		incoming_feed(g3, (int)sizeof(g3));
	else if (T.demo_iac_step == 3)
		incoming_feed(g4, (int)sizeof(g4));
	else if (T.demo_iac_step == 4)
		incoming_feed(g5a, (int)sizeof(g5a));
	else if (T.demo_iac_step == 5)
		incoming_feed(g5b, (int)sizeof(g5b));
	else
		return;
	T.demo_iac_step++;
	term_serial_dump();
}

static void tcp_close_quiet(void)
{
	mmb_net_tcp_close();
	T.tcp = 0;
	T.net_lost = 0;
}

/* The socket reported an error or EOF: remember why, keep whatever is
   still in the ring, and let mmb_term_poll() render both. */
static void tcp_lost(const char *why)
{
	unsigned n;

	mmb_net_tcp_close();
	T.tcp = 0;
	strncpy(T.net_msg, "Connection closed", sizeof(T.net_msg) - 1);
	T.net_msg[sizeof(T.net_msg) - 1] = 0;
	if (why && why[0])
	{
		n = (unsigned)strlen(T.net_msg);
		if (n + 2 < sizeof(T.net_msg))
		{
			T.net_msg[n++] = ':';
			T.net_msg[n++] = ' ';
			strncpy(T.net_msg + n, why, sizeof(T.net_msg) - 1 - n);
			T.net_msg[sizeof(T.net_msg) - 1] = 0;
		}
	}
	T.net_lost = 1;
	term_log_event(T.net_msg);
	ser("!NET ");
	ser(T.net_msg);
	ser("\r\n");
}

static void term_net_lost_report(void)
{
	T.net_lost = 0;
	if (mmb_net_rxbuf_used(&term_rx))
		term_rx_interpret();
	if (T.cur_col != 0)
		pane_newline();
	pane_puts(T.net_msg);
	pane_newline();
	T.net_fail = 1;
	mark_dirty_full();
	term_draw();
	term_serial_dump();
}

static void replay_closed(const char *why)
{
	if (!T.replay || !T.tcp)
		return;
	tcp_lost(why);
	term_net_lost_report();
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
	term_net_send("\r", 1);
}

static void send_line(void)
{
	T.line[T.linelen] = 0;
	if (T.linelen)
		term_net_send(T.line, (unsigned)T.linelen);
	if (T.tcp)
		term_net_send("\r", 1);
	else
		term_net_send("\r\n", 2);
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
		term_net_send(T.esc_buf, (unsigned)T.esc_len);
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
				term_open_menu();
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
		s = "Alt-X to leave";
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
	mmb_val host, portv, pathv;
	int port, i, no_args, file_mode, file_sz;
	char file_path[88];

	mmb_skip_sp();
	file_mode = 0;
	file_sz = 0;
	file_path[0] = 0;
	port = 0;
	host = mmb_str_val("");
	no_args = (*G.p == 0 || *G.p == ':' || *G.p == '\'');
	if (mmb_match("REPLAY"))
	{
		pathv = mmb_expr();
		if (pathv.type != T_STR)
			mmb_syntax();
		if (mmb_vfs_resolve(pathv.s, file_path, (int)sizeof(file_path)) != 0)
			mmb_error("?FILE");
		file_sz = mmb_vfs_size(file_path);
		if (file_sz < 0)
			mmb_error("?FILE");
		file_mode = 1;
		no_args = 0;
	}
	else if (!no_args)
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
	if (!term_rx.data)
		mmb_net_rxbuf_init(&term_rx, term_rx_store, MMB_NET_RX_CAP);
	mmb_net_rxbuf_reset(&term_rx);
	if (file_mode)
	{
		strncpy(T.file_path, file_path, sizeof(T.file_path) - 1);
		T.file_path[sizeof(T.file_path) - 1] = 0;
		T.file_sz = file_sz;
		T.file_replay = 1;
		T.tcp = 1;
		strncpy(T.host, "file", sizeof(T.host) - 1);
	}
	else if (!no_args)
	{
		strncpy(T.host, host.s, sizeof(T.host) - 1);
		T.host[sizeof(T.host) - 1] = 0;
		T.port = port;
	}
	T.saved_mode = G.gfx.mode;
	T.saved_bits = G.gfx.bits;
	T.demo = host_is_demo();
	T.demo_burst = (strcasecmp(T.host, "demoburst") == 0);
	T.demo_iac = (strcasecmp(T.host, "demoiac") == 0);
	T.replay = !T.file_replay && host_is_replay();
	T.letterbox = 1;
	term_reset_pen();

	ser("TERM\r\n");
	if (!T.demo && !T.replay && !T.file_replay && mmb_tcp_any_open())
		mmb_error("?FILE");
	if (T.replay)
	{
		T.tcp = 1;
		T.mon_ansi = -1;
		T.mon_iac = -1;
		T.mon_sb = -1;
	}
	else if (T.file_replay)
		ser("!REPLAY START\r\n");
	else if (!T.demo && T.host[0])
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

	if (T.replay)
	{
		pane_puts("Connected");
		pane_newline();
	}
	else if (T.file_replay)
	{
		pane_puts("Replay");
		pane_newline();
	}
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
		pane_puts("Disconnected - Alt+T for Bookmarks");
		pane_newline();
	}
	else if (T.demo && !T.demo_iac)
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
	if (!T.file_replay)
		term_serial_dump();
	if (T.tcp && !T.file_replay)
		telnet_announce();
	else if (T.file_replay)
		file_replay_poll();
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
	if (T.file_wait && (unsigned char)c != 1 && !T.alt)
	{
		T.file_pend_n = 0;
		T.file_wait = 0;
		file_replay_poll();
		if (T.need_draw)
			term_draw();
		return "";
	}
	if (T.replay && replay_key(c))
	{
		if (T.need_draw)
			term_draw();
		replay_mon();
		return "";
	}
	if ((unsigned char)c != 1)
	{
		unsigned char kb = (unsigned char)c;
		if (!T.alt && !T.esc && !T.dlg && !T.menu &&
		    (T.tcp || T.demo) && swallow_crlf_pair(c))
			return "";
		if (T.tcp && (kb == '\n' || kb == '\r'))
			kb = '\r';
		term_log_tx(&kb, 1);
	}
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
		if (c == 't' || c == 'f')
		{
			term_open_menu();
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
		T.alt_pend = 0;
		if (!T.menu)
			term_close_menu();
		return "";
	}
	if ((unsigned char)c == 1)
	{
		T.alt = 1;
		T.alt_pend = 1;
		term_overlay_chrome();
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
				esc_reset();
				term_close_menu();
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
		if (c == 'w' || c == 'W' || c == 'o' || c == 'O')
		{
			term_toggle_letterbox();
			return "";
		}
		if (c == 'b' || c == 'B' || c == 'k' || c == 'K')
		{
			term_bm_open_list();
			return "";
		}
		return "";
	}
	if (!T.tcp && !T.demo)
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
				term_net_send(esc, 2);
			}
			else
				term_net_send(&b, 1);
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
		{
			T.linelen--;
			term_echo_byte(8);
			term_echo_flush();
		}
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
	int got;

	if (!T.active)
		return;
	term_log_poll();
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
			esc_reset();
			term_close_menu();
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
	if (T.demo_iac)
		demo_iac_tick();
	else if (T.demo && T.demo_line <= 43 && mmb_now_ms() >= T.demo_next)
		demo_emit_line();
	if (T.file_replay)
	{
		file_replay_poll();
		if (T.need_draw)
			term_draw();
		return;
	}
	if (T.replay)
	{
		term_rx_interpret();
		if (T.need_draw)
			term_draw();
		ansi_watchdog();
		sb_watchdog();
		if (T.need_draw)
			term_draw();
		replay_mon();
		return;
	}
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
	{
		if (T.net_lost)
			term_net_lost_report();
		if (T.need_draw)
			term_draw();
		return;
	}
	got = term_tcp_ingest(TM_RECV_IDLE);
	if (got < 0)
		return;
	term_maybe_serial_dump(got);
	if (T.need_draw)
		term_draw();
	got = term_tcp_ingest(TM_RECV_IDLE);
	if (got < 0)
		return;
	term_maybe_serial_dump(got);
	if (T.need_draw)
		term_draw();
	mmb_net_yield();
}
