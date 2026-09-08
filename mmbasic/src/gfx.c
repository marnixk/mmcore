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

unsigned mmb_rgb_pack(int r, int g, int b)
{
	if (r < 0) r = 0; if (r > 255) r = 255;
	if (g < 0) g = 0; if (g > 255) g = 255;
	if (b < 0) b = 0; if (b > 255) b = 255;
	return ((unsigned)r << 16) | ((unsigned)g << 8) | (unsigned)b;
}

void mmb_rgb_unpack(unsigned c, int *r, int *g, int *b)
{
	*r = (int)((c >> 16) & 255);
	*g = (int)((c >> 8) & 255);
	*b = (int)(c & 255);
}

unsigned mmb_quantize(unsigned rgb888)
{
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
	return mmb_rgb_pack(r, g, b);
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
	if (mmb_keyword_eq(n, "BLACK")) return 0x000000;
	if (mmb_keyword_eq(n, "RED")) return 0xFF0000;
	if (mmb_keyword_eq(n, "GREEN")) return 0x00FF00;
	if (mmb_keyword_eq(n, "BLUE")) return 0x0000FF;
	if (mmb_keyword_eq(n, "YELLOW")) return 0xFFFF00;
	if (mmb_keyword_eq(n, "CYAN")) return 0x00FFFF;
	if (mmb_keyword_eq(n, "MAGENTA")) return 0xFF00FF;
	if (mmb_keyword_eq(n, "WHITE")) return 0xFFFFFF;
	if (mmb_keyword_eq(n, "ORANGE")) return 0xFF8000;
	if (mmb_keyword_eq(n, "PINK")) return 0xFF80FF;
	if (mmb_keyword_eq(n, "GOLD")) return 0xFFD700;
	if (mmb_keyword_eq(n, "SALMON")) return 0xFA8072;
	if (mmb_keyword_eq(n, "BROWN")) return 0xA52A2A;
	if (mmb_keyword_eq(n, "GRAY")) return 0x808080;
	if (mmb_keyword_eq(n, "GREY")) return 0x808080;
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
}

static uint32_t *page_buf(int n)
{
	unsigned bytes;
	if (n < 0 || n >= G.gfx.pages)
		mmb_error("?PAGE");
	if (G.gfx.page[n])
		return G.gfx.page[n];
	bytes = (unsigned)G.gfx.w * (unsigned)G.gfx.h * sizeof(uint32_t);
	G.gfx.page[n] = G.plat->alloc(bytes);
	if (!G.gfx.page[n])
		mmb_error("?OUT OF MEMORY");
	memset(G.gfx.page[n], 0, bytes);
	return G.gfx.page[n];
}

uint32_t *mmb_gfx_buf_for(int page, int *w, int *h)
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
	if (p == G.gfx.display_page)
		mmb_gfx_present();
}

void mmb_gfx_copy_page(int src, int dst)
{
	unsigned bytes;
	uint32_t *s, *d;
	if (src < 0 || src >= G.gfx.pages || dst < 0 || dst >= G.gfx.pages)
		mmb_error("?PAGE");
	s = page_buf(src);
	d = page_buf(dst);
	bytes = (unsigned)G.gfx.w * (unsigned)G.gfx.h * sizeof(uint32_t);
	memcpy(d, s, bytes);
}

/* CMM2 BLIT lives in gfx_cmm2.c (source page + orientation). */

void mmb_gfx_present(void)
{
	int x, y, hw, hh;
	uint32_t *pg;
	if (!G.plat || !G.plat->set_pixel)
		return;
	pg = page_buf(G.gfx.display_page);
	hw = G.plat->hdmi_width ? G.plat->hdmi_width() : G.gfx.w;
	hh = G.plat->hdmi_height ? G.plat->hdmi_height() : G.gfx.h;
	if (hw > G.gfx.w)
		hw = G.gfx.w;
	if (hh > G.gfx.h)
		hh = G.gfx.h;
	for (y = 0; y < hh; y++)
		for (x = 0; x < hw; x++)
			G.plat->set_pixel(x, y, pg[y * G.gfx.w + x]);
	mmb_sprite_overlay();
}

