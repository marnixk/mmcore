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
