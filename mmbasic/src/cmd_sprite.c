#include "mmb_priv.h"

static unsigned char LIFO[MMB_MAX_SPRITE];
static unsigned char zeroLIFO[MMB_MAX_SPRITE];
static int LIFOn;
static int zeroLIFOn;

typedef struct {
	int x, y, w, h;
} spr_rect;

static spr_rect s_rects[MMB_MAX_SPRITE * 2];
static int s_nrect;

static int sprite_ix(int n)
{
	if (n < 0 || n >= MMB_MAX_SPRITE)
		mmb_error("?SPRITE");
	return n;
}

static void sprite_free_ix(int ix)
{
	if (G.gfx.sprite[ix].pix)
	{
		G.plat->free(G.gfx.sprite[ix].pix);
		G.gfx.sprite[ix].pix = 0;
	}
	if (G.gfx.sprite[ix].npix)
	{
		G.plat->free(G.gfx.sprite[ix].npix);
		G.gfx.sprite[ix].npix = 0;
	}
	if (G.gfx.sprite[ix].store)
	{
		G.plat->free(G.gfx.sprite[ix].store);
		G.gfx.sprite[ix].store = 0;
	}
	if (G.gfx.sprite[ix].astore)
	{
		G.plat->free(G.gfx.sprite[ix].astore);
		G.gfx.sprite[ix].astore = 0;
	}
	G.gfx.sprite[ix].used = 0;
	G.gfx.sprite[ix].vis = 0;
	G.gfx.sprite[ix].w = 0;
	G.gfx.sprite[ix].h = 0;
	G.gfx.sprite[ix].has_next = 0;
}

void mmb_sprite_reset(void)
{
	int i;
	for (i = 0; i < MMB_MAX_SPRITE; i++)
		sprite_free_ix(i);
	LIFOn = 0;
	zeroLIFOn = 0;
	s_nrect = 0;
}

static void sprite_rebuild_npix(int ix)
{
	int n, i;
	uint16_t *npix;
	uint32_t *pix = G.gfx.sprite[ix].pix;

	if (G.gfx.sprite[ix].npix)
	{
		G.plat->free(G.gfx.sprite[ix].npix);
		G.gfx.sprite[ix].npix = 0;
	}
	if (!pix || G.gfx.sprite[ix].w <= 0 || G.gfx.sprite[ix].h <= 0)
		return;
	n = G.gfx.sprite[ix].w * G.gfx.sprite[ix].h;
	npix = G.plat->alloc((unsigned)n * sizeof(uint16_t));
	if (!npix)
		mmb_error("?OUT OF MEMORY");
	for (i = 0; i < n; i++)
	{
		unsigned c = pix[i];
		if ((c & 0xFF000000u) == 0 || (c & 0xFFFFFFu) == 0)
			npix[i] = 0;
		else
			npix[i] = (uint16_t)mmb_rgb_to_native(c);
	}
	G.gfx.sprite[ix].npix = npix;
}

static void sprite_store(int n, uint32_t *pix, int w, int h)
{
	int ix = sprite_ix(n);
	sprite_free_ix(ix);
	G.gfx.sprite[ix].pix = pix;
	G.gfx.sprite[ix].w = w;
	G.gfx.sprite[ix].h = h;
	G.gfx.sprite[ix].used = pix ? 1 : 0;
	G.gfx.sprite[ix].vis = 0;
	G.gfx.sprite[ix].x = 0;
	G.gfx.sprite[ix].y = 0;
	G.gfx.sprite[ix].layer = 1;
	G.gfx.sprite[ix].has_next = 0;
	if (pix)
		sprite_rebuild_npix(ix);
}

static void ensure_store(int ix)
{
	unsigned bytes;
	if (G.gfx.sprite[ix].w <= 0 || G.gfx.sprite[ix].h <= 0)
		return;
	bytes = (unsigned)G.gfx.sprite[ix].w * (unsigned)G.gfx.sprite[ix].h *
		sizeof(uint16_t);
	if (!G.gfx.sprite[ix].store)
	{
		G.gfx.sprite[ix].store = G.plat->alloc(bytes);
		if (!G.gfx.sprite[ix].store)
			mmb_error("?OUT OF MEMORY");
	}
	if (G.gfx.write_page == 1 && !G.gfx.sprite[ix].astore)
	{
		G.gfx.sprite[ix].astore =
			G.plat->alloc((unsigned)G.gfx.sprite[ix].w *
				      (unsigned)G.gfx.sprite[ix].h);
		if (!G.gfx.sprite[ix].astore)
			mmb_error("?OUT OF MEMORY");
	}
}

