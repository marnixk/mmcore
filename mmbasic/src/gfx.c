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

static int map_y(int y)
{
	if (G.opt.y_axis_up)
		return G.gfx.h - 1 - y;
	return y;
}

static void free_pages(void)
{
	int i;
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

void mmb_gfx_blit(int sx, int sy, int w, int h, int dx, int dy)
{
	uint32_t *pg;
	int xi, yi;
	int xstep, ystep, xend, yend;

	if (w <= 0 || h <= 0)
		return;
	pg = page_buf(G.gfx.write_page);
	if (dy > sy || (dy == sy && dx > sx))
	{
		xstep = 1;
		ystep = 1;
		xend = w;
		yend = h;
	}
	else
	{
		xstep = -1;
		ystep = -1;
		xend = -1;
		yend = -1;
	}
	for (yi = (ystep > 0 ? 0 : h - 1); yi != yend; yi += ystep)
	{
		for (xi = (xstep > 0 ? 0 : w - 1); xi != xend; xi += xstep)
		{
			int ux = sx + xi, uy = sy + yi;
			int vx = dx + xi, vy = dy + yi;
			int by, bv;
			if (ux < 0 || uy < 0 || ux >= G.gfx.w || uy >= G.gfx.h)
				continue;
			if (vx < 0 || vy < 0 || vx >= G.gfx.w || vy >= G.gfx.h)
				continue;
			by = map_y(uy);
			bv = map_y(vy);
			pg[bv * G.gfx.w + vx] = pg[by * G.gfx.w + ux];
			if (G.gfx.write_page == G.gfx.display_page && G.plat && G.plat->set_pixel)
				G.plat->set_pixel(vx, bv, pg[bv * G.gfx.w + vx]);
		}
	}
}

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
}

void mmb_gfx_init(void)
{
	int hw = G.plat && G.plat->hdmi_width ? G.plat->hdmi_width() : 640;
	int hh = G.plat && G.plat->hdmi_height ? G.plat->hdmi_height() : 480;
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
	free_pages();
	G.gfx.mode = mode;
	G.gfx.bits = bits;
	G.gfx.w = w;
	G.gfx.h = h;
	G.gfx.pages = MMB_MAX_PAGES;
	if ((unsigned)w * (unsigned)h > 800u * 600u)
		G.gfx.pages = 2;
	G.gfx.write_page = 0;
	G.gfx.display_page = 0;
	page_buf(0);
	if (G.plat && G.plat->fill_screen)
		G.plat->fill_screen(0);
}

void mmb_gfx_plot(int x, int y, unsigned rgb)
{
	uint32_t *pg;
	int by;
	y = map_y(y);
	if (x < 0 || y < 0 || x >= G.gfx.w || y >= G.gfx.h)
		return;
	rgb = mmb_quantize(rgb);
	pg = page_buf(G.gfx.write_page);
	by = y;
	pg[by * G.gfx.w + x] = rgb;
	if (G.gfx.write_page == G.gfx.display_page && G.plat && G.plat->set_pixel)
		G.plat->set_pixel(x, by, rgb);
}

unsigned mmb_gfx_get(int x, int y)
{
	uint32_t *pg;
	y = map_y(y);
	if (x < 0 || y < 0 || x >= G.gfx.w || y >= G.gfx.h)
		return 0;
	pg = page_buf(G.gfx.write_page);
	return pg[y * G.gfx.w + x];
}

void mmb_gfx_cls(unsigned rgb)
{
	int x, y;
	uint32_t *pg;
	unsigned bytes;
	rgb = mmb_quantize(rgb);
	pg = page_buf(G.gfx.write_page);
	bytes = (unsigned)G.gfx.w * (unsigned)G.gfx.h;
	for (y = 0; y < G.gfx.h; y++)
		for (x = 0; x < G.gfx.w; x++)
			pg[y * G.gfx.w + x] = rgb;
	(void)bytes;
	if (G.gfx.write_page == G.gfx.display_page && G.plat && G.plat->fill_screen)
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

void mmb_gfx_circle(int cx, int cy, int r, unsigned rgb, int lw, int fill)
{
	int yy, ri;
	if (r < 0)
		return;
	if (lw < 1)
		lw = 1;
	if (fill >= 0)
	{
		unsigned fcol = mmb_quantize((unsigned)fill);
		for (yy = -r; yy <= r; yy++)
		{
			int xx, w2 = 0;
			while (w2 * w2 + yy * yy <= r * r)
				w2++;
			w2--;
			for (xx = -w2; xx <= w2; xx++)
				mmb_gfx_plot(cx + xx, cy + yy, fcol);
		}
	}
	for (ri = r; ri > r - lw; ri--)
		circle_outline(cx, cy, ri, rgb);
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
				if (bits & (0x80 >> col))
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
