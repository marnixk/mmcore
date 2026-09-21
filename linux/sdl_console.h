/*
 * ANSI/CP437 text console for the SDL2 native backend (LN-05).
 *
 * Renders the interpreter's `write_screen` byte stream (prompt, PRINT, COLOUR,
 * LOCATE, CLS, and remote CONNECT/TERM ANSI) into the software framebuffer
 * using the VGA 8x16 CP437 font and the 16-colour CGA palette that
 * `mmb_console_apply_colour` targets.
 */
#ifndef MMB_SDL_CONSOLE_H
#define MMB_SDL_CONSOLE_H

void sdl_console_reset(void);
void sdl_console_resize(void);
void sdl_console_write(const char *s, unsigned n);
void sdl_console_fill(unsigned rgb888);

#endif
