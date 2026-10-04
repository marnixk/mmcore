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
static int s_dirty;
static int s_dirty_x0, s_dirty_y0, s_dirty_x1, s_dirty_y1;

/* Union a changed rectangle into the pending dirty area. A running program
 * changes only a small part of the screen between presents; converting and
 * uploading just that area makes the per-present cost proportional to the
 * change rather than to the panel size (a Chromebook is 1366x768). */
void sdl_video_mark_dirty_rect(int x, int y, int w, int h)
{
	int x1, y1;

	if (w <= 0 || h <= 0)
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
	if (!s_dirty)
	{
		s_dirty_x0 = x;
		s_dirty_y0 = y;
		s_dirty_x1 = x1;
		s_dirty_y1 = y1;
		s_dirty = 1;
		return;
	}
	if (x < s_dirty_x0)
		s_dirty_x0 = x;
	if (y < s_dirty_y0)
		s_dirty_y0 = y;
	if (x1 > s_dirty_x1)
		s_dirty_x1 = x1;
	if (y1 > s_dirty_y1)
		s_dirty_y1 = y1;
}

void sdl_video_dirty_rect(int *x, int *y, int *w, int *h)
{
	if (x)
		*x = s_dirty ? s_dirty_x0 : 0;
	if (y)
		*y = s_dirty ? s_dirty_y0 : 0;
	if (w)
		*w = s_dirty ? s_dirty_x1 - s_dirty_x0 : 0;
	if (h)
		*h = s_dirty ? s_dirty_y1 - s_dirty_y0 : 0;
}

/* Mark the whole screen. Unlike sdl_video_mark_dirty_rect() this replaces any
 * pending rectangle: callers use it after a mode change or a full-screen fill,
 * when an older, larger rectangle may no longer fit the new buffers. */
void sdl_video_mark_dirty(void)
{
	if (s_w <= 0 || s_h <= 0)
	{
		s_dirty = 0;
		return;
	}
	s_dirty_x0 = 0;
	s_dirty_y0 = 0;
	s_dirty_x1 = s_w;
	s_dirty_y1 = s_h;
	s_dirty = 1;
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
	 * scanout stays black; other builds and drivers use SDL's default. */
	s_ren = SDL_CreateRenderer(s_win, sdl_video_render_driver_index(),
				   SDL_RENDERER_ACCELERATED |
				   SDL_RENDERER_PRESENTVSYNC);
	if (!s_ren)
		s_ren = SDL_CreateRenderer(s_win, -1, SDL_RENDERER_SOFTWARE);
	if (!s_ren)
		return 0;

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

void sdl_video_present(void)
{
	int x0, y0, x1, y1, y, x;
	int ow = 0, oh = 0, dx, dy, dw, dh;
	SDL_Rect src, dst;

	if (!s_dirty)
		return;
	if (!s_tex || !s_fb || !s_stage)
		return;

	/* Take the pending dirty rectangle and clear it before the (possibly
	 * slow) upload so a caller that draws again during the present starts a
	 * fresh rectangle. */
	x0 = s_dirty_x0;
	y0 = s_dirty_y0;
	x1 = s_dirty_x1;
	y1 = s_dirty_y1;
	s_dirty = 0;
	if (x0 >= x1 || y0 >= y1)
		return;

	/* Convert RGB555 (green at bit 6) to SDL's RGB565, but only the region
	 * that changed. */
	for (y = y0; y < y1; y++)
	{
		const uint16_t *srow = s_fb + (size_t)y * (size_t)s_w;
		uint16_t *drow = s_stage + (size_t)y * (size_t)s_w;

		for (x = x0; x < x1; x++)
		{
			uint16_t v = srow[x];
			unsigned r = (v >> 11) & 0x1Fu;
			unsigned g = (v >> 6) & 0x1Fu;
			unsigned b = v & 0x1Fu;
			unsigned g6 = (g << 1) | (g >> 4);

			drow[x] = (uint16_t)((r << 11) | (g6 << 5) | b);
		}
	}

	/* Update only that rectangle of the streaming texture. SDL_UpdateTexture
	 * treats a non-NULL rect as a sub-region whose pixels start at the given
	 * pointer, with pitch still spanning a full row. */
	src.x = x0;
	src.y = y0;
	src.w = x1 - x0;
	src.h = y1 - y0;
	SDL_UpdateTexture(s_tex, &src,
			  s_stage + (size_t)y0 * (size_t)s_w + x0,
			  s_w * (int)sizeof(uint16_t));

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

	/* Copy the whole texture, not just the dirty sub-rectangle. SDL's
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
	SDL_RenderPresent(s_ren);
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
	/* The drawable changed: force a full repaint (and a fresh letterbox). */
	sdl_video_mark_dirty();
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
