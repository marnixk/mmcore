#include "sdl_video.h"
#include "sdl_scale.h"

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Older SDL2 headers predate these hints; the strings are still accepted by
 * SDL_SetHint on every version (an unknown hint is simply ignored). */
#ifndef SDL_HINT_APP_ID
#define SDL_HINT_APP_ID "SDL_APP_ID"
#endif
#ifndef SDL_HINT_VIDEO_X11_WMCLASS
#define SDL_HINT_VIDEO_X11_WMCLASS "SDL_VIDEO_X11_WMCLASS"
#endif

/* Runtime window icon, generated from assets/branding/mmcore-app-icon.png by
 * scripts/gen-appicon.py --carray and compiled into the SDL build. */
extern const unsigned char mmcore_icon[];
extern const unsigned int mmcore_icon_w;
extern const unsigned int mmcore_icon_h;

static SDL_Window *s_win;
static SDL_Renderer *s_ren;
static SDL_Texture *s_tex;
static uint16_t *s_fb;     /* RGB555 (green bit 6) */
static uint16_t *s_stage;  /* RGB565 for SDL */
static int s_w, s_h;
static int s_window_scale = 1; /* window client size = FB size * this */
static int s_quit;

/* A small fixed list of changed rectangles. A few 16x16 sprites spread over
 * the screen stay a few small uploads instead of one screen-sized union box
 * (#1367). Only when the list is full does a new rect coalesce into the one
 * whose union grows least; whole-screen marks still replace the list. */
#define SDL_DIRTY_MAX 16
typedef struct {
	int x, y, w, h;
} sdl_dirty_rect;
static sdl_dirty_rect s_dirty[SDL_DIRTY_MAX];
static int s_dirty_n;

/* One display-period pacing grid (#1367). The same grid gates the line-loop
 * present and the scanout alignment, so the two can no longer beat against
 * each other. It is derived from the actual refresh rate, in microseconds so
 * 60 Hz is 16667 us and not the old 16 ms / 17 ms pair. */
static sdl_pace s_pace;
static unsigned long long s_frame_period_us; /* cached, 0 until first use */

static unsigned long long pace_now_us(void)
{
	Uint64 freq = SDL_GetPerformanceFrequency();
	Uint64 count = SDL_GetPerformanceCounter();

	if (!freq)
		return (unsigned long long)SDL_GetTicks() * 1000ull;
	return (count / freq) * 1000000ull +
	       (count % freq) * 1000000ull / freq;
}

unsigned long long sdl_video_frame_period_us(void)
{
	SDL_DisplayMode mode;
	int hz = 0;

	if (s_frame_period_us)
		return s_frame_period_us;
	if (SDL_GetCurrentDisplayMode(0, &mode) == 0)
		hz = mode.refresh_rate;
	if (hz < 24 || hz > 240)
		hz = 60;
	s_frame_period_us = (unsigned long long)((1000000.0 / (double)hz) + 0.5);
	if (!s_frame_period_us)
		s_frame_period_us = 16667;
	return s_frame_period_us;
}

void sdl_pace_init(sdl_pace *p, unsigned long long period_us,
		   unsigned long long now_us)
{
	if (!p)
		return;
	if (!period_us)
		period_us = 16667;
	p->period_us = period_us;
	p->next_us = now_us + period_us;
	p->valid = 1;
}

int sdl_pace_peek(const sdl_pace *p, unsigned long long now_us)
{
	if (!p || !p->valid)
		return 1;
	return now_us >= p->next_us;
}

int sdl_pace_step(sdl_pace *p, unsigned long long now_us)
{
	if (!p || !p->valid)
	{
		if (p)
			sdl_pace_init(p, p->period_us, now_us);
		return 1;
	}
	if (now_us < p->next_us)
		return 0;
	if (now_us - p->next_us >= p->period_us)
		/* Fell more than a frame behind (a pause, a mode change): resync
		 * to now rather than presenting a backlog of catch-up frames. */
		p->next_us = now_us + p->period_us;
	else
		p->next_us += p->period_us;
	return 1;
}

