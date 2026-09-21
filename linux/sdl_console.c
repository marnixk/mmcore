#include "sdl_console.h"

#include "sdl_video.h"
#include "mmb_priv.h"

#include <stdint.h>
#include <string.h>

#define CELL_W 8
#define CELL_H 16
#define MAX_COLS 256
#define MAX_ROWS 80
#define MAX_PARAM 8

/* Same 16 CGA colours as util.c's rgb_to_ansi. */
static const unsigned s_pal_rgb[16] = {
	0x000000, 0xAA0000, 0x00AA00, 0xAA5500, 0x0000AA, 0xAA00AA, 0x00AAAA,
	0xAAAAAA, 0x555555, 0xFF5555, 0x55FF55, 0xFFFF55, 0x5555FF, 0xFF55FF,
	0x55FFFF, 0xFFFFFF
};

static uint16_t s_pal[16];
static int s_cols, s_rows;
static int s_cx, s_cy;
static unsigned s_fg, s_bg;
static int s_bold;

/* Escape state machine (persists across write_screen calls). */
static int s_esc; /* 0 none, 1 ESC, 2 CSI, 3 CSI '?', 4 SS3 */
static int s_par[MAX_PARAM];
static int s_npar;
static int s_have_par;

static void init_pal(void)
{
	int i;

	for (i = 0; i < 16; i++)
		s_pal[i] = (uint16_t)sdl_rgb_to_native(s_pal_rgb[i]);
}

static void draw_cell(int col, int row, unsigned ch)
{
	const unsigned char *g;
	uint16_t *fb, fg, bg;
	int sw, r, c, x, y;

	if (col < 0 || row < 0 || col >= s_cols || row >= s_rows)
		return;
	fb = sdl_video_fb();
	if (!fb)
		return;
	sw = sdl_video_width();
	x = col * CELL_W;
	y = row * CELL_H;
	g = &mmb_cp437_8x16[(ch & 0xFFu) * 16u];
	fg = s_pal[s_fg];
	bg = s_pal[s_bg];
	for (r = 0; r < CELL_H; r++)
	{
		unsigned bits = g[r];
		uint16_t *dst = fb + (size_t)(y + r) * sw + x;

		for (c = 0; c < CELL_W; c++)
			dst[c] = (bits & (0x80u >> c)) ? fg : bg;
	}
}

static void clear_cells(int x0, int y0, int x1, int y1, unsigned bg)
{
	uint16_t *fb = sdl_video_fb();
	int sw = sdl_video_width();
	int sh = sdl_video_height();
	uint16_t v = s_pal[bg];
	int x, y, r, c;

	if (!fb)
		return;
	if (x0 < 0)
		x0 = 0;
	if (y0 < 0)
		y0 = 0;
	if (x1 > s_cols)
		x1 = s_cols;
	if (y1 > s_rows)
		y1 = s_rows;
	for (y = y0; y < y1; y++)
	{
		int py = y * CELL_H;

		for (x = x0; x < x1; x++)
		{
			int px = x * CELL_W;

			for (r = 0; r < CELL_H; r++)
			{
				int dy = py + r;
				uint16_t *dst;

				if (dy < 0 || dy >= sh)
					continue;
				dst = fb + (size_t)dy * sw + px;
				for (c = 0; c < CELL_W && px + c < sw; c++)
					dst[c] = v;
			}
		}
	}
}

static void scroll_up(void)
{
	uint16_t *fb = sdl_video_fb();
	int sw = sdl_video_width();
	int sh = sdl_video_height();
	int band = CELL_H;
	int last = (sh > band) ? sh - band : 0;
	uint16_t bg = s_pal[s_bg];
	size_t i;

	if (!fb)
		return;
	if (last > 0)
		memmove(fb, fb + (size_t)band * sw,
			(size_t)last * sw * sizeof(uint16_t));
	for (i = (size_t)last * sw; i < (size_t)sh * sw; i++)
		fb[i] = bg;
}

static void newline(void)
{
	s_cy++;
	if (s_cy >= s_rows)
	{
		scroll_up();
		s_cy = s_rows - 1;
	}
}

static void put_char(unsigned ch)
{
	if (ch >= 32)
	{
		draw_cell(s_cx, s_cy, ch);
		s_cx++;
		if (s_cx >= s_cols)
		{
			s_cx = 0;
			newline();
		}
	}
}

static void erase_line(int mode)
{
	if (mode == 0)
		clear_cells(s_cx, s_cy, s_cols, s_cy + 1, s_bg);
	else if (mode == 1)
		clear_cells(0, s_cy, s_cx + 1, s_cy + 1, s_bg);
	else if (mode == 2)
		clear_cells(0, s_cy, s_cols, s_cy + 1, s_bg);
}

static void erase_display(int mode)
{
	if (mode == 0)
	{
		erase_line(0);
		clear_cells(0, s_cy + 1, s_cols, s_rows, s_bg);
	}
	else if (mode == 1)
	{
		erase_line(1);
		clear_cells(0, 0, s_cols, s_cy, s_bg);
	}
	else if (mode == 2)
	{
		clear_cells(0, 0, s_cols, s_rows, s_bg);
	}
}

