/*
 * Host test for the framebuffer (KMS/DRM) default video driver (#828):
 *
 *  - a build with MMB_SDL_FRAMEBUFFER defaults SDL_VIDEODRIVER to kmsdrm only
 *    when the caller has not chosen a driver;
 *  - an explicit SDL_VIDEODRIVER is never overwritten (headless tests, a
 *    desktop session, or a user override);
 *  - a normal desktop build has no forced default.
 *
 * Run with argv[1] "fb" for a framebuffer build, anything else for desktop.
 * Runs headless under SDL_VIDEODRIVER=dummy where it opens a window.
 */
#include "sdl_video.h"

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

int main(int argc, char **argv)
{
	const int framebuffer = (argc > 1 && strcmp(argv[1], "fb") == 0);

	unsetenv("SDL_VIDEODRIVER");

	if (framebuffer)
	{
		CHECK(sdl_video_default_driver() &&
		      strcmp(sdl_video_default_driver(), "kmsdrm") == 0,
		      "framebuffer build defaults to kmsdrm");

		sdl_video_apply_default_driver();
		CHECK(SDL_getenv("SDL_VIDEODRIVER") &&
		      strcmp(SDL_getenv("SDL_VIDEODRIVER"), "kmsdrm") == 0,
		      "an unset driver picks kmsdrm");

		/* An explicit driver must survive open() (no kmsdrm forced). */
		SDL_setenv("SDL_VIDEODRIVER", "dummy", 1);
		sdl_video_apply_default_driver();
		CHECK(strcmp(SDL_getenv("SDL_VIDEODRIVER"), "dummy") == 0,
		      "an explicit driver is preserved");

		if (!sdl_video_open(320, 240))
		{
			fprintf(stderr, "FAIL sdl_video_open: %s\n",
				SDL_GetError());
			return 1;
		}
		CHECK(strcmp(SDL_getenv("SDL_VIDEODRIVER"), "dummy") == 0,
		      "open keeps the caller's driver");
		CHECK(sdl_video_is_fullscreen(),
		      "framebuffer open starts fullscreen");
		CHECK(SDL_ShowCursor(SDL_QUERY) == SDL_DISABLE,
		      "framebuffer open hides the system cursor");
		sdl_video_close();
	}
	else
	{
		CHECK(sdl_video_default_driver() == 0,
		      "desktop build has no forced driver");
		sdl_video_apply_default_driver();
		CHECK(SDL_getenv("SDL_VIDEODRIVER") == 0,
		      "desktop build leaves the driver unset");
	}

	if (fails)
		return 1;
	printf("all checks passed\n");
	return 0;
}