static int dirty_area(const sdl_dirty_rect *r)
{
	return r->w * r->h;
}

/* Area of the bounding box of a and b. */
static int dirty_union_area(const sdl_dirty_rect *r, int x, int y, int x1,
			    int y1)
{
	int ux0 = r->x < x ? r->x : x;
	int uy0 = r->y < y ? r->y : y;
	int ux1 = r->x + r->w > x1 ? r->x + r->w : x1;
	int uy1 = r->y + r->h > y1 ? r->y + r->h : y1;

	return (ux1 - ux0) * (uy1 - uy0);
}

/* Union a changed rectangle into the pending dirty list. A running program
 * changes only a small part of the screen between presents; converting and
 * uploading just those areas makes the per-present cost proportional to the
 * change rather than to the panel size (a Chromebook is 1366x768). */
void sdl_video_mark_dirty_rect(int x, int y, int w, int h)
{
	int x1, y1, i, best, best_grow;

	if (w <= 0 || h <= 0 || s_w <= 0 || s_h <= 0)
		return;
	x1 = x + w;
	y1 = y + h;
	if (x < 0)
		x = 0;
	if (y < 0)
		y = 0;
	if (x1 > s_w)
		x1 = s_w;
	if (y1 > s_h)
		y1 = s_h;
	if (x >= x1 || y >= y1)
		return;

	/* Already covered: nothing to do. */
	for (i = 0; i < s_dirty_n; i++)
	{
		const sdl_dirty_rect *r = &s_dirty[i];

		if (x >= r->x && y >= r->y && x1 <= r->x + r->w &&
		    y1 <= r->y + r->h)
			return;
	}

	if (s_dirty_n < SDL_DIRTY_MAX)
	{
		sdl_dirty_rect *r = &s_dirty[s_dirty_n++];

		r->x = x;
		r->y = y;
		r->w = x1 - x;
		r->h = y1 - y;
		return;
	}

	/* The list is full: coalesce into the rect whose union grows least. */
	best = 0;
	best_grow = dirty_union_area(&s_dirty[0], x, y, x1, y1) -
		    dirty_area(&s_dirty[0]);
	for (i = 1; i < s_dirty_n; i++)
	{
		int grow = dirty_union_area(&s_dirty[i], x, y, x1, y1) -
			   dirty_area(&s_dirty[i]);

		if (grow < best_grow)
		{
			best = i;
			best_grow = grow;
		}
	}
	{
		sdl_dirty_rect *r = &s_dirty[best];
		int ux0 = r->x < x ? r->x : x;
		int uy0 = r->y < y ? r->y : y;
		int ux1 = r->x + r->w > x1 ? r->x + r->w : x1;
		int uy1 = r->y + r->h > y1 ? r->y + r->h : y1;

		r->x = ux0;
		r->y = uy0;
		r->w = ux1 - ux0;
		r->h = uy1 - uy0;
	}
}

void sdl_video_dirty_rect(int *x, int *y, int *w, int *h)
{
	int i, x0 = 0, y0 = 0, x1 = 0, y1 = 0;

	if (s_dirty_n > 0)
	{
		x0 = s_dirty[0].x;
		y0 = s_dirty[0].y;
		x1 = x0 + s_dirty[0].w;
		y1 = y0 + s_dirty[0].h;
		for (i = 1; i < s_dirty_n; i++)
		{
			if (s_dirty[i].x < x0)
				x0 = s_dirty[i].x;
			if (s_dirty[i].y < y0)
				y0 = s_dirty[i].y;
			if (s_dirty[i].x + s_dirty[i].w > x1)
				x1 = s_dirty[i].x + s_dirty[i].w;
			if (s_dirty[i].y + s_dirty[i].h > y1)
				y1 = s_dirty[i].y + s_dirty[i].h;
		}
	}
	if (x)
		*x = x0;
	if (y)
		*y = y0;
	if (w)
		*w = x1 - x0;
	if (h)
		*h = y1 - y0;
}

