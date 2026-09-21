/*
 * SDL2 platform backend (LN-03): window, software framebuffer, the basic
 * mmb_platform hooks. Text/TUI/graphics rendering land in LN-05..LN-07.
 *
 * Native pixels are RGB555 with green at bit 6 (Circle COLOR16), so the
 * interpreter's colour model and page storage match the Pi exactly; only the
 * final SDL present converts to RGB565.
 */
#include "mmbasic.h"
#include "mmb_priv.h"
#include "sdl_video.h"
#include "sdl_console.h"
#include "sdl_tui.h"
#include "sdl_input.h"
#include "sdl_audio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void sdl_serial(const char *s, unsigned n)
{
	fwrite(s, 1, n, stdout);
	fflush(stdout);
}

static void *sdl_alloc(unsigned n)
{
	return malloc(n ? n : 1u);
}

static void sdl_free(void *p)
{
	free(p);
}

static unsigned sdl_millis(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (unsigned)((unsigned long long)ts.tv_sec * 1000ull +
			  (unsigned long long)ts.tv_nsec / 1000000ull);
}

static int sdl_width(void)
{
	return sdl_video_width() > 0 ? sdl_video_width() : 640;
}

static int sdl_height(void)
{
	return sdl_video_height() > 0 ? sdl_video_height() : 480;
}

static void sdl_set_pixel(int x, int y, unsigned rgb)
{
	uint16_t *fb = sdl_video_fb();

	if (!fb || x < 0 || y < 0 || x >= sdl_video_width() ||
	    y >= sdl_video_height())
		return;
	fb[y * sdl_video_width() + x] = (uint16_t)sdl_rgb_to_native(rgb);
}

static unsigned sdl_get_pixel(int x, int y)
{
	uint16_t *fb = sdl_video_fb();

	if (!fb || x < 0 || y < 0 || x >= sdl_video_width() ||
	    y >= sdl_video_height())
		return 0;
	return sdl_native_to_rgb(fb[y * sdl_video_width() + x]);
}

static void sdl_screen(const char *s, unsigned n)
{
	sdl_console_write(s, n);
}

static void sdl_fill(unsigned rgb)
{
	sdl_console_fill(rgb);
}

static int sdl_resize(int w, int h)
{
	if (!sdl_video_resize(w, h))
		return 0;
	sdl_console_resize();
	return 1;
}

static void sdl_present_native(int x, int y, int w, int h, const void *pix,
			       int stride)
{
	const uint16_t *src = pix;
	uint16_t *fb = sdl_video_fb();
	int row, sw = sdl_video_width(), sh = sdl_video_height();

	if (!fb || !src || w <= 0 || h <= 0)
		return;
	for (row = 0; row < h; row++)
	{
		int dy = y + row;
		int col;

		if (dy < 0 || dy >= sh)
			continue;
		for (col = 0; col < w; col++)
		{
			int dx = x + col;

			if (dx < 0 || dx >= sw)
				continue;
			fb[dy * sw + dx] = src[row * stride + col];
		}
	}
}

static void sdl_present_rgb(int x, int y, int w, int h,
			    const unsigned *rgb888, int stride)
{
	uint16_t *fb = sdl_video_fb();
	int row, sw = sdl_video_width(), sh = sdl_video_height();

	if (!fb || !rgb888 || w <= 0 || h <= 0)
		return;
	for (row = 0; row < h; row++)
	{
		int dy = y + row;
		int col;

		if (dy < 0 || dy >= sh)
			continue;
		for (col = 0; col < w; col++)
		{
			int dx = x + col;

			if (dx < 0 || dx >= sw)
				continue;
			fb[dy * sw + dx] =
				(uint16_t)sdl_rgb_to_native(rgb888[row * stride + col]);
		}
	}
}

/* Called from mmb_check_break while a program runs: pump SDL input and
 * repaint so the window stays alive during long RUNs. */
static void sdl_poll_input(void)
{
	if (!mmb_is_running())
		return;
	sdl_input_pump();
	sdl_video_present();
}

static void sdl_present_wait(void)
{
	/* Presents are synchronous copies into the framebuffer. */
}

static void sdl_present_set_flip(int on)
{
	/* The software framebuffer is single-buffered; SDL presents the whole
	 * frame. PAGE DISPLAY works without a hardware flip. */
	(void)on;
}

static int sdl_wait_vsync(void)
{
	/* SDL_RENDERER_PRESENTVSYNC already paces; no separate wait. */
	return 0;
}

static int sdl_read_line(char **out, int hide)
{
	char buf[512];
	size_t n = 0;
	int c;

	(void)hide;
	if (!out)
		return -1;
	while ((c = fgetc(stdin)) != EOF)
	{
		if (c == '\n')
			break;
		if (c == '\r')
			continue;
		if (n + 1 < sizeof buf)
			buf[n++] = (char)c;
	}
	if (c == EOF && n == 0)
		return -1;
	buf[n] = 0;
	*out = malloc(n + 1);
	if (!*out)
		return -1;
	memcpy(*out, buf, n + 1);
	return 0;
}

static void sdl_reboot(void)
{
	/* Native build: treat REBOOT as a clean exit request. */
	exit(0);
}

static const mmb_platform sdl_plat = {
	.write_serial = sdl_serial,
	.write_screen = sdl_screen,
	.set_pixel = sdl_set_pixel,
	.get_pixel = sdl_get_pixel,
	.fill_screen = sdl_fill,
	.hdmi_width = sdl_width,
	.hdmi_height = sdl_height,
	.resize_hdmi = sdl_resize,
	.alloc = sdl_alloc,
	.free = sdl_free,
	.millis = sdl_millis,
	.read_line = sdl_read_line,
	.poll_input = sdl_poll_input,
	.reboot = sdl_reboot,
	.alt_held = sdl_input_alt_held,
	.ctrl_alt_held = sdl_input_ctrl_alt_held,
	.audio_set_target = sdl_audio_set_target,
	.audio_enable = sdl_audio_enable,
	.audio_write = sdl_audio_write,
	.audio_free_frames = sdl_audio_free_frames,
	.audio_queued_frames = sdl_audio_queued_frames,
	.audio_have_device = sdl_audio_have_device,
	.audio_kick = sdl_audio_kick,
	.audio_flush = sdl_audio_flush,
	.present_rgb = sdl_present_rgb,
	.present_native = sdl_present_native,
	.present_wait = sdl_present_wait,
	.present_set_flip = sdl_present_set_flip,
	.wait_vsync = sdl_wait_vsync,
	.rgb_to_native = sdl_rgb_to_native,
	.native_to_rgb = sdl_native_to_rgb,
	.video_cols = sdl_tui_cols,
	.video_rows = sdl_tui_rows,
	.tui_prepare = sdl_tui_prepare,
	.tui_glyph = sdl_tui_glyph,
	.tui_present = sdl_tui_present,
	.tui_scroll = sdl_tui_scroll,
	.tui_glyph2x = sdl_tui_glyph2x,
	.tui_glyph_n = sdl_tui_glyph_n,
	.tui_glyph_n_px = sdl_tui_glyph_n_px,
	.tui_fill_px = sdl_tui_fill_px,
	.tui_set_font = sdl_tui_set_font,
};

void mmb_platform_bind_sdl(void)
{
	sdl_console_reset();
	mmb_init(&sdl_plat);
}
