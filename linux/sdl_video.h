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

void sdl_video_present(void);
int sdl_video_pump(void);
int sdl_video_should_quit(void);

#endif