int sdl_video_dirty_count(void)
{
	return s_dirty_n;
}

int sdl_video_dirty_rect_at(int i, int *x, int *y, int *w, int *h)
{
	if (i < 0 || i >= s_dirty_n)
		return 0;
	if (x)
		*x = s_dirty[i].x;
	if (y)
		*y = s_dirty[i].y;
	if (w)
		*w = s_dirty[i].w;
	if (h)
		*h = s_dirty[i].h;
	return 1;
}

/* Mark the whole screen. Unlike sdl_video_mark_dirty_rect() this replaces any
 * pending rectangles: callers use it after a mode change or a full-screen
 * fill, when an older, larger rectangle may no longer fit the new buffers. */
void sdl_video_mark_dirty(void)
{
	if (s_w <= 0 || s_h <= 0)
	{
		s_dirty_n = 0;
		return;
	}
	s_dirty[0].x = 0;
	s_dirty[0].y = 0;
	s_dirty[0].w = s_w;
	s_dirty[0].h = s_h;
	s_dirty_n = 1;
}

unsigned sdl_rgb_to_native(unsigned rgb888)
{
	unsigned r = (rgb888 >> 16) & 255u;
	unsigned g = (rgb888 >> 8) & 255u;
	unsigned b = rgb888 & 255u;
	return ((r >> 3) << 11) | ((g >> 3) << 6) | (b >> 3);
}

unsigned sdl_native_to_rgb(unsigned native)
{
	unsigned r = (native >> 11) & 0x1Fu;
	unsigned g = (native >> 6) & 0x1Fu;
	unsigned b = native & 0x1Fu;
	return ((r * 255u / 31u) << 16) | ((g * 255u / 31u) << 8) |
	       (b * 255u / 31u);
}

static void free_buffers(void)
{
	free(s_fb);
	free(s_stage);
	s_fb = 0;
	s_stage = 0;
}

/* Give the window a stable identity before the video subsystem starts, so the
 * shell can associate it with the installed .desktop entry (and its icon). A
 * framebuffer (KMS/DRM) build has no window manager to resolve them. */
static void set_app_identity(void)
{
	SDL_SetHint(SDL_HINT_APP_NAME, "mmcore");
#ifndef MMB_SDL_FRAMEBUFFER
	SDL_SetHint(SDL_HINT_APP_ID, "com.marnixk.mmcore"); /* Wayland app-id */
	SDL_SetHint(SDL_HINT_VIDEO_X11_WMCLASS, "mmcore");  /* X11 WM_CLASS */
#endif
}

/* Framebuffer builds render straight to DRM/KMS with no X11/Wayland, so they
 * default SDL to the kmsdrm driver. An explicit SDL_VIDEODRIVER always wins
 * (headless tests, a desktop session, or a user overriding it). */
const char *sdl_video_default_driver(void)
{
#ifdef MMB_SDL_FRAMEBUFFER
	return "kmsdrm";
#else
	return 0;
#endif
}

void sdl_video_apply_default_driver(void)
{
	const char *driver = sdl_video_default_driver();

	if (driver && driver[0] && !SDL_getenv("SDL_VIDEODRIVER"))
		SDL_setenv("SDL_VIDEODRIVER", driver, 1);
}

/* The render driver a framebuffer (KMS/DRM) build should use, or NULL when SDL
 * may choose. kmsdrm has no window surface: only the GLES2/EGL renderer's
 * present reaches the KMS scanout. SDL otherwise picks desktop GL, which renders
 * into a buffer that never reaches the CRTC (a black screen), and the software
 * renderer has no window surface to present at all. */
const char *sdl_video_default_render_driver(void)
{
#ifdef MMB_SDL_FRAMEBUFFER
	return "opengles2";
#else
	return 0;
#endif
}