static void lifo_add(unsigned char *q, int *n, int v)
{
	int i, j = 0;
	for (i = 0; i < *n; i++)
	{
		if (q[i] != (unsigned char)v)
			q[j++] = q[i];
	}
	q[j] = (unsigned char)v;
	*n = j + 1;
}

static void lifo_remove(unsigned char *q, int *n, int v)
{
	int i, j = 0;
	for (i = 0; i < *n; i++)
	{
		if (q[i] != (unsigned char)v)
			q[j++] = q[i];
	}
	*n = j;
}

static void sprite_lifo_add(int ix)
{
	if (G.gfx.sprite[ix].layer == 0)
		lifo_add(zeroLIFO, &zeroLIFOn, ix);
	else
		lifo_add(LIFO, &LIFOn, ix);
}

static void sprite_lifo_remove(int ix)
{
	lifo_remove(LIFO, &LIFOn, ix);
	lifo_remove(zeroLIFO, &zeroLIFOn, ix);
}

static uint16_t *sprite_buf(int *pw, int *ph)
{
	return mmb_gfx_buf_for(MMB_PAGE_CUR, pw, ph);
}

static int sprite_page_visible(void)
{
	if (mmb_gfx_writing_fb())
		return 0;
	return G.gfx.write_page == G.gfx.display_page || G.gfx.write_page == 1;
}

static int sprite_clip(int ix, int *ox, int *oy, int *ow, int *oh,
		       int *sx, int *sy, int *pw, int *ph)
{
	int x = G.gfx.sprite[ix].x;
	int y = G.gfx.sprite[ix].y;
	int w = G.gfx.sprite[ix].w;
	int h = G.gfx.sprite[ix].h;

	sprite_buf(pw, ph);
	*sx = 0;
	*sy = 0;
	if (x < 0)
	{
		*sx = -x;
		w += x;
		x = 0;
	}
	if (y < 0)
	{
		*sy = -y;
		h += y;
		y = 0;
	}
	if (x + w > *pw)
		w = *pw - x;
	if (y + h > *ph)
		h = *ph - y;
	if (w <= 0 || h <= 0)
		return 0;
	*ox = x;
	*oy = y;
	*ow = w;
	*oh = h;
	return 1;
}

static void rect_push(int x, int y, int w, int h)
{
	int pw, ph;

	if (w <= 0 || h <= 0 || s_nrect >= MMB_MAX_SPRITE * 2)
		return;
	sprite_buf(&pw, &ph);
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
	if (x + w > pw)
		w = pw - x;
	if (y + h > ph)
		h = ph - y;
	if (w <= 0 || h <= 0)
		return;
	s_rects[s_nrect].x = x;
	s_rects[s_nrect].y = y;
	s_rects[s_nrect].w = w;
	s_rects[s_nrect].h = h;
	s_nrect++;
}

static void rect_flush(void)
{
	int i;

	if (sprite_page_visible())
	{
		for (i = 0; i < s_nrect; i++)
			mmb_gfx_present_rect(s_rects[i].x, s_rects[i].y,
					     s_rects[i].w, s_rects[i].h);
	}
	s_nrect = 0;
}

static void copy_alpha_rect(uint8_t *dst, int dst_stride,
			    const uint8_t *src, int src_stride,
			    int w, int h)
{
	int y;

	if (!dst || !src || w <= 0 || h <= 0)
		return;
	for (y = 0; y < h; y++)
		memcpy(dst + y * dst_stride, src + y * src_stride, (unsigned)w);
}

static void blit_restore(int ix)
{
	int ox, oy, ow, oh, sx, sy, pw, ph, sw;
	uint16_t *pg;

	if (!G.gfx.sprite[ix].vis || !G.gfx.sprite[ix].store)
		return;
	rect_push(G.gfx.sprite[ix].x, G.gfx.sprite[ix].y,
		  G.gfx.sprite[ix].w, G.gfx.sprite[ix].h);
	if (sprite_clip(ix, &ox, &oy, &ow, &oh, &sx, &sy, &pw, &ph))
	{
		sw = G.gfx.sprite[ix].w;
		pg = sprite_buf(&pw, &ph);
		mmb_blit_copy_rect16(pg + oy * pw + ox, pw,
				     G.gfx.sprite[ix].store + sy * sw + sx, sw,
				     ow, oh);
		if (G.gfx.write_page == 1 && G.gfx.page1_alpha &&
		    G.gfx.sprite[ix].astore)
			copy_alpha_rect(G.gfx.page1_alpha + oy * pw + ox, pw,
					G.gfx.sprite[ix].astore + sy * sw + sx, sw,
					ow, oh);
	}
	G.gfx.sprite[ix].vis = 0;
}

