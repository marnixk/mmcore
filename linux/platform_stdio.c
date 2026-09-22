/*
 * Headless stdio platform for the native (Linux/macOS) build.
 *
 * LN-01 foundation: enough of the mmb_platform contract to boot the
 * interpreter, run programs, and read/write the A: ramdisk. Screen output is
 * discarded; serial output goes to stdout so program output appears once
 * (mmb_console_write writes serial then screen). The SDL2 backend replaces
 * this in LN-03+.
 */
#include "mmbasic.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static void stdio_serial(const char *s, unsigned n)
{
	fwrite(s, 1, n, stdout);
	fflush(stdout);
}

static void stdio_screen(const char *s, unsigned n)
{
	(void)s;
	(void)n;
}

static void *stdio_alloc(unsigned n)
{
	return malloc(n ? n : 1u);
}

static void stdio_free(void *p)
{
	free(p);
}

static unsigned stdio_millis(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (unsigned)((unsigned long long)ts.tv_sec * 1000ull +
			  (unsigned long long)ts.tv_nsec / 1000000ull);
}

static int stdio_width(void)
{
	return 640;
}

static int stdio_height(void)
{
	return 480;
}

static int stdio_host_width(void)
{
	return 640;
}

static int stdio_host_height(void)
{
	return 480;
}

static const char *stdio_runtime(void)
{
#ifdef __APPLE__
	return "mac";
#else
	return "linux";
#endif
}

static void stdio_fill(unsigned rgb)
{
	(void)rgb;
}

static void stdio_pixel(int x, int y, unsigned rgb)
{
	(void)x;
	(void)y;
	(void)rgb;
}

static const mmb_platform stdio_plat = {
	.write_serial = stdio_serial,
	.write_screen = stdio_screen,
	.set_pixel = stdio_pixel,
	.fill_screen = stdio_fill,
	.hdmi_width = stdio_width,
	.hdmi_height = stdio_height,
	.host_width = stdio_host_width,
	.host_height = stdio_host_height,
	.runtime = stdio_runtime,
	.alloc = stdio_alloc,
	.free = stdio_free,
	.millis = stdio_millis,
	.can_quit = 1,
};

void mmb_platform_bind_stdio(void)
{
	mmb_init(&stdio_plat);
}
