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
#include <circle/synchronize.h>
#include <circle/dmachannel.h>
#include <circle/machineinfo.h>
#include <circle/atomic.h>

static CKernel *s_kernel;

/* SetArea may DMA from these bounce buffers on hardware (SCREEN_DMA_BURST_LENGTH).
 * Circle heap pointers are already cache-line aligned; sizes must be too so
 * CleanAndInvalidateDataCacheRange does not touch neighbouring heap metadata.
 * QEMU builds define NO_SCREEN_DMA_BURST_LENGTH via configure --qemu. */
static unsigned dma_buf_size(unsigned n)
{
	return (unsigned)CACHE_ALIGN_SIZE(u8, n);
}

/*
 * Virtual-offset double-buffer (Pi ≤ 4 hardware). Soft pages stay on the heap;
 * HDMI FB is 2× tall. Immediate presents / TUI / SetPixel use SetDrawOffsetY so
 * they hit the scanned-out half. PAGE DISPLAY writes the hidden half, waits
 * for VSync, then SetVirtualOffset. Other full-frame presents stay on the
 * visible half. Disabled under QEMU (NO_SCREEN_DMA_BURST_LENGTH) and on
 * Pi 5 (RASPPI > 4) — see Circle patch.
 */
static unsigned s_fb_front; /* 0 or 1: half currently scanned out */
static int s_fb_flip_ok;    /* virt height >= 2 * height */
static int s_want_page_flip; /* consumed by the next full-frame present */

static void fb_flip_reset(CBcmFrameBuffer *fb)
{
	s_fb_front = 0;
	s_fb_flip_ok = 0;
	s_want_page_flip = 0;
	if (!fb)
		return;
#if RASPPI <= 4 && !defined(NO_SCREEN_DMA_BURST_LENGTH)
	if (fb->GetVirtHeight() >= fb->GetHeight() * 2)
	{
		s_fb_flip_ok = 1;
		fb->SetDrawOffsetY(0);
		fb->SetVirtualOffset(0, 0);
	}
#else
	fb->SetDrawOffsetY(0);
#endif
}

static void fb_draw_visible(CBcmFrameBuffer *fb)
{
	if (fb && s_fb_flip_ok)
		fb->SetDrawOffsetY(s_fb_front * fb->GetHeight());
}

#ifndef NO_SCREEN_DMA_BURST_LENGTH
/* Serialise presents against any leftover async SetArea from older kernels. */
static volatile int s_present_busy;

static void plat_present_wait(void)
{
	while (AtomicGet(&s_present_busy))
		;
}

static void plat_set_area(CBcmFrameBuffer *fb, const CDisplay::TArea &area,
			  const void *pix)
{
	plat_present_wait();
	fb_draw_visible(fb);
	/* Synchronous DMA Wait() — IRQ completion can leave s_present_busy /
	 * m_nDMAInUse stuck, which hangs TERM Alt-X in plat_fill / free_pages. */
	fb->SetArea(area, pix);
}

/* Synchronous SetArea into the back half, then virt-offset flip (tear-free). */
static void plat_set_area_flip(CBcmFrameBuffer *fb, const CDisplay::TArea &area,
			       const void *pix)
{
	unsigned back;
	unsigned height;

	if (!fb || !s_fb_flip_ok)
	{
		plat_set_area(fb, area, pix);
		return;
	}
	plat_present_wait();
	height = fb->GetHeight();
	back = 1u - s_fb_front;
	fb->SetDrawOffsetY(back * height);
	/* Sync copy: must finish before SetVirtualOffset. */
	fb->SetArea(area, pix);
	fb->WaitForVerticalSync();
	fb->SetVirtualOffset(0, back * height);
	s_fb_front = back;
	/* Draw offset already targets the new visible half. */
}
#else
static void plat_present_wait(void)
{
}

static void plat_set_area(CBcmFrameBuffer *fb, const CDisplay::TArea &area,
			  const void *pix)
{
	fb->SetArea(area, pix);
}

static void plat_set_area_flip(CBcmFrameBuffer *fb, const CDisplay::TArea &area,
			       const void *pix)
{
	/* QEMU: no virt-offset flip; sync present into the single buffer. */
	plat_set_area(fb, area, pix);
}
#endif

