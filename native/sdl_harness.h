/*
 * Test-only input injection and framebuffer capture for the native SDL build
 * (#633, PAINT rewrite wave 0C).
 *
 * A headless harness can script a session by pointing MMB_SDL_HARNESS at a
 * text file. The file is read incrementally and exactly one command is
 * executed per main-loop frame, so the app sees each synthetic event on its
 * own frame (a poll-based app such as PAINT needs mouse move / down / up
 * spread across frames, or the whole gesture collapses into one state).
 *
 * Commands (one per line, '#' starts a comment):
 *   feed TEXT         type TEXT plus Enter into the REPL / running program
 *   mouse X Y         move the pointer to framebuffer pixel (X, Y)
 *   down [l|r|m]      press a mouse button (default left)
 *   up [l|r|m]        release a mouse button (default left)
 *   click [l|r|m]     press then release across two frames
 *   key [mod+]NAME    press a key; mods are alt/ctrl/shift/gui
 *   text STRING       deliver a text-input event (printable characters)
 *   shot FILE         write the current framebuffer to FILE as a PPM
 *   mark FILE         write FILE so the harness can wait for a point in time
 *   quit              end the session (the loop exits, PAINT still on screen)
 *
 * Enabling the harness also reports a mouse as present (calls sdl_input_init
 * and pt_force_mouse) so PAINT - which refuses to start without a pointer -
 * enters. With no harness and no MMB_PAINT_FORCE_MOUSE the real target
 * behaviour is unchanged: PAINT reports that it needs a mouse.
 */
#ifndef MMB_SDL_HARNESS_H
#define MMB_SDL_HARNESS_H

void sdl_harness_init(void);

/* Execute at most one queued command. Safe to call when disabled. */
void sdl_harness_poll(void);

#endif
