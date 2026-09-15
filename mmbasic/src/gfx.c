#include "mmb_priv.h"

static const struct { int id, w, h; } kModes[] = {
	{ 1, 800, 600 },
	{ 2, 640, 400 },
	{ 3, 320, 200 },
	{ 4, 480, 432 },
	{ 5, 240, 216 },
	{ 6, 256, 240 },
	{ 7, 320, 240 },
	{ 8, 640, 480 },
	{ 9, 1024, 768 },
	{ 10, 848, 480 },
	{ 11, 1280, 720 },
	{ 12, 960, 540 },
	{ 13, 400, 300 },
	{ 14, 960, 540 },
	{ 15, 1280, 1024 },
	{ 16, 1920, 1080 },
	{ 17, 384, 240 },
};

#define MMB_RGB_AFLAG 0x10000000u

/* Software COLOR16 (5-5-5, green at bit 6) when platform hooks are unset. */
static unsigned soft_rgb_to_native(unsigned rgb888)
{
	unsigned r = (rgb888 >> 16) & 255u;
	unsigned g = (rgb888 >> 8) & 255u;
	unsigned b = rgb888 & 255u;
	return ((r >> 3) << 11) | ((g >> 3) << 6) | (b >> 3);
}

static unsigned soft_native_to_rgb(unsigned native)
{
	unsigned r = (native >> 11) & 0x1Fu;
	unsigned g = (native >> 6) & 0x1Fu;
	unsigned b = native & 0x1Fu;
	r = r * 255u / 31u;
	g = g * 255u / 31u;
	b = b * 255u / 31u;
	return (r << 16) | (g << 8) | b;
}

unsigned mmb_rgb_to_native(unsigned rgb888)
{
	if (G.plat && G.plat->rgb_to_native)
		return G.plat->rgb_to_native(rgb888);
	return soft_rgb_to_native(rgb888);
}

unsigned mmb_native_to_rgb(unsigned native)
{
	if (G.plat && G.plat->native_to_rgb)
		return G.plat->native_to_rgb(native);
	return soft_native_to_rgb(native);
}

uint16_t mmb_pix_store(unsigned rgb888, unsigned *alpha_out)
{
	unsigned q = mmb_quantize(rgb888);
	unsigned rgb = q & 0xFFFFFFu;
	unsigned alpha;

	if (q & MMB_RGB_AFLAG)
	{
		alpha = (q >> 24) & 15u;
		if (alpha == 0)
			alpha = 0;
	}
	else if (rgb == 0)
		alpha = 0;
	else
		alpha = 255;
	if (alpha_out)
		*alpha_out = alpha;
	return (uint16_t)mmb_rgb_to_native(rgb);
}

unsigned mmb_pix_load(uint16_t pix, unsigned alpha_byte)
{
	unsigned rgb = mmb_native_to_rgb(pix);
	/* 0..15: CMM2 AFLAG alpha (0 = transparent). 255 = fully opaque. */
	if (alpha_byte <= 15)
		return MMB_RGB_AFLAG | (alpha_byte << 24) | rgb;
	return rgb;
}

unsigned mmb_rgb_pack(int r, int g, int b)
{
	if (r < 0) r = 0; if (r > 255) r = 255;
	if (g < 0) g = 0; if (g > 255) g = 255;
	if (b < 0) b = 0; if (b > 255) b = 255;
	return ((unsigned)r << 16) | ((unsigned)g << 8) | (unsigned)b;
}

unsigned mmb_rgb_pack_a(int r, int g, int b, int a)
{
	if (a < 0)
		a = 0;
	if (a > 15)
		a = 15;
	return MMB_RGB_AFLAG | ((unsigned)a << 24) | mmb_rgb_pack(r, g, b);
}

static unsigned overlay_blend(unsigned base, unsigned over)
{
	unsigned rgb = over & 0xFFFFFFu;
	int a, br, bg, bb, rr, rg, rb;
	if (over & MMB_RGB_AFLAG)
	{
		a = (int)((over >> 24) & 15);
		if (a <= 0)
			return base & 0xFFFFFFu;
		if (a >= 15)
			return rgb;
		mmb_rgb_unpack(base, &br, &bg, &bb);
		mmb_rgb_unpack(over, &rr, &rg, &rb);
		return mmb_rgb_pack((br * (15 - a) + rr * a) / 15,
				    (bg * (15 - a) + rg * a) / 15,
				    (bb * (15 - a) + rb * a) / 15);
	}
	if (rgb == 0)
		return base & 0xFFFFFFu;
	return rgb;
}

void mmb_rgb_unpack(unsigned c, int *r, int *g, int *b)
{
	*r = (int)((c >> 16) & 255);
	*g = (int)((c >> 8) & 255);
	*b = (int)(c & 255);
}

