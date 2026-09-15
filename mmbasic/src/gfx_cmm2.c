#include "mmb_priv.h"
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static unsigned lerp_rgb(unsigned a, unsigned b, double t)
{
	int ar, ag, ab, br, bg, bb;
	if (t <= 0)
		return a;
	if (t >= 1)
		return b;
	mmb_rgb_unpack(a, &ar, &ag, &ab);
	mmb_rgb_unpack(b, &br, &bg, &bb);
	return mmb_rgb_pack((int)(ar + (br - ar) * t + 0.5),
			    (int)(ag + (bg - ag) * t + 0.5),
			    (int)(ab + (bb - ab) * t + 0.5));
}

static uint32_t *snap_rect(int page, int x, int y, int w, int h)
{
	int i, j;
	uint32_t *tmp;
	unsigned bytes;
	if (w <= 0 || h <= 0)
		mmb_error("?SYNTAX ERROR");
	bytes = (unsigned)w * (unsigned)h * sizeof(uint32_t);
	tmp = G.plat->alloc(bytes);
	if (!tmp)
		mmb_error("?OUT OF MEMORY");
	for (j = 0; j < h; j++)
		for (i = 0; i < w; i++)
			tmp[j * w + i] = mmb_gfx_get_page(x + i, y + j, page);
	return tmp;
}

static void put_dest(int x, int y, unsigned rgb, int skip_black)
{
	if (skip_black && (rgb & 0xFFFFFFu) == 0)
		return;
	mmb_gfx_plot(x, y, rgb);
}

static unsigned snap_nn(uint32_t *tmp, int w, int h, double fx, double fy)
{
	int x = (int)floor(fx + 0.5);
	int y = (int)floor(fy + 0.5);
	if (x < 0 || y < 0 || x >= w || y >= h)
		return 0;
	return tmp[y * w + x];
}

static unsigned snap_bl(uint32_t *tmp, int w, int h, double fx, double fy)
{
	int x0 = (int)floor(fx);
	int y0 = (int)floor(fy);
	double tx = fx - (double)x0;
	double ty = fy - (double)y0;
	unsigned c00, c10, c01, c11;
	if (x0 < 0 || y0 < 0 || x0 >= w || y0 >= h)
		c00 = 0;
	else
		c00 = tmp[y0 * w + x0];
	if (x0 + 1 < 0 || y0 < 0 || x0 + 1 >= w || y0 >= h)
		c10 = 0;
	else
		c10 = tmp[y0 * w + (x0 + 1)];
	if (x0 < 0 || y0 + 1 < 0 || x0 >= w || y0 + 1 >= h)
		c01 = 0;
	else
		c01 = tmp[(y0 + 1) * w + x0];
	if (x0 + 1 < 0 || y0 + 1 < 0 || x0 + 1 >= w || y0 + 1 >= h)
		c11 = 0;
	else
		c11 = tmp[(y0 + 1) * w + (x0 + 1)];
	return lerp_rgb(lerp_rgb(c00, c10, tx), lerp_rgb(c01, c11, tx), ty);
}

static int blit_ix(int n)
{
	if (n < 1 || n > MMB_MAX_BLIT)
		mmb_error("?BLIT");
	return n - 1;
}

void mmb_gfx_fb_create(int w, int h)
{
	unsigned bytes;
	if (G.gfx.fb)
		mmb_error("?FRAMEBUFFER");
	if (w < G.gfx.w || h < G.gfx.h || w > MMB_FB_MAX_W || h > MMB_FB_MAX_H)
		mmb_error("?FRAMEBUFFER");
	bytes = (unsigned)w * (unsigned)h * sizeof(uint16_t);
	G.gfx.fb = G.plat->alloc(bytes);
	if (!G.gfx.fb)
		mmb_error("?OUT OF MEMORY");
	memset(G.gfx.fb, 0, bytes);
	G.gfx.fb_w = w;
	G.gfx.fb_h = h;
}

void mmb_gfx_fb_write(void)
{
	if (!G.gfx.fb)
		mmb_error("?FRAMEBUFFER");
	G.gfx.write_fb = 1;
}

