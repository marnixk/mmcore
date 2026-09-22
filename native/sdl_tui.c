#include "sdl_tui.h"

#include "mmb_priv.h"
#include "sdl_video.h"

#include <string.h>

#define TUI_CW 8
#define TUI_CH 16

extern const unsigned char mmb_tnr_16x32[95 * 32 * 2];
extern const unsigned char mmb_tnr_24x48[95 * 48 * 3];
extern const unsigned char mmb_tnr_32x64[95 * 64 * 4];

static const unsigned char *s_font = mmb_cp437_8x16;

static void plot(int x, int y, uint16_t v)
{
	uint16_t *fb;
	int w, h;

	if (x < 0 || y < 0)
		return;
	w = sdl_video_width();
	h = sdl_video_height();
	if (x >= w || y >= h)
		return;
	fb = sdl_video_fb();
	if (fb)
	{
		fb[(size_t)y * w + x] = v;
		sdl_video_mark_dirty();
	}
}

static unsigned char glyph_row(unsigned ch, unsigned y)
{
	if (y >= TUI_CH)
		return 0;
	return s_font[(ch & 0xFFu) * TUI_CH + y];
}

int sdl_tui_cols(void)
{
	int w = sdl_video_width();

	return (w > 0 ? w : 640) / TUI_CW;
}

int sdl_tui_rows(void)
{
	int h = sdl_video_height();

	return (h > 0 ? h : 480) / TUI_CH;
}

void sdl_tui_prepare(void)
{
	uint16_t *fb = sdl_video_fb();
	size_t n;

	if (!fb)
		return;
	n = (size_t)sdl_video_width() * (size_t)sdl_video_height();
	memset(fb, 0, n * sizeof(uint16_t));
	sdl_video_mark_dirty();
}

void sdl_tui_set_font(const unsigned char *font)
{
	s_font = font ? font : mmb_cp437_8x16;
}

void sdl_tui_glyph(int col, int row, unsigned ch, unsigned fg_rgb, unsigned bg_rgb)
{
	uint16_t fg = (uint16_t)sdl_rgb_to_native(fg_rgb);
	uint16_t bg = (uint16_t)sdl_rgb_to_native(bg_rgb);
	int x0 = col * TUI_CW;
	int y0 = row * TUI_CH;
	int x, y;

	for (y = 0; y < TUI_CH; y++)
	{
		unsigned bits = glyph_row(ch, (unsigned)y);

		for (x = 0; x < TUI_CW; x++)
			plot(x0 + x, y0 + y,
			     (bits & (0x80u >> x)) ? fg : bg);
	}
}

void sdl_tui_present(int y0, int y1)
{
	(void)y0;
	(void)y1;
	sdl_video_present();
}

void sdl_tui_scroll(int x, int y, int w, int h, int dy, unsigned fill_rgb)
{
	uint16_t *fb = sdl_video_fb();
	int sw, sh, row, bpp = (int)sizeof(uint16_t);
	uint16_t fill;
	int ady;

	if (!fb || w < 1 || h < 1 || dy == 0)
		return;
	sw = sdl_video_width();
	sh = sdl_video_height();
	if (x < 0)
		x = 0;
	if (y < 0)
		y = 0;
	if (x >= sw || y >= sh)
		return;
	if (x + w > sw)
		w = sw - x;
	if (y + h > sh)
		h = sh - y;
	ady = dy < 0 ? -dy : dy;
	if (w < 1 || h < 1 || ady >= h)
		return;
	fill = (uint16_t)sdl_rgb_to_native(fill_rgb);
	if (dy > 0)
	{
		for (row = y; row < y + h - dy; row++)
			memmove(fb + (size_t)row * sw + x,
				fb + (size_t)(row + dy) * sw + x,
				(size_t)w * bpp);
		for (row = y + h - dy; row < y + h; row++)
		{
			int px;

			for (px = x; px < x + w; px++)
				fb[(size_t)row * sw + px] = fill;
		}
	}
	else
	{
		for (row = y + h - 1; row >= y + ady; row--)
			memmove(fb + (size_t)row * sw + x,
				fb + (size_t)(row - ady) * sw + x,
				(size_t)w * bpp);
		for (row = y; row < y + ady; row++)
		{
			int px;

			for (px = x; px < x + w; px++)
				fb[(size_t)row * sw + px] = fill;
		}
	}
	sdl_video_mark_dirty();
	sdl_tui_present(y, y + h - 1);
}