/* The SDL render-driver index a framebuffer build should create, or -1 for
 * SDL's default. kmsdrm has no window surface: only the GLES2/EGL renderer's
 * present reaches the KMS scanout. SDL otherwise picks desktop GL, which renders
 * into a buffer that never reaches the CRTC (a black screen), and the software
 * renderer has no window surface to present at all.
 *
 * Selecting the index directly is deterministic; the SDL_HINT_RENDER_DRIVER
 * hint is only a preference and SDL falls back to another driver. An explicit
 * SDL_RENDER_DRIVER still wins, and a build without that driver (headless
 * dummy) falls back to SDL's default. */
int sdl_video_render_driver_index(void)
{
	const char *want = sdl_video_default_render_driver();
	int i, n;

	if (!want || !want[0] || SDL_getenv("SDL_RENDER_DRIVER"))
		return -1;
	n = SDL_GetNumRenderDrivers();
	for (i = 0; i < n; ++i)
	{
		SDL_RendererInfo info;

		if (SDL_GetRenderDriverInfo(i, &info) == 0 && info.name &&
		    strcmp(info.name, want) == 0)
			return i;
	}
	return -1;
}

/* Show the M avatar in the taskbar/dock even when launched outside a package
 * (no .desktop entry to resolve). SDL_SetWindowIcon copies the surface. */
static void set_window_icon(void)
{
	SDL_Surface *icon;

	if (!s_win || !mmcore_icon_w || !mmcore_icon_h)
		return;
	icon = SDL_CreateRGBSurfaceWithFormatFrom(
		(void *)mmcore_icon, (int)mmcore_icon_w,
		(int)mmcore_icon_h, 32, (int)(mmcore_icon_w * 4),
		SDL_PIXELFORMAT_RGBA32);
	if (!icon)
		return;
	SDL_SetWindowIcon(s_win, icon);
	SDL_FreeSurface(icon);
}

int sdl_video_open(int w, int h)
{
	set_app_identity();
	sdl_video_apply_default_driver();
	if (SDL_Init(SDL_INIT_VIDEO) != 0)
		return 0;
	SDL_InitSubSystem(SDL_INIT_AUDIO); /* non-fatal if unavailable */
	SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");

	s_win = SDL_CreateWindow("mmcore", SDL_WINDOWPOS_CENTERED,
				 SDL_WINDOWPOS_CENTERED, w, h,
				 SDL_WINDOW_RESIZABLE);
	if (!s_win)
		return 0;
	set_window_icon();

	/* A framebuffer build must pick GLES2/EGL explicitly (index) or the KMS
	 * scanout stays black; other builds and drivers use SDL's default.
	 *
	 * Deliberately no SDL_RENDERER_PRESENTVSYNC (#1367): a blocking present
	 * stalls the interpreter and its SETTICK/ON KEY handlers for up to a
	 * whole frame. sdl_video_present() paces the scanout copy on its own
	 * display-period grid instead, which also removes the second clock the
	 * old frame wait added. */
	s_ren = SDL_CreateRenderer(s_win, sdl_video_render_driver_index(),
				   SDL_RENDERER_ACCELERATED);
	if (!s_ren)
		s_ren = SDL_CreateRenderer(s_win, -1, SDL_RENDERER_SOFTWARE);
	if (!s_ren)
		return 0;
	s_frame_period_us = 0;
	s_pace.valid = 0;
	s_pace.next_us = 0;
	s_pace.period_us = sdl_video_frame_period_us();

	if (!sdl_video_resize(w, h))
		return 0;
#ifdef MMB_SDL_FRAMEBUFFER
	/* No window manager and no reliable system cursor on DRM/KMS: the
	 * interpreter's own software cursor (PAINT, MOUSE ON) is the pointer. */
	SDL_ShowCursor(SDL_DISABLE);
	/* No desktop to go windowed on: fill the display from the first frame. */
	sdl_video_set_fullscreen(1);
#endif
	return 1;
}

