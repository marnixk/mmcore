#include "sdl_video.h"
#include "sdl_scale.h"

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static SDL_Window *s_win;
static SDL_Renderer *s_ren;
static SDL_Texture *s_tex;
static uint16_t *s_fb;     /* RGB555 (green bit 6) */
static uint16_t *s_stage;  /* RGB565 for SDL */
static int s_w, s_h;
static int s_quit;
static int s_dirty = 1;

void sdl_video_mark_dirty(void)
{
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

int sdl_video_open(int w, int h)
{
	if (SDL_Init(SDL_INIT_VIDEO) != 0)
		return 0;
	SDL_InitSubSystem(SDL_INIT_AUDIO); /* non-fatal if unavailable */
	SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");

	s_win = SDL_CreateWindow("MMBasic", SDL_WINDOWPOS_CENTERED,
				 SDL_WINDOWPOS_CENTERED, w, h,
				 SDL_WINDOW_RESIZABLE);
	if (!s_win)
		return 0;

	s_ren = SDL_CreateRenderer(s_win, -1,
				   SDL_RENDERER_ACCELERATED |
				   SDL_RENDERER_PRESENTVSYNC);
	if (!s_ren)
		s_ren = SDL_CreateRenderer(s_win, -1, SDL_RENDERER_SOFTWARE);
	if (!s_ren)
		return 0;

	return sdl_video_resize(w, h);
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
	s_dirty = 1;
	if (s_win)
		SDL_SetWindowSize(s_win, w, h);
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

void sdl_video_present(void)
{
	size_t n, i;
	int ow = 0, oh = 0, dx, dy, dw, dh;
	SDL_Rect dst;

	if (!s_dirty)
		return;
	if (!s_tex || !s_fb || !s_stage)
		return;

	n = (size_t)s_w * (size_t)s_h;
	for (i = 0; i < n; i++)
	{
		uint16_t v = s_fb[i];
		unsigned r = (v >> 11) & 0x1Fu;
		unsigned g = (v >> 6) & 0x1Fu;
		unsigned b = v & 0x1Fu;
		unsigned g6 = (g << 1) | (g >> 4);
		s_stage[i] = (uint16_t)((r << 11) | (g6 << 5) | b);
	}

	SDL_UpdateTexture(s_tex, 0, s_stage, s_w * (int)sizeof(uint16_t));

	/* Integer scale into the drawable, centred with black bars. */
	sdl_video_host_size(&ow, &oh);
	sdl_scale_viewport(ow, oh, s_w, s_h, &dx, &dy, &dw, &dh);
	dst.x = dx;
	dst.y = dy;
	dst.w = dw;
	dst.h = dh;

	SDL_SetRenderDrawColor(s_ren, 0, 0, 0, 255);
	SDL_RenderClear(s_ren);
	SDL_RenderCopy(s_ren, s_tex, 0, &dst);
	SDL_RenderPresent(s_ren);
	s_dirty = 0;
}

void sdl_video_request_quit(void)
{
	s_quit = 1;
}

void sdl_video_toggle_fullscreen(void)
{
	Uint32 flags;

	if (!s_win)
		return;
	flags = SDL_GetWindowFlags(s_win);
	if (flags & SDL_WINDOW_FULLSCREEN_DESKTOP)
	{
		SDL_SetWindowFullscreen(s_win, 0);
	}
	else
	{
		/* Pin to the primary display before going borderless-fullscreen. */
		SDL_SetWindowPosition(s_win, SDL_WINDOWPOS_CENTERED_DISPLAY(0),
				      SDL_WINDOWPOS_CENTERED_DISPLAY(0));
		SDL_SetWindowFullscreen(s_win, SDL_WINDOW_FULLSCREEN_DESKTOP);
	}
	/* The drawable changed: force a full repaint (and a fresh letterbox). */
	sdl_video_mark_dirty();
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