static void blit_show(int ix, int x, int y)
{
	int ox, oy, ow, oh, sx, sy, pw, ph, sw;
	uint16_t *pg;

	G.gfx.sprite[ix].x = x;
	G.gfx.sprite[ix].y = y;
	ensure_store(ix);
	if (!G.gfx.sprite[ix].store || !G.gfx.sprite[ix].npix)
	{
		G.gfx.sprite[ix].vis = 1;
		return;
	}
	sw = G.gfx.sprite[ix].w;
	if (sprite_clip(ix, &ox, &oy, &ow, &oh, &sx, &sy, &pw, &ph))
	{
		pg = sprite_buf(&pw, &ph);
		mmb_blit_copy_rect16(G.gfx.sprite[ix].store + sy * sw + sx, sw,
				     pg + oy * pw + ox, pw, ow, oh);
		if (G.gfx.write_page == 1 && G.gfx.page1_alpha)
		{
			ensure_store(ix);
			if (G.gfx.sprite[ix].astore)
				copy_alpha_rect(G.gfx.sprite[ix].astore + sy * sw + sx, sw,
						G.gfx.page1_alpha + oy * pw + ox, pw,
						ow, oh);
		}
		mmb_blit_sprite_trans16(pg + oy * pw + ox, pw,
					G.gfx.sprite[ix].npix + sy * sw + sx, sw,
					ow, oh);
		if (G.gfx.write_page == 1 && G.gfx.page1_alpha)
		{
			int i, j;
			for (j = 0; j < oh; j++)
			{
				uint16_t *row = G.gfx.sprite[ix].npix + (sy + j) * sw + sx;
				uint8_t *al = G.gfx.page1_alpha + (oy + j) * pw + ox;
				for (i = 0; i < ow; i++)
				{
					if (row[i])
					{
						al[i] = 255;
						G.gfx.page1_any = 1;
					}
				}
			}
		}
	}
	G.gfx.sprite[ix].vis = 1;
	rect_push(x, y, G.gfx.sprite[ix].w, G.gfx.sprite[ix].h);
}

static void hide_down_to(int ix, int *found, int *zerolifo)
{
	int i;

	*found = -1;
	*zerolifo = 0;
	for (i = LIFOn - 1; i >= 0; i--)
	{
		blit_restore(LIFO[i]);
		if (LIFO[i] == (unsigned char)ix)
		{
			*found = i;
			return;
		}
	}
	for (i = zeroLIFOn - 1; i >= 0; i--)
	{
		blit_restore(zeroLIFO[i]);
		if (zeroLIFO[i] == (unsigned char)ix)
		{
			*found = i;
			*zerolifo = 1;
			return;
		}
	}
}

static void reshow_from(int found, int zerolifo)
{
	int i;

	if (found < 0)
		return;
	if (zerolifo)
	{
		for (i = found; i < zeroLIFOn; i++)
			blit_show(zeroLIFO[i], G.gfx.sprite[zeroLIFO[i]].x,
				  G.gfx.sprite[zeroLIFO[i]].y);
		for (i = 0; i < LIFOn; i++)
			blit_show(LIFO[i], G.gfx.sprite[LIFO[i]].x,
				  G.gfx.sprite[LIFO[i]].y);
	}
	else
	{
		for (i = found; i < LIFOn; i++)
			blit_show(LIFO[i], G.gfx.sprite[LIFO[i]].x,
				  G.gfx.sprite[LIFO[i]].y);
	}
}

static void sprite_show_at(int ix, int x, int y, int layer)
{
	int found, zerolifo, was;

	if (!G.gfx.sprite[ix].used)
		return;
	was = G.gfx.sprite[ix].vis;
	if (was)
	{
		hide_down_to(ix, &found, &zerolifo);
		if (G.gfx.sprite[ix].layer != layer)
		{
			sprite_lifo_remove(ix);
			G.gfx.sprite[ix].layer = layer;
			sprite_lifo_add(ix);
			blit_show(ix, x, y);
			reshow_from(found, zerolifo);
		}
		else
		{
			G.gfx.sprite[ix].layer = layer;
			blit_show(ix, x, y);
			if (found >= 0)
				reshow_from(found + 1, zerolifo);
		}
	}
	else
	{
		G.gfx.sprite[ix].layer = layer;
		sprite_lifo_add(ix);
		blit_show(ix, x, y);
	}
	rect_flush();
}

