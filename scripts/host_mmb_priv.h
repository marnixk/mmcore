#ifndef HOST_MMB_PRIV_H
#define HOST_MMB_PRIV_H

#include <stddef.h>
#include <string.h>

typedef struct {
	void *(*alloc)(unsigned n);
	void (*free)(void *p);
} mmb_plat_t;

typedef struct {
	mmb_plat_t *plat;
} mmb_globals_t;

extern mmb_globals_t G;

unsigned mmb_rgb_pack(int r, int g, int b);
void mmb_gfx_plot(int x, int y, unsigned rgb);

#endif
