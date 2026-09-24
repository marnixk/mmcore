/*
 * SDL2 window + software framebuffer for the native backend.
 *
 * The software framebuffer stores MMBasic "native" pixels: 16-bit RGB555
 * with green at bit 6, matching Circle's COLOR16 so pages, sprites and the
 * colour model behave identically to the Pi. Presentation converts to SDL's
 * RGB565 (a per-frame pass done only on the Linux side).
 */
#ifndef MMB_SDL_VIDEO_H
#define MMB_SDL_VIDEO_H

#include <stdint.h>

int sdl_video_open(int w, int h);
void sdl_video_close(void);
int sdl_video_resize(int w, int h);

uint16_t *sdl_video_fb(void);
int sdl_video_width(void);
int sdl_video_height(void);

/* Pixel size of the host drawable the framebuffer is presented into. This is
 * the window in windowed mode and the display in fullscreen: what MM.HOST.HRES
 * and MM.HOST.VRES report. */
void sdl_video_host_size(int *w, int *h);

/* Map a host window/drawable point to software-framebuffer pixels, undoing the
 * integer-scale letterbox viewport. Returns 0 when there is no framebuffer. */
int sdl_video_window_to_fb(int wx, int wy, int *fx, int *fy);

/* RGB888 <-> native (RGB555, green at bit 6) matching Circle's COLOR16. */
unsigned sdl_rgb_to_native(unsigned rgb888);
unsigned sdl_native_to_rgb(unsigned native);

void sdl_video_present(void);
void sdl_video_mark_dirty(void);
int sdl_video_should_quit(void);
void sdl_video_request_quit(void);
void sdl_video_toggle_fullscreen(void);

/* Request desktop-fullscreen (1) or a window (0). Only acts when the current
 * state differs, so a startup --fullscreen and the Alt+Enter toggle behave
 * identically. */
void sdl_video_set_fullscreen(int on);

/* Whether the window is in desktop-fullscreen (drawable is the display). */
int sdl_video_is_fullscreen(void);

/* Current window title (empty when no window is open); for tests/debug. */
const char *sdl_video_window_title(void);

/* Write the software framebuffer as a binary PPM (headless test/debug aid). */
int sdl_video_dump_ppm(const char *path);

#endif