unsigned mmb_quantize(unsigned rgb888)
{
	unsigned hi = rgb888 & 0xFF000000u;
	int r, g, b;
	mmb_rgb_unpack(rgb888, &r, &g, &b);
	switch (G.gfx.bits)
	{
	case 8: /* RGB332 */
		r = (r >> 5) * 255 / 7;
		g = (g >> 5) * 255 / 7;
		b = (b >> 6) * 255 / 3;
		break;
	case 12: /* RGB444 */
		r = (r >> 4) * 255 / 15;
		g = (g >> 4) * 255 / 15;
		b = (b >> 4) * 255 / 15;
		break;
	case 16: /* RGB565 */
		r = (r >> 3) * 255 / 31;
		g = (g >> 2) * 255 / 63;
		b = (b >> 3) * 255 / 31;
		break;
	default:
		break;
	}
	return hi | mmb_rgb_pack(r, g, b);
}

unsigned mmb_ibm_colour(int n)
{
	static const unsigned pal[16] = {
		0x000000u, 0x0000AAu, 0x00AA00u, 0x00AAAAu,
		0xAA0000u, 0xAA00AAu, 0xAA5500u, 0xAAAAAAu,
		0x555555u, 0x5555FFu, 0x55FF55u, 0x55FFFFu,
		0xFF5555u, 0xFF55FFu, 0xFFFF55u, 0xFFFFFFu
	};
	if (n < 0)
		n = 0;
	n &= 31;
	if (n >= 16)
		n -= 16;
	return pal[n];
}

unsigned mmb_colour_from_int(int64_t v)
{
	if (v >= 0 && v <= 31)
		return mmb_ibm_colour((int)v);
	return (unsigned)v;
}

unsigned mmb_named_colour(const char *name, int *ok)
{
	char n[32];
	int i = 0;
	*ok = 1;
	while (name[i] && i < 31)
	{
		n[i] = name[i];
		i++;
	}
	n[i] = 0;
	mmb_upper(n);
	if (mmb_keyword_eq(n, "BLACK")) return mmb_ibm_colour(0);
	if (mmb_keyword_eq(n, "BLUE")) return mmb_ibm_colour(1);
	if (mmb_keyword_eq(n, "GREEN")) return mmb_ibm_colour(2);
	if (mmb_keyword_eq(n, "CYAN")) return mmb_ibm_colour(3);
	if (mmb_keyword_eq(n, "RED")) return mmb_ibm_colour(4);
	if (mmb_keyword_eq(n, "MAGENTA")) return mmb_ibm_colour(5);
	if (mmb_keyword_eq(n, "BROWN")) return mmb_ibm_colour(6);
	if (mmb_keyword_eq(n, "LIGHTGRAY") || mmb_keyword_eq(n, "LIGHTGREY"))
		return mmb_ibm_colour(7);
	if (mmb_keyword_eq(n, "DARKGRAY") || mmb_keyword_eq(n, "DARKGREY") ||
	    mmb_keyword_eq(n, "LIGHTBLACK"))
		return mmb_ibm_colour(8);
	if (mmb_keyword_eq(n, "LIGHTBLUE")) return mmb_ibm_colour(9);
	if (mmb_keyword_eq(n, "LIGHTGREEN")) return mmb_ibm_colour(10);
	if (mmb_keyword_eq(n, "LIGHTCYAN")) return mmb_ibm_colour(11);
	if (mmb_keyword_eq(n, "LIGHTRED")) return mmb_ibm_colour(12);
	if (mmb_keyword_eq(n, "LIGHTMAGENTA")) return mmb_ibm_colour(13);
	if (mmb_keyword_eq(n, "YELLOW") || mmb_keyword_eq(n, "LIGHTYELLOW"))
		return mmb_ibm_colour(14);
	if (mmb_keyword_eq(n, "WHITE") || mmb_keyword_eq(n, "LIGHTWHITE"))
		return mmb_ibm_colour(15);
	if (mmb_keyword_eq(n, "GRAY") || mmb_keyword_eq(n, "GREY")) return 0x808080;
	if (mmb_keyword_eq(n, "ORANGE")) return 0xFF8000;
	if (mmb_keyword_eq(n, "PINK")) return 0xFF80FF;
	if (mmb_keyword_eq(n, "GOLD")) return 0xFFD700;
	if (mmb_keyword_eq(n, "SALMON")) return 0xFA8072;
	*ok = 0;
	return 0;
}

int mmb_gfx_map_y(int y, int h)
{
	if (G.opt.y_axis_up)
		return h - 1 - y;
	return y;
}

int mmb_gfx_writing_fb(void)
{
	return G.gfx.write_fb && G.gfx.fb != 0;
}

static int tgt_w(void)
{
	return mmb_gfx_writing_fb() ? G.gfx.fb_w : G.gfx.w;
}

static int tgt_h(void)
{
	return mmb_gfx_writing_fb() ? G.gfx.fb_h : G.gfx.h;
}

static int map_y(int y)
{
	return mmb_gfx_map_y(y, tgt_h());
}

static void free_soft(void)
{
	int i;
	if (G.gfx.fb)
	{
		G.plat->free(G.gfx.fb);
		G.gfx.fb = 0;
	}
	if (G.gfx.fb_bak)
	{
		G.plat->free(G.gfx.fb_bak);
		G.gfx.fb_bak = 0;
	}
	G.gfx.write_fb = 0;
	G.gfx.fb_w = 0;
	G.gfx.fb_h = 0;
	for (i = 0; i < MMB_MAX_BLIT; i++)
	{
		if (G.gfx.blit[i].pix)
		{
			G.plat->free(G.gfx.blit[i].pix);
			G.gfx.blit[i].pix = 0;
		}
		G.gfx.blit[i].used = 0;
		G.gfx.blit[i].w = 0;
		G.gfx.blit[i].h = 0;
	}
}