void mmb_gfx_fb_backup(void)
{
	unsigned bytes;
	if (!G.gfx.fb)
		mmb_error("?FRAMEBUFFER");
	bytes = (unsigned)G.gfx.fb_w * (unsigned)G.gfx.fb_h * sizeof(uint16_t);
	if (!G.gfx.fb_bak)
	{
		G.gfx.fb_bak = G.plat->alloc(bytes);
		if (!G.gfx.fb_bak)
			mmb_error("?OUT OF MEMORY");
	}
	memcpy(G.gfx.fb_bak, G.gfx.fb, bytes);
}

void mmb_gfx_fb_restore(int x, int y, int w, int h, int all)
{
	int i, j;
	if (!G.gfx.fb || !G.gfx.fb_bak)
		mmb_error("?FRAMEBUFFER");
	if (all)
	{
		memcpy(G.gfx.fb, G.gfx.fb_bak,
		       (unsigned)G.gfx.fb_w * (unsigned)G.gfx.fb_h * sizeof(uint16_t));
		return;
	}
	if (w < 0) { x += w; w = -w; }
	if (h < 0) { y += h; h = -h; }
	for (j = 0; j < h; j++)
	{
		int yy = y + j;
		if (yy < 0 || yy >= G.gfx.fb_h)
			continue;
		for (i = 0; i < w; i++)
		{
			int xx = x + i;
			if (xx < 0 || xx >= G.gfx.fb_w)
				continue;
			G.gfx.fb[yy * G.gfx.fb_w + xx] = G.gfx.fb_bak[yy * G.gfx.fb_w + xx];
		}
	}
}

void mmb_gfx_fb_window(int x, int y, int page)
{
	int i, j, dw, dh;
	uint16_t *d;
	if (!G.gfx.fb)
		mmb_error("?FRAMEBUFFER");
	if (page == MMB_PAGE_FB)
		mmb_error("?PAGE");
	d = mmb_gfx_buf_for(page, &dw, &dh);
	for (j = 0; j < G.gfx.h && j < dh; j++)
	{
		int sy = y + j;
		for (i = 0; i < G.gfx.w && i < dw; i++)
		{
			int sx = x + i;
			uint16_t c = 0;
			if (sx >= 0 && sy >= 0 && sx < G.gfx.fb_w && sy < G.gfx.fb_h)
				c = G.gfx.fb[sy * G.gfx.fb_w + sx];
			d[j * dw + i] = c;
			if (page == 1 && G.gfx.page1_alpha)
				G.gfx.page1_alpha[j * dw + i] = c ? 255 : 0;
		}
	}
	mmb_gfx_present_if(page);
}

void mmb_gfx_fb_close(void)
{
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
}

