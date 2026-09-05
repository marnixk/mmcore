#include "kernel.h"
#include "mmbasic.h"
#include <circle/alloc.h>
#include <circle/util.h>
#include <circle/timer.h>
#include <circle/screen.h>
#include <circle/bcmframebuffer.h>

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
	plat.alloc = plat_alloc;
	plat.free = plat_free;
	plat.millis = plat_millis;
	mmb_init(&plat);
}