static const unsigned char *tnr_native(int scale, unsigned ch, unsigned *gw,
				       unsigned *gh, unsigned *rowb)
{
	if (ch < 32 || ch > 126)
		return 0;
	ch -= 32;
	if (scale == 2)
	{
		*gw = 16;
		*gh = 32;
		*rowb = 2;
		return mmb_tnr_16x32 + ch * 32 * 2;
	}
	if (scale == 3)
	{
		*gw = 24;
		*gh = 48;
		*rowb = 3;
		return mmb_tnr_24x48 + ch * 48 * 3;
	}
	if (scale == 4)
	{
		*gw = 32;
		*gh = 64;
		*rowb = 4;
		return mmb_tnr_32x64 + ch * 64 * 4;
	}
	return 0;
}

void sdl_tui_glyph_n_px(int x_px, int y_px, unsigned ch, unsigned fg_rgb,
			unsigned bg_rgb, int scale, int ink_only)
{
	const unsigned char *glyph;
	uint16_t fg = (uint16_t)sdl_rgb_to_native(fg_rgb);
	uint16_t bg = (uint16_t)sdl_rgb_to_native(bg_rgb);
	unsigned gw, gh, rowb, x, y;
	int y0 = y_px;

	if (scale < 1)
		scale = 1;
	if (scale > 4)
		scale = 4;
	if (y0 < 0)
		return;

	glyph = tnr_native(scale, ch, &gw, &gh, &rowb);
	if (glyph)
	{
		for (y = 0; y < gh; y++)
			for (x = 0; x < gw; x++)
			{
				unsigned bits = glyph[y * rowb + x / 8];
				int on = (bits & (0x80u >> (x % 8))) != 0;

				if (ink_only && !on)
					continue;
				plot(x_px + (int)x, y0 + (int)y, on ? fg : bg);
			}
		return;
	}

	for (y = 0; y < TUI_CH; y++)
	{
		unsigned bits = glyph_row(ch, y);
		unsigned sy, sx;

		for (sy = 0; sy < (unsigned)scale; sy++)
			for (x = 0; x < TUI_CW; x++)
			{
				int on = (bits & (0x80u >> x)) != 0;

				if (ink_only && !on)
					continue;
				for (sx = 0; sx < (unsigned)scale; sx++)
					plot(x_px + (int)(x * scale + sx),
					     y0 + (int)(y * scale + sy),
					     on ? fg : bg);
			}
	}
}

void sdl_tui_glyph_n(int col, int row, unsigned ch, unsigned fg_rgb,
		     unsigned bg_rgb, int scale)
{
	if (scale < 1)
		scale = 1;
	if (scale > 4)
		scale = 4;
	if (col < 0 || row < 0)
		return;
	sdl_tui_glyph_n_px(col * TUI_CW, row * TUI_CH, ch, fg_rgb, bg_rgb, scale, 0);
}

void sdl_tui_glyph2x(int col, int row, unsigned ch, unsigned fg_rgb, unsigned bg_rgb)
{
	sdl_tui_glyph_n(col, row, ch, fg_rgb, bg_rgb, 2);
}

void sdl_tui_fill_px(int x_px, int y_px, int w, int h, unsigned rgb)
{
	uint16_t fill = (uint16_t)sdl_rgb_to_native(rgb);
	int x, y;

	if (w < 1 || h < 1)
		return;
	for (y = 0; y < h; y++)
		for (x = 0; x < w; x++)
			plot(x_px + x, y_px + y, fill);
}
