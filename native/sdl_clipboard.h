/*
 * Host OS clipboard bridge for the SDL2 native build (#525).
 *
 * Uses SDL's text clipboard when the video driver provides one (X11, Wayland,
 * Cocoa, Windows) so the AppImage copy/pastes with the desktop. Headless runs
 * (SDL_VIDEODRIVER=dummy, MMB_SDL_DUMP automation) keep an in-process buffer
 * instead, so a copy/paste still round-trips within the same session.
 */
#ifndef MMB_SDL_CLIPBOARD_H
#define MMB_SDL_CLIPBOARD_H

/* Seed the in-process fallback (MMB_CLIPBOARD env var, automation). */
void sdl_clipboard_init(void);

/* malloc'd UTF-8 copy of the host clipboard, or NULL when empty. */
char *sdl_clipboard_get(void);

/* Store UTF-8 text; always succeeds (falls back to the in-process buffer). */
int sdl_clipboard_set(const char *utf8);

#endif
