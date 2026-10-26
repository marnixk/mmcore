/*
 * Host test for native/sdl_tui.c dirty-rectangle marking (#1379).
 *
 * sdl_tui.c used to mark a dirty rect per pixel through plot(), so an 8x16
 * glyph queued 128 marks against the fixed dirty list introduced by #1367.
 * Each glyph, scaled glyph and fill must now write the framebuffer without
 * marking and record one enclosing rectangle instead. Over-marking is safe,
 * under-marking is not, so every case also checks the rectangle covers the
 * pixels actually written.
 */
#include "sdl_tui.h"
#include "sdl_video.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

const unsigned char mmb_cp437_8x16[256 * 16] = {
	[65 * 16 + 0] = 0x18,
	[65 * 16 + 1] = 0x24,
	[65 * 16 + 2] = 0x42,
	[65 * 16 + 3] = 0x42,
	[65 * 16 + 4] = 0x7E,
	[65 * 16 + 5] = 0x42,
	[65 * 16 + 6] = 0x42,
	[65 * 16 + 7] = 0x42,
};
const unsigned char mmb_tnr_16x32[95 * 32 * 2];
const unsigned char mmb_tnr_24x48[95 * 48 * 3];
const unsigned char mmb_tnr_32x64[95 * 64 * 4];

#define MAX_MARKS 32

static uint16_t g_fb[640 * 480];
static int g_marks;
static int g_mx[MAX_MARKS], g_my[MAX_MARKS];
static int g_mw[MAX_MARKS], g_mh[MAX_MARKS];

uint16_t *sdl_video_fb(void) { return g_fb; }
int sdl_video_width(void) { return 640; }
int sdl_video_height(void) { return 480; }
void sdl_video_present(void) {}

void sdl_video_mark_dirty(void)
{
	g_marks = 1;
	g_mx[0] = 0;
	g_my[0] = 0;
	g_mw[0] = 640;
	g_mh[0] = 480;
}

void sdl_video_mark_dirty_rect(int x, int y, int w, int h)
{
	int x1, y1;

	if (w <= 0 || h <= 0)
		return;
	x1 = x + w;
	y1 = y + h;
	if (x < 0)
		x = 0;
	if (y < 0)
		y = 0;
	if (x1 > 640)
		x1 = 640;
	if (y1 > 480)
		y1 = 480;
	if (x >= x1 || y >= y1)
		return;
	if (g_marks < MAX_MARKS)
	{
		g_mx[g_marks] = x;
		g_my[g_marks] = y;
		g_mw[g_marks] = x1 - x;
		g_mh[g_marks] = y1 - y;
	}
	g_marks++;
}

unsigned sdl_rgb_to_native(unsigned rgb888)
{
	unsigned r = (rgb888 >> 16) & 255u;
	unsigned g = (rgb888 >> 8) & 255u;
	unsigned b = rgb888 & 255u;

	return ((r >> 3) << 11) | ((g >> 3) << 6) | (b >> 3);
}

unsigned sdl_native_to_rgb(unsigned native)
{
	unsigned r = (native >> 11) & 31u;
	unsigned g = (native >> 6) & 31u;
	unsigned b = native & 31u;

	return ((r << 3) | (r >> 2)) << 16 | ((g << 3) | (g >> 2)) << 8 |
	       ((b << 3) | (b >> 2));
}

static int fails;

static void reset_marks(void)
{
	g_marks = 0;
}

static void check_one_mark(int ex, int ey, int ew, int eh, const char *msg)
{
	if (g_marks != 1)
	{
		fprintf(stderr, "FAIL %s: %d marks, want 1\n", msg, g_marks);
		fails++;
		return;
	}
	if (g_mx[0] != ex || g_my[0] != ey || g_mw[0] != ew ||
	    g_mh[0] != eh)
	{
		fprintf(stderr,
			"FAIL %s: got (%d,%d,%d,%d) want (%d,%d,%d,%d)\n",
			msg, g_mx[0], g_my[0], g_mw[0], g_mh[0], ex, ey, ew,
			eh);
		fails++;
	}
}

static void check_no_mark(const char *msg)
{
	if (g_marks != 0)
	{
		fprintf(stderr, "FAIL %s: %d marks, want 0\n", msg, g_marks);
		fails++;
	}
}

int main(void)
{
	const uint16_t fg = (uint16_t)sdl_rgb_to_native(0xFFFFFFu);
	const uint16_t bg = 0;

	memset(g_fb, 0, sizeof(g_fb));

	/* One glyph is one 8x16 cell mark, not 128 pixel marks. */
	reset_marks();
	sdl_tui_glyph(0, 0, 'A', 0xFFFFFFu, 0);
	check_one_mark(0, 0, 8, 16, "glyph marks its cell");
	/* ...and it still paints the glyph. */
	if (g_fb[0] != bg || g_fb[3] != fg)
	{
		fprintf(stderr, "FAIL glyph pixels: got %u/%u want %u/%u\n",
			g_fb[0], g_fb[3], bg, fg);
		fails++;
	}

	reset_marks();
	sdl_tui_glyph(3, 2, 'A', 0xFFFFFFu, 0);
	check_one_mark(24, 32, 8, 16, "glyph mark is cell-aligned");

	/* A fully clipped cell writes nothing and must not mark. */
	reset_marks();
	sdl_tui_glyph(-1, 0, 'A', 0xFFFFFFu, 0);
	check_no_mark("glyph left of screen is not marked");
	reset_marks();
	sdl_tui_glyph(0, -1, 'A', 0xFFFFFFu, 0);
	check_no_mark("glyph above screen is not marked");

	/* Scaled character cells mark their scaled extent once. */
	reset_marks();
	sdl_tui_glyph_n(2, 1, 'A', 0xFFFFFFu, 0, 2);
	check_one_mark(16, 16, 16, 32, "2x glyph marks its scaled cell");

	/* The built-in TNR font path marks the glyph box once. */
	reset_marks();
	sdl_tui_glyph_n_px(100, 100, 'A', 0xFFFFFFu, 0, 2, 0);
	check_one_mark(100, 100, 16, 32, "2x TNR glyph marks its box");
	reset_marks();
	sdl_tui_glyph_n_px(100, 100, 'A', 0xFFFFFFu, 0, 2, 1);
	check_one_mark(100, 100, 16, 32, "ink-only TNR glyph marks its box");

	/* The fallback path scales the 8x16 cell. */
	reset_marks();
	sdl_tui_glyph_n_px(200, 50, 1, 0xFFFFFFu, 0, 3, 0);
	check_one_mark(200, 50, 24, 48, "3x fallback glyph marks its box");

	/* Fills mark the filled rectangle once. */
	reset_marks();
	sdl_tui_fill_px(10, 20, 5, 7, 0xFFFFFFu);
	check_one_mark(10, 20, 5, 7, "fill marks its rectangle");

	/* A partially clipped fill marks the visible part once. */
	reset_marks();
	sdl_tui_fill_px(635, 10, 10, 5, 0xFFFFFFu);
	check_one_mark(635, 10, 5, 5, "clipped fill marks the visible part");

	reset_marks();
	sdl_tui_fill_px(700, 10, 5, 5, 0xFFFFFFu);
	check_no_mark("off-screen fill is not marked");

	if (fails)
		return 1;
	printf("all checks passed\n");
	return 0;
}