static void hide_all_keep_pos(void)
{
	int i;
	for (i = LIFOn - 1; i >= 0; i--)
		blit_restore(LIFO[i]);
	for (i = zeroLIFOn - 1; i >= 0; i--)
		blit_restore(zeroLIFO[i]);
}

static void show_all_from_lifo(void)
{
	int i, ix;

	for (i = 0; i < zeroLIFOn; i++)
	{
		ix = zeroLIFO[i];
		if (G.gfx.sprite[ix].has_next)
		{
			G.gfx.sprite[ix].x = G.gfx.sprite[ix].next_x;
			G.gfx.sprite[ix].y = G.gfx.sprite[ix].next_y;
			G.gfx.sprite[ix].has_next = 0;
		}
		blit_show(ix, G.gfx.sprite[ix].x, G.gfx.sprite[ix].y);
	}
	for (i = 0; i < LIFOn; i++)
	{
		ix = LIFO[i];
		if (G.gfx.sprite[ix].has_next)
		{
			G.gfx.sprite[ix].x = G.gfx.sprite[ix].next_x;
			G.gfx.sprite[ix].y = G.gfx.sprite[ix].next_y;
			G.gfx.sprite[ix].has_next = 0;
		}
		blit_show(ix, G.gfx.sprite[ix].x, G.gfx.sprite[ix].y);
	}
}

static void add_png_ext(char *path, int pathsz)
{
	char *dot = 0, *q = path;
	while (*q)
	{
		if (*q == '.')
			dot = q;
		if (*q == '/' || *q == '\\')
			dot = 0;
		q++;
	}
	if (!dot && (int)strlen(path) + 4 < pathsz)
		strcat(path, ".png");
}

static void sprite_loadpng(void)
{
	mmb_val v;
	char path[128];
	unsigned char *file = 0;
	uint32_t *pix = 0;
	int id, w = 0, h = 0, sz, has_trans = 0;
	unsigned got = 0, trans_rgb = 0;

	id = (int)mmb_as_int(mmb_expr());
	mmb_skip_sp();
	if (*G.p == ',')
		G.p++;
	v = mmb_expr();
	if (v.type != T_STR)
		mmb_syntax();
	strncpy(path, v.s, sizeof(path) - 1);
	path[sizeof(path) - 1] = 0;
	add_png_ext(path, sizeof(path));
	mmb_skip_sp();
	if (*G.p == ',')
	{
		G.p++;
		has_trans = 1;
		trans_rgb = mmb_colour_from_int(mmb_as_int(mmb_expr()));
	}
	sz = mmb_vfs_size(path);
	if (sz < 0)
	{
		uint32_t *empty = G.plat->alloc(8 * 8 * sizeof(uint32_t));
		if (empty)
			memset(empty, 0, 8 * 8 * sizeof(uint32_t));
		sprite_store(id, empty, 8, 8);
		return;
	}
	file = G.plat->alloc((unsigned)sz + 1);
	if (!file)
		mmb_error("?OUT OF MEMORY");
	if (mmb_vfs_read(path, file, (unsigned)sz, &got) != 0)
	{
		G.plat->free(file);
		mmb_error("?PNG");
	}
	if (mmb_png_decode_rgba(file, got, &pix, &w, &h) != 0)
	{
		G.plat->free(file);
		mmb_error("?PNG");
	}
	G.plat->free(file);
	if (has_trans && pix)
	{
		int i, np = w * h;
		for (i = 0; i < np; i++)
		{
			if ((pix[i] & 0xFFFFFFu) == trans_rgb)
				pix[i] = 0;
		}
	}
	sprite_store(id, pix, w, h);
}

static void sprite_read(void)
{
	mmb_val a[6];
	int n = 0, id, x, y, w, h, page, i, j;
	uint32_t *pix;

	id = (int)mmb_as_int(mmb_expr());
	mmb_skip_sp();
	if (*G.p == ',')
		G.p++;
	while (n < 6 && *G.p && *G.p != ':' && *G.p != '\'')
	{
		mmb_skip_sp();
		if (*G.p == ',' || *G.p == 0)
		{
			a[n++] = mmb_int_val(0);
			if (*G.p == ',')
				G.p++;
			continue;
		}
		a[n++] = mmb_expr();
		mmb_skip_sp();
		if (*G.p == ',')
		{
			G.p++;
			continue;
		}
		break;
	}
	if (n < 4)
		mmb_syntax();
	x = (int)mmb_as_int(a[0]);
	y = (int)mmb_as_int(a[1]);
	w = (int)mmb_as_int(a[2]);
	h = (int)mmb_as_int(a[3]);
	page = n >= 5 ? (int)mmb_as_int(a[4]) : G.gfx.write_page;
	if (w <= 0 || h <= 0)
		mmb_error("?SPRITE");
	pix = G.plat->alloc((unsigned)w * (unsigned)h * sizeof(uint32_t));
	if (!pix)
		mmb_error("?OUT OF MEMORY");
	for (j = 0; j < h; j++)
		for (i = 0; i < w; i++)
		{
			unsigned c = mmb_gfx_get_page(x + i, y + j, page);
			if ((c & 0xFFFFFFu) == 0)
				c = 0;
			else
				c |= 0xFF000000u;
			pix[j * w + i] = c;
		}
	sprite_store(id, pix, w, h);
}

