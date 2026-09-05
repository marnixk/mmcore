#include "kernel.h"
#include "audio.h"
#include "mmbasic.h"
#include <circle/alloc.h>
#include <circle/util.h>
#include <circle/timer.h>
#include <circle/screen.h>
#include <circle/bcmframebuffer.h>
#include <circle/display.h>
#include <circle/font.h>
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
	if (s_kernel)
		s_kernel->Serial().Write(s, n);
}

static void plat_write_screen(const char *s, unsigned n)
{
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
	(void)x;
	(void)y;
	return 0;
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
#define BOX_V  0xB3
#define BOX_H  0xC4
#define BOX_TL 0xDA
#define BOX_TR 0xBF
#define BOX_BL 0xC0
#define BOX_BR 0xD9
#define BOX_LT 0xC3
#define BOX_RT 0xB4
#define BOX_TT 0xC2
#define BOX_BT 0xC1
#define BOX_X  0xC5

static u8 *s_tui_pix;
static unsigned s_tui_cap;
static unsigned s_tui_w, s_tui_h, s_tui_pitch;

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

static u8 box_row(unsigned ch, unsigned y)
{
	const u8 cx = 0x18, L = 0xF8, R = 0x1F, H = 0xFF;
	int mid = (y == 7 || y == 8);
	int up = (y <= 8);
	int down = (y >= 7);
	switch (ch)
	{
	case BOX_V:  return cx;
	case BOX_H:  return mid ? H : 0;
	case BOX_TL: return mid ? R : (down ? cx : 0);
	case BOX_TR: return mid ? L : (down ? cx : 0);
	case BOX_BL: return mid ? R : (up ? cx : 0);
	case BOX_BR: return mid ? L : (up ? cx : 0);
	case BOX_LT: return mid ? R : cx;
	case BOX_RT: return mid ? L : cx;
	case BOX_TT: return mid ? H : (down ? cx : 0);
	case BOX_BT: return mid ? H : (up ? cx : 0);
	case BOX_X:  return mid ? H : cx;
	default:     return 0;
	}
}

static int is_box(unsigned ch)
{
	return ch == BOX_V || ch == BOX_H || ch == BOX_TL || ch == BOX_TR ||
	       ch == BOX_BL || ch == BOX_BR || ch == BOX_LT || ch == BOX_RT ||
	       ch == BOX_TT || ch == BOX_BT || ch == BOX_X;
}

static u8 glyph_row(unsigned ch, unsigned y)
{
	const u8 *data;
	if (y >= TUI_CH)
		return 0;
	if (is_box(ch))
		return box_row(ch, y);
	if (ch < Font8x16.first_char || ch > Font8x16.last_char)
		return 0;
	data = static_cast<const u8 *>(Font8x16.data);
	return data[(ch - Font8x16.first_char) * Font8x16.height + y];
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
	audio_init();
	mmb_init(&plat);
}