static void plat_present_set_flip(int on)
{
	s_want_page_flip = on ? 1 : 0;
}

static int present_wants_flip(CScreenDevice *sc, int x, int y, int w, int h)
{
	if (!s_fb_flip_ok || !sc || !s_want_page_flip)
		return 0;
	if (x != 0 || y != 0 ||
	    w != (int)sc->GetWidth() || h != (int)sc->GetHeight())
		return 0;
	s_want_page_flip = 0;
	return 1;
}

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

static CBcmFrameBuffer *plat_fb_visible(void);

static void plat_write_screen(const char *s, unsigned n)
{
	if (!mmb_opt_console_screen())
		return;
	if (!s_kernel || !s || !n)
		return;
	(void)plat_fb_visible();
	s_kernel->Screen().Write(s, n);
}

static CBcmFrameBuffer *plat_fb_visible(void)
{
	CBcmFrameBuffer *fb;

	if (!s_kernel)
		return 0;
	fb = s_kernel->Screen().GetFrameBuffer();
	if (!fb)
		return 0;
	plat_present_wait();
	fb_draw_visible(fb);
	return fb;
}

static void plat_set_pixel(int x, int y, unsigned rgb)
{
	CBcmFrameBuffer *fb;
	CScreenDevice *sc;
	CDisplay::TRawColor c;

	if (!s_kernel)
		return;
	sc = &s_kernel->Screen();
	if (x < 0 || y < 0 || (unsigned)x >= sc->GetWidth() ||
	    (unsigned)y >= sc->GetHeight())
		return;
	c = (CDisplay::TRawColor)rgb_to_raw(rgb);
	fb = plat_fb_visible();
	if (s_fb_flip_ok && fb)
	{
		fb->SetPixel((unsigned)x, (unsigned)y, c);
	}
	else
	{
		sc->SetPixel((unsigned)x, (unsigned)y, (TScreenColor)c);
	}
}

static unsigned plat_get_pixel(int x, int y)
{
	CBcmFrameBuffer *fb;
	CScreenDevice *sc;
	CDisplay::TRawColor raw;

	if (!s_kernel)
		return 0;
	sc = &s_kernel->Screen();
	if (x < 0 || y < 0 || (unsigned)x >= sc->GetWidth() ||
	    (unsigned)y >= sc->GetHeight())
		return 0;
	fb = plat_fb_visible();
	if (s_fb_flip_ok && fb)
	{
		u8 *base = (u8 *)(uintptr)fb->GetBuffer();
		unsigned pitch = fb->GetPitch();
		unsigned oy = fb->GetDrawOffsetY();
#if DEPTH == 32
		raw = *(u32 *)(base + ((unsigned)y + oy) * pitch + (unsigned)x * 4);
#elif DEPTH == 16
		raw = *(u16 *)(base + ((unsigned)y + oy) * pitch + (unsigned)x * 2);
#else
		raw = base[((unsigned)y + oy) * pitch + (unsigned)x];
#endif
	}
	else
	{
		raw = sc->GetPixel((unsigned)x, (unsigned)y);
	}
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
	CBcmFrameBuffer *fb;
	CDisplay::TRawColor c;
	unsigned w, h, x, y;

	if (!s_kernel)
		return;
	/* HDMI only: home + erase-to-end, then paint pixels so a coloured
	 * CLS is not undone by the terminal wipe. Never write ANSI to serial
	 * (tests assert send_line("CLS") == ""). */
	CScreenDevice &sc = s_kernel->Screen();
	plat_present_wait();
	/* MODE/TERM exit may skip resize; still drop virt-offset back to half 0
	 * so the console writes the scanned-out plane (#315). */
	fb_flip_reset(sc.GetFrameBuffer());
	static const char home[] = "\x1b[H\x1b[J";
	sc.Write(home, sizeof(home) - 1);

	c = (CDisplay::TRawColor)rgb_to_raw(rgb);
	w = sc.GetWidth();
	h = sc.GetHeight();
	fb = plat_fb_visible();
	if (s_fb_flip_ok && fb)
	{
		for (y = 0; y < h; y++)
			for (x = 0; x < w; x++)
				fb->SetPixel(x, y, c);
		return;
	}
	for (y = 0; y < h; y++)
		for (x = 0; x < w; x++)
			sc.SetPixel(x, y, (TScreenColor)c);
}

