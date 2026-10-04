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

/* Map a framebuffer sub-rectangle (sx0,sy0)-(sx1,sy1) through the same
 * viewport sdl_scale_viewport() produces, returning the destination
 * sub-rectangle (dx0,dy0)-(dx1,dy1) inside the drawable. The source is
 * clamped to the framebuffer; the destination is rounded outward so a
 * partial present never leaves a one-pixel sliver of an integer scale
 * unmapped, and clamped to the viewport. An empty source or a degenerate
 * viewport yields an empty destination. */
void sdl_scale_map_rect(int out_w, int out_h, int tex_w, int tex_h,
			int sx0, int sy0, int sx1, int sy1,
			int *dx0, int *dy0, int *dx1, int *dy1);

#ifdef __cplusplus
}
#endif

#endif
