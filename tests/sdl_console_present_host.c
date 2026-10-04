/*
 * Host test for native/sdl_console.c presentation and dirty rectangles.
 *
 * Regression (#1044): the console used to call sdl_video_present() at the end
 * of every write. The front end edits a line one character at a time, so under
 * SDL_RENDERER_PRESENTVSYNC each character advanced a frame and the prompt
 * cursor appeared to slide/animate. Circle's scanout coalesces those writes,
 * so the console must only mark the framebuffer dirty and let the event loop
 * present once per burst.
 *
 * #1081: each write must mark only the cells it touched. This checks the exact
 * marked rectangle for a single character (the glyph cell plus the cursor cell
 * it advances to) and for a cursor-only move (the old and new cursor cells).
 */
#include "sdl_console.h"
#include "sdl_video.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

const unsigned char mmb_cp437_8x16[256 * 16] = { 0 };

static uint16_t g_fb[640 * 480];
static int g_presents;
static int g_dirty;
static int g_dx0, g_dy0, g_dx1, g_dy1;

uint16_t *sdl_video_fb(void) { return g_fb; }
int sdl_video_width(void) { return 640; }
int sdl_video_height(void) { return 480; }

void sdl_video_present(void) { g_presents++; }

void sdl_video_mark_dirty(void)
{
	g_dirty++;
	g_dx0 = 0;
	g_dy0 = 0;
	g_dx1 = 640;
	g_dy1 = 480;
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
	g_dirty++;
	if (g_dx1 <= g_dx0 || g_dy1 <= g_dy0)
	{
		g_dx0 = x;
		g_dy0 = y;
		g_dx1 = x1;
		g_dy1 = y1;
		return;
	}
	if (x < g_dx0)
		g_dx0 = x;
	if (y < g_dy0)
		g_dy0 = y;
	if (x1 > g_dx1)
		g_dx1 = x1;
	if (y1 > g_dy1)
		g_dy1 = y1;
}

void sdl_video_dirty_rect(int *x, int *y, int *w, int *h)
{
	int dirty = g_dx1 > g_dx0 && g_dy1 > g_dy0;

	if (x)
		*x = dirty ? g_dx0 : 0;
	if (y)
		*y = dirty ? g_dy0 : 0;
	if (w)
		*w = dirty ? g_dx1 - g_dx0 : 0;
	if (h)
		*h = dirty ? g_dy1 - g_dy0 : 0;
}

unsigned sdl_rgb_to_native(unsigned rgb888)
{
	unsigned r = (rgb888 >> 16) & 255u;
	unsigned g = (rgb888 >> 8) & 255u;
	unsigned b = rgb888 & 255u;

	return ((r >> 3) << 11) | ((g >> 3) << 6) | (b >> 3);
}

static int fails;

static void reset_dirty(void)
{
	g_dx0 = g_dy0 = g_dx1 = g_dy1 = 0;
}

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

int main(void)
{
	sdl_console_reset();
	g_presents = 0;
	g_dirty = 0;
	reset_dirty();

	/* A line editor burst: each character is its own write. */
	sdl_console_write("HE", 2);
	sdl_console_write("L", 1);
	sdl_console_write("L", 1);
	sdl_console_write("O", 1);

	if (g_presents != 0)
	{
		fprintf(stderr, "FAIL: console wrote presented %d times\n",
			g_presents);
		fails++;
	}
	if (g_dirty == 0)
	{
		fprintf(stderr, "FAIL: console writes did not mark dirty\n");
		fails++;
	}

	/* A single character at the home position marks its glyph cell and the
	 * cursor cell it advances to, nothing more. */
	sdl_console_reset();
	reset_dirty();
	g_dirty = 0;
	sdl_console_write("A", 1);
	check_rect(0, 0, 16, 16, "single character marks char + cursor cell");
	if (g_dirty == 0)
	{
		fprintf(stderr, "FAIL: single character did not mark dirty\n");
		fails++;
	}

	/* A cursor-only move (CSI 2 C: forward two) marks the old and the new
	 * cursor cells, not the whole screen. */
	reset_dirty();
	sdl_console_write("\x1b[2C", 4);
	check_rect(8, 0, 24, 16, "cursor move marks old + new cursor cell");

	g_presents = 0;
	sdl_console_fill(0);
	if (g_presents != 0)
	{
		fprintf(stderr, "FAIL: console fill presented %d times\n",
			g_presents);
		fails++;
	}

	if (fails)
		return 1;
	printf("all checks passed\n");
	return 0;
}