static void sprite_show(void)
{
	int id, x, y, layer = 1, ix;
	id = (int)mmb_as_int(mmb_expr());
	mmb_skip_sp();
	if (*G.p == ',')
		G.p++;
	x = (int)mmb_as_int(mmb_expr());
	mmb_skip_sp();
	if (*G.p == ',')
		G.p++;
	y = (int)mmb_as_int(mmb_expr());
	mmb_skip_sp();
	if (*G.p == ',')
	{
		G.p++;
		layer = (int)mmb_as_int(mmb_expr());
	}
	ix = sprite_ix(id);
	sprite_show_at(ix, x, y, layer);
}

static void sprite_hide(void)
{
	int ix = sprite_ix((int)mmb_as_int(mmb_expr()));
	int found, zerolifo;

	if (!G.gfx.sprite[ix].vis)
		return;
	hide_down_to(ix, &found, &zerolifo);
	sprite_lifo_remove(ix);
	reshow_from(found, zerolifo);
	rect_flush();
}

static void sprite_next(void)
{
	int ix, x, y;
	ix = sprite_ix((int)mmb_as_int(mmb_expr()));
	mmb_skip_sp();
	if (*G.p == ',')
		G.p++;
	x = (int)mmb_as_int(mmb_expr());
	mmb_skip_sp();
	if (*G.p == ',')
		G.p++;
	y = (int)mmb_as_int(mmb_expr());
	G.gfx.sprite[ix].next_x = x;
	G.gfx.sprite[ix].next_y = y;
	G.gfx.sprite[ix].has_next = 1;
}

static void sprite_move(void)
{
	int id, x, y, ix;
	mmb_skip_sp();
	if (*G.p == 0 || *G.p == ':' || *G.p == '\'')
	{
		hide_all_keep_pos();
		show_all_from_lifo();
		rect_flush();
		return;
	}
	id = (int)mmb_as_int(mmb_expr());
	mmb_skip_sp();
	if (*G.p == ',')
		G.p++;
	x = (int)mmb_as_int(mmb_expr());
	mmb_skip_sp();
	if (*G.p == ',')
		G.p++;
	y = (int)mmb_as_int(mmb_expr());
	ix = sprite_ix(id);
	if (G.gfx.sprite[ix].vis)
		sprite_show_at(ix, x, y, G.gfx.sprite[ix].layer);
	else
	{
		G.gfx.sprite[ix].x = x;
		G.gfx.sprite[ix].y = y;
	}
}

void mmb_sprite_overlay(void)
{
}

void mmb_cmd_sprite(void)
{
	if (mmb_match("LOADPNG") || mmb_match("LOAD"))
	{
		if (mmb_match("PNG"))
			;
		sprite_loadpng();
		return;
	}
	if (mmb_match("READ"))
	{
		sprite_read();
		return;
	}
	if (mmb_match("SHOW"))
	{
		sprite_show();
		return;
	}
	if (mmb_match("HIDE"))
	{
		sprite_hide();
		return;
	}
	if (mmb_match("NEXT"))
	{
		sprite_next();
		return;
	}
	if (mmb_match("MOVE"))
	{
		sprite_move();
		return;
	}
	if (mmb_match("CLOSE") || mmb_match("RESTORE"))
	{
		int ix;
		mmb_skip_sp();
		if (*G.p && *G.p != ':' && *G.p != '\'')
		{
			ix = sprite_ix((int)mmb_as_int(mmb_expr()));
			if (G.gfx.sprite[ix].vis)
			{
				blit_restore(ix);
				sprite_lifo_remove(ix);
				rect_flush();
			}
			sprite_free_ix(ix);
		}
		else
			mmb_sprite_reset();
		return;
	}
	mmb_syntax();
}