void sdl_video_close(void)
{
	free_buffers();
	if (s_tex)
		SDL_DestroyTexture(s_tex);
	if (s_ren)
		SDL_DestroyRenderer(s_ren);
	if (s_win)
		SDL_DestroyWindow(s_win);
	s_tex = 0;
	s_ren = 0;
	s_win = 0;
	SDL_Quit();
}

int sdl_video_resize(int w, int h)
{
	if (w <= 0 || h <= 0)
		return 0;

	free_buffers();
	s_fb = calloc((size_t)w * (size_t)h, sizeof(uint16_t));
	s_stage = malloc((size_t)w * (size_t)h * sizeof(uint16_t));
	if (!s_fb || !s_stage)
	{
		free_buffers();
		return 0;
	}

	if (s_tex)
		SDL_DestroyTexture(s_tex);
	s_tex = SDL_CreateTexture(s_ren, SDL_PIXELFORMAT_RGB565,
				  SDL_TEXTUREACCESS_STREAMING, w, h);
	if (!s_tex)
		return 0;

	s_w = w;
	s_h = h;
	sdl_video_mark_dirty();
	/* A mode change may also change the refresh rate: re-derive the grid and
	 * re-anchor it so the first new frame is not delayed by a stale phase. */
	s_frame_period_us = 0;
	s_pace.period_us = sdl_video_frame_period_us();
	s_pace.next_us = 0;
	s_pace.valid = 0; /* present the first frame of the new mode at once */
	if (s_win)
		SDL_SetWindowSize(s_win, w * s_window_scale,
				  h * s_window_scale);
	return 1;
}

uint16_t *sdl_video_fb(void)
{
	return s_fb;
}

int sdl_video_width(void)
{
	return s_w;
}

int sdl_video_height(void)
{
	return s_h;
}

void sdl_video_host_size(int *w, int *h)
{
	int ow = 0, oh = 0;

	if (s_ren)
		SDL_GetRendererOutputSize(s_ren, &ow, &oh);
	if ((ow <= 0 || oh <= 0) && s_win)
		SDL_GetWindowSize(s_win, &ow, &oh);
	if (ow <= 0 || oh <= 0)
	{
		ow = s_w;
		oh = s_h;
	}
	if (w)
		*w = ow;
	if (h)
		*h = oh;
}

int sdl_video_window_to_fb(int wx, int wy, int *fx, int *fy)
{
	int ow = 0, oh = 0, dx, dy, dw, dh;

	if (s_w <= 0 || s_h <= 0)
		return 0;
	sdl_video_host_size(&ow, &oh);
	sdl_scale_viewport(ow, oh, s_w, s_h, &dx, &dy, &dw, &dh);
	if (dw <= 0 || dh <= 0)
		return 0;
	if (fx)
		*fx = (wx - dx) * s_w / dw;
	if (fy)
		*fy = (wy - dy) * s_h / dh;
	return 1;
}

/* Fill one letterbox band black. A zero-area band (no bar on that edge) is
 * skipped; the draw colour must already be black. */
static void present_clear_band(int x, int y, int w, int h)
{
	SDL_Rect r;

	if (w <= 0 || h <= 0)
		return;
	r.x = x;
	r.y = y;
	r.w = w;
	r.h = h;
	SDL_RenderFillRect(s_ren, &r);
}

int sdl_video_present_is_vsynced(void)
{
	/* The renderer is created without PRESENTVSYNC and the present path
	 * paces itself, so it never relies on the swap blocking for the blank. */
	return 0;
}

int sdl_video_present_due(void)
{
	return sdl_pace_peek(&s_pace, pace_now_us());
}

