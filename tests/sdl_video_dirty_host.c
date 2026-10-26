/*
 * Host test for native/sdl_video.c dirty-rectangle presentation and present
 * pacing (#1080, #1367).
 *
 * A graphics-heavy program changes only a small part of the screen between
 * presents. sdl_video_present() must convert and upload a small list of
 * changed rectangles, not one union bounding box over the whole panel, so a
 * few 16x16 sprite boxes stay a few small uploads. It must also pace itself on
 * one display-period grid: at most one present per display frame, no second
 * clock (the old 16 ms limiter plus 17 ms frame wait) and no catch-up burst
 * after a stall.
 *
 * Runs headless under SDL_VIDEODRIVER=dummy.
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

static void check_union(int ex, int ey, int ew, int eh, const char *msg)
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

static void check_rect_at(int i, int ex, int ey, int ew, int eh,
			  const char *msg)
{
	int x = -1, y = -1, w = -1, h = -1;

	if (!sdl_video_dirty_rect_at(i, &x, &y, &w, &h))
	{
		fprintf(stderr, "FAIL %s: no rect %d\n", msg, i);
		fails++;
		return;
	}
	if (x != ex || y != ey || w != ew || h != eh)
	{
		fprintf(stderr, "FAIL %s: rect %d got (%d,%d,%d,%d) want "
			"(%d,%d,%d,%d)\n",
			msg, i, x, y, w, h, ex, ey, ew, eh);
		fails++;
	}
}

/* A producer faster than the display must not make presents beat against the
 * display: every display frame with new content presents exactly once, with no
 * 1->3 ms wait ramp and no rhythmic long gap (the old bug produced 18-frame
 * periodicity). */
static void check_present_pacing(void)
{
	const unsigned long long period = 16667; /* 60 Hz in microseconds */
	const unsigned long long tick = 500;     /* poll granularity */
	sdl_pace p;
	unsigned long long t, last = 0, max_gap = 0, min_gap = ~0ull;
	int presents = 0, pending = 0, first = 1;

	sdl_pace_init(&p, period, 0);
	for (t = 0; t <= 2000000ull; t += tick)
	{
		if (t % 16000ull == 0)
			pending = 1; /* program draws every 16 ms */
		if (sdl_pace_step(&p, t) && pending)
		{
			unsigned long long gap;

			pending = 0;
			presents++;
			if (!first)
			{
				gap = t - last;
				if (gap > max_gap)
					max_gap = gap;
				if (gap < min_gap)
					min_gap = gap;
			}
			first = 0;
			last = t;
		}
	}
	/* ~120 display frames in 2 s: one per frame, never two. */
	CHECK(presents >= 119 && presents <= 121, "one present per display frame");
	CHECK(min_gap >= period - tick, "presents never land in the same frame");
	CHECK(max_gap <= period + tick, "no rhythmic dropped-frame stutter");

	/* A slower producer (20 ms) still presents on the frame grid, so its
	 * visible cadence cannot ramp against the display. */
	first = 1;
	pending = 0;
	presents = 0;
	max_gap = 0;
	min_gap = ~0ull;
	last = 0;
	sdl_pace_init(&p, period, 0);
	for (t = 0; t <= 2000000ull; t += tick)
	{
		if (t % 20000ull == 0)
			pending = 1;
		if (sdl_pace_step(&p, t) && pending)
		{
			unsigned long long gap;

			pending = 0;
			presents++;
			if (!first)
			{
				gap = t - last;
				if (gap > max_gap)
					max_gap = gap;
				if (gap < min_gap)
					min_gap = gap;
			}
			first = 0;
			last = t;
		}
	}
	/* A 50 fps producer keeps its 50 presents/s on a 3:2 display cadence
	 * (one 33 ms gap per five frames), never locking to 30 Hz (which would
	 * be every frame doubled). */
	CHECK(presents >= 98 && presents <= 102, "20 ms producer keeps its cadence");
	CHECK(min_gap >= period - tick, "presents never land in the same frame");
	CHECK(max_gap <= 2 * period + tick, "50 fps producer never locks to 30 Hz");

	/* A stall resyncs to now instead of firing a backlog of catch-up
	 * presents. */
	sdl_pace_init(&p, period, 0);
	CHECK(!sdl_pace_step(&p, 0), "no present before the first boundary");
	CHECK(sdl_pace_peek(&p, period) == 1, "peek sees the boundary");
	CHECK(sdl_pace_peek(&p, 0) == 0, "peek is non-mutating");
	CHECK(sdl_pace_step(&p, period), "first boundary presents");
	CHECK(sdl_pace_step(&p, period + 5 * period), "stall resyncs once");
	CHECK(!sdl_pace_step(&p, period + 5 * period + 1),
	      "no catch-up burst after a stall");
}