static void free_pages(void)
{
	int i;
	free_soft();
	for (i = 0; i < MMB_MAX_PAGES; i++)
	{
		if (G.gfx.page[i])
		{
			G.plat->free(G.gfx.page[i]);
			G.gfx.page[i] = 0;
		}
	}
	if (G.gfx.page1_alpha)
	{
		G.plat->free(G.gfx.page1_alpha);
		G.gfx.page1_alpha = 0;
	}
	if (G.gfx.present_scratch)
	{
		G.plat->free(G.gfx.present_scratch);
		G.gfx.present_scratch = 0;
	}
}

void ensure_page1_alpha(void)
{
	unsigned bytes;
	if (G.gfx.page1_alpha)
		return;
	bytes = (unsigned)G.gfx.w * (unsigned)G.gfx.h;
	if (!G.plat || !G.plat->alloc)
		return;
	G.gfx.page1_alpha = G.plat->alloc(bytes);
	if (G.gfx.page1_alpha)
		memset(G.gfx.page1_alpha, 0, bytes);
}

static uint16_t *page_buf(int n)
{
	unsigned bytes;
	if (n < 0 || n >= G.gfx.pages)
		mmb_error("?PAGE");
	if (G.gfx.page[n])
		return G.gfx.page[n];
	bytes = (unsigned)G.gfx.w * (unsigned)G.gfx.h * sizeof(uint16_t);
	G.gfx.page[n] = G.plat->alloc(bytes);
	if (!G.gfx.page[n])
		mmb_error("?OUT OF MEMORY");
	memset(G.gfx.page[n], 0, bytes);
	if (n == 1)
		ensure_page1_alpha();
	return G.gfx.page[n];
}

uint16_t *mmb_gfx_buf_for(int page, int *w, int *h)
{
	if (page == MMB_PAGE_CUR)
	{
		if (mmb_gfx_writing_fb())
		{
			*w = G.gfx.fb_w;
			*h = G.gfx.fb_h;
			return G.gfx.fb;
		}
		page = G.gfx.write_page;
	}
	if (page == MMB_PAGE_FB)
	{
		if (!G.gfx.fb)
			mmb_error("?FRAMEBUFFER");
		*w = G.gfx.fb_w;
		*h = G.gfx.fb_h;
		return G.gfx.fb;
	}
	if (page < 0 || page >= G.gfx.pages)
		mmb_error("?PAGE");
	*w = G.gfx.w;
	*h = G.gfx.h;
	return page_buf(page);
}

void mmb_gfx_dirty_reset(void)
{
	G.gfx.dirty = 0;
}

void mmb_gfx_dirty_add(int x, int y, int w, int h)
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
	if (x1 > G.gfx.w)
		x1 = G.gfx.w;
	if (y1 > G.gfx.h)
		y1 = G.gfx.h;
	if (x >= x1 || y >= y1)
		return;
	if (!G.gfx.dirty)
	{
		G.gfx.dirty_x0 = x;
		G.gfx.dirty_y0 = y;
		G.gfx.dirty_x1 = x1;
		G.gfx.dirty_y1 = y1;
		G.gfx.dirty = 1;
		return;
	}
	if (x < G.gfx.dirty_x0)
		G.gfx.dirty_x0 = x;
	if (y < G.gfx.dirty_y0)
		G.gfx.dirty_y0 = y;
	if (x1 > G.gfx.dirty_x1)
		G.gfx.dirty_x1 = x1;
	if (y1 > G.gfx.dirty_y1)
		G.gfx.dirty_y1 = y1;
}

void mmb_gfx_dirty_flush(void)
{
	if (G.gfx.dirty)
		mmb_gfx_present();
}

static void present_wait_dma(void)
{
	if (G.plat && G.plat->present_wait)
		G.plat->present_wait();
}

void mmb_gfx_present_if(int page)
{
	int p = page;
	if (p == MMB_PAGE_FB)
		return;
	if (p == MMB_PAGE_CUR)
	{
		if (mmb_gfx_writing_fb())
			return;
		p = G.gfx.write_page;
	}
	if (p == G.gfx.display_page || p == 1)
	{
		/* Full frame if nothing was tracked; else flush dirty AABB. */
		if (!G.gfx.dirty)
			mmb_gfx_dirty_add(0, 0, G.gfx.w, G.gfx.h);
		mmb_gfx_present();
	}
}