/* Convert one changed rectangle to RGB565 and upload just that region. */
static void upload_rect(const sdl_dirty_rect *r)
{
	SDL_Rect src;
	int x, y, x0 = r->x, y0 = r->y, x1 = r->x + r->w, y1 = r->y + r->h;

	/* Convert RGB555 (green at bit 6) to SDL's RGB565, but only this
	 * region. */
	for (y = y0; y < y1; y++)
	{
		const uint16_t *srow = s_fb + (size_t)y * (size_t)s_w;
		uint16_t *drow = s_stage + (size_t)y * (size_t)s_w;

		for (x = x0; x < x1; x++)
		{
			uint16_t v = srow[x];
			unsigned rr = (v >> 11) & 0x1Fu;
			unsigned gg = (v >> 6) & 0x1Fu;
			unsigned bb = v & 0x1Fu;
			unsigned g6 = (gg << 1) | (gg >> 4);

			drow[x] = (uint16_t)((rr << 11) | (g6 << 5) | bb);
		}
	}

	/* SDL_UpdateTexture treats a non-NULL rect as a sub-region whose pixels
	 * start at the given pointer, with pitch still spanning a full row. */
	src.x = x0;
	src.y = y0;
	src.w = r->w;
	src.h = r->h;
	SDL_UpdateTexture(s_tex, &src, s_stage + (size_t)y0 * (size_t)s_w + x0,
			  s_w * (int)sizeof(uint16_t));
}

/* Upload every pending rectangle, then copy the whole texture into the
 * drawable and present it. The list is consumed before the (possibly slow)
 * upload so a caller that draws again during the present starts a fresh
 * list. */
static void flush_present(void)
{
	sdl_dirty_rect list[SDL_DIRTY_MAX];
	int n, i;
	int ow = 0, oh = 0, dx, dy, dw, dh;
	SDL_Rect dst;

	if (!s_tex || !s_fb || !s_stage)
		return; /* keep the rects pending until the buffers exist */
	n = s_dirty_n;
	s_dirty_n = 0;
	if (n <= 0)
		return;
	for (i = 0; i < n; i++)
		list[i] = s_dirty[i];
	for (i = 0; i < n; i++)
		upload_rect(&list[i]);

	/* Integer scale into the drawable, centred with black bars. */
	sdl_video_host_size(&ow, &oh);
	sdl_scale_viewport(ow, oh, s_w, s_h, &dx, &dy, &dw, &dh);

	/* Clear only the letterbox bands the image does not cover, so the bars
	 * stay black without a full-drawable SDL_RenderClear. */
	SDL_SetRenderDrawColor(s_ren, 0, 0, 0, 255);
	present_clear_band(0, 0, ow, dy);
	present_clear_band(0, dy + dh, ow, oh - (dy + dh));
	present_clear_band(0, dy, dx, dh);
	present_clear_band(dx + dw, dy, ow - (dx + dw), dh);

	/* Copy the whole texture, not just the dirty sub-rectangles. SDL's
	 * accelerated renderer flips between back buffers on SDL_RenderPresent,
	 * so any cell a partial copy leaves untouched belongs to an older frame.
	 * Windowed compositors hide that, but fullscreen buffer flipping shows it
	 * as ghosts: a block cursor left on every other character cell. Repainting
	 * the full viewport makes each present self-contained; the dirty-rect
	 * edit above still bounds the CPU conversion and texture upload. */
	dst.x = dx;
	dst.y = dy;
	dst.w = dw;
	dst.h = dh;
	SDL_RenderCopy(s_ren, s_tex, 0, &dst);
	/* #1265: with no blocking vsync the copy reaches scanout as soon as it
	 * runs, so it must start at a frame boundary to avoid tearing. The grid
	 * above is that boundary; this is the same clock that gates the poll,
	 * so no second wait is needed. */
	SDL_RenderPresent(s_ren);
}

void sdl_video_present(void)
{
	if (!sdl_pace_step(&s_pace, pace_now_us()))
		return; /* not at a display frame boundary yet */
	if (s_dirty_n <= 0)
		return;
	flush_present();
}

