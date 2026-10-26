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

/* Mark the current program frame ready and service the display grid. The
 * present happens at the display frame boundary derived from the refresh
 * rate; called between boundaries it just leaves the dirty regions pending
 * for the line-loop poll to flush (#1367). Never blocks for vsync: the
 * renderer is created without SDL_RENDERER_PRESENTVSYNC and the copy is paced
 * here instead, so SETTICK/ON KEY handlers keep their cadence. */
void sdl_video_present(void);

/* Flush any pending dirty regions immediately, ignoring the display grid.
 * Only for shutdown / a final frame, where waiting another display frame
 * would lose the last drawing. */
void sdl_video_present_now(void);

void sdl_video_mark_dirty(void);

/* Non-zero when the display frame grid is at a present boundary now. A cheap,
 * non-mutating peek: the per-line poll uses it so it only calls
 * sdl_video_present() at a boundary instead of on every program line. */
int sdl_video_present_due(void);

/* The display frame period in microseconds, derived from the current mode's
 * refresh rate (60 Hz fallback when the driver reports nothing useful). */
unsigned long long sdl_video_frame_period_us(void);

/* Whether presents are believed to wait for the vertical blank. The SDL
 * backend no longer relies on a blocking present (it always paces itself), so
 * this is 0; kept as a debug aid. */
int sdl_video_present_is_vsynced(void);

/* Mark only a rectangle of the software framebuffer as changed. The present
 * then converts and uploads just those regions instead of the whole panel,
 * which keeps the per-present cost proportional to what a graphics-heavy
 * program actually drew. A small fixed list of disjoint rectangles is kept;
 * only when it is full is a new rectangle coalesced into the one whose union
 * grows least. Coordinates are clamped to the framebuffer; an empty rect is
 * ignored. sdl_video_mark_dirty() remains the whole-screen case. */
void sdl_video_mark_dirty_rect(int x, int y, int w, int h);

/* The bounding box of every dirty rectangle not yet presented, or w == 0 when
 * clean. Test/debug aid. */
void sdl_video_dirty_rect(int *x, int *y, int *w, int *h);

/* Number of pending dirty rectangles (test/debug). */
int sdl_video_dirty_count(void);

/* Copy pending dirty rectangle i (0 <= i < sdl_video_dirty_count()) to the
 * out pointers. Returns 1 when i is in range, 0 otherwise. Test/debug aid. */
int sdl_video_dirty_rect_at(int i, int *x, int *y, int *w, int *h);

/* Deterministic present pacing grid. sdl_video.c uses this for the real
 * present path; tests feed synthetic timestamps to prove there is a single
 * display-period grid and at most one present per frame (#1367). */
typedef struct {
	unsigned long long period_us;
	unsigned long long next_us;
	int valid;
} sdl_pace;

void sdl_pace_init(sdl_pace *p, unsigned long long period_us,
		   unsigned long long now_us);
/* Non-zero when now_us is at/past the next boundary; does not advance. */
int sdl_pace_peek(const sdl_pace *p, unsigned long long now_us);
/* Advance the grid at a boundary and return 1 when a present should run now.
 * If we fell more than a whole frame behind (pause, mode change), resync to
 * now instead of accumulating a backlog of catch-up presents. */
int sdl_pace_step(sdl_pace *p, unsigned long long now_us);

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