void mmb_gfx_init(void)
{
	int hw = G.plat && G.plat->hdmi_width ? G.plat->hdmi_width() : 640;
	int hh = G.plat && G.plat->hdmi_height ? G.plat->hdmi_height() : 480;
	mmb_sprite_reset();
	free_pages();
	memset(&G.gfx, 0, sizeof(G.gfx));
	G.gfx.fg = 0xFFFFFF;
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
	page_buf(0);
	if (G.plat && G.plat->fill_screen)
		G.plat->fill_screen(0);
	mmb_console_apply_colour();
}

void mmb_gfx_plot(int x, int y, unsigned rgb)
{
	uint32_t *pg;
	int tw, th, by;
	tw = tgt_w();
	th = tgt_h();
	y = map_y(y);
	if (x < 0 || y < 0 || x >= tw || y >= th)
		return;
	rgb = mmb_quantize(rgb);
	if (mmb_gfx_writing_fb())
		pg = G.gfx.fb;
	else
		pg = page_buf(G.gfx.write_page);
	by = y;
	pg[by * tw + x] = rgb;
	if (!mmb_gfx_writing_fb() && G.gfx.write_page == G.gfx.display_page &&
	    G.plat && G.plat->set_pixel)
		G.plat->set_pixel(x, by, rgb);
}

unsigned mmb_gfx_get_page(int x, int y, int page)
{
	int w, h, by;
	uint32_t *pg = mmb_gfx_buf_for(page, &w, &h);
	by = mmb_gfx_map_y(y, h);
	if (x < 0 || by < 0 || x >= w || by >= h)
		return 0;
	return pg[by * w + x];
}

unsigned mmb_gfx_get(int x, int y)
{
	return mmb_gfx_get_page(x, y, MMB_PAGE_CUR);
}