void mmb_gfx_blit_copy(int x1, int y1, int x2, int y2, int w, int h, int srcpage, int ori)
{
	uint32_t *tmp;
	uint16_t *src, *dst;
	int i, j, sw, sh, dw, dh;
	unsigned row_bytes, total;

	if (w <= 0 || h <= 0)
		return;

	/* Opaque, unrotated rect: native DMA / row copy (skip RGB888 snap). */
	if (ori == 0)
	{
		src = mmb_gfx_buf_for(srcpage, &sw, &sh);
		dst = mmb_gfx_buf_for(MMB_PAGE_CUR, &dw, &dh);
		if (src && dst &&
		    x1 >= 0 && y1 >= 0 && x1 + w <= sw && y1 + h <= sh &&
		    x2 >= 0 && y2 >= 0 && x2 + w <= dw && y2 + h <= dh)
		{
			int same = (src == dst);
			int overlap = same &&
				!(x2 + w <= x1 || x1 + w <= x2 ||
				  y2 + h <= y1 || y1 + h <= y2);

			if (!overlap)
			{
				row_bytes = (unsigned)w * sizeof(uint16_t);
				total = row_bytes * (unsigned)h;

				if (w == sw && w == dw)
				{
					/* Contiguous slab on both sides. */
					uint16_t *s = src + y1 * sw + x1;
					uint16_t *d = dst + y2 * dw + x2;
					if (total >= 4096u && G.plat && G.plat->dma_copy &&
					    G.plat->dma_copy(d, s, total))
						;
					else
						memcpy(d, s, total);
				}
				else if (w == sw && total >= 4096u && G.plat &&
					 G.plat->dma_copy2d)
				{
					/* Contiguous source → pitched dest. */
					uint16_t *s = src + y1 * sw + x1;
					uint16_t *d = dst + y2 * dw + x2;
					unsigned stride =
						(unsigned)(dw - w) * sizeof(uint16_t);
					if (!G.plat->dma_copy2d(d, s, row_bytes,
								(unsigned)h, stride))
					{
						for (j = 0; j < h; j++)
							memcpy(d + j * dw, s + j * sw,
							       row_bytes);
					}
				}
				else
				{
					for (j = 0; j < h; j++)
					{
						uint16_t *s = src + (y1 + j) * sw + x1;
						uint16_t *d = dst + (y2 + j) * dw + x2;
						if (row_bytes >= 4096u && G.plat &&
						    G.plat->dma_copy &&
						    G.plat->dma_copy(d, s, row_bytes))
							;
						else
							memcpy(d, s, row_bytes);
					}
				}

				if (G.gfx.write_page == 1 && !mmb_gfx_writing_fb())
				{
					ensure_page1_alpha();
					if (G.gfx.page1_alpha)
					{
						for (j = 0; j < h; j++)
						{
							uint16_t *row =
								dst + (y2 + j) * dw + x2;
							uint8_t *al =
								G.gfx.page1_alpha +
								(y2 + j) * dw + x2;
							for (i = 0; i < w; i++)
							{
								al[i] = row[i] ? 255 : 0;
								if (row[i])
									G.gfx.page1_any = 1;
							}
						}
					}
				}
				if (!mmb_gfx_writing_fb() &&
				    G.gfx.write_page == G.gfx.display_page)
				{
					mmb_gfx_dirty_add(x2, y2, w, h);
					mmb_gfx_dirty_flush();
				}
				return;
			}
		}
	}

	tmp = snap_rect(srcpage, x1, y1, w, h);
	for (j = 0; j < h; j++)
	{
		for (i = 0; i < w; i++)
		{
			int si = (ori & 1) ? w - 1 - i : i;
			int sj = (ori & 2) ? h - 1 - j : j;
			unsigned c = tmp[sj * w + si];
			if ((ori & 4) && (c & 0xFFFFFFu) == 0)
				continue;
			mmb_gfx_plot(x2 + i, y2 + j, c);
		}
	}
	G.plat->free(tmp);
}

void mmb_gfx_blit_read(int n, int x, int y, int w, int h, int srcpage)
{
	int ix = blit_ix(n);
	if (w <= 0 || h <= 0)
		mmb_error("?BLIT");
	if (G.gfx.blit[ix].pix)
	{
		G.plat->free(G.gfx.blit[ix].pix);
		G.gfx.blit[ix].pix = 0;
	}
	G.gfx.blit[ix].pix = snap_rect(srcpage, x, y, w, h);
	G.gfx.blit[ix].used = 1;
	G.gfx.blit[ix].w = w;
	G.gfx.blit[ix].h = h;
}

void mmb_gfx_blit_write(int n, int x, int y, int ori)
{
	int ix = blit_ix(n);
	int w, h, i, j;
	uint32_t *pix;
	if (!G.gfx.blit[ix].used || !G.gfx.blit[ix].pix)
		mmb_error("?BLIT");
	w = G.gfx.blit[ix].w;
	h = G.gfx.blit[ix].h;
	pix = G.gfx.blit[ix].pix;
	for (j = 0; j < h; j++)
	{
		for (i = 0; i < w; i++)
		{
			int si = (ori & 1) ? w - 1 - i : i;
			int sj = (ori & 2) ? h - 1 - j : j;
			unsigned c = pix[sj * w + si];
			if ((ori & 4) && (c & 0xFFFFFFu) == 0)
				continue;
			mmb_gfx_plot(x + i, y + j, c);
		}
	}
}

void mmb_gfx_blit_close(int n)
{
	int ix = blit_ix(n);
	if (G.gfx.blit[ix].pix)
	{
		G.plat->free(G.gfx.blit[ix].pix);
		G.gfx.blit[ix].pix = 0;
	}
	G.gfx.blit[ix].used = 0;
	G.gfx.blit[ix].w = 0;
	G.gfx.blit[ix].h = 0;
}

