/*
 * Host test for native/sdl_console.c presentation.
 *
 * Regression: the console used to call sdl_video_present() at the end of every
 * write. The front end edits a line one character at a time, so under
 * SDL_RENDERER_PRESENTVSYNC each character advanced a frame and the prompt
 * cursor appeared to slide/animate. Circle's scanout coalesces those writes,
 * so the console must only mark the framebuffer dirty and let the event loop
 * present once per burst.
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

uint16_t *sdl_video_fb(void) { return g_fb; }
int sdl_video_width(void) { return 640; }
int sdl_video_height(void) { return 480; }

void sdl_video_present(void) { g_presents++; }
void sdl_video_mark_dirty(void) { g_dirty++; }

unsigned sdl_rgb_to_native(unsigned rgb888)
{
	unsigned r = (rgb888 >> 16) & 255u;
	unsigned g = (rgb888 >> 8) & 255u;
	unsigned b = rgb888 & 255u;

	return ((r >> 3) << 11) | ((g >> 3) << 6) | (b >> 3);
}

static int fails;

int main(void)
{
	sdl_console_reset();
	g_presents = 0;
	g_dirty = 0;

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