int main(void)
{
	int i;

	if (!sdl_video_open(640, 480))
	{
		fprintf(stderr, "FAIL sdl_video_open: %s\n", SDL_GetError());
		return 1;
	}

	/* #1367: the backend no longer relies on a blocking swap for pacing. */
	CHECK(sdl_video_present_is_vsynced() == 0,
	      "pacing is explicit, not renderer vsync");

	/* A fresh open marks the whole framebuffer dirty as one rect. */
	CHECK(sdl_video_dirty_count() == 1, "open marks one rect");
	check_rect_at(0, 0, 0, 640, 480, "open marks the full frame");
	check_union(0, 0, 640, 480, "open union is the full frame");
	sdl_video_present_now();
	CHECK(sdl_video_dirty_count() == 0, "present consumes the list");

	/* Two disjoint 16x16-ish boxes stay two small rects, not one union box
	 * spanning most of the frame (#1367). */
	sdl_video_mark_dirty_rect(10, 20, 16, 16);
	sdl_video_mark_dirty_rect(300, 200, 16, 16);
	CHECK(sdl_video_dirty_count() == 2, "two changes stay two rects");
	check_rect_at(0, 10, 20, 16, 16, "first box unchanged");
	check_rect_at(1, 300, 200, 16, 16, "second box unchanged");

	/* A contained change does not add or grow a rect. */
	sdl_video_mark_dirty_rect(12, 22, 4, 4);
	CHECK(sdl_video_dirty_count() == 2, "contained change is absorbed");

	/* Negative/off-screen coordinates clamp to the framebuffer. */
	sdl_video_mark_dirty_rect(-5, -5, 10, 10);
	CHECK(sdl_video_dirty_count() == 3, "negative origin is a new rect");
	check_rect_at(2, 0, 0, 5, 5, "negative origin clamps");
	sdl_video_mark_dirty_rect(6000, 6000, 10, 10);
	CHECK(sdl_video_dirty_count() == 3, "off-screen rect is ignored");

	/* An empty rectangle is ignored. */
	sdl_video_mark_dirty_rect(1, 2, 0, 0);
	CHECK(sdl_video_dirty_count() == 3, "empty rect is ignored");

	/* The fixed list coalesces only when full, and the union still covers
	 * every change. */
	for (i = 0; i < 20; i++)
		sdl_video_mark_dirty_rect((i % 8) * 70, (i / 8) * 40, 10, 10);
	CHECK(sdl_video_dirty_count() <= 16, "list is bounded");
	{
		int x, y, w, h;

		sdl_video_dirty_rect(&x, &y, &w, &h);
		CHECK(x == 0 && y == 0 && x + w >= 490 + 10 &&
			      y + h >= 200 + 16,
		      "coalesced union still covers the changes");
	}
	sdl_video_present_now();
	CHECK(sdl_video_dirty_count() == 0, "present consumes a full list");

	/* A whole-screen mark replaces (does not union) the pending list, which
	 * keeps a MODE change from leaving a stale larger rect pointing past the
	 * end of the new buffer. */
	sdl_video_mark_dirty_rect(1, 1, 3, 3);
	sdl_video_mark_dirty_rect(20, 20, 3, 3);
	sdl_video_mark_dirty();
	CHECK(sdl_video_dirty_count() == 1, "full mark replaces the list");
	check_rect_at(0, 0, 0, 640, 480, "full mark is one full rect");
	sdl_video_present_now();

	/* A forced present re-anchors the grid; the next paced present is due
	 * immediately, and a later display boundary consumes the following
	 * rect. (The exact boundary arithmetic is the deterministic
	 * sdl_pace test above.) */
	sdl_video_present_now();
	sdl_video_mark_dirty_rect(1, 1, 2, 2);
	sdl_video_present();
	CHECK(sdl_video_dirty_count() == 0, "present after a forced frame is due");
	sdl_video_mark_dirty_rect(2, 2, 2, 2);
	CHECK(sdl_video_dirty_count() == 1, "rect stays pending between frames");
	SDL_Delay(20);
	CHECK(sdl_video_present_due() == 1, "next display boundary is due");
	sdl_video_present();
	CHECK(sdl_video_dirty_count() == 0, "boundary consumes the rect");

	check_present_pacing();

	sdl_video_close();

	if (fails)
		return 1;
	printf("all checks passed\n");
	return 0;
}