void mmb_gfx_cls(unsigned rgb)
{
	int x, y, tw, th;
	uint32_t *pg;
	rgb = mmb_quantize(rgb);
	tw = tgt_w();
	th = tgt_h();
	if (mmb_gfx_writing_fb())
		pg = G.gfx.fb;
	else
		pg = page_buf(G.gfx.write_page);
	for (y = 0; y < th; y++)
		for (x = 0; x < tw; x++)
			pg[y * tw + x] = rgb;
	if (!mmb_gfx_writing_fb() && G.gfx.write_page == G.gfx.display_page &&
	    G.plat && G.plat->fill_screen)
		G.plat->fill_screen(rgb);
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

static const unsigned char kFont8x8[95][8] = {
	{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
	{0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00},
	{0x36,0x36,0x00,0x00,0x00,0x00,0x00,0x00},
	{0x36,0x36,0x7F,0x36,0x7F,0x36,0x36,0x00},
	{0x0C,0x3E,0x03,0x1E,0x30,0x1F,0x0C,0x00},
	{0x00,0x63,0x33,0x18,0x0C,0x66,0x63,0x00},
	{0x1C,0x36,0x1C,0x6E,0x3B,0x33,0x6E,0x00},
	{0x06,0x06,0x03,0x00,0x00,0x00,0x00,0x00},
	{0x18,0x0C,0x06,0x06,0x06,0x0C,0x18,0x00},
	{0x06,0x0C,0x18,0x18,0x18,0x0C,0x06,0x00},
	{0x01,0x63,0x3E,0x1C,0x3E,0x63,0x01,0x00},
	{0x08,0x08,0x3E,0x3E,0x08,0x08,0x00,0x00},
	{0x00,0x00,0x00,0x00,0x00,0x06,0x06,0x03},
	{0x40,0x40,0x40,0x40,0x40,0x40,0x40,0x00},
	{0x00,0x00,0x00,0x00,0x00,0x06,0x06,0x00},
	{0x60,0x30,0x18,0x0C,0x06,0x03,0x01,0x00},
	{0x3E,0x63,0x73,0x7B,0x6F,0x67,0x3E,0x00},
	{0x0C,0x0E,0x0C,0x0C,0x0C,0x0C,0x3F,0x00},
	{0x1E,0x33,0x30,0x1C,0x06,0x33,0x3F,0x00},
	{0x1E,0x33,0x30,0x1C,0x30,0x33,0x1E,0x00},
	{0x38,0x3C,0x36,0x33,0x7F,0x30,0x78,0x00},
	{0x3F,0x03,0x1F,0x30,0x30,0x33,0x1E,0x00},
	{0x1C,0x06,0x03,0x1F,0x33,0x33,0x1E,0x00},
	{0x3F,0x33,0x30,0x18,0x0C,0x0C,0x0C,0x00},
	{0x1E,0x33,0x33,0x1E,0x33,0x33,0x1E,0x00},
	{0x1E,0x33,0x33,0x3E,0x30,0x18,0x0E,0x00},
	{0x00,0x06,0x06,0x00,0x00,0x06,0x06,0x00},
	{0x00,0x06,0x06,0x00,0x00,0x06,0x06,0x03},
	{0x08,0x1C,0x36,0x63,0x00,0x00,0x00,0x00},
	{0x00,0x00,0x7E,0x00,0x00,0x7E,0x00,0x00},
	{0x00,0x00,0x63,0x36,0x1C,0x08,0x00,0x00},
	{0x1E,0x33,0x30,0x18,0x0C,0x00,0x0C,0x00},
	{0x3E,0x63,0x7B,0x7B,0x7B,0x03,0x1E,0x00},
	{0x0C,0x1E,0x33,0x33,0x3F,0x33,0x33,0x00},
	{0x3F,0x66,0x66,0x3E,0x66,0x66,0x3F,0x00},
	{0x3C,0x66,0x03,0x03,0x03,0x66,0x3C,0x00},
	{0x1F,0x36,0x66,0x66,0x66,0x36,0x1F,0x00},
	{0x7F,0x46,0x16,0x1E,0x16,0x46,0x7F,0x00},
	{0x7F,0x46,0x16,0x1E,0x16,0x06,0x0F,0x00},
	{0x3C,0x66,0x03,0x03,0x73,0x66,0x7C,0x00},
	{0x33,0x33,0x33,0x3F,0x33,0x33,0x33,0x00},
	{0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00},
	{0x78,0x30,0x30,0x30,0x33,0x33,0x1E,0x00},
	{0x67,0x66,0x36,0x1E,0x36,0x66,0x67,0x00},
	{0x0F,0x06,0x06,0x06,0x46,0x66,0x7F,0x00},
	{0x63,0x77,0x7F,0x7F,0x6B,0x63,0x63,0x00},
	{0x63,0x67,0x6F,0x7B,0x73,0x63,0x63,0x00},
	{0x1C,0x36,0x63,0x63,0x63,0x36,0x1C,0x00},
	{0x3F,0x66,0x66,0x3E,0x06,0x06,0x0F,0x00},
	{0x1E,0x33,0x33,0x33,0x3B,0x1E,0x38,0x00},
	{0x3F,0x66,0x66,0x3E,0x36,0x66,0x67,0x00},
	{0x1C,0x36,0x06,0x0E,0x18,0x36,0x1C,0x00},
	{0x7E,0x18,0x18,0x18,0x18,0x18,0x18,0x00},
	{0x33,0x33,0x33,0x33,0x33,0x33,0x3F,0x00},
	{0x63,0x63,0x63,0x36,0x1C,0x08,0x00,0x00},
	{0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00},
	{0x63,0x63,0x36,0x1C,0x36,0x63,0x63,0x00},
	{0x66,0x66,0x66,0x3C,0x18,0x18,0x18,0x00},
	{0x7F,0x63,0x31,0x18,0x4C,0x66,0x7F,0x00},
	{0x3C,0x30,0x30,0x30,0x30,0x30,0x3C,0x00},
	{0x03,0x06,0x0C,0x18,0x30,0x60,0x40,0x00},
	{0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0x00},
	{0x08,0x1C,0x36,0x63,0x00,0x00,0x00,0x00},
	{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF},
	{0x0C,0x0C,0x06,0x00,0x00,0x00,0x00,0x00},
	{0x00,0x00,0x1E,0x30,0x3E,0x33,0x6E,0x00},
	{0x00,0x07,0x06,0x06,0x06,0x66,0x3C,0x00},
	{0x00,0x00,0x1C,0x06,0x06,0x06,0x1C,0x00},
	{0x00,0x38,0x30,0x30,0x30,0x33,0x1E,0x00},
	{0x00,0x00,0x1E,0x33,0x3F,0x03,0x1E,0x00},
	{0x00,0x1C,0x36,0x06,0x0F,0x06,0x06,0x00},
	{0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x1F},
	{0x00,0x07,0x06,0x36,0x6E,0x66,0x67,0x00},
	{0x00,0x0C,0x00,0x0E,0x0C,0x0C,0x1E,0x00},
	{0x00,0x30,0x00,0x30,0x30,0x33,0x33,0x1E},
	{0x00,0x07,0x06,0x66,0x36,0x1E,0x67,0x00},
	{0x00,0x0F,0x06,0x06,0x06,0x06,0x3F,0x00},
	{0x00,0x00,0x33,0x7F,0x7F,0x6B,0x63,0x00},
	{0x00,0x00,0x1F,0x33,0x33,0x33,0x33,0x00},
	{0x00,0x00,0x1E,0x33,0x33,0x33,0x1E,0x00},
	{0x00,0x00,0x3B,0x66,0x66,0x3E,0x06,0x0F},
	{0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x78},
	{0x00,0x00,0x3B,0x6E,0x66,0x06,0x0F,0x00},
	{0x00,0x00,0x3E,0x03,0x1E,0x30,0x1F,0x00},
	{0x00,0x08,0x1C,0x3E,0x0C,0x0C,0x3E,0x00},
	{0x00,0x00,0x33,0x33,0x33,0x33,0x6E,0x00},
	{0x00,0x00,0x63,0x63,0x36,0x1C,0x08,0x00},
	{0x00,0x00,0x63,0x6B,0x7F,0x7F,0x36,0x00},
	{0x00,0x00,0x63,0x36,0x1C,0x36,0x63,0x00},
	{0x00,0x00,0x33,0x33,0x33,0x3E,0x30,0x1F},
	{0x00,0x00,0x3F,0x19,0x0C,0x26,0x3F,0x00},
	{0x38,0x0C,0x0C,0x07,0x0C,0x0C,0x38,0x00},
	{0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00},
	{0x07,0x0C,0x0C,0x38,0x0C,0x0C,0x07,0x00},
	{0x6E,0x3B,0x00,0x00,0x00,0x00,0x00,0x00},
};

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
	while (*s)
	{
		unsigned char ch = (unsigned char)*s++;
		int row, col, gi;
		unsigned char bits;
		if (ch == '\n')
		{
			cx = x;
			y += 8 * scale;
			continue;
		}
		if (ch < 32 || ch > 126)
			ch = '?';
		gi = (int)ch - 32;
		for (row = 0; row < 8; row++)
		{
			bits = kFont8x8[gi][row];
			for (col = 0; col < 8; col++)
				if (bits & (1u << col))
				{
					int sx, sy;
					for (sy = 0; sy < scale; sy++)
						for (sx = 0; sx < scale; sx++)
							mmb_gfx_plot(cx + col * scale + sx,
								     y + row * scale + sy, rgb);
				}
		}
		cx += 8 * scale;
	}
}