void mmb_gfx_copy_page(int src, int dst, int blit)
{
	unsigned n, i, bytes;
	uint16_t *s, *d;
	if (src < 0 || src >= G.gfx.pages || dst < 0 || dst >= G.gfx.pages)
		mmb_error("?PAGE");
	s = page_buf(src);
	d = page_buf(dst);
	n = (unsigned)G.gfx.w * (unsigned)G.gfx.h;
	bytes = n * sizeof(uint16_t);
	if (!blit)
	{
		/* Opaque full-page copy: DMA when large enough and available. */
		if (bytes >= 4096u && G.plat && G.plat->dma_copy &&
		    G.plat->dma_copy(d, s, bytes))
			;
		else
			memcpy(d, s, bytes);
		if (dst == 1 && src != 1)
		{
			ensure_page1_alpha();
			if (G.gfx.page1_alpha)
			{
				for (i = 0; i < n; i++)
					G.gfx.page1_alpha[i] = s[i] ? 255 : 0;
			}
		}
		if (dst == G.gfx.display_page || dst == 1)
			mmb_gfx_dirty_add(0, 0, G.gfx.w, G.gfx.h);
		return;
	}
	for (i = 0; i < n; i++)
	{
		if (s[i] != 0)
		{
			d[i] = s[i];
			if (dst == 1)
			{
				ensure_page1_alpha();
				if (G.gfx.page1_alpha)
				{
					if (src == 1)
						; /* same alpha plane */
					else
						G.gfx.page1_alpha[i] = 255;
				}
			}
		}
	}
	if (dst == G.gfx.display_page || dst == 1)
		mmb_gfx_dirty_add(0, 0, G.gfx.w, G.gfx.h);
}

static uint16_t *composite_display(void)
{
	uint16_t *base, *over, *out;
	uint8_t *oa;
	unsigned n, i, bytes;
	base = page_buf(G.gfx.display_page);
	if (G.gfx.display_page == 1 || !G.gfx.page[1])
		return base;
	over = G.gfx.page[1];
	oa = G.gfx.page1_alpha;
	n = (unsigned)G.gfx.w * (unsigned)G.gfx.h;
	bytes = n * sizeof(uint16_t);
	if (!G.gfx.present_scratch)
	{
		if (!G.plat || !G.plat->alloc)
			return base;
		G.gfx.present_scratch = G.plat->alloc(bytes);
		if (!G.gfx.present_scratch)
			return base;
	}
	out = G.gfx.present_scratch;
	for (i = 0; i < n; i++)
	{
		unsigned base_rgb = mmb_native_to_rgb(base[i]);
		unsigned over_a = oa ? oa[i] : (over[i] ? 255u : 0u);
		unsigned over_rgb = mmb_pix_load(over[i], over_a);
		unsigned blended = overlay_blend(base_rgb, over_rgb);
		out[i] = (uint16_t)mmb_rgb_to_native(blended);
	}
	return out;
}

static void present_native_or_rgb(int x, int y, int w, int h,
				  const uint16_t *pix, int stride)
{
	if (G.plat->present_native)
	{
		G.plat->present_native(x, y, w, h, pix, stride);
		return;
	}
	if (G.plat->present_rgb)
	{
		unsigned *tmp;
		int i, j;
		unsigned n = (unsigned)w * (unsigned)h;
		tmp = G.plat->alloc(n * sizeof(unsigned));
		if (!tmp)
			goto pixel_fallback;
		for (j = 0; j < h; j++)
			for (i = 0; i < w; i++)
				tmp[j * w + i] = mmb_native_to_rgb(pix[j * stride + i]);
		G.plat->present_rgb(x, y, w, h, tmp, w);
		G.plat->free(tmp);
		return;
	}
pixel_fallback:
	if (!G.plat->set_pixel)
		return;
	{
		int px, py;
		for (py = 0; py < h; py++)
			for (px = 0; px < w; px++)
				G.plat->set_pixel(x + px, y + py,
						  mmb_native_to_rgb(pix[py * stride + px]));
	}
}

/* CMM2 BLIT lives in gfx_cmm2.c (source page + orientation). */

void mmb_gfx_present(void)
{
	int hw, hh;
	uint16_t *pg;
	if (G.opt.profiling && G.running)
		G.prof.gfx_present++;
	if (!G.plat)
		return;
	present_wait_dma();
	pg = composite_display();
	hw = G.plat->hdmi_width ? G.plat->hdmi_width() : G.gfx.w;
	hh = G.plat->hdmi_height ? G.plat->hdmi_height() : G.gfx.h;
	if (hw > G.gfx.w)
		hw = G.gfx.w;
	if (hh > G.gfx.h)
		hh = G.gfx.h;
	if (G.gfx.dirty)
	{
		int x = G.gfx.dirty_x0;
		int y = G.gfx.dirty_y0;
		int x1 = G.gfx.dirty_x1;
		int y1 = G.gfx.dirty_y1;
		G.gfx.dirty = 0;
		if (x1 > hw)
			x1 = hw;
		if (y1 > hh)
			y1 = hh;
		if (x < x1 && y < y1)
			present_native_or_rgb(x, y, x1 - x, y1 - y,
					     pg + y * G.gfx.w + x, G.gfx.w);
		mmb_sprite_overlay();
		return;
	}
	present_native_or_rgb(0, 0, hw, hh, pg, G.gfx.w);
	mmb_sprite_overlay();
}

