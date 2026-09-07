#include "mmb_priv.h"

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
#define TTYPE_IS     0
#define TTYPE_SEND   1

#define TM_COLS     80
#define TM_CH       16
#define TM_CW       8
#define TM_MAX_ROWS 66
#define TM_LINE     256
#define TM_ESC_BUF  16

#define TM_BG       0x12161Cu
#define TM_FG       0xD4CFC4u
#define TM_DIM      0x8A8680u

#define TM_FADE_MS      180
#define TM_SCROLL_MS    100
#define TM_DEMO_MIN_MS  120
#define TM_DEMO_MAX_MS  200

typedef struct {
	int active;
	int demo;
	int tcp;
	int net_fail;
	char net_msg[48];
	char host[80];
	int port;
	int saved_mode;
	int saved_bits;
	int pane_left;
	int pane_rows;
	int vid_rows;
	int vid_cols;
	int cur_row;
	int cur_col;
	char cell[TM_MAX_ROWS][TM_COLS];
	unsigned char row_alpha[TM_MAX_ROWS];
	unsigned row_fade_start[TM_MAX_ROWS];
	int scroll_anim;
	int scroll_off;
	unsigned scroll_start;
	int char_mode;
	int no_echo;
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
	int demo_line;
	unsigned demo_next;
	int serial_gen;
	int need_draw;
} tm_state;

static tm_state T;

