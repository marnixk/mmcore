/*
 * ANSI/CP437 text console for the SDL2 native backend (LN-05).
 *
 * Renders the interpreter's `write_screen` byte stream (prompt, PRINT, COLOUR,
 * LOCATE, CLS, and remote CONNECT/TERM ANSI) into the software framebuffer
 * using the VGA 8x16 CP437 font. Foregrounds use the 16-colour CGA palette;
 * backgrounds honour the 24-bit `ESC[48;2;R;G;Bm` that
 * `mmb_console_apply_colour` emits, so PRINT paper is the exact colour CLS
 * filled.
 */
#ifndef MMB_SDL_CONSOLE_H
#define MMB_SDL_CONSOLE_H

void sdl_console_reset(void);
void sdl_console_resize(void);
void sdl_console_write(const char *s, unsigned n);
void sdl_console_fill(unsigned rgb888);

/* Virtual-console screen snapshot/restore (returns 1 when handled). */
int sdl_console_save(int slot, int tui);
int sdl_console_restore(int slot, int tui);

#endif
