/*
 * SDL2 platform backend (LN-03): window, software framebuffer, the basic
 * mmb_platform hooks. Text/TUI/graphics rendering land in LN-05..LN-07.
 *
 * Native pixels are RGB555 with green at bit 6 (Circle COLOR16), so the
 * interpreter's colour model and page storage match the Pi exactly; only the
 * final SDL present converts to RGB565.
 */
#include "win_compat.h"

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
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

#include <SDL.h>

static void sdl_serial(const char *s, unsigned n)
{
	/* The SDL window is the display. Mirror the serial stream to stdout for
	 * headless/automation runs (pipes, MMB_SDL_DUMP) but keep an interactive
	 * terminal quiet, otherwise every REPL line is printed twice. */
	static int checked, mirror;

	if (!checked)
	{
		checked = 1;
		mirror = !isatty(STDOUT_FILENO) || getenv("MMB_SDL_SERIAL") != 0;
	}
	if (!mirror)
		return;
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

static int sdl_host_width(void)
{
	int w = 0, h = 0;

	sdl_video_host_size(&w, &h);
	return w > 0 ? w : sdl_width();
}

static int sdl_host_height(void)
{
	int w = 0, h = 0;

	sdl_video_host_size(&w, &h);
	return h > 0 ? h : sdl_height();
}

static const char *sdl_runtime(void)
{
#ifdef __APPLE__
	return "mac";
#elif defined(_WIN32)
	return "windows";
#else
	return "linux";
#endif
}

static void sdl_set_pixel(int x, int y, unsigned rgb)
{
	uint16_t *fb = sdl_video_fb();

	if (!fb || x < 0 || y < 0 || x >= sdl_video_width() ||
	    y >= sdl_video_height())
		return;
	fb[y * sdl_video_width() + x] = (uint16_t)sdl_rgb_to_native(rgb);
	sdl_video_mark_dirty();
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
	sdl_video_mark_dirty();
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
	sdl_video_mark_dirty();
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

/* Poll piped stdin (headless/automation). Returns 1 with a byte in *c, 0 when
 * nothing is ready, -1 once stdin is closed. */
static int sdl_stdin_byte(int *c)
{
#ifdef _WIN32
	if (!mmb_stdin_ready())
		return 0;
#else
	fd_set rfds;
	struct timeval tv;

	FD_ZERO(&rfds);
	FD_SET(STDIN_FILENO, &rfds);
	tv.tv_sec = 0;
	tv.tv_usec = 0;
	if (select(STDIN_FILENO + 1, &rfds, 0, 0, &tv) <= 0)
		return 0;
#endif
	*c = fgetc(stdin);
	if (*c == EOF)
		return -1;
	return 1;
}

/* One byte for the blocking line prompt. The window is the primary source;
 * piped stdin keeps the headless tests working. */
static int sdl_line_byte(void)
{
	int c;

	if (sdl_video_should_quit())
		return -2;
	sdl_input_pump();
	if (sdl_video_should_quit())
		return -2;
	c = mmb_inkey_pop();
	if (c >= 0)
		return c;
	switch (sdl_stdin_byte(&c))
	{
	case 1:
		return c;
	case -1:
		return -3; /* stdin closed: finish the line */
	default:
		return -1;
	}
}

static int sdl_read_line(char **out, int hide)
{
	unsigned cap = 128, n = 0;
	char *buf;

	if (!out)
		return -1;
	*out = 0;
	buf = malloc(cap);
	if (!buf)
		return -1;
	buf[0] = 0;
	sdl_input_begin_line();
	for (;;)
	{
		int c = sdl_line_byte();

		if (c == -2)
		{
			free(buf);
			sdl_input_end_line();
			return -2;
		}
		if (c == -3)
		{
			buf[n] = 0;
			*out = buf;
			sdl_input_end_line();
			return 0;
		}
		if (c < 0)
		{
			mmb_poll();
			SDL_Delay(5);
			sdl_video_present();
			continue;
		}
		if (c == '\r' || c == '\n')
		{
			buf[n] = 0;
			mmb_console_write("\r\n");
			*out = buf;
			sdl_input_end_line();
			return 0;
		}
		if (c == 8 || c == 127)
		{
			if (n > 0)
			{
				n--;
				mmb_console_write("\b \b");
			}
			continue;
		}
		if (c == mmb_break_key())
		{
			free(buf);
			sdl_input_end_line();
			return -2;
		}
		if (c < 32 || c >= 0x80)
			continue; /* ignore control and mapped navigation keys */
		if (n + 2 >= cap)
		{
			unsigned ncap = cap * 2;
			char *nb = realloc(buf, ncap);

			if (!nb)
			{
				free(buf);
				sdl_input_end_line();
				return -1;
			}
			buf = nb;
			cap = ncap;
		}
		{
			char echo[2];

			buf[n++] = (char)c;
			echo[0] = hide ? '*' : (char)c;
			echo[1] = 0;
			mmb_console_write(echo);
		}
	}
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
	.host_width = sdl_host_width,
	.host_height = sdl_host_height,
	.runtime = sdl_runtime,
	.alloc = sdl_alloc,
	.free = sdl_free,
	.millis = sdl_millis,
	.read_line = sdl_read_line,
	.poll_input = sdl_poll_input,
	.reboot = sdl_reboot,
	.can_quit = 1,
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
	.console_save = sdl_console_save,
	.console_restore = sdl_console_restore,
};

void mmb_platform_bind_sdl(void)
{
	sdl_console_reset();
	mmb_init(&sdl_plat);
}