void mmb_gfx_image_resize(int x, int y, int w, int h, int nx, int ny, int nw, int nh,
			  int srcpage, int fast, int skip_black)
{
	uint32_t *tmp;
	int i, j;
	if (w <= 0 || h <= 0 || nw <= 0 || nh <= 0)
		return;
	tmp = snap_rect(srcpage, x, y, w, h);
	for (j = 0; j < nh; j++)
	{
		for (i = 0; i < nw; i++)
		{
			double fx = ((double)i + 0.5) * (double)w / (double)nw - 0.5;
			double fy = ((double)j + 0.5) * (double)h / (double)nh - 0.5;
			unsigned c = fast ? snap_nn(tmp, w, h, fx, fy)
					  : snap_bl(tmp, w, h, fx, fy);
			put_dest(nx + i, ny + j, c, skip_black);
		}
	}
	G.plat->free(tmp);
}

void mmb_gfx_image_rotate(int x, int y, int w, int h, int nx, int ny, double angle,
			  int srcpage, int fast, int skip_black)
{
	uint32_t *tmp;
	int i, j;
	double rad, cs, sn, cx, cy;
	if (w <= 0 || h <= 0)
		return;
	tmp = snap_rect(srcpage, x, y, w, h);
	rad = angle * M_PI / 180.0;
	cs = cos(rad);
	sn = sin(rad);
	cx = ((double)w - 1.0) * 0.5;
	cy = ((double)h - 1.0) * 0.5;
	for (j = 0; j < h; j++)
	{
		for (i = 0; i < w; i++)
		{
			double dx = (double)i - cx;
			double dy = (double)j - cy;
			/* inverse of screen-clockwise rotation */
			double sx = cx + dx * cs + dy * sn;
			double sy = cy - dx * sn + dy * cs;
			unsigned c = fast ? snap_nn(tmp, w, h, sx, sy)
					  : snap_bl(tmp, w, h, sx, sy);
			put_dest(nx + i, ny + j, c, skip_black);
		}
	}
	G.plat->free(tmp);
}

void mmb_gfx_image_warp_h(int x, int y, int w, int h, int x1, int y1, int h1,
			  int x2, int y2, int h2, int srcpage, int skip_black)
{
	uint32_t *tmp;
	int minx, maxx, miny, maxy, px, py;
	if (w <= 0 || h <= 0)
		return;
	tmp = snap_rect(srcpage, x, y, w, h);
	minx = x1 < x2 ? x1 : x2;
	maxx = x1 > x2 ? x1 : x2;
	miny = y1;
	if (y1 + h1 < miny) miny = y1 + h1;
	if (y2 < miny) miny = y2;
	if (y2 + h2 < miny) miny = y2 + h2;
	maxy = y1 + h1;
	if (y1 > maxy) maxy = y1;
	if (y2 + h2 > maxy) maxy = y2 + h2;
	if (y2 > maxy) maxy = y2;
	for (py = miny; py <= maxy; py++)
	{
		for (px = minx; px <= maxx; px++)
		{
			double u, ty, hh, v, sx, sy;
			unsigned c;
			if (x2 == x1)
			{
				if (px != x1)
					continue;
				u = 0;
			}
			else
				u = ((double)px - (double)x1) / ((double)x2 - (double)x1);
			if (u < 0 || u > 1)
				continue;
			ty = (double)y1 + u * ((double)y2 - (double)y1);
			hh = (double)h1 + u * ((double)h2 - (double)h1);
			if (hh <= 0)
				continue;
			v = ((double)py - ty) / hh;
			if (v < 0 || v > 1)
				continue;
			sx = u * ((double)w - 1.0);
			sy = v * ((double)h - 1.0);
			c = snap_bl(tmp, w, h, sx, sy);
			put_dest(px, py, c, skip_black);
		}
	}
	G.plat->free(tmp);
}

