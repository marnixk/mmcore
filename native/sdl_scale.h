/*
 * Integer-scale viewport maths for the SDL backend, kept free of SDL headers
 * so the host test can compile and exercise it directly.
 */
#ifndef MMB_SDL_SCALE_H
#define MMB_SDL_SCALE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Fit a tex_w x tex_h framebuffer into an out_w x out_h drawable.
 *
 * Upscaling is by the largest whole integer factor that fits, so pixels stay
 * square and crisp; the leftover area is centred. When the drawable is smaller
 * than the framebuffer the image is aspect-fit (fractional) instead, so it is
 * never cropped. Returns the destination rectangle in *dx, *dy, *dw, *dh.
 */
void sdl_scale_viewport(int out_w, int out_h, int tex_w, int tex_h,
			int *dx, int *dy, int *dw, int *dh);

#ifdef __cplusplus
}
#endif

#endif
