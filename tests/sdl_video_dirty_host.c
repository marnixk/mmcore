/*
 * Host test for the dirty-rectangle present contract (#1080).
 *
 * Before the fix, the RUN poll presented the whole texture on every program
 * line (a full RGB555->RGB565 convert plus a full SDL_UpdateTexture across
 * vsync), so an on-screen LINE loop ran ~100x slower than the same loop on a
 * hidden page. sdl_video_present() now converts and uploads only the union of
 * the changed rectangles.
 *
 * This drives the MMB_SDL_TEST hook that exposes the pending rectangle and
 * checks the accumulate / clamp / replace / consume contract that the present
 * path relies on. Runs headless under SDL_VIDEODRIVER=dummy.
 */
#include "sdl_video.h"

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

static int no_rect(void)
{
	int x, y, w, h;

	return !sdl_video_test_dirty_rect(&x, &y, &w, &h);
}

static int rect_is(int x, int y, int w, int h)
{
	int rx, ry, rw, rh;

	if (!sdl_video_test_dirty_rect(&rx, &ry, &rw, &rh))
		return 0;
	return rx == x && ry == y && rw == w && rh == h;
}

int main(void)
{
	if (!sdl_video_open(640, 480))
	{
		fprintf(stderr, "FAIL sdl_video_open: %s\n", SDL_GetError());
		return 1;
	}

	/* Opening (and every resize) leaves the whole screen pending. */
	CHECK(rect_is(0, 0, 640, 480), "open leaves the whole screen pending");

	/* Presenting consumes the pending rectangle. */
	sdl_video_present();
	CHECK(no_rect(), "present consumes the pending rectangle");

	/* Two rectangles union to their bounding box. */
	sdl_video_mark_dirty_rect(10, 20, 30, 40); /* 10..40, 20..60 */
	sdl_video_mark_dirty_rect(50, 5, 10, 10);  /* 50..60,  5..15 */
	CHECK(rect_is(10, 5, 50, 55), "changed rectangles union");

	/* A rectangle partly off the origin clamps to the framebuffer. */
	sdl_video_present();
	sdl_video_mark_dirty_rect(-10, -10, 20, 20);
	CHECK(rect_is(0, 0, 10, 10), "a negative origin clamps to 0,0");

	/* A rectangle past the far edge clamps to the framebuffer. */
	sdl_video_present();
	sdl_video_mark_dirty_rect(630, 470, 100, 100);
	CHECK(rect_is(630, 470, 10, 10), "a rect past the edge clamps");

	/* An entirely off-screen rectangle marks nothing. */
	sdl_video_present();
	sdl_video_mark_dirty_rect(700, 700, 10, 10);
	CHECK(no_rect(), "an off-screen rectangle is ignored");

	/* A full repaint replaces a smaller pending rect with the whole screen. */
	sdl_video_present();
	sdl_video_mark_dirty_rect(10, 10, 5, 5);
	sdl_video_mark_dirty();
	CHECK(rect_is(0, 0, 640, 480), "mark_dirty replaces with the whole screen");

	/* A MODE change (resize to smaller) must not leave a stale, larger rect
	 * pointing past the new buffers. */
	sdl_video_present();
	sdl_video_mark_dirty_rect(0, 0, 640, 480);
	sdl_video_resize(320, 240);
	CHECK(rect_is(0, 0, 320, 240), "resize replaces the pending rectangle");

	/* And a rectangle after the resize clamps to the new framebuffer. */
	sdl_video_present();
	sdl_video_mark_dirty_rect(300, 230, 100, 100);
	CHECK(rect_is(300, 230, 20, 10), "a rect clamps to the resized framebuffer");

	sdl_video_close();

	if (fails)
		return 1;
	printf("all checks passed\n");
	return 0;
}