static unsigned lerp_rgb(unsigned a, unsigned b, int t256)
{
	int ar = (int)((a >> 16) & 255);
	int ag = (int)((a >> 8) & 255);
	int ab = (int)(a & 255);
	int br = (int)((b >> 16) & 255);
	int bg = (int)((b >> 8) & 255);
	int bb = (int)(b & 255);
	int r = ar + ((br - ar) * t256) / 256;
	int g = ag + ((bg - ag) * t256) / 256;
	int bl = ab + ((bb - ab) * t256) / 256;
	return mmb_rgb_pack(r, g, bl);
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

static void term_serial_dump(void)
{
	int r, c;
	char line[TM_COLS + 1];

	if (!T.active)
		return;
	for (r = 0; r < T.pane_rows; r++)
	{
		for (c = 0; c < TM_COLS; c++)
			line[c] = T.cell[r][c] ? T.cell[r][c] : ' ';
		line[TM_COLS] = 0;
		ser(line);
		ser("\r\n");
	}
	T.serial_gen++;
}

static void term_layout(void)
{
	T.vid_cols = G.plat && G.plat->video_cols ? G.plat->video_cols() : 120;
	T.vid_rows = G.plat && G.plat->video_rows ? G.plat->video_rows() : 33;
	T.pane_left = (T.vid_cols - TM_COLS) / 2;
	if (T.pane_left < 0)
		T.pane_left = 0;
	T.pane_rows = T.vid_rows - 1;
	if (T.pane_rows < 1)
		T.pane_rows = 1;
	if (T.pane_rows > TM_MAX_ROWS)
		T.pane_rows = TM_MAX_ROWS;
}

static void wait_ms(unsigned ms)
{
	unsigned start = mmb_now_ms();
	while (mmb_now_ms() - start < ms)
	{
		if (G.plat && G.plat->poll_input)
			G.plat->poll_input();
	}
}

static void fade_row(int y0, int y1, int w, int t256)
{
	int x, y;
	for (y = y0; y < y1; y++)
		for (x = 0; x < w; x++)
		{
			unsigned px = G.plat->get_pixel(x, y);
			G.plat->set_pixel(x, y, lerp_rgb(px, TM_BG, t256));
		}
}

static void fade_out_screen(void)
{
	int w, h, rows, r, y0, y1;
	unsigned step;

	if (!G.plat || !G.plat->get_pixel || !G.plat->set_pixel)
		return;
	w = G.plat->hdmi_width ? G.plat->hdmi_width() : 640;
	h = G.plat->hdmi_height ? G.plat->hdmi_height() : 480;
	rows = h / TM_CH;
	if (rows < 1)
		rows = 1;
	step = 1000u / (unsigned)rows;
	if (step < 1u)
		step = 1u;
	for (r = 0; r < rows; r++)
	{
		y0 = r * TM_CH;
		y1 = y0 + TM_CH;
		if (y1 > h)
			y1 = h;
		fade_row(y0, y1, w, 128);
		fade_row(y0, y1, w, 256);
		ser(".");
		wait_ms(step);
	}
	ser("\r\n");
}

static void pane_clear_row(int row)
{
	int c;
	if (row < 0 || row >= T.pane_rows)
		return;
	for (c = 0; c < TM_COLS; c++)
		T.cell[row][c] = ' ';
	T.row_alpha[row] = 255;
}

static void pane_scroll_up(void)
{
	int r, c;
	if (T.pane_rows <= 1)
		return;
	for (r = 0; r < T.pane_rows - 1; r++)
	{
		for (c = 0; c < TM_COLS; c++)
			T.cell[r][c] = T.cell[r + 1][c];
		T.row_alpha[r] = T.row_alpha[r + 1];
		T.row_fade_start[r] = T.row_fade_start[r + 1];
	}
	pane_clear_row(T.pane_rows - 1);
	T.cur_row = T.pane_rows - 1;
	T.cur_col = 0;
}

static void pane_newline(void)
{
	if (T.cur_row >= T.pane_rows - 1)
	{
		if (!T.scroll_anim)
		{
			T.scroll_anim = 1;
			T.scroll_off = 0;
			T.scroll_start = mmb_now_ms();
		}
		return;
	}
	T.cur_row++;
	T.cur_col = 0;
	T.row_alpha[T.cur_row] = 0;
	T.row_fade_start[T.cur_row] = mmb_now_ms();
	T.need_draw = 1;
}

static void pane_put(char ch)
{
	if (T.cur_row < 0 || T.cur_row >= T.pane_rows)
		return;
	if (T.cur_col >= TM_COLS)
	{
		pane_newline();
		if (T.scroll_anim)
			return;
	}
	if (T.cur_col < TM_COLS)
	{
		T.cell[T.cur_row][T.cur_col++] = ch;
		if (T.row_alpha[T.cur_row] < 255)
		{
			T.row_alpha[T.cur_row] = 0;
			T.row_fade_start[T.cur_row] = mmb_now_ms();
		}
		T.need_draw = 1;
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

static int pane_x0(void) { return T.pane_left * TM_CW; }
static int pane_x1(void) { return (T.pane_left + TM_COLS) * TM_CW - 1; }
static int pane_y1(void) { return T.pane_rows * TM_CH - 1; }

static void scroll_pixels_up_one(void)
{
	int x, y, x0, x1, y1;
	unsigned px;

	if (!G.plat || !G.plat->get_pixel || !G.plat->set_pixel)
		return;
	x0 = pane_x0();
	x1 = pane_x1();
	y1 = pane_y1();
	for (y = 0; y < y1; y++)
		for (x = x0; x <= x1; x++)
		{
			px = G.plat->get_pixel(x, y + 1);
			G.plat->set_pixel(x, y, px);
		}
	for (x = x0; x <= x1; x++)
		for (y = y1 - TM_CH + 1; y <= y1; y++)
			if (y >= 0)
				G.plat->set_pixel(x, y, TM_BG);
}

static void term_draw_status(void)
{
	char left[24];
	char right[64];
	int row, c, n;

	row = T.vid_rows - 1;
	for (c = 0; c < T.vid_cols; c++)
		G.plat->tui_glyph(c, row, ' ', TM_DIM, TM_BG);
	left[0] = 'F';
	left[1] = '1';
	left[2] = '0';
	left[3] = ' ';
	left[4] = ' ';
	left[5] = 'e';
	left[6] = 'x';
	left[7] = 'i';
	left[8] = 't';
	left[9] = 0;
	for (c = 0; left[c]; c++)
		G.plat->tui_glyph(T.pane_left + c, row, (unsigned)left[c], TM_DIM, TM_BG);
	right[0] = 0;
	if (T.net_fail && T.net_msg[0])
		strncpy(right, T.net_msg, sizeof(right) - 1);
	else if (T.demo)
		strncpy(right, "demo", sizeof(right) - 1);
	else if (T.host[0])
		fmt_hostport(right, sizeof(right));
	right[sizeof(right) - 1] = 0;
	n = (int)strlen(right);
	for (c = 0; c < n && c < TM_COLS; c++)
		G.plat->tui_glyph(T.pane_left + TM_COLS - n + c, row,
			(unsigned)right[c], TM_DIM, TM_BG);
}

static void term_draw(void)
{
	int row, col, c, r, fg;
	unsigned ch;

	if (!G.plat || !G.plat->tui_glyph || !G.plat->tui_present)
		return;
	for (row = 0; row < T.vid_rows; row++)
		for (col = 0; col < T.vid_cols; col++)
			G.plat->tui_glyph(col, row, ' ', TM_BG, TM_BG);
	for (r = 0; r < T.pane_rows; r++)
	{
		fg = (int)lerp_rgb(TM_BG, TM_FG, (int)T.row_alpha[r]);
		for (c = 0; c < TM_COLS; c++)
		{
			ch = (unsigned)T.cell[r][c];
			if (!ch)
				ch = ' ';
			G.plat->tui_glyph(T.pane_left + c, r, ch, (unsigned)fg, TM_BG);
		}
	}
	term_draw_status();
	G.plat->tui_present(0, T.vid_rows * TM_CH - 1);
	T.need_draw = 0;
}

static void term_exit(void)
{
	if (T.tcp)
	{
		mmb_net_tcp_close();
		T.tcp = 0;
	}
	if (T.saved_mode)
	{
		int bits = T.saved_bits;
		if (bits != 8 && bits != 12 && bits != 16 && bits != 32)
			bits = 16;
		mmb_gfx_set_mode(T.saved_mode, bits);
	}
	else if (G.plat && G.plat->fill_screen)
		G.plat->fill_screen(0);
	mmb_console_apply_colour();
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

static void apply_option(int cmd, int opt)
{
	if (opt == TELOPT_ECHO)
	{
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
	if (b == 0)
		return;
	if (b == '\r')
		T.cur_col = 0;
	else if (b == '\n')
		pane_newline();
	else if (b == 8 || b == 127)
	{
		if (T.cur_col > 0)
		{
			T.cur_col--;
			T.cell[T.cur_row][T.cur_col] = ' ';
			T.need_draw = 1;
		}
	}
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
	mmb_net_tcp_send("\r\n", 2);
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
		if (c == 'A' || c == 'B' || c == 'C' || c == 'D')
		{
			esc_send();
			return 1;
		}
		if (c >= '0' && c <= '9')
		{
			T.csi_n = c - '0';
			T.esc = 4;
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
			esc_send();
			return 1;
		}
		esc_send();
		return 1;
	}
	if (T.esc == 5)
	{
		if (c == 'P')
		{
			esc_reset();
			term_exit();
			return 1;
		}
		esc_send();
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
	else if (T.demo_line <= 42)
	{
		fmt_line_num(buf, T.demo_line - 2);
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

static void term_update_fade(void)
{
	int r, a, changed;
	unsigned now, el;

	now = mmb_now_ms();
	changed = 0;
	for (r = 0; r < T.pane_rows; r++)
	{
		if (T.row_alpha[r] >= 255)
			continue;
		el = now - T.row_fade_start[r];
		a = (int)(el * 256u / TM_FADE_MS);
		if (a > 255)
			a = 255;
		if (a != (int)T.row_alpha[r])
		{
			T.row_alpha[r] = (unsigned char)a;
			changed = 1;
		}
	}
	if (changed)
		T.need_draw = 1;
}

static void term_update_scroll(void)
{
	int want, y0, y1;

	if (!T.scroll_anim)
		return;
	want = (int)((mmb_now_ms() - T.scroll_start) * 16 / TM_SCROLL_MS);
	if (want > 16)
		want = 16;
	while (T.scroll_off < want)
	{
		scroll_pixels_up_one();
		T.scroll_off++;
	}
	if (T.scroll_off >= 16)
	{
		pane_scroll_up();
		T.row_alpha[T.cur_row] = 0;
		T.row_fade_start[T.cur_row] = mmb_now_ms();
		T.scroll_anim = 0;
		T.scroll_off = 0;
		T.need_draw = 1;
		term_serial_dump();
		return;
	}
	y0 = pane_y1() - TM_CH + 1;
	y1 = pane_y1();
	if (y0 < 0)
		y0 = 0;
	if (G.plat && G.plat->tui_present)
		G.plat->tui_present(y0, y1);
}

void mmb_cmd_term(void)
{
	mmb_val host, portv;
	int port, mode, bits, i;

	mmb_skip_sp();
	if (*G.p == 0 || *G.p == ':' || *G.p == '\'')
		mmb_syntax();
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

	memset(&T, 0, sizeof(T));
	strncpy(T.host, host.s, sizeof(T.host) - 1);
	T.host[sizeof(T.host) - 1] = 0;
	T.port = port;
	T.saved_mode = G.gfx.mode;
	T.saved_bits = G.gfx.bits;
	T.demo = (strcasecmp(T.host, "demo") == 0);

	ser("TERM\r\n");
	fade_out_screen();

	mode = (G.gfx.mode == 16) ? 16 : 14;
	bits = T.saved_bits;
	if (bits != 8 && bits != 12 && bits != 16 && bits != 32)
		bits = 16;
	mmb_gfx_set_mode(mode, bits);

	if (G.plat && G.plat->tui_prepare)
		G.plat->tui_prepare();
	term_layout();
	for (i = 0; i < T.pane_rows; i++)
	{
		pane_clear_row(i);
		T.row_alpha[i] = 255;
	}
	T.cur_row = 0;
	T.cur_col = 0;
	T.active = 1;

	if (!T.demo)
	{
		if (mmb_net_tcp_open(T.host, T.port) != 0)
		{
			T.net_fail = 1;
			if (!mmb_net_available())
				strncpy(T.net_msg, "Network not available", sizeof(T.net_msg) - 1);
			else
				strncpy(T.net_msg, "Connect failed", sizeof(T.net_msg) - 1);
			T.row_alpha[0] = 0;
			T.row_fade_start[0] = mmb_now_ms();
			pane_puts(T.net_msg);
			pane_newline();
		}
		else
			T.tcp = 1;
	}
	else
	{
		T.demo_next = mmb_now_ms();
		demo_emit_line();
		demo_emit_line();
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
	if (c == 27)
	{
		T.esc = 1;
		T.esc_len = 0;
		if (T.esc_len < TM_ESC_BUF)
			T.esc_buf[T.esc_len++] = c;
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
	if (!T.tcp)
		return "";
	if (swallow_crlf_pair(c))
		return "";
	if (T.char_mode)
	{
		if (c == '\r' || c == '\n')
		{
			send_enter();
			return "";
		}
		b = (unsigned char)c;
		if (b == IAC)
		{
			unsigned char esc[2] = { IAC, IAC };
			mmb_net_tcp_send(esc, 2);
		}
		else
			mmb_net_tcp_send(&b, 1);
		return "";
	}
	if (c == '\r' || c == '\n')
	{
		send_line();
		return "";
	}
	if (c == 8 || c == 127)
	{
		if (T.linelen > 0)
			T.linelen--;
		return "";
	}
	if ((unsigned char)c < 32)
		return "";
	if (T.linelen + 1 < TM_LINE)
		T.line[T.linelen++] = c;
	return "";
}

void mmb_term_poll(void)
{
	unsigned char buf[512];
	int n, i, loops;

	if (!T.active)
		return;
	term_update_fade();
	term_update_scroll();
	if (T.demo && !T.scroll_anim && T.demo_line <= 42 &&
	    mmb_now_ms() >= T.demo_next)
		demo_emit_line();
	if (T.need_draw && !T.scroll_anim)
		term_draw();
	if (!T.tcp)
		return;
	for (loops = 0; loops < 32; loops++)
	{
		n = mmb_net_tcp_recv(buf, sizeof(buf));
		if (n < 0)
		{
			tcp_close_quiet();
			return;
		}
		if (n == 0)
			return;
		for (i = 0; i < n; i++)
			incoming_byte(buf[i]);
		term_serial_dump();
		if (T.need_draw && !T.scroll_anim)
			term_draw();
	}
}