void mmb_gfx_present_rect(int x, int y, int w, int h)
{
	int hw, hh, x1, y1;
	uint16_t *pg;
	if (!G.plat)
		return;
	present_wait_dma();
	pg = composite_display();
	hw = G.plat->hdmi_width ? G.plat->hdmi_width() : G.gfx.w;
	hh = G.plat->hdmi_height ? G.plat->hdmi_height() : G.gfx.h;
	if (hw > G.gfx.w)
		hw = G.gfx.w;
	if (hh > G.gfx.h)
		hh = G.gfx.h;
	if (x < 0)
	{
		w += x;
		x = 0;
	}
	if (y < 0)
	{
		h += y;
		y = 0;
	}
	if (w < 0)
		w = 0;
	if (h < 0)
		h = 0;
	x1 = x + w;
	y1 = y + h;
	if (x1 > hw)
		x1 = hw;
	if (y1 > hh)
		y1 = hh;
	if (x >= x1 || y >= y1)
		return;
	present_native_or_rgb(x, y, x1 - x, y1 - y, pg + y * G.gfx.w + x, G.gfx.w);
}

void mmb_gfx_init(void)
{
	int hw = G.plat && G.plat->hdmi_width ? G.plat->hdmi_width() : 640;
	int hh = G.plat && G.plat->hdmi_height ? G.plat->hdmi_height() : 480;
	mmb_sprite_reset();
	free_pages();
	memset(&G.gfx, 0, sizeof(G.gfx));
	G.gfx.fg = 0x808080u;
	G.gfx.bg = 0;
	G.gfx.font = 1;
	G.gfx.font_scale = 1;
	G.gfx.write_page = 0;
	G.gfx.display_page = 0;
	if (hw == 640 && hh == 480)
		mmb_gfx_set_mode(8, 16);
	else
		mmb_gfx_set_mode(1, 8);
}

void mmb_gfx_apply_default_mode(void)
{
	int mode = G.opt.default_mode;
	int bits = G.gfx.bits ? G.gfx.bits : 8;
	unsigned i;
	int ok = 0;

	if (mode <= 0)
		return;
	for (i = 0; i < sizeof(kModes) / sizeof(kModes[0]); i++)
		if (kModes[i].id == mode)
			ok = 1;
	if (!ok)
		return;
	if ((mode == 9 || mode == 11 || mode == 12 || mode == 14) && bits == 12)
		bits = 8;
	if (G.gfx.mode == mode && G.gfx.bits == bits)
		return;
	mmb_gfx_set_mode(mode, bits);
}

void mmb_gfx_set_mode(int mode, int bits)
{
	unsigned i;
	int w = 0, h = 0;
	if (bits != 8 && bits != 12 && bits != 16 && bits != 32)
		mmb_error("?INVALID MODE");
	if ((mode == 9 || mode == 11 || mode == 12 || mode == 14) && bits == 12)
		mmb_error("?INVALID MODE");
	for (i = 0; i < sizeof(kModes) / sizeof(kModes[0]); i++)
		if (kModes[i].id == mode)
		{
			w = kModes[i].w;
			h = kModes[i].h;
			break;
		}
	if (!w)
		mmb_error("?INVALID MODE");
	if (G.plat && G.plat->resize_hdmi)
		G.plat->resize_hdmi(w, h);
	free_pages();
	G.gfx.mode = mode;
	G.gfx.bits = bits;
	G.gfx.w = w;
	G.gfx.h = h;
	G.gfx.pages = MMB_MAX_PAGES;
	G.gfx.write_page = 0;
	G.gfx.display_page = 0;
	mmb_gfx_dirty_reset();
	page_buf(0);
	if (G.plat && G.plat->fill_screen)
		G.plat->fill_screen(0);
	mmb_console_apply_colour();
}

void mmb_gfx_plot(int x, int y, unsigned rgb)
{
	uint16_t *pg;
	int tw, th, by;
	unsigned alpha;
	uint16_t np;
	tw = tgt_w();
	th = tgt_h();
	y = map_y(y);
	if (x < 0 || y < 0 || x >= tw || y >= th)
		return;
	np = mmb_pix_store(rgb, &alpha);
	if (mmb_gfx_writing_fb())
		pg = G.gfx.fb;
	else
		pg = page_buf(G.gfx.write_page);
	by = y;
	pg[by * tw + x] = np;
	if (!mmb_gfx_writing_fb() && G.gfx.write_page == 1)
	{
		ensure_page1_alpha();
		if (G.gfx.page1_alpha)
			G.gfx.page1_alpha[by * tw + x] = (uint8_t)alpha;
	}
	if (mmb_gfx_writing_fb())
		return;
	/* Expand dirty AABB when drawing to the visible page or page-1 overlay. */
	if (G.gfx.write_page == G.gfx.display_page || G.gfx.write_page == 1)
		mmb_gfx_dirty_add(x, by, 1, 1);
	rgb = mmb_pix_load(np, alpha);
	if (G.gfx.write_page == G.gfx.display_page && G.plat && G.plat->set_pixel)
	{
		unsigned shown = rgb;
		if (G.gfx.display_page != 1 && G.gfx.page[1])
		{
			unsigned oa = G.gfx.page1_alpha
					     ? G.gfx.page1_alpha[by * tw + x]
					     : (G.gfx.page[1][by * tw + x] ? 255u : 0u);
			shown = overlay_blend(rgb, mmb_pix_load(G.gfx.page[1][by * tw + x], oa));
		}
		G.plat->set_pixel(x, by, shown & 0xFFFFFFu);
	}
	else if (G.gfx.write_page == 1 && G.gfx.display_page != 1 &&
		 G.plat && G.plat->set_pixel)
	{
		uint16_t *base = page_buf(G.gfx.display_page);
		G.plat->set_pixel(x, by,
				  overlay_blend(mmb_native_to_rgb(base[by * tw + x]), rgb) &
					  0xFFFFFFu);
	}
}