static int plat_w(void)
{
	return s_kernel ? (int)s_kernel->Screen().GetWidth() : 640;
}

static int plat_h(void)
{
	return s_kernel ? (int)s_kernel->Screen().GetHeight() : 480;
}

static void plat_term_present_drain(void);

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

	plat_term_present_drain();

	/* Resize() leaves the device unusable on failure; restore the
	 * previous timing immediately so later writes cannot crash. */
	if (sc.Resize((unsigned)w, (unsigned)h))
	{
		sc.SetCursorBlock(TRUE);
		fb_flip_reset(sc.GetFrameBuffer());
		return 1;
	}
	sc.Resize(prev_w, prev_h);
	sc.SetCursorBlock(TRUE);
	fb_flip_reset(sc.GetFrameBuffer());
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

static int plat_read_raw(unsigned char *buf, unsigned n)
{
	if (!s_kernel)
		return -1;
	return s_kernel->ReadRaw(buf, n);
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
static u8 *s_present_pix;
static unsigned s_present_cap;

extern "C" const u8 mmb_cp437_8x16[256 * 16];
extern "C" const u8 mmb_tnr_16x32[95 * 32 * 2];
extern "C" const u8 mmb_tnr_24x48[95 * 48 * 3];
extern "C" const u8 mmb_tnr_32x64[95 * 64 * 4];

static const u8 *s_tui_font = mmb_cp437_8x16;

static u8 glyph_row(const u8 *font, unsigned ch, unsigned y)
{
	if (y >= TUI_CH)
		return 0;
	ch &= 0xFFu;
	return font[ch * TUI_CH + y];
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
	unsigned need = dma_buf_size(pitch * h);
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
		u8 bits = glyph_row(s_tui_font, ch, y);
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

static void plat_plot_tui(u8 *dst, unsigned x, TScreenColor c)
{
#if DEPTH == 32
	reinterpret_cast<u32 *>(dst)[x] = (u32)c;
#elif DEPTH == 16
	reinterpret_cast<u16 *>(dst)[x] = (u16)c;
#else
	dst[x] = (u8)c;
#endif
}

static const u8 *tnr_native(int scale, unsigned ch, unsigned *gw, unsigned *gh, unsigned *rowb)
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

static void plat_tui_glyph_n_px(int x_px, int y_px, unsigned ch, unsigned fg_rgb,
				unsigned bg_rgb, int scale, int ink_only)
{
	unsigned y0, x, y, sx, sy, n, gw, gh, rowb;
	const u8 *glyph;
	TScreenColor fg, bg, c;
	u8 *dst;
	u8 bits;
	int on, x0;

	if (scale < 1)
		scale = 1;
	if (scale > 4)
		scale = 4;
	n = (unsigned)scale;
	if (!s_tui_pix || y_px < 0)
		return;
	x0 = x_px;
	y0 = (unsigned)y_px;
	if (y0 >= s_tui_h)
		return;
	fg = (TScreenColor)rgb_to_raw(fg_rgb);
	bg = (TScreenColor)rgb_to_raw(bg_rgb);
	glyph = tnr_native(scale, ch, &gw, &gh, &rowb);
	if (glyph)
	{
		for (y = 0; y < gh; y++)
		{
			if (y0 + y >= s_tui_h)
				break;
			for (x = 0; x < gw; x++)
			{
				int sx = x0 + (int)x;
				if (sx < 0)
					continue;
				if ((unsigned)sx >= s_tui_w)
					break;
				dst = s_tui_pix + (y0 + y) * s_tui_pitch +
				      (unsigned)sx * (DEPTH / 8);
				bits = glyph[y * rowb + x / 8];
				on = (bits & (u8)(0x80 >> (x % 8))) != 0;
				if (ink_only && !on)
					continue;
				c = on ? fg : bg;
				plat_plot_tui(dst, 0, c);
			}
		}
		return;
	}
	for (y = 0; y < TUI_CH; y++)
	{
		bits = glyph_row(s_tui_font, ch, y);
		for (sy = 0; sy < n; sy++)
		{
			if (y0 + y * n + sy >= s_tui_h)
				return;
			dst = s_tui_pix + (y0 + y * n + sy) * s_tui_pitch + x0 * (DEPTH / 8);
			for (x = 0; x < TUI_CW; x++)
			{
				on = (bits & (u8)(0x80 >> x)) != 0;
				if (ink_only && !on)
					continue;
				c = on ? fg : bg;
				for (sx = 0; sx < n; sx++)
				{
					if (x0 + x * n + sx >= s_tui_w)
						break;
					plat_plot_tui(dst, x * n + sx, c);
				}
			}
		}
	}
}

static void plat_tui_glyph_n(int col, int row, unsigned ch, unsigned fg_rgb, unsigned bg_rgb,
			     int scale)
{
	if (scale < 1)
		scale = 1;
	if (scale > 4)
		scale = 4;
	if (!s_tui_pix || col < 0 || row < 0)
		return;
	if ((unsigned)col * TUI_CW + TUI_CW * (unsigned)scale > s_tui_w ||
	    (unsigned)row * TUI_CH + TUI_CH * (unsigned)scale > s_tui_h)
		return;
	plat_tui_glyph_n_px(col * TUI_CW, row * TUI_CH, ch, fg_rgb, bg_rgb, scale, 0);
}

static void plat_tui_fill_px(int x, int y, int w, int h, unsigned rgb)
{
	int bpp;
	TScreenColor fill;

	if (!s_tui_pix || w < 1 || h < 1)
		return;
	if (x < 0)
	{
		w += x;
		x = 0;
	}
	if (y < 0)
	{
		h += y;
		y = 0;
	}
	if ((unsigned)x >= s_tui_w || (unsigned)y >= s_tui_h)
		return;
	if ((unsigned)(x + w) > s_tui_w)
		w = (int)s_tui_w - x;
	if ((unsigned)(y + h) > s_tui_h)
		h = (int)s_tui_h - y;
	if (w < 1 || h < 1)
		return;
	bpp = DEPTH / 8;
	fill = (TScreenColor)rgb_to_raw(rgb);
	plat_fill_rows(x, y, y + h, w, bpp, fill);
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
	CBcmFrameBuffer *fb;
	if (!s_kernel || !s_tui_pix || y0 > y1)
		return;
	fb = s_kernel->Screen().GetFrameBuffer();
	if (!fb)
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
	plat_present_wait();
	fb_draw_visible(fb);
	fb->SetArea(area, s_tui_pix + (unsigned)y0 * s_tui_pitch);
}

static unsigned plat_rgb_to_native(unsigned rgb888)
{
	return rgb_to_raw(rgb888);
}

static unsigned plat_native_to_rgb(unsigned native)
{
#if DEPTH == 32
	{
		unsigned b = native & 0xFF;
		unsigned g = (native >> 8) & 0xFF;
		unsigned r = (native >> 16) & 0xFF;
		return (r << 16) | (g << 8) | b;
	}
#elif DEPTH == 16
	{
		unsigned r = (native >> 11) & 0x1F;
		unsigned g = (native >> 6) & 0x1F;
		unsigned b = native & 0x1F;
		r = r * 255 / 31;
		g = g * 255 / 31;
		b = b * 255 / 31;
		return (r << 16) | (g << 8) | b;
	}
#else
	{
		unsigned v = native;
		unsigned r = v & 0xE0;
		unsigned g = (v << 3) & 0xE0;
		unsigned b = (v << 6) & 0xC0;
		return (r << 16) | (g << 8) | b;
	}
#endif
}

static int present_clip(CScreenDevice *sc, int *x, int *y, int *w, int *h,
			int stride, const void **pix, unsigned bpp)
{
	const u8 *p = static_cast<const u8 *>(*pix);

	if (*x < 0)
	{
		*w += *x;
		p += (unsigned)(-*x) * bpp;
		*x = 0;
	}
	if (*y < 0)
	{
		*h += *y;
		p += (unsigned)(-*y) * (unsigned)stride * bpp;
		*y = 0;
	}
	if (*x + *w > (int)sc->GetWidth())
		*w = (int)sc->GetWidth() - *x;
	if (*y + *h > (int)sc->GetHeight())
		*h = (int)sc->GetHeight() - *y;
	if (*w < 1 || *h < 1)
		return 0;
	*pix = p;
	return 1;
}

static u8 *present_bounce(unsigned need)
{
	if (!s_present_pix || s_present_cap < need)
	{
		if (s_present_pix)
			free(s_present_pix);
		s_present_pix = static_cast<u8 *>(malloc(need));
		s_present_cap = s_present_pix ? need : 0;
	}
	return s_present_pix;
}

static void plat_present_rgb(int x, int y, int w, int h,
			    const unsigned *rgb888, int stride)
{
	CDisplay::TArea area;
	CBcmFrameBuffer *fb;
	CScreenDevice *sc;
	int px, py;
	unsigned need, bpp = (unsigned)(DEPTH / 8);
	TScreenColor *dst;
	const void *pix = rgb888;

	if (!s_kernel || !rgb888 || w < 1 || h < 1 || stride < w)
		return;
	sc = &s_kernel->Screen();
	fb = sc->GetFrameBuffer();
	if (!fb)
		return;
	if (!present_clip(sc, &x, &y, &w, &h, stride, &pix, sizeof(unsigned)))
		return;
	rgb888 = static_cast<const unsigned *>(pix);
	need = dma_buf_size((unsigned)w * (unsigned)h * bpp);
	if (!present_bounce(need))
		return;
	plat_present_wait();
	dst = reinterpret_cast<TScreenColor *>(s_present_pix);
	for (py = 0; py < h; py++)
	{
		const unsigned *src = rgb888 + py * stride;
		TScreenColor *out = dst + py * w;
		for (px = 0; px < w; px++)
			out[px] = (TScreenColor)rgb_to_raw(src[px]);
	}
	area.x1 = (unsigned)x;
	area.x2 = (unsigned)(x + w - 1);
	area.y1 = (unsigned)y;
	area.y2 = (unsigned)(y + h - 1);
	if (present_wants_flip(sc, x, y, w, h))
		plat_set_area_flip(fb, area, s_present_pix);
	else
		plat_set_area(fb, area, s_present_pix);
}

#define TERM_PRESENT_EMPTY 0xffffffffu

static u8 *s_term_bounce[2];
static unsigned s_term_bounce_cap;
#ifndef NO_SCREEN_DMA_BURST_LENGTH
static volatile int s_term_in_flight;
/* Set when the in-flight DMA reads the caller's page rather than a bounce. */
static volatile int s_term_direct;
static volatile unsigned s_term_up_lo = TERM_PRESENT_EMPTY;
static volatile unsigned s_term_up_hi = TERM_PRESENT_EMPTY;
static int s_term_pending;
static int s_term_px, s_term_py, s_term_pw, s_term_ph;
static int s_term_flight_idx;
static int s_term_standby_idx;
#endif

static unsigned term_bounce_need(int w, int h)
{
	return dma_buf_size((unsigned)w * (unsigned)h * (unsigned)(DEPTH / 8));
}

static int term_bounce_ensure(unsigned need)
{
	int i;

	if (s_term_bounce_cap >= need)
		return 1;
	for (i = 0; i < 2; i++)
	{
		u8 *p = static_cast<u8 *>(malloc(need));
		if (!p)
			return 0;
		if (s_term_bounce[i])
			free(s_term_bounce[i]);
		s_term_bounce[i] = p;
	}
	s_term_bounce_cap = need;
	return 1;
}

static void term_copy_to_bounce(void *dst, const void *src, int w, int h,
				int stride, unsigned bpp)
{
	int py;
	unsigned row = (unsigned)w * bpp;

	for (py = 0; py < h; py++)
		memcpy(static_cast<u8 *>(dst) + py * row,
		       static_cast<const u8 *>(src) + (unsigned)py * (unsigned)stride * bpp,
		       row);
}

#ifndef NO_SCREEN_DMA_BURST_LENGTH
static void term_present_kick(CBcmFrameBuffer *fb, int x, int y, int w, int h,
			      const void *pix);

static void term_present_done(void *param)
{
	int pending, x, y, w, h, idx;
	void *pix;
	CBcmFrameBuffer *fb;

	(void)param;
	AtomicSet(&s_term_in_flight, 0);
	pending = 0;
	DisableInterrupts();
	if (s_term_pending)
	{
		pending = 1;
		s_term_pending = 0;
		x = s_term_px;
		y = s_term_py;
		w = s_term_pw;
		h = s_term_ph;
		idx = s_term_standby_idx;
	}
	EnableInterrupts();
	if (!pending || !s_kernel)
		return;
	pix = s_term_bounce[idx];
	if (!pix)
		return;
	fb = s_kernel->Screen().GetFrameBuffer();
	if (!fb)
		return;
	s_term_flight_idx = idx;
	s_term_direct = 0;
	term_present_kick(fb, x, y, w, h, pix);
}

static void term_present_kick(CBcmFrameBuffer *fb, int x, int y, int w, int h,
			      const void *pix)
{
	CDisplay::TArea area;

	area.x1 = (unsigned)x;
	area.x2 = (unsigned)(x + w - 1);
	area.y1 = (unsigned)y;
	area.y2 = (unsigned)(y + h - 1);
	fb_draw_visible(fb);
	AtomicSet(&s_term_in_flight, 1);
	fb->SetArea(area, pix, term_present_done, nullptr);
}
static void term_merge_pending(int x, int y, int w, int h)
{
	int x2, nx2;

	if (!s_term_pending)
	{
		s_term_px = x;
		s_term_py = y;
		s_term_pw = w;
		s_term_ph = h;
		s_term_up_lo = (unsigned)y;
		s_term_up_hi = (unsigned)(y + h - 1);
		return;
	}
	x2 = s_term_px + s_term_pw;
	nx2 = x + w;
	if (x < s_term_px)
		s_term_px = x;
	if (nx2 > x2)
		x2 = nx2;
	s_term_pw = x2 - s_term_px;
	if ((unsigned)y < s_term_up_lo)
		s_term_up_lo = (unsigned)y;
	if ((unsigned)(y + h - 1) > s_term_up_hi)
		s_term_up_hi = (unsigned)(y + h - 1);
	s_term_py = (int)s_term_up_lo;
	s_term_ph = (int)(s_term_up_hi - s_term_up_lo + 1);
}
#endif

static void plat_term_present_async(int x, int y, int w, int h,
				    const void *pix, int stride)
{
	CBcmFrameBuffer *fb;
	CScreenDevice *sc;
	const TScreenColor *src;
	unsigned need, bpp;
	void *bounce;

	if (!s_kernel || !pix || w < 1 || h < 1 || stride < w)
		return;
	sc = &s_kernel->Screen();
	fb = sc->GetFrameBuffer();
	if (!fb)
		return;
	bpp = (unsigned)(DEPTH / 8);
	if (!present_clip(sc, &x, &y, &w, &h, stride, &pix, bpp))
		return;
	src = static_cast<const TScreenColor *>(pix);
#ifdef NO_SCREEN_DMA_BURST_LENGTH
	need = term_bounce_need(w, h);
	if (stride == w)
		bounce = const_cast<void *>(pix);
	else
	{
		if (!term_bounce_ensure(need))
			return;
		bounce = s_term_bounce[0];
		term_copy_to_bounce(bounce, src, w, h, stride, bpp);
	}
	fb_draw_visible(fb);
	{
		CDisplay::TArea area;
		area.x1 = (unsigned)x;
		area.x2 = (unsigned)(x + w - 1);
		area.y1 = (unsigned)y;
		area.y2 = (unsigned)(y + h - 1);
		fb->SetArea(area, bounce);
	}
	return;
#else
	{
	const TScreenColor *page = src - y * stride - x;
	int standby, merged_w, merged_h;

	merged_w = w;
	merged_h = h;
	if (AtomicGet(&s_term_in_flight))
	{
		DisableInterrupts();
		term_merge_pending(x, y, w, h);
		merged_w = s_term_pw;
		merged_h = s_term_ph;
		x = s_term_px;
		y = s_term_py;
		standby = 1 - s_term_flight_idx;
		s_term_standby_idx = standby;
		EnableInterrupts();
		need = term_bounce_need(merged_w, merged_h);
		if (!term_bounce_ensure(need))
		{
			DisableInterrupts();
			s_term_pending = 0;
			s_term_up_lo = TERM_PRESENT_EMPTY;
			s_term_up_hi = TERM_PRESENT_EMPTY;
			EnableInterrupts();
			return;
		}
		bounce = s_term_bounce[standby];
		term_copy_to_bounce(bounce, page + y * stride + x, merged_w, merged_h,
				    stride, bpp);
		DisableInterrupts();
		s_term_pending = 1;
		/* DMA may have finished during the copy; kick ourselves. */
		if (!AtomicGet(&s_term_in_flight))
		{
			s_term_pending = 0;
			s_term_up_lo = TERM_PRESENT_EMPTY;
			s_term_up_hi = TERM_PRESENT_EMPTY;
			s_term_flight_idx = standby;
			s_term_direct = 0;
			EnableInterrupts();
			term_present_kick(fb, x, y, merged_w, merged_h, bounce);
			return;
		}
		EnableInterrupts();
		return;
	}
	/* No DMA in flight: a contiguous source can be DMAed straight from the
	 * caller's page, so full-width TERM frames skip the bounce copy (#330). */
	if (stride == w)
	{
		s_term_flight_idx = 0;
		s_term_direct = 1;
		s_term_up_lo = TERM_PRESENT_EMPTY;
		s_term_up_hi = TERM_PRESENT_EMPTY;
		s_term_pending = 0;
		term_present_kick(fb, x, y, w, h, src);
		return;
	}
	need = term_bounce_need(w, h);
	if (!term_bounce_ensure(need))
		return;
	s_term_flight_idx = 0;
	s_term_direct = 0;
	bounce = s_term_bounce[0];
	term_copy_to_bounce(bounce, src, w, h, stride, bpp);
	s_term_up_lo = TERM_PRESENT_EMPTY;
	s_term_up_hi = TERM_PRESENT_EMPTY;
	s_term_pending = 0;
	term_present_kick(fb, x, y, w, h, bounce);
	}
#endif
}

static void plat_term_present_drain(void)
{
#ifdef NO_SCREEN_DMA_BURST_LENGTH
	return;
#else
	int pending, x, y, w, h, idx;
	void *pix;
	CBcmFrameBuffer *fb;

	for (;;)
	{
		while (AtomicGet(&s_term_in_flight))
			;
		pending = 0;
		DisableInterrupts();
		if (s_term_pending)
		{
			pending = 1;
			s_term_pending = 0;
			x = s_term_px;
			y = s_term_py;
			w = s_term_pw;
			h = s_term_ph;
			idx = s_term_standby_idx;
			s_term_up_lo = TERM_PRESENT_EMPTY;
			s_term_up_hi = TERM_PRESENT_EMPTY;
		}
		EnableInterrupts();
		if (!pending)
			return;
		if (!s_kernel)
			return;
		pix = s_term_bounce[idx];
		if (!pix)
			return;
		fb = s_kernel->Screen().GetFrameBuffer();
		if (!fb)
			return;
		s_term_flight_idx = idx;
		s_term_direct = 0;
		term_present_kick(fb, x, y, w, h, pix);
	}
#endif
}

static int plat_term_present_locked(void)
{
#ifdef NO_SCREEN_DMA_BURST_LENGTH
	return 0;
#else
	return AtomicGet(&s_term_in_flight) && s_term_direct;
#endif
}

static void plat_present_native(int x, int y, int w, int h,
				const void *pix, int stride)
{
	CDisplay::TArea area;
	CBcmFrameBuffer *fb;
	CScreenDevice *sc;
	int py;
	unsigned need, bpp = (unsigned)(DEPTH / 8);
	const TScreenColor *src;
	void *buf;

	if (!s_kernel || !pix || w < 1 || h < 1 || stride < w)
		return;
	sc = &s_kernel->Screen();
	fb = sc->GetFrameBuffer();
	if (!fb)
		return;
	if (!present_clip(sc, &x, &y, &w, &h, stride, &pix, bpp))
		return;
	src = static_cast<const TScreenColor *>(pix);
	area.x1 = (unsigned)x;
	area.x2 = (unsigned)(x + w - 1);
	area.y1 = (unsigned)y;
	area.y2 = (unsigned)(y + h - 1);
	/* Tight contiguous rows: SetArea can DMA straight from the page. */
	if (stride == w)
	{
		if (present_wants_flip(sc, x, y, w, h))
			plat_set_area_flip(fb, area, src);
		else
			plat_set_area(fb, area, src);
		return;
	}
	need = dma_buf_size((unsigned)w * (unsigned)h * bpp);
	buf = present_bounce(need);
	if (!buf)
		return;
	plat_present_wait();
	for (py = 0; py < h; py++)
		memcpy(static_cast<TScreenColor *>(buf) + py * w,
		       src + py * stride,
		       (unsigned)w * bpp);
	if (present_wants_flip(sc, x, y, w, h))
		plat_set_area_flip(fb, area, buf);
	else
		plat_set_area(fb, area, buf);
}

static int plat_wait_vsync(void)
{
#ifdef NO_SDHOST
	return 0;
#else
	CBcmFrameBuffer *fb;

	if (!s_kernel)
		return 0;
	fb = s_kernel->Screen().GetFrameBuffer();
	if (!fb)
		return 0;
	return fb->WaitForVerticalSync() ? 1 : 0;
#endif
}

/* Memory DMA for opaque PAGE COPY / BLIT. Disabled under QEMU
 * (NO_SCREEN_DMA_BURST_LENGTH) — prefer memcpy when screen DMA is off. */
static CDMAChannel *s_mem_dma;

static CDMAChannel *mem_dma_channel(void)
{
	unsigned ch;

	if (s_mem_dma)
		return s_mem_dma;
#if RASPPI >= 4
	ch = DMA_CHANNEL_EXTENDED;
#else
	ch = DMA_CHANNEL_NORMAL;
#endif
	s_mem_dma = new CDMAChannel(ch);
	return s_mem_dma;
}

static int plat_dma_copy(void *dst, const void *src, unsigned nbytes)
{
#ifdef NO_SCREEN_DMA_BURST_LENGTH
	(void)dst;
	(void)src;
	(void)nbytes;
	return 0;
#else
	CDMAChannel *dma;

	if (!dst || !src || nbytes == 0)
		return 0;
	dma = mem_dma_channel();
	if (!dma)
		return 0;
	dma->SetupMemCopy(dst, src, nbytes, 0, TRUE);
	dma->Start();
	return dma->Wait() ? 1 : 0;
#endif
}

static int plat_dma_copy2d(void *dst, const void *src, unsigned block_len,
			   unsigned block_count, unsigned block_stride)
{
#ifdef NO_SCREEN_DMA_BURST_LENGTH
	(void)dst;
	(void)src;
	(void)block_len;
	(void)block_count;
	(void)block_stride;
	return 0;
#else
	CDMAChannel *dma;
	unsigned dest_span;

	if (!dst || !src || block_len == 0 || block_count == 0)
		return 0;
	dma = mem_dma_channel();
	if (!dma)
		return 0;
	/* SetupMemCopy2D does not maintain the destination cache. */
	dest_span = block_count * block_len;
	if (block_count > 1)
		dest_span += (block_count - 1) * block_stride;
	CleanAndInvalidateDataCacheRange((uintptr)dst, dest_span);
	dma->SetupMemCopy2D(dst, src, block_len, block_count, block_stride, 0);
	dma->Start();
	if (!dma->Wait())
		return 0;
	CleanAndInvalidateDataCacheRange((uintptr)dst, dest_span);
	return 1;
#endif
}

void mmb_platform_bind(CKernel *k)
{
	static mmb_platform plat;
	s_kernel = k;
	if (k && k->Screen().GetFrameBuffer())
		fb_flip_reset(k->Screen().GetFrameBuffer());
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
	plat.read_raw = plat_read_raw;
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
	plat.tui_glyph_n_px = plat_tui_glyph_n_px;
	plat.tui_fill_px = plat_tui_fill_px;
	plat.tui_set_font = plat_tui_set_font;
	plat.alt_held = plat_alt_held;
	plat.present_rgb = plat_present_rgb;
	plat.present_native = plat_present_native;
	plat.present_wait = plat_present_wait;
	plat.term_present_async = plat_term_present_async;
	plat.term_present_drain = plat_term_present_drain;
	plat.term_present_locked = plat_term_present_locked;
	plat.present_set_flip = plat_present_set_flip;
	plat.rgb_to_native = plat_rgb_to_native;
	plat.native_to_rgb = plat_native_to_rgb;
	plat.wait_vsync = plat_wait_vsync;
	plat.dma_copy = plat_dma_copy;
	plat.dma_copy2d = plat_dma_copy2d;
	audio_init();
	mmb_init(&plat);
}
