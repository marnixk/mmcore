#include "sdl_scale.h"

void sdl_scale_viewport(int out_w, int out_h, int tex_w, int tex_h,
			int *dx, int *dy, int *dw, int *dh)
{
	int scale;

	if (out_w <= 0 || out_h <= 0 || tex_w <= 0 || tex_h <= 0)
	{
		*dx = *dy = *dw = *dh = 0;
		return;
	}

	scale = out_w / tex_w;
	if (out_h / tex_h < scale)
		scale = out_h / tex_h;

	if (scale >= 1)
	{
		*dw = tex_w * scale;
		*dh = tex_h * scale;
	}
	else if ((long)out_w * tex_h <= (long)out_h * tex_w)
	{
		*dw = out_w;
		*dh = (int)((long)out_w * tex_h / tex_w);
	}
	else
	{
		*dh = out_h;
		*dw = (int)((long)out_h * tex_w / tex_h);
	}

	*dx = (out_w - *dw) / 2;
	*dy = (out_h - *dh) / 2;
}

void sdl_scale_map_rect(int out_w, int out_h, int tex_w, int tex_h,
			int sx0, int sy0, int sx1, int sy1,
			int *dx0, int *dy0, int *dx1, int *dy1)
{
	int vx, vy, vw, vh, ax0, ay0, ax1, ay1;

	*dx0 = *dy0 = *dx1 = *dy1 = 0;
	sdl_scale_viewport(out_w, out_h, tex_w, tex_h, &vx, &vy, &vw, &vh);
	if (vw <= 0 || vh <= 0 || tex_w <= 0 || tex_h <= 0)
		return;
	if (sx0 < 0)
		sx0 = 0;
	if (sy0 < 0)
		sy0 = 0;
	if (sx1 > tex_w)
		sx1 = tex_w;
	if (sy1 > tex_h)
		sy1 = tex_h;
	if (sx0 >= sx1 || sy0 >= sy1)
		return;

	/* Round the far edge up (ceil) so the destination covers every pixel the
	 * source maps onto; an exact integer scale is unaffected. */
	ax0 = vx + (int)((long)sx0 * vw / tex_w);
	ax1 = vx + (int)(((long)sx1 * vw + tex_w - 1) / tex_w);
	ay0 = vy + (int)((long)sy0 * vh / tex_h);
	ay1 = vy + (int)(((long)sy1 * vh + tex_h - 1) / tex_h);

	if (ax0 < vx)
		ax0 = vx;
	if (ay0 < vy)
		ay0 = vy;
	if (ax1 > vx + vw)
		ax1 = vx + vw;
	if (ay1 > vy + vh)
		ay1 = vy + vh;
	if (ax0 >= ax1 || ay0 >= ay1)
		return;
	*dx0 = ax0;
	*dy0 = ay0;
	*dx1 = ax1;
	*dy1 = ay1;
}