unsigned mmb_gfx_get_page(int x, int y, int page)
{
	int w, h, by, resolved;
	uint16_t *pg;
	unsigned alpha = 255;

	resolved = page;
	if (resolved == MMB_PAGE_CUR)
	{
		if (mmb_gfx_writing_fb())
			resolved = MMB_PAGE_FB;
		else
			resolved = G.gfx.write_page;
	}
	pg = mmb_gfx_buf_for(page, &w, &h);
	by = mmb_gfx_map_y(y, h);
	if (x < 0 || by < 0 || x >= w || by >= h)
		return 0;
	if (resolved == 1 && G.gfx.page1_alpha)
		alpha = G.gfx.page1_alpha[by * w + x];
	return mmb_pix_load(pg[by * w + x], alpha);
}

unsigned mmb_gfx_get(int x, int y)
{
	return mmb_gfx_get_page(x, y, MMB_PAGE_CUR);
}

/* AArch64 has no REP STOSD; STP of duplicated halfwords fills eight pixels. */
static void fill_u16(uint16_t *dst, unsigned n, uint16_t v)
{
	uint16_t *end;

	if (!n)
		return;
	end = dst + n;
#if defined(__aarch64__)
	{
		uint16_t *blk;
		uint64_t quad;
		uint32_t pair;

		if (((uintptr_t)dst & 1u) && dst < end)
			*dst++ = v;
		if (((uintptr_t)dst & 2u) && dst < end)
			*dst++ = v;
		blk = dst + ((unsigned)(end - dst) & ~3u);
		if (dst < blk)
		{
			pair = ((uint32_t)v << 16) | (uint32_t)v;
			quad = ((uint64_t)pair << 32) | (uint64_t)pair;
			__asm__ volatile(
				"1:\n\t"
				"stp %[quad], %[quad], [%[p]], #16\n\t"
				"cmp %[p], %[blk]\n\t"
				"b.lo 1b\n"
				: [p] "+r"(dst)
				: [blk] "r"(blk), [quad] "r"(quad)
				: "memory", "cc");
		}
	}
#endif
	while (dst < end)
		*dst++ = v;
}

void mmb_gfx_cls(unsigned rgb)
{
	int tw, th;
	uint16_t *pg;
	unsigned alpha;
	uint16_t np;
	np = mmb_pix_store(rgb, &alpha);
	tw = tgt_w();
	th = tgt_h();
	if (mmb_gfx_writing_fb())
		pg = G.gfx.fb;
	else
		pg = page_buf(G.gfx.write_page);
	fill_u16(pg, (unsigned)tw * (unsigned)th, np);
	if (!mmb_gfx_writing_fb() && G.gfx.write_page == 1)
	{
		ensure_page1_alpha();
		if (G.gfx.page1_alpha)
			memset(G.gfx.page1_alpha, (int)alpha, (unsigned)tw * (unsigned)th);
	}
	if (mmb_gfx_writing_fb())
		return;
	if (G.gfx.write_page == G.gfx.display_page &&
	    G.plat && G.plat->fill_screen)
	{
		if (G.gfx.display_page != 1 && G.gfx.page[1])
		{
			/* Overlay: full present (clear dirty so we do not clip). */
			mmb_gfx_dirty_reset();
			mmb_gfx_present();
		}
		else
		{
			G.plat->fill_screen(mmb_native_to_rgb(np));
			mmb_gfx_dirty_reset();
		}
	}
	else if (G.gfx.write_page == 1)
	{
		mmb_gfx_dirty_reset();
		mmb_gfx_present();
	}
	else if (G.gfx.write_page == G.gfx.display_page)
	{
		mmb_gfx_dirty_add(0, 0, tw, th);
		mmb_gfx_present();
	}
}

void mmb_gfx_line(int x0, int y0, int x1, int y1, unsigned rgb, int lw)
{
	int dx, dy, sx, sy, err, i;
	int adx, ady, horiz;
	if (lw < 1)
		lw = 1;
	dx = x1 - x0;
	dy = y1 - y0;
	adx = dx; if (adx < 0) adx = -adx;
	ady = dy; if (ady < 0) ady = -ady;
	horiz = adx >= ady;
	sx = x0 < x1 ? 1 : -1;
	sy = y0 < y1 ? 1 : -1;
	dx = adx;
	dy = ady;
	err = dx - dy;
	for (;;)
	{
		if (lw <= 1)
			mmb_gfx_plot(x0, y0, rgb);
		else
			for (i = -(lw / 2); i <= lw / 2; i++)
				if (horiz)
					mmb_gfx_plot(x0, y0 + i, rgb);
				else
					mmb_gfx_plot(x0 + i, y0, rgb);
		if (x0 == x1 && y0 == y1)
			break;
		{
			int e2 = 2 * err;
			if (e2 > -dy) { err -= dy; x0 += sx; }
			if (e2 < dx) { err += dx; y0 += sy; }
		}
	}
}

