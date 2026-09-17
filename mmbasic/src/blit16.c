#include "mmb_priv.h"

#if !defined(__aarch64__)
void mmb_blit_copy_u16(uint16_t *dst, const uint16_t *src, unsigned n)
{
	if (n)
		memcpy(dst, src, n * sizeof(uint16_t));
}

void mmb_blit_copy_rect16(uint16_t *dst, int dst_stride,
			 const uint16_t *src, int src_stride,
			 int w, int h)
{
	int y;

	if (w <= 0 || h <= 0)
		return;
	for (y = 0; y < h; y++)
		mmb_blit_copy_u16(dst + y * dst_stride, src + y * src_stride,
				  (unsigned)w);
}
#endif

void mmb_blit_sprite_row16(uint16_t *dst, const uint16_t *src, unsigned n)
{
	unsigned i = 0, run;

	while (i < n)
	{
		if (!src[i])
		{
			i++;
			continue;
		}
		run = i;
		while (i < n && src[i])
			i++;
		mmb_blit_copy_u16(dst + run, src + run, i - run);
	}
}

void mmb_blit_sprite_trans16(uint16_t *dst, int dst_stride,
			     const uint16_t *src, int src_stride,
			     int w, int h)
{
	int y;

	if (w <= 0 || h <= 0)
		return;
	for (y = 0; y < h; y++)
		mmb_blit_sprite_row16(dst + y * dst_stride,
				      src + y * src_stride, (unsigned)w);
}
