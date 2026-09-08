#include "kernel.h"
#include "audio.h"
#include "mmbasic.h"
#include <circle/alloc.h>
#include <circle/util.h>
#include <circle/timer.h>
#include <circle/screen.h>
#include <circle/bcmframebuffer.h>
#include <circle/display.h>
#include <circle/startup.h>

static CKernel *s_kernel;

static unsigned rgb_to_raw(unsigned rgb)
{
	unsigned r = (rgb >> 16) & 255;
	unsigned g = (rgb >> 8) & 255;
	unsigned b = rgb & 255;
#if DEPTH == 32
	return COLOR32(r, g, b, 255);
#elif DEPTH == 16
	return COLOR16(r >> 3, g >> 3, b >> 3);
#else
	return (unsigned)((r & 0xE0) | ((g >> 3) & 0x1C) | (b >> 6));
#endif
}

static void plat_write_serial(const char *s, unsigned n)
{
	if (!mmb_opt_console_serial())
		return;
	if (s_kernel)
		s_kernel->Serial().Write(s, n);
}

static void plat_write_screen(const char *s, unsigned n)
{
	if (!mmb_opt_console_screen())
		return;
	if (s_kernel)
		s_kernel->Screen().Write(s, n);
}

static void plat_set_pixel(int x, int y, unsigned rgb)
{
	if (!s_kernel)
		return;
	CScreenDevice &sc = s_kernel->Screen();
	if (x < 0 || y < 0 || (unsigned)x >= sc.GetWidth() || (unsigned)y >= sc.GetHeight())
		return;
	sc.SetPixel((unsigned)x, (unsigned)y, (TScreenColor)rgb_to_raw(rgb));
}

static unsigned plat_get_pixel(int x, int y)
{
	TScreenColor raw;

	if (!s_kernel)
		return 0;
	CScreenDevice &sc = s_kernel->Screen();
	if (x < 0 || y < 0 || (unsigned)x >= sc.GetWidth() || (unsigned)y >= sc.GetHeight())
		return 0;
	raw = sc.GetPixel((unsigned)x, (unsigned)y);
#if DEPTH == 32
	{
		unsigned b = (unsigned)raw & 0xFF;
		unsigned g = ((unsigned)raw >> 8) & 0xFF;
		unsigned r = ((unsigned)raw >> 16) & 0xFF;
		return (r << 16) | (g << 8) | b;
	}
#elif DEPTH == 16
	{
		unsigned r = ((unsigned)raw >> 11) & 0x1F;
		unsigned g = ((unsigned)raw >> 6) & 0x1F;
		unsigned b = (unsigned)raw & 0x1F;
		r = r * 255 / 31;
		g = g * 255 / 31;
		b = b * 255 / 31;
		return (r << 16) | (g << 8) | b;
	}
#else
	{
		unsigned v = (unsigned)raw;
		unsigned r = v & 0xE0;
		unsigned g = (v << 3) & 0xE0;
		unsigned b = (v << 6) & 0xC0;
		return (r << 16) | (g << 8) | b;
	}
#endif
}

static void plat_fill(unsigned rgb)
{
	if (!s_kernel)
		return;
	/* HDMI only: home + erase-to-end, then paint pixels so a coloured
	 * CLS is not undone by the terminal wipe. Never write ANSI to serial
	 * (tests assert send_line("CLS") == ""). */
	static const char home[] = "\x1b[H\x1b[J";
	s_kernel->Screen().Write(home, sizeof(home) - 1);

	CScreenDevice &sc = s_kernel->Screen();
	TScreenColor c = (TScreenColor)rgb_to_raw(rgb);
	unsigned w = sc.GetWidth(), h = sc.GetHeight(), x, y;
	for (y = 0; y < h; y++)
		for (x = 0; x < w; x++)
			sc.SetPixel(x, y, c);
}

static int plat_w(void)
{
	return s_kernel ? (int)s_kernel->Screen().GetWidth() : 640;
}

static int plat_h(void)
{
	return s_kernel ? (int)s_kernel->Screen().GetHeight() : 480;
}

