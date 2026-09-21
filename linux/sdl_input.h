/*
 * SDL2 keyboard input -> MMBasic byte stream (LN-04).
 *
 * Produces the same encodings the Circle backend delivers: Alt as a 0x01
 * prefix + letter, xterm CSI for arrows/Home/End/Ins/Del/PgUp/PgDn/F-keys,
 * Shift+Tab as CSI Z, Ctrl+Enter as CSI 29~, and raw control bytes for
 * Ctrl+letter. Alt+Enter at the prompt toggles fullscreen.
 */
#ifndef MMB_SDL_INPUT_H
#define MMB_SDL_INPUT_H

void sdl_input_init(void);

/* Drain queued SDL events, dispatching keys to the front end (or the RUN
 * inkey queue) and handling quit/resize/fullscreen. */
void sdl_input_pump(void);

int sdl_input_alt_held(void);
int sdl_input_ctrl_alt_held(void);

#endif