static void set_sgr(void)
{
	int any = (s_have_par || s_npar > 0);
	int count = any ? s_npar + 1 : 1;
	int i;

	for (i = 0; i < count; i++)
	{
		int p = any ? s_par[i] : 0;

		if (p == 0)
		{
			s_fg = 7;
			s_bg = 0;
			s_bold = 0;
		}
		else if (p == 1)
			s_bold = 1;
		else if (p == 22)
			s_bold = 0;
		else if (p >= 30 && p <= 37)
			s_fg = (unsigned)(p - 30 + (s_bold ? 8 : 0));
		else if (p >= 90 && p <= 97)
			s_fg = (unsigned)(p - 90 + 8);
		else if (p == 39)
			s_fg = 7;
		else if (p >= 40 && p <= 47)
			s_bg = (unsigned)(p - 40);
		else if (p >= 100 && p <= 107)
			s_bg = (unsigned)(p - 100 + 8);
		else if (p == 49)
			s_bg = 0;
	}
}

static void csi_execute(int final)
{
	int n = (s_have_par || s_npar > 0) ? s_par[0] : 0;

	switch (final)
	{
	case 'm':
		set_sgr();
		break;
	case 'H':
	case 'f':
	{
		int row = s_npar >= 0 && s_have_par ? s_par[0] : 1;
		int col = (s_npar >= 1) ? s_par[1] : 1;

		if (row < 1)
			row = 1;
		if (col < 1)
			col = 1;
		s_cy = row - 1;
		s_cx = col - 1;
		if (s_cx >= s_cols)
			s_cx = s_cols - 1;
		if (s_cy >= s_rows)
			s_cy = s_rows - 1;
		break;
	}
	case 'A':
		s_cy -= (n > 0) ? n : 1;
		if (s_cy < 0)
			s_cy = 0;
		break;
	case 'B':
		s_cy += (n > 0) ? n : 1;
		if (s_cy >= s_rows)
			s_cy = s_rows - 1;
		break;
	case 'C':
		s_cx += (n > 0) ? n : 1;
		if (s_cx >= s_cols)
			s_cx = s_cols - 1;
		break;
	case 'D':
		s_cx -= (n > 0) ? n : 1;
		if (s_cx < 0)
			s_cx = 0;
		break;
	case 'G':
		s_cx = ((n > 0) ? n : 1) - 1;
		if (s_cx < 0)
			s_cx = 0;
		if (s_cx >= s_cols)
			s_cx = s_cols - 1;
		break;
	case 'd':
		s_cy = ((n > 0) ? n : 1) - 1;
		if (s_cy < 0)
			s_cy = 0;
		if (s_cy >= s_rows)
			s_cy = s_rows - 1;
		break;
	case 'J':
		erase_display(n);
		break;
	case 'K':
		erase_line(n);
		break;
	default:
		break; /* cursor visibility (?25h/l) and others ignored */
	}
}

static void esc_byte(unsigned char b)
{
	switch (s_esc)
	{
	case 0:
		if (b == 0x1B)
			s_esc = 1;
		else if (b == 0x07)
			; /* BEL */
		else if (b == 0x08)
		{
			if (s_cx > 0)
				s_cx--;
		}
		else if (b == 0x09)
		{
			s_cx = (s_cx + 8) & ~7;
			if (s_cx >= s_cols)
				s_cx = s_cols - 1;
		}
		else if (b == 0x0A)
			newline();
		else if (b == 0x0D)
			s_cx = 0;
		else
			put_char(b);
		break;
	case 1:
		if (b == '[')
		{
			s_esc = 2;
			s_npar = 0;
			s_have_par = 0;
			s_par[0] = 0;
		}
		else if (b == 'O')
			s_esc = 4;
		else
			s_esc = 0;
		break;
	case 4:
		s_esc = 0; /* SS3 final ignored */
		break;
	case 2:
		if (b == '?' && s_npar == 0 && !s_have_par)
		{
			s_esc = 3;
			break;
		}
		/* fall through */
	case 3:
		if (b >= '0' && b <= '9')
		{
			if (s_npar < MAX_PARAM)
			{
				s_par[s_npar] = s_par[s_npar] * 10 + (b - '0');
				s_have_par = 1;
			}
		}
		else if (b == ';')
		{
			if (s_npar < MAX_PARAM - 1)
			{
				s_npar++;
				s_par[s_npar] = 0;
			}
			s_have_par = 0;
		}
		else if (b >= 0x40 && b <= 0x7E)
		{
			csi_execute(b);
			s_esc = 0;
		}
		else
			s_esc = 0;
		break;
	default:
		s_esc = 0;
		break;
	}
}

void sdl_console_reset(void)
{
	int w = sdl_video_width();
	int h = sdl_video_height();
	uint16_t *fb = sdl_video_fb();
	size_t n, i;

	init_pal();
	s_cols = w / CELL_W;
	s_rows = h / CELL_H;
	if (s_cols > MAX_COLS)
		s_cols = MAX_COLS;
	if (s_rows > MAX_ROWS)
		s_rows = MAX_ROWS;
	s_cx = 0;
	s_cy = 0;
	s_fg = 7;
	s_bg = 0;
	s_bold = 0;
	s_esc = 0;
	if (fb)
	{
		n = (size_t)w * (size_t)h;
		for (i = 0; i < n; i++)
			fb[i] = s_pal[0];
	}
}

void sdl_console_resize(void)
{
	sdl_console_reset();
}

void sdl_console_write(const char *s, unsigned n)
{
	unsigned i;

	for (i = 0; i < n; i++)
		esc_byte((unsigned char)s[i]);
	sdl_video_present();
}

void sdl_console_fill(unsigned rgb888)
{
	uint16_t *fb = sdl_video_fb();
	uint16_t v = (uint16_t)sdl_rgb_to_native(rgb888);
	int w = sdl_video_width();
	int h = sdl_video_height();
	size_t n, i;

	if (fb)
	{
		n = (size_t)w * (size_t)h;
		for (i = 0; i < n; i++)
			fb[i] = v;
	}
	s_cx = 0;
	s_cy = 0;
	sdl_video_present();
}