static int plat_resize_hdmi(int w, int h)
{
	unsigned prev_w, prev_h;

	if (!s_kernel || w < 1 || h < 1)
		return 0;

	CScreenDevice &sc = s_kernel->Screen();
	prev_w = sc.GetWidth();
	prev_h = sc.GetHeight();
	if (prev_w == (unsigned)w && prev_h == (unsigned)h)
		return 1;

	/* Resize() leaves the device unusable on failure; restore the
	 * previous timing immediately so later writes cannot crash. */
	if (sc.Resize((unsigned)w, (unsigned)h))
		return 1;
	sc.Resize(prev_w, prev_h);
	return 0;
}

static void *plat_alloc(unsigned n)
{
	return malloc(n);
}

static void plat_free(void *p)
{
	if (p)
		free(p);
}

static unsigned plat_millis(void)
{
	return CTimer::GetClockTicks() / 1000;
}


static int plat_read_line(char *buf, unsigned maxn, int hide)
{
	if (!s_kernel)
		return -1;
	return s_kernel->ReadLine(buf, maxn, hide);
}

static void plat_poll_input(void)
{
	if (!s_kernel || !mmb_is_running())
		return;
	s_kernel->PollInputChars(mmb_break_key());
}

static int plat_take_break(void)
{
	return s_kernel ? s_kernel->TakeBreak() : 0;
}

static void plat_reboot(void)
{
	reboot();
}

static void plat_audio_set_target(int target)
{
	audio_set_target(target);
}

static void plat_audio_enable(int on)
{
	audio_enable(on);
}

static int plat_audio_write(const short *pcm, unsigned nframes)
{
	return audio_write(pcm, nframes);
}

static unsigned plat_audio_free_frames(void)
{
	return audio_free_frames();
}

static unsigned plat_audio_queued_frames(void)
{
	return audio_queued_frames();
}

static int plat_audio_have_device(void)
{
	return audio_have_device();
}

static void plat_audio_kick(void)
{
	audio_kick();
}

static void plat_audio_flush(void)
{
	audio_flush();
}

#define TUI_CW 8
#define TUI_CH 16

static u8 *s_tui_pix;
static unsigned s_tui_cap;
static unsigned s_tui_w, s_tui_h, s_tui_pitch;

extern "C" const u8 mmb_cp437_8x16[256 * 16];

static const u8 *s_tui_font = mmb_cp437_8x16;

static u8 glyph_row(unsigned ch, unsigned y)
{
	if (y >= TUI_CH)
		return 0;
	ch &= 0xFFu;
	return s_tui_font[ch * TUI_CH + y];
}

static void plat_tui_set_font(const unsigned char *font)
{
	s_tui_font = font ? font : mmb_cp437_8x16;
}

static int plat_video_cols(void)
{
	int w = plat_w();
	if (w < TUI_CW)
		w = 640;
	return w / TUI_CW;
}

static int plat_video_rows(void)
{
	int h = plat_h();
	if (h < TUI_CH)
		h = 480;
	return h / TUI_CH;
}

static void plat_tui_prepare(void)
{
	unsigned w = s_kernel ? s_kernel->Screen().GetWidth() : 640;
	unsigned h = s_kernel ? s_kernel->Screen().GetHeight() : 480;
	unsigned pitch = w * (DEPTH / 8);
	unsigned need = pitch * h;
	if (!s_tui_pix || s_tui_cap < need)
	{
		if (s_tui_pix)
			free(s_tui_pix);
		s_tui_pix = static_cast<u8 *>(malloc(need));
		s_tui_cap = s_tui_pix ? need : 0;
	}
	s_tui_w = w;
	s_tui_h = h;
	s_tui_pitch = pitch;
	if (s_tui_pix)
		memset(s_tui_pix, 0, need);
}

