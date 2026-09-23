/*
 * Host OS clipboard bridge for the SDL2 native build (#525).
 *
 * SDL_SetClipboardText/GetClipboardText talk to the real desktop clipboard
 * under X11/Wayland/Cocoa/Windows. The dummy and headless video drivers do not
 * implement it, so a second copy is kept in-process; get prefers the OS
 * clipboard and falls back to that buffer.
 */
#include "sdl_clipboard.h"

#include <SDL.h>
#include <stdlib.h>
#include <string.h>

static char *s_fallback;

void sdl_clipboard_init(void)
{
	const char *seed = getenv("MMB_CLIPBOARD");

	free(s_fallback);
	s_fallback = 0;
	if (seed)
		sdl_clipboard_set(seed);
}

char *sdl_clipboard_get(void)
{
	char *out = 0;

	if (SDL_HasClipboardText())
	{
		char *t = SDL_GetClipboardText();

		if (t && t[0])
			out = strdup(t);
		SDL_free(t);
	}
	if (out)
		return out;
	if (s_fallback && s_fallback[0])
		return strdup(s_fallback);
	return 0;
}

int sdl_clipboard_set(const char *utf8)
{
	char *copy;

	if (!utf8)
		utf8 = "";
	copy = strdup(utf8);
	if (copy)
	{
		free(s_fallback);
		s_fallback = copy;
	}
	SDL_SetClipboardText(utf8); /* best effort: no-op on headless drivers */
	return 0;
}
