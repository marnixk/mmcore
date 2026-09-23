/*
 * Host test for native/sdl_video.c window setup (#617, #630):
 *
 *  - the window opens windowed with the title "mmcore";
 *  - sdl_video_set_fullscreen() only flips when the state differs, so the
 *    startup --fullscreen path and the Alt+Enter toggle agree;
 *  - the embedded icon surface is accepted by SDL_SetWindowIcon.
 *
 * Runs headless under SDL_VIDEODRIVER=dummy. The generated branding array is
 * not needed here: a 1x1 stand-in supplies the symbols the real build links
 * from native/build/mmcore_icon.o.
 */
#include "sdl_video.h"

#include <SDL.h>
#include <stdio.h>
#include <string.h>

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

int main(void)
{
	if (!sdl_video_open(640, 480))
	{
		fprintf(stderr, "FAIL sdl_video_open: %s\n", SDL_GetError());
		return 1;
	}

	CHECK(strcmp(sdl_video_window_title(), "mmcore") == 0,
	      "window title is mmcore");
	CHECK(!sdl_video_is_fullscreen(), "a fresh window starts windowed");

	sdl_video_set_fullscreen(1);
	CHECK(sdl_video_is_fullscreen(), "set_fullscreen(1) enters fullscreen");
	sdl_video_set_fullscreen(1);
	CHECK(sdl_video_is_fullscreen(), "set_fullscreen(1) is idempotent");
	sdl_video_set_fullscreen(0);
	CHECK(!sdl_video_is_fullscreen(), "set_fullscreen(0) leaves fullscreen");
	sdl_video_set_fullscreen(0);
	CHECK(!sdl_video_is_fullscreen(), "set_fullscreen(0) is idempotent");

	sdl_video_toggle_fullscreen();
	CHECK(sdl_video_is_fullscreen(), "toggle from windowed");
	sdl_video_toggle_fullscreen();
	CHECK(!sdl_video_is_fullscreen(), "toggle back to windowed");

	sdl_video_close();

	if (fails)
		return 1;
	printf("all checks passed\n");
	return 0;
}