static void plat_tui_glyph(int col, int row, unsigned ch, unsigned fg_rgb, unsigned bg_rgb)
{
	unsigned x0, y0, x, y;
	TScreenColor fg, bg;
	if (!s_tui_pix || col < 0 || row < 0)
		return;
	x0 = (unsigned)col * TUI_CW;
	y0 = (unsigned)row * TUI_CH;
	if (x0 + TUI_CW > s_tui_w || y0 + TUI_CH > s_tui_h)
		return;
	fg = (TScreenColor)rgb_to_raw(fg_rgb);
	bg = (TScreenColor)rgb_to_raw(bg_rgb);
	for (y = 0; y < TUI_CH; y++)
	{
		u8 bits = glyph_row(ch, y);
		u8 *dst = s_tui_pix + (y0 + y) * s_tui_pitch + x0 * (DEPTH / 8);
		for (x = 0; x < TUI_CW; x++)
		{
			TScreenColor c = (bits & (u8)(0x80 >> x)) ? fg : bg;
#if DEPTH == 32
			reinterpret_cast<u32 *>(dst)[x] = (u32)c;
#elif DEPTH == 16
			reinterpret_cast<u16 *>(dst)[x] = (u16)c;
#else
			dst[x] = (u8)c;
#endif
		}
	}
}

static void plat_tui_present(int y0, int y1);

static void plat_fill_rows(int x, int y0, int y1, int w, int bpp, TScreenColor fill)
{
	int row, px;
	u8 *dst;

	for (row = y0; row < y1; row++)
	{
		dst = s_tui_pix + (unsigned)row * s_tui_pitch + (unsigned)x * (unsigned)bpp;
		for (px = 0; px < w; px++)
		{
#if DEPTH == 32
			reinterpret_cast<u32 *>(dst)[px] = (u32)fill;
#elif DEPTH == 16
			reinterpret_cast<u16 *>(dst)[px] = (u16)fill;
#else
			dst[px] = (u8)fill;
#endif
		}
	}
}

static void plat_tui_scroll(int x, int y, int w, int h, int dy, unsigned fill_rgb)
{
	int row, bpp, ady;
	u8 *dst, *src;
	TScreenColor fill;

	if (!s_tui_pix || w < 1 || h < 1 || dy == 0)
		return;
	if (x < 0)
		x = 0;
	if (y < 0)
		y = 0;
	if ((unsigned)x >= s_tui_w || (unsigned)y >= s_tui_h)
		return;
	if ((unsigned)(x + w) > s_tui_w)
		w = (int)s_tui_w - x;
	if ((unsigned)(y + h) > s_tui_h)
		h = (int)s_tui_h - y;
	ady = dy < 0 ? -dy : dy;
	if (w < 1 || h < 1 || ady >= h)
		return;
	bpp = DEPTH / 8;
	if (dy > 0)
	{
		for (row = y; row < y + h - dy; row++)
		{
			dst = s_tui_pix + (unsigned)row * s_tui_pitch + (unsigned)x * (unsigned)bpp;
			src = s_tui_pix + (unsigned)(row + dy) * s_tui_pitch + (unsigned)x * (unsigned)bpp;
			memcpy(dst, src, (unsigned)w * (unsigned)bpp);
		}
		fill = (TScreenColor)rgb_to_raw(fill_rgb);
		plat_fill_rows(x, y + h - dy, y + h, w, bpp, fill);
	}
	else
	{
		for (row = y + h - 1; row >= y + ady; row--)
		{
			dst = s_tui_pix + (unsigned)row * s_tui_pitch + (unsigned)x * (unsigned)bpp;
			src = s_tui_pix + (unsigned)(row - ady) * s_tui_pitch + (unsigned)x * (unsigned)bpp;
			memcpy(dst, src, (unsigned)w * (unsigned)bpp);
		}
		fill = (TScreenColor)rgb_to_raw(fill_rgb);
		plat_fill_rows(x, y, y + ady, w, bpp, fill);
	}
	plat_tui_present(y, y + h - 1);
}