void mmb_gfx_image_warp_v(int x, int y, int w, int h, int x1, int y1, int w1,
			  int x2, int y2, int w2, int srcpage, int skip_black)
{
	uint32_t *tmp;
	int minx, maxx, miny, maxy, px, py;
	if (w <= 0 || h <= 0)
		return;
	tmp = snap_rect(srcpage, x, y, w, h);
	miny = y1 < y2 ? y1 : y2;
	maxy = y1 > y2 ? y1 : y2;
	minx = x1;
	if (x1 + w1 < minx) minx = x1 + w1;
	if (x2 < minx) minx = x2;
	if (x2 + w2 < minx) minx = x2 + w2;
	maxx = x1 + w1;
	if (x1 > maxx) maxx = x1;
	if (x2 + w2 > maxx) maxx = x2 + w2;
	if (x2 > maxx) maxx = x2;
	for (py = miny; py <= maxy; py++)
	{
		for (px = minx; px <= maxx; px++)
		{
			double v, tx, ww, u, sx, sy;
			unsigned c;
			if (y2 == y1)
			{
				if (py != y1)
					continue;
				v = 0;
			}
			else
				v = ((double)py - (double)y1) / ((double)y2 - (double)y1);
			if (v < 0 || v > 1)
				continue;
			tx = (double)x1 + v * ((double)x2 - (double)x1);
			ww = (double)w1 + v * ((double)w2 - (double)w1);
			if (ww <= 0)
				continue;
			u = ((double)px - tx) / ww;
			if (u < 0 || u > 1)
				continue;
			sx = u * ((double)w - 1.0);
			sy = v * ((double)h - 1.0);
			c = snap_bl(tmp, w, h, sx, sy);
			put_dest(px, py, c, skip_black);
		}
	}
	G.plat->free(tmp);
}

void mmb_gfx_page_scroll(int page, int dx, int dy, int fill, int has_fill)
{
	int w, h, i, j;
	uint16_t *pg, *tmp;
	uint8_t *al = 0, *atmp = 0;
	unsigned bytes, fcol = 0, falpha = 0;
	pg = mmb_gfx_buf_for(page, &w, &h);
	bytes = (unsigned)w * (unsigned)h * sizeof(uint16_t);
	tmp = G.plat->alloc(bytes);
	if (!tmp)
		mmb_error("?OUT OF MEMORY");
	memcpy(tmp, pg, bytes);
	if (page == 1 && G.gfx.page1_alpha)
	{
		al = G.gfx.page1_alpha;
		atmp = G.plat->alloc((unsigned)w * (unsigned)h);
		if (atmp)
			memcpy(atmp, al, (unsigned)w * (unsigned)h);
	}
	if (has_fill && fill >= 0)
	{
		uint16_t np = mmb_pix_store((unsigned)fill, &falpha);
		fcol = np;
	}
	for (j = 0; j < h; j++)
	{
		for (i = 0; i < w; i++)
		{
			int si, sj;
			/* dest (i,j) came from a pixel that moved right dx, up dy */
			si = i - dx;
			sj = j + dy;
			if (!has_fill)
			{
				si %= w;
				if (si < 0)
					si += w;
				sj %= h;
				if (sj < 0)
					sj += h;
				pg[j * w + i] = tmp[sj * w + si];
				if (al && atmp)
					al[j * w + i] = atmp[sj * w + si];
			}
			else if (si < 0 || sj < 0 || si >= w || sj >= h)
			{
				if (fill != -1)
				{
					pg[j * w + i] = (uint16_t)fcol;
					if (al)
						al[j * w + i] = (uint8_t)falpha;
				}
			}
			else
			{
				pg[j * w + i] = tmp[sj * w + si];
				if (al && atmp)
					al[j * w + i] = atmp[sj * w + si];
			}
		}
	}
	G.plat->free(tmp);
	if (atmp)
		G.plat->free(atmp);
	mmb_gfx_present_if(page);
}

void mmb_gfx_page_logic(int op, int p1, int p2, int dst)
{
	int w1, h1, w2, h2, wd, hd, w, h, i, j;
	uint16_t *a, *b, *d;
	a = mmb_gfx_buf_for(p1, &w1, &h1);
	b = mmb_gfx_buf_for(p2, &w2, &h2);
	d = mmb_gfx_buf_for(dst, &wd, &hd);
	w = w1; if (w2 < w) w = w2; if (wd < w) w = wd;
	h = h1; if (h2 < h) h = h2; if (hd < h) h = hd;
	for (j = 0; j < h; j++)
	{
		for (i = 0; i < w; i++)
		{
			unsigned ca = a[j * w1 + i];
			unsigned cb = b[j * w2 + i];
			unsigned r;
			if (op == '&')
				r = ca & cb;
			else if (op == '|')
				r = ca | cb;
			else
				r = ca ^ cb;
			d[j * wd + i] = (uint16_t)r;
		}
	}
	mmb_gfx_present_if(dst);
}

