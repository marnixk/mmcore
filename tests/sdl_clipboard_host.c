/*
 * Host test for native/sdl_clipboard.c (#525).
 *
 * The SDL text clipboard is unavailable under the dummy video driver, so the
 * backend must fall back to its in-process buffer: copy/paste then round-trips
 * within a session. The MMB_CLIPBOARD seed is honoured at init.
 */
#include "sdl_clipboard.h"

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;

static void expect(const char *name, const char *got, const char *want)
{
	if (!got || strcmp(got, want) != 0)
	{
		fprintf(stderr, "FAIL %s: got %s want %s\n", name,
			got ? got : "(null)", want);
		fails++;
	}
}

int main(void)
{
	char *t;

	setenv("SDL_VIDEODRIVER", "dummy", 1);
	unsetenv("MMB_CLIPBOARD");
	if (SDL_Init(SDL_INIT_VIDEO) != 0)
	{
		fprintf(stderr, "SDL init failed: %s\n", SDL_GetError());
		return 1;
	}

	/* Nothing copied yet. */
	sdl_clipboard_init();
	t = sdl_clipboard_get();
	if (t)
	{
		fprintf(stderr, "FAIL empty: got %s\n", t);
		fails++;
		free(t);
	}

	/* Round-trip through the in-process fallback. */
	sdl_clipboard_set("HELLO CLIP");
	t = sdl_clipboard_get();
	expect("set/get", t, "HELLO CLIP");
	free(t);

	/* Overwrite. */
	sdl_clipboard_set("SECOND");
	t = sdl_clipboard_get();
	expect("overwrite", t, "SECOND");
	free(t);

	/* init() honours the MMB_CLIPBOARD seed. */
	setenv("MMB_CLIPBOARD", "SEEDED", 1);
	sdl_clipboard_init();
	t = sdl_clipboard_get();
	expect("seed", t, "SEEDED");
	free(t);

	/* A later copy replaces the seed. */
	sdl_clipboard_set("LATER");
	t = sdl_clipboard_get();
	expect("seed replaced", t, "LATER");
	free(t);

	SDL_Quit();
	if (fails)
		return 1;
	printf("all checks passed\n");
	return 0;
}