static void plat_tui_glyph_n(int col, int row, unsigned ch, unsigned fg_rgb, unsigned bg_rgb,
			     int scale)
{
	unsigned x0, y0, x, y, sx, sy, n;
	TScreenColor fg, bg, c;
	u8 *dst;

	if (scale < 1)
		scale = 1;
	if (scale > 4)
		scale = 4;
	n = (unsigned)scale;
	if (!s_tui_pix || col < 0 || row < 0)
		return;
	x0 = (unsigned)col * TUI_CW;
	y0 = (unsigned)row * TUI_CH;
	if (x0 + TUI_CW * n > s_tui_w || y0 + TUI_CH * n > s_tui_h)
		return;
	fg = (TScreenColor)rgb_to_raw(fg_rgb);
	bg = (TScreenColor)rgb_to_raw(bg_rgb);
	for (y = 0; y < TUI_CH; y++)
	{
		u8 bits = glyph_row(ch, y);
		for (sy = 0; sy < n; sy++)
		{
			dst = s_tui_pix + (y0 + y * n + sy) * s_tui_pitch + x0 * (DEPTH / 8);
			for (x = 0; x < TUI_CW; x++)
			{
				c = (bits & (u8)(0x80 >> x)) ? fg : bg;
				for (sx = 0; sx < n; sx++)
				{
#if DEPTH == 32
					reinterpret_cast<u32 *>(dst)[x * n + sx] = (u32)c;
#elif DEPTH == 16
					reinterpret_cast<u16 *>(dst)[x * n + sx] = (u16)c;
#else
					dst[x * n + sx] = (u8)c;
#endif
				}
			}
		}
	}
}

static void plat_tui_glyph2x(int col, int row, unsigned ch, unsigned fg_rgb, unsigned bg_rgb)
{
	plat_tui_glyph_n(col, row, ch, fg_rgb, bg_rgb, 2);
}

static int plat_alt_held(void)
{
	return s_kernel ? s_kernel->AltHeld() : 0;
}

static void plat_tui_present(int y0, int y1)
{
	CDisplay::TArea area;
	if (!s_kernel || !s_tui_pix || y0 > y1)
		return;
	if (!s_kernel->Screen().GetFrameBuffer())
		return;
	if (y0 < 0)
		y0 = 0;
	if (y1 >= (int)s_tui_h)
		y1 = (int)s_tui_h - 1;
	if (y1 < y0)
		return;
	area.x1 = 0;
	area.x2 = s_tui_w - 1;
	area.y1 = (unsigned)y0;
	area.y2 = (unsigned)y1;
	s_kernel->Screen().GetFrameBuffer()->SetArea(
		area, s_tui_pix + (unsigned)y0 * s_tui_pitch);
}

void mmb_platform_bind(CKernel *k)
{
	static mmb_platform plat;
	s_kernel = k;
	plat.write_serial = plat_write_serial;
	plat.write_screen = plat_write_screen;
	plat.set_pixel = plat_set_pixel;
	plat.get_pixel = plat_get_pixel;
	plat.fill_screen = plat_fill;
	plat.hdmi_width = plat_w;
	plat.hdmi_height = plat_h;
	plat.resize_hdmi = plat_resize_hdmi;
	plat.alloc = plat_alloc;
	plat.free = plat_free;
	plat.millis = plat_millis;
	plat.read_line = plat_read_line;
	plat.poll_input = plat_poll_input;
	plat.take_break = plat_take_break;
	plat.reboot = plat_reboot;
	plat.audio_set_target = plat_audio_set_target;
	plat.audio_enable = plat_audio_enable;
	plat.audio_write = plat_audio_write;
	plat.audio_free_frames = plat_audio_free_frames;
	plat.audio_queued_frames = plat_audio_queued_frames;
	plat.audio_have_device = plat_audio_have_device;
	plat.audio_kick = plat_audio_kick;
	plat.audio_flush = plat_audio_flush;
	plat.video_cols = plat_video_cols;
	plat.video_rows = plat_video_rows;
	plat.tui_prepare = plat_tui_prepare;
	plat.tui_glyph = plat_tui_glyph;
	plat.tui_present = plat_tui_present;
	plat.tui_scroll = plat_tui_scroll;
	plat.tui_glyph2x = plat_tui_glyph2x;
	plat.tui_glyph_n = plat_tui_glyph_n;
	plat.tui_set_font = plat_tui_set_font;
	plat.alt_held = plat_alt_held;
	audio_init();
	mmb_init(&plat);
}
