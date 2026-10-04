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

/* The video driver a framebuffer (KMS/DRM) build should default to, or NULL
 * when the build has no forced default and SDL chooses. */
const char *sdl_video_default_driver(void);

/* Apply sdl_video_default_driver() by setting SDL_VIDEODRIVER, but only when
 * the caller has not already chosen a driver. Called from sdl_video_open(). */
void sdl_video_apply_default_driver(void);

/* The render driver a framebuffer build must use (GLES2/EGL, the only one
 * whose present reaches the KMS scanout), or NULL when SDL chooses. */
const char *sdl_video_default_render_driver(void);

/* SDL render-driver index for sdl_video_default_render_driver(), or -1 to let
 * SDL choose. Called from sdl_video_open(). */
int sdl_video_render_driver_index(void);

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

/* Mark the whole screen as changed. This replaces (does not union) any
 * pending rectangle, so a MODE change cannot leave a stale rect pointing past
 * the new, possibly smaller buffers. */
void sdl_video_mark_dirty(void);

/* Union a changed rectangle (framebuffer pixels) into the pending present
 * region, clamped to the framebuffer. sdl_video_present() converts and uploads
 * only this region (#1080). */
void sdl_video_mark_dirty_rect(int x, int y, int w, int h);

#ifdef MMB_SDL_TEST
/* Test-only: pending present rectangle in framebuffer pixels, or 0 when
 * nothing is pending. Host tests cover the accumulate/clamp/replace/consume
 * contract (#1080). */
int sdl_video_test_dirty_rect(int *x, int *y, int *w, int *h);
#endif

int sdl_video_should_quit(void);
void sdl_video_request_quit(void);
void sdl_video_toggle_fullscreen(void);

/* Request desktop-fullscreen (1) or a window (0). Only acts when the current
 * state differs, so a startup --fullscreen and the Alt+Enter toggle behave
 * identically. */
void sdl_video_set_fullscreen(int on);

/* Whether the window is in desktop-fullscreen (drawable is the display). */
int sdl_video_is_fullscreen(void);

/* Windowed client-size multiplier (`--double` sets 2). sdl_video_resize()
 * keeps the window at scale x the framebuffer size. Values below 1 are
 * clamped to 1. While fullscreen the drawable is the display, so the scale
 * only takes effect on the windowed size (including when leaving fullscreen). */
void sdl_video_set_window_scale(int scale);

/* Current windowed client-size multiplier (default 1). */
int sdl_video_window_scale(void);

/* Current window title (empty when no window is open); for tests/debug. */
const char *sdl_video_window_title(void);

/* Write the software framebuffer as a binary PPM (headless test/debug aid). */
int sdl_video_dump_ppm(const char *path);

#endif