void mmb_gfx_box(int x, int y, int w, int h, unsigned rgb, int lw, int fill)
{
	int i, j;
	unsigned fcol;
	if (w < 0) { x += w; w = -w; }
	if (h < 0) { y += h; h = -h; }
	if (fill >= 0)
	{
		fcol = mmb_quantize((unsigned)fill);
		for (j = 0; j < h; j++)
			for (i = 0; i < w; i++)
				mmb_gfx_plot(x + i, y + j, fcol);
	}
	if (lw < 1)
		lw = 1;
	for (i = 0; i < lw; i++)
	{
		mmb_gfx_line(x + i, y + i, x + w - 1 - i, y + i, rgb, 1);
		mmb_gfx_line(x + i, y + h - 1 - i, x + w - 1 - i, y + h - 1 - i, rgb, 1);
		mmb_gfx_line(x + i, y + i, x + i, y + h - 1 - i, rgb, 1);
		mmb_gfx_line(x + w - 1 - i, y + i, x + w - 1 - i, y + h - 1 - i, rgb, 1);
	}
}

static void hspan(int x0, int x1, int y, unsigned rgb)
{
	int x;
	if (x0 > x1)
	{
		int t = x0;
		x0 = x1;
		x1 = t;
	}
	for (x = x0; x <= x1; x++)
		mmb_gfx_plot(x, y, rgb);
}

static void circle_outline(int cx, int cy, int r, unsigned rgb)
{
	int x = r, y = 0, err = 0;
	if (r < 0)
		return;
	while (x >= y)
	{
		mmb_gfx_plot(cx + x, cy + y, rgb);
		mmb_gfx_plot(cx + y, cy + x, rgb);
		mmb_gfx_plot(cx - y, cy + x, rgb);
		mmb_gfx_plot(cx - x, cy + y, rgb);
		mmb_gfx_plot(cx - x, cy - y, rgb);
		mmb_gfx_plot(cx - y, cy - x, rgb);
		mmb_gfx_plot(cx + y, cy - x, rgb);
		mmb_gfx_plot(cx + x, cy - y, rgb);
		y++;
		if (err <= 0)
			err += 2 * y + 1;
		if (err > 0)
		{
			x--;
			err -= 2 * x + 1;
		}
	}
}

/* Midpoint disk: eight-way symmetry, one horizontal span per pair. */
static void fill_disk(int cx, int cy, int r, unsigned rgb)
{
	int x = 0, y = r;
	int f = 1 - r;
	int ddx = 1;
	int ddy = -2 * r;
	if (r < 0)
		return;
	hspan(cx - r, cx + r, cy, rgb);
	while (x < y)
	{
		if (f >= 0)
		{
			y--;
			ddy += 2;
			f += ddy;
		}
		x++;
		ddx += 2;
		f += ddx;
		hspan(cx - x, cx + x, cy + y, rgb);
		hspan(cx - x, cx + x, cy - y, rgb);
		hspan(cx - y, cx + y, cy + x, rgb);
		hspan(cx - y, cx + y, cy - x, rgb);
	}
}

/* Annulus r_in < radius <= r_out using incremental integer extents. */
static void fill_ring(int cx, int cy, int r_out, int r_in, unsigned rgb)
{
	int y, xo = 0, xi = 0;
	if (r_out < 0)
		return;
	if (r_in < 0)
	{
		fill_disk(cx, cy, r_out, rgb);
		return;
	}
	for (y = r_out; y >= 0; y--)
	{
		int64_t y2 = (int64_t)y * y;
		int64_t ro2 = (int64_t)r_out * r_out;
		int64_t ri2 = (int64_t)r_in * r_in;
		while ((int64_t)(xo + 1) * (xo + 1) + y2 <= ro2)
			xo++;
		while (xo > 0 && (int64_t)xo * xo + y2 > ro2)
			xo--;
		if (y > r_in)
			xi = -1;
		else
		{
			while ((int64_t)(xi + 1) * (xi + 1) + y2 <= ri2)
				xi++;
			while (xi > 0 && (int64_t)xi * xi + y2 > ri2)
				xi--;
		}
		if (xi < 0)
		{
			hspan(cx - xo, cx + xo, cy + y, rgb);
			if (y)
				hspan(cx - xo, cx + xo, cy - y, rgb);
		}
		else
		{
			if (cx - xo <= cx - xi - 1)
				hspan(cx - xo, cx - xi - 1, cy + y, rgb);
			if (cx + xi + 1 <= cx + xo)
				hspan(cx + xi + 1, cx + xo, cy + y, rgb);
			if (y)
			{
				if (cx - xo <= cx - xi - 1)
					hspan(cx - xo, cx - xi - 1, cy - y, rgb);
				if (cx + xi + 1 <= cx + xo)
					hspan(cx + xi + 1, cx + xo, cy - y, rgb);
			}
		}
	}
}