void sdl_video_present_now(void)
{
	if (s_dirty_n > 0)
		flush_present();
	/* Re-anchor: the next paced present is due immediately rather than
	 * waiting out a phase that belonged to the forced frame. */
	s_pace.period_us = sdl_video_frame_period_us();
	s_pace.next_us = 0;
	s_pace.valid = 0;
}

void sdl_video_request_quit(void)
{
	s_quit = 1;
}

void sdl_video_set_fullscreen(int on)
{
	Uint32 flags;

	if (!s_win)
		return;
	flags = SDL_GetWindowFlags(s_win);
	if (on)
	{
		if (flags & SDL_WINDOW_FULLSCREEN_DESKTOP)
			return;
		/* Pin to the primary display before going borderless-fullscreen. */
		SDL_SetWindowPosition(s_win, SDL_WINDOWPOS_CENTERED_DISPLAY(0),
				      SDL_WINDOWPOS_CENTERED_DISPLAY(0));
		SDL_SetWindowFullscreen(s_win, SDL_WINDOW_FULLSCREEN_DESKTOP);
	}
	else
	{
		if (!(flags & SDL_WINDOW_FULLSCREEN_DESKTOP))
			return;
		SDL_SetWindowFullscreen(s_win, 0);
		/* --double: force the 2x client back (the platform may keep the
		 * last size otherwise). At 1:1 leave SDL's own restore of the
		 * windowed size alone, including a user's manual resize. */
		if (s_window_scale > 1)
			SDL_SetWindowSize(s_win, s_w * s_window_scale,
					  s_h * s_window_scale);
	}
	/* The drawable changed: force a full repaint (and a fresh letterbox), and
	 * re-anchor the pacing grid in case the refresh rate changed. */
	sdl_video_mark_dirty();
	s_frame_period_us = 0;
	s_pace.period_us = sdl_video_frame_period_us();
	s_pace.next_us = 0;
	s_pace.valid = 0; /* the repaint is due immediately */
}

/* Set the windowed client-size multiplier (1 = 1:1, 2 = --double). Applies
 * immediately unless fullscreen owns the drawable; sdl_video_resize() honours
 * it thereafter so a MODE change keeps the scaled client. */
void sdl_video_set_window_scale(int scale)
{
	if (scale < 1)
		scale = 1;
	s_window_scale = scale;
	if (!s_win)
		return;
	if (SDL_GetWindowFlags(s_win) & SDL_WINDOW_FULLSCREEN_DESKTOP)
		return;
	SDL_SetWindowSize(s_win, s_w * scale, s_h * scale);
	sdl_video_mark_dirty();
}

int sdl_video_window_scale(void)
{
	return s_window_scale;
}

void sdl_video_toggle_fullscreen(void)
{
	if (!s_win)
		return;
	sdl_video_set_fullscreen(sdl_video_is_fullscreen() ? 0 : 1);
}

int sdl_video_is_fullscreen(void)
{
	if (!s_win)
		return 0;
	return (SDL_GetWindowFlags(s_win) & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0;
}

const char *sdl_video_window_title(void)
{
	return s_win ? SDL_GetWindowTitle(s_win) : "";
}

int sdl_video_should_quit(void)
{
	return s_quit;
}

int sdl_video_dump_ppm(const char *path)
{
	FILE *f;
	int x, y;

	if (!s_fb || !path)
		return 0;
	f = fopen(path, "wb");
	if (!f)
		return 0;
	fprintf(f, "P6\n%d %d\n255\n", s_w, s_h);
	for (y = 0; y < s_h; y++)
	{
		for (x = 0; x < s_w; x++)
		{
			unsigned rgb = sdl_native_to_rgb(s_fb[y * s_w + x]);
			unsigned char px[3] = {
				(unsigned char)((rgb >> 16) & 255u),
				(unsigned char)((rgb >> 8) & 255u),
				(unsigned char)(rgb & 255u)
			};

			fwrite(px, 1, 3, f);
		}
	}
	fclose(f);
	return 1;
}
