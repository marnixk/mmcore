/*
 * Host test for native/sdl_video.c dirty-rectangle presentation (#1080).
 *
 * A graphics-heavy program changes only a small part of the screen between
 * presents. sdl_video_present() must convert and upload only the union of the
 * changed rectangles, not the whole panel, or a Chromebook at 1366x768 pays a
 * full-frame RGB555->RGB565 pass for every LINE/PLOT. This checks the
 * accumulation/clamping/consume contract of sdl_video_mark_dirty_rect() and
 * sdl_video_dirty_rect(); the end-to-end timing guard lives in
 * test_linux_native.py.
 *
 * Runs headless under SDL_VIDEODRIVER=dummy.
 */
#include "sdl_video.h"
#include "sdl_scale.h"

#include <SDL.h>
#include <stdio.h>

/* Stand-ins for the generated branding array the real build links. */
const unsigned char mmcore_icon[4] = {0, 0, 255, 255};
const unsigned int mmcore_icon_w = 1;
const unsigned int mmcore_icon_h = 1;

static int fails;

#define CHECK(cond, msg)                                                       \
	do                                                                     \
	{                                                                      \
		if (!(cond))                                                   \
		{                                                              \
			fprintf(stderr, "FAIL %s\n", msg);                     \
			fails++;                                               \
		}                                                              \
	} while (0)

static void check_rect(int ex, int ey, int ew, int eh, const char *msg)
{
	int x = -1, y = -1, w = -1, h = -1;

	sdl_video_dirty_rect(&x, &y, &w, &h);
	if (x != ex || y != ey || w != ew || h != eh)
	{
		fprintf(stderr, "FAIL %s: got (%d,%d,%d,%d) want (%d,%d,%d,%d)\n",
			msg, x, y, w, h, ex, ey, ew, eh);
		fails++;
	}
}

static void check_map(int ow, int oh, int tw, int th,
		      int sx0, int sy0, int sx1, int sy1,
		      int ex0, int ey0, int ex1, int ey1, const char *msg)
{
	int x0 = -1, y0 = -1, x1 = -1, y1 = -1;

	sdl_scale_map_rect(ow, oh, tw, th, sx0, sy0, sx1, sy1, &x0, &y0, &x1, &y1);
	if (x0 != ex0 || y0 != ey0 || x1 != ex1 || y1 != ey1)
	{
		fprintf(stderr, "FAIL %s: got (%d,%d,%d,%d) want (%d,%d,%d,%d)\n",
			msg, x0, y0, x1, y1, ex0, ey0, ex1, ey1);
		fails++;
	}
}

int main(void)
{
	if (!sdl_video_open(640, 480))
	{
		fprintf(stderr, "FAIL sdl_video_open: %s\n", SDL_GetError());
		return 1;
	}

	/* A fresh open marks the whole framebuffer dirty. */
	check_rect(0, 0, 640, 480, "open marks the full frame");
	sdl_video_present();
	check_rect(0, 0, 0, 0, "present consumes the dirty rect");

	/* The union of two disjoint changes covers both. */
	sdl_video_mark_dirty_rect(10, 20, 30, 40);
	sdl_video_mark_dirty_rect(50, 60, 10, 10);
	check_rect(10, 20, 50, 50, "two rectangles union");

	/* A contained change does not grow the rectangle. */
	sdl_video_mark_dirty_rect(15, 25, 4, 4);
	check_rect(10, 20, 50, 50, "contained change is absorbed");

	/* Negative/off-screen coordinates clamp to the framebuffer. */
	sdl_video_mark_dirty_rect(-5, -5, 10, 10);
	check_rect(0, 0, 60, 70, "negative origin clamps");
	sdl_video_mark_dirty_rect(6000, 6000, 10, 10);
	check_rect(0, 0, 60, 70, "off-screen rect is ignored");

	/* An empty rectangle is ignored; present clears the pending area. */
	sdl_video_mark_dirty_rect(1, 2, 0, 0);
	check_rect(0, 0, 60, 70, "empty rect is ignored");
	sdl_video_present();
	check_rect(0, 0, 0, 0, "second present consumes");

	/* A whole-screen mark replaces (does not union) the pending rectangle,
	 * which is what keeps a MODE change from leaving a stale larger rect
	 * pointing past the end of the new buffer. */
	sdl_video_mark_dirty_rect(1, 1, 3, 3);
	sdl_video_mark_dirty();
	check_rect(0, 0, 640, 480, "full mark replaces the pending rect");

	/* #1083: map a dirty framebuffer sub-rectangle through the viewport to
	 * the destination sub-rectangle the present copies. */
	check_map(1280, 960, 640, 480, 10, 20, 40, 60, 20, 40, 80, 120,
		  "2x integer scale maps the sub-rect");
	check_map(1280, 960, 640, 480, 0, 0, 640, 480, 0, 0, 1280, 960,
		  "2x full frame maps to the viewport");
	check_map(1280, 720, 640, 480, 8, 16, 16, 32, 328, 136, 336, 152,
		  "1:1 letterbox offset is applied");
	check_map(500, 400, 640, 480, 0, 0, 640, 480, 0, 12, 500, 387,
		  "aspect-fit full frame maps to the viewport");
	check_map(1280, 960, 640, 480, -5, -5, 650, 490, 0, 0, 1280, 960,
		  "out-of-bounds source is clamped");
	check_map(0, 0, 640, 480, 10, 10, 20, 20, 0, 0, 0, 0,
		  "degenerate drawable maps to empty");

	sdl_video_close();

	if (fails)
		return 1;
	printf("all checks passed\n");
	return 0;
}