void mmb_gfx_box_logic(int op, int x, int y, int w, int h, unsigned col, int page)
{
	int sw, sh, i, j;
	uint16_t *pg;
	uint16_t cn;
	cn = (uint16_t)mmb_rgb_to_native(mmb_quantize(col));
	pg = mmb_gfx_buf_for(page, &sw, &sh);
	if (w < 0) { x += w; w = -w; }
	if (h < 0) { y += h; h = -h; }
	for (j = 0; j < h; j++)
	{
		int yy = mmb_gfx_map_y(y + j, sh);
		if (yy < 0 || yy >= sh)
			continue;
		for (i = 0; i < w; i++)
		{
			int xx = x + i;
			uint16_t c;
			if (xx < 0 || xx >= sw)
				continue;
			c = pg[yy * sw + xx];
			if (op == '&')
				c &= cn;
			else if (op == '|')
				c |= cn;
			else
				c ^= cn;
			pg[yy * sw + xx] = c;
		}
	}
	mmb_gfx_present_if(page);
}

void mmb_gfx_bitmap(int x, int y, const unsigned char *bits, int nbytes,
		    int bw, int bh, int scale, unsigned fg, unsigned bg, int fill_bg)
{
	int row, col, bit = 0, byte = 0;
	unsigned char cur = 0;
	if (scale < 1)
		scale = 1;
	if (bw < 1)
		bw = 8;
	if (bh < 1)
		bh = 8;
	fg = mmb_quantize(fg);
	bg = mmb_quantize(bg);
	for (row = 0; row < bh; row++)
	{
		for (col = 0; col < bw; col++)
		{
			int on, sx, sy;
			if (bit == 0)
			{
				cur = (byte < nbytes) ? bits[byte] : 0;
				byte++;
				bit = 8;
			}
			bit--;
			on = (cur >> bit) & 1;
			if (on || fill_bg)
			{
				for (sy = 0; sy < scale; sy++)
					for (sx = 0; sx < scale; sx++)
						mmb_gfx_plot(x + col * scale + sx,
							     y + row * scale + sy,
							     on ? fg : bg);
			}
		}
	}
}

void mmb_gfx_fill_poly(const int *xs, const int *ys, int n, unsigned rgb)
{
	int i, y, miny, maxy;
	if (n < 3)
		return;
	miny = maxy = ys[0];
	for (i = 1; i < n; i++)
	{
		if (ys[i] < miny) miny = ys[i];
		if (ys[i] > maxy) maxy = ys[i];
	}
	rgb = mmb_quantize(rgb);
	for (y = miny; y <= maxy; y++)
	{
		int xsct[MMB_TURTLE_MAX], ns = 0, a, b;
		for (i = 0; i < n; i++)
		{
			int j = (i + 1) % n;
			int y0 = ys[i], y1 = ys[j], x0 = xs[i], x1 = xs[j];
			if (y0 == y1)
				continue;
			if ((y >= y0 && y < y1) || (y >= y1 && y < y0))
			{
				double t = (double)(y - y0) / (double)(y1 - y0);
				if (ns < MMB_TURTLE_MAX)
					xsct[ns++] = x0 + (int)(t * (x1 - x0));
			}
		}
		for (a = 0; a < ns; a++)
			for (b = a + 1; b < ns; b++)
				if (xsct[b] < xsct[a])
				{
					int t = xsct[a];
					xsct[a] = xsct[b];
					xsct[b] = t;
				}
		for (a = 0; a + 1 < ns; a += 2)
		{
			int x;
			for (x = xsct[a]; x <= xsct[a + 1]; x++)
				mmb_gfx_plot(x, y, rgb);
		}
	}
}

void mmb_turtle_init_state(int cls)
{
	if (cls)
		mmb_gfx_cls(0);
	G.gfx.turtle_on = 1;
	G.gfx.turtle_x = (mmb_gfx_writing_fb() ? G.gfx.fb_w : G.gfx.w) * 0.5;
	G.gfx.turtle_y = (mmb_gfx_writing_fb() ? G.gfx.fb_h : G.gfx.h) * 0.5;
	G.gfx.turtle_hdg = 0;
	G.gfx.turtle_pen = 1;
	G.gfx.turtle_pen_col = 0xFFFFFF;
	G.gfx.turtle_fill_col = 0x00FF00;
	G.gfx.turtle_filling = 0;
	G.gfx.turtle_fn = 0;
}