void mmb_gfx_circle(int cx, int cy, int r, unsigned rgb, int lw, int fill)
{
	if (r < 0)
		return;
	if (lw < 1)
		lw = 1;
	if (fill >= 0 && lw > 1)
	{
		int inner = r - lw;
		fill_disk(cx, cy, r, rgb);
		if (inner >= 0)
			fill_disk(cx, cy, inner, mmb_quantize((unsigned)fill));
		return;
	}
	if (fill >= 0)
		fill_disk(cx, cy, r, mmb_quantize((unsigned)fill));
	if (lw <= 1)
		circle_outline(cx, cy, r, rgb);
	else
		fill_ring(cx, cy, r, r - lw, rgb);
}

void mmb_gfx_rbox(int x, int y, int w, int h, int r, unsigned rgb, int lw, int fill)
{
	if (r < 0)
		r = 0;
	if (fill >= 0)
	{
		mmb_gfx_box(x + r, y, w - 2 * r, h, rgb, 1, fill);
		mmb_gfx_box(x, y + r, w, h - 2 * r, rgb, 1, fill);
		mmb_gfx_circle(x + r, y + r, r, rgb, 1, fill);
		mmb_gfx_circle(x + w - 1 - r, y + r, r, rgb, 1, fill);
		mmb_gfx_circle(x + r, y + h - 1 - r, r, rgb, 1, fill);
		mmb_gfx_circle(x + w - 1 - r, y + h - 1 - r, r, rgb, 1, fill);
		return;
	}
	mmb_gfx_line(x + r, y, x + w - 1 - r, y, rgb, lw);
	mmb_gfx_line(x + r, y + h - 1, x + w - 1 - r, y + h - 1, rgb, lw);
	mmb_gfx_line(x, y + r, x, y + h - 1 - r, rgb, lw);
	mmb_gfx_line(x + w - 1, y + r, x + w - 1, y + h - 1 - r, rgb, lw);
	mmb_gfx_circle(x + r, y + r, r, rgb, lw, -1);
	mmb_gfx_circle(x + w - 1 - r, y + r, r, rgb, lw, -1);
	mmb_gfx_circle(x + r, y + h - 1 - r, r, rgb, lw, -1);
	mmb_gfx_circle(x + w - 1 - r, y + h - 1 - r, r, rgb, lw, -1);
}

void mmb_gfx_triangle(int x1, int y1, int x2, int y2, int x3, int y3, unsigned rgb, int fill)
{
	mmb_gfx_line(x1, y1, x2, y2, rgb, 1);
	mmb_gfx_line(x2, y2, x3, y3, rgb, 1);
	mmb_gfx_line(x3, y3, x1, y1, rgb, 1);
	if (fill >= 0)
	{
		int minx = x1, maxx = x1, miny = y1, maxy = y1, x, y;
		unsigned fcol = mmb_quantize((unsigned)fill);
		if (x2 < minx) minx = x2; if (x3 < minx) minx = x3;
		if (x2 > maxx) maxx = x2; if (x3 > maxx) maxx = x3;
		if (y2 < miny) miny = y2; if (y3 < miny) miny = y3;
		if (y2 > maxy) maxy = y2; if (y3 > maxy) maxy = y3;
		for (y = miny; y <= maxy; y++)
			for (x = minx; x <= maxx; x++)
			{
				int d1 = (x - x2) * (y1 - y2) - (x1 - x2) * (y - y2);
				int d2 = (x - x3) * (y2 - y3) - (x2 - x3) * (y - y3);
				int d3 = (x - x1) * (y3 - y1) - (x3 - x1) * (y - y1);
				int has_neg = (d1 < 0) || (d2 < 0) || (d3 < 0);
				int has_pos = (d1 > 0) || (d2 > 0) || (d3 > 0);
				if (!(has_neg && has_pos))
					mmb_gfx_plot(x, y, fcol);
			}
	}
}

void mmb_gfx_glyph_cp437(int x, int y, unsigned ch, unsigned rgb)
{
	unsigned row, col;

	ch &= 0xFFu;
	for (row = 0; row < 16; row++)
	{
		unsigned char bits = mmb_cp437_8x16[ch * 16 + row];
		for (col = 0; col < 8; col++)
		{
			if (bits & (unsigned char)(0x80u >> col))
				mmb_gfx_plot(x + (int)col, y + (int)row, rgb);
		}
	}
}

void mmb_gfx_text(int x, int y, const char *s, unsigned rgb)
{
	int cx = x, scale = G.gfx.font_scale;
	if (scale < 1)
		scale = 1;
	if (!s)
		return;
	while (*s)
	{
		unsigned char ch = (unsigned char)*s++;
		unsigned row, col;
		if (ch == '\n')
		{
			cx = x;
			y += 16 * scale;
			continue;
		}
		if (ch == '\r')
			continue;
		for (row = 0; row < 16; row++)
		{
			unsigned char bits = mmb_cp437_8x16[ch * 16u + row];
			for (col = 0; col < 8; col++)
			{
				if (bits & (unsigned char)(0x80u >> col))
				{
					int sx, sy;
					for (sy = 0; sy < scale; sy++)
						for (sx = 0; sx < scale; sx++)
							mmb_gfx_plot(cx + (int)col * scale + sx,
								     y + (int)row * scale + sy, rgb);
				}
			}
		}
		cx += 8 * scale;
	}
}
