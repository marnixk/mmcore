#include "sdl_scale.h"

#include <stdio.h>

static int fails;

static void expect(const char *name, int got, int want)
{
	if (got != want)
	{
		fprintf(stderr, "FAIL %s: got %d want %d\n", name, got, want);
		fails++;
	}
}

static void check(const char *name, int ow, int oh, int tw, int th,
		  int dx, int dy, int dw, int dh)
{
	int ax, ay, aw, ah;

	sdl_scale_viewport(ow, oh, tw, th, &ax, &ay, &aw, &ah);
	expect(name, ax, dx);
	expect(name, ay, dy);
	expect(name, aw, dw);
	expect(name, ah, dh);
}

int main(void)
{
	/* Exact fit: no bars. */
	check("640x480 in 640x480", 640, 480, 640, 480, 0, 0, 640, 480);
	/* 1080p / 480p = 2.25 -> integer factor 2, side bars. */
	check("480p in 1080p", 1920, 1080, 640, 480, 320, 60, 1280, 960);
	/* Height limits the factor. */
	check("480p in 1280x720", 1280, 720, 640, 480, 320, 120, 640, 480);
	/* 4K / 480p = 4.5 -> factor 4. */
	check("480p in 4K", 3840, 2160, 640, 480, 640, 120, 2560, 1920);
	/* Small integer factor with bars on both axes. */
	check("600x400 in 1400x1000", 1400, 1000, 600, 400, 100, 100, 1200, 800);
	/* Drawable smaller than the framebuffer: aspect-fit, not cropped. */
	check("640x480 in 500x400", 500, 400, 640, 480, 0, 12, 500, 375);
	check("640x480 in 400x500", 400, 500, 640, 480, 0, 100, 400, 300);
	/* Degenerate sizes collapse to an empty rect. */
	check("zero drawable", 0, 480, 640, 480, 0, 0, 0, 0);

	if (fails)
		return 1;
	printf("all checks passed\n");
	return 0;
}
