#include "mmb_priv.h"
#include "tui.h"

/* Visible sprites in draw order, bottom (index 0) to top. Kept sorted by
   (layer, show sequence) whenever SPRITE SHOW changes the list; rendering
   just walks it. */
static int ORDER[MMB_MAX_SPRITE];
static int ORDERN;
static int s_seq;

static void order_remove(int ix);
static void hide_all_keep_pos(void);
static void show_all_from_order(void);

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
	G.gfx.sprite[ix].seq = 0;
	G.gfx.sprite[ix].has_next = 0;
	order_remove(ix);
}

void mmb_sprite_reset(void)
{
	int i;
	for (i = 0; i < MMB_MAX_SPRITE; i++)
		sprite_free_ix(i);
	ORDERN = 0;
	s_seq = 0;
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
	G.gfx.sprite[ix].seq = 0;
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

static int order_pos(int ix)
{
	int i;
	for (i = 0; i < ORDERN; i++)
		if (ORDER[i] == ix)
			return i;
	return -1;
}

static void order_remove(int ix)
{
	int i, j = 0;
	for (i = 0; i < ORDERN; i++)
		if (ORDER[i] != ix)
			ORDER[j++] = ORDER[i];
	ORDERN = j;
}

/* Insert ix at its sorted position: ascending layer, then show sequence. */
static void order_insert_sorted(int ix)
{
	int layer = G.gfx.sprite[ix].layer;
	int seq = G.gfx.sprite[ix].seq;
	int i, p;

	order_remove(ix);
	p = ORDERN;
	for (i = 0; i < ORDERN; i++)
	{
		int other = ORDER[i];
		if (G.gfx.sprite[other].layer > layer ||
		    (G.gfx.sprite[other].layer == layer && G.gfx.sprite[other].seq > seq))
		{
			p = i;
			break;
		}
	}
	for (i = ORDERN; i > p; i--)
		ORDER[i] = ORDER[i - 1];
	ORDER[p] = ix;
	ORDERN++;
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

/* Restore the scene from the top of the draw list down to position pos. */
static void restore_from_top_to(int pos)
{
	int i;
	for (i = ORDERN - 1; i >= pos; i--)
		blit_restore(ORDER[i]);
}

static void show_one(int ix)
{
	if (G.gfx.sprite[ix].has_next)
	{
		G.gfx.sprite[ix].x = G.gfx.sprite[ix].next_x;
		G.gfx.sprite[ix].y = G.gfx.sprite[ix].next_y;
		G.gfx.sprite[ix].has_next = 0;
	}
	blit_show(ix, G.gfx.sprite[ix].x, G.gfx.sprite[ix].y);
}

/* Redraw from position pos upward, in draw order. */
static void show_from(int pos)
{
	int i;
	for (i = pos; i < ORDERN; i++)
		show_one(ORDER[i]);
}

static void sprite_show_at(int ix, int x, int y, int layer)
{
	int was, layer_changed, p;

	if (!G.gfx.sprite[ix].used)
		return;
	if (layer < 0)
		layer = 0;
	if (layer > 8)
		layer = 8;
	was = G.gfx.sprite[ix].vis;
	layer_changed = was && G.gfx.sprite[ix].layer != layer;
	if (layer_changed)
	{
		/* The whole stack can shift; restore everything and redraw in order. */
		hide_all_keep_pos();
		G.gfx.sprite[ix].x = x;
		G.gfx.sprite[ix].y = y;
		G.gfx.sprite[ix].layer = layer;
		order_insert_sorted(ix);
		show_all_from_order();
		rect_flush();
		return;
	}
	if (!was)
	{
		G.gfx.sprite[ix].seq = ++s_seq;
		G.gfx.sprite[ix].x = x;
		G.gfx.sprite[ix].y = y;
		G.gfx.sprite[ix].layer = layer;
		order_insert_sorted(ix);
		p = order_pos(ix);
		if (p == ORDERN - 1)
		{
			show_one(ix); /* topmost: no need to disturb the rest */
		}
		else
		{
			hide_all_keep_pos();
			show_all_from_order();
		}
		rect_flush();
		return;
	}
	/* Already visible at the same layer: move within the existing order. */
	p = order_pos(ix);
	restore_from_top_to(p);
	G.gfx.sprite[ix].x = x;
	G.gfx.sprite[ix].y = y;
	G.gfx.sprite[ix].layer = layer;
	show_one(ix);
	show_from(p + 1);
	rect_flush();
}

static void hide_all_keep_pos(void)
{
	int i;
	for (i = ORDERN - 1; i >= 0; i--)
		blit_restore(ORDER[i]);
}

static void show_all_from_order(void)
{
	show_from(0);
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
	int p;

	if (!G.gfx.sprite[ix].vis)
		return;
	p = order_pos(ix);
	if (p < 0)
		return;
	restore_from_top_to(p);
	order_remove(ix);
	show_from(p);
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
		show_all_from_order();
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
	{
		int p = order_pos(ix);
		if (p < 0)
			return;
		restore_from_top_to(p);
		G.gfx.sprite[ix].x = x;
		G.gfx.sprite[ix].y = y;
		show_one(ix);
		show_from(p + 1);
		rect_flush();
	}
	else
	{
		G.gfx.sprite[ix].x = x;
		G.gfx.sprite[ix].y = y;
	}
}

void mmb_sprite_overlay(void)
{
}

/* ------------------------------------------------------------------ */
/* Grid sprite / bitmap font editor                                    */
/*                                                                     */
/* A keyboard-driven, grid-constrained pixel editor. SPRITE EDIT is a  */
/* single cell, SPRITE SHEET lays several cells in a grid and SPRITE   */
/* FONT edits a bitmap-font charset. Saving writes a real PNG sheet    */
/* (and, for fonts, the FontDescription JSON) so SPRITE LOADPNG and    */
/* fontLoad() read the result straight back.                           */
/* ------------------------------------------------------------------ */

#define SM_MAX_CELL  16
#define SM_MAX_CELLS 64

#define SM_SPRITE 0
#define SM_SHEET  1
#define SM_FONT   2

#define SM_ESC_NONE 0
#define SM_ESC_GOT  1
#define SM_ESC_CSI  2
#define SM_ESC_SS3  3
#define SM_ESC_IDLE_MS 60

#define SM_TRANS 16

/* Same 59-glyph ASCII set as ramdisk/fonts/08X08-F1.json. */
static const char SM_CHARSET[] =
	" !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ";

typedef struct {
	int active;
	int mode;
	char path[160];
	char label[96];
	int cell;
	int cols;
	int rows;
	int ncells;
	int frame;
	int cx, cy;
	int colour;
	int dirty;
	unsigned char pix[SM_MAX_CELLS][SM_MAX_CELL * SM_MAX_CELL];
	char status[96];
	int esc_state, esc_at, alt_pend;
} sm_state;

static sm_state SM;

static const char *sm_base(const char *p)
{
	const char *s = p;
	while (p && *p)
	{
		if (*p == '/' || *p == ':' || *p == '\\')
			s = p + 1;
		p++;
	}
	return s;
}

static int sm_nearest(unsigned rgb)
{
	int best = 0, bd = 1 << 30, i;
	int r = (int)((rgb >> 16) & 255), g = (int)((rgb >> 8) & 255);
	int b = (int)(rgb & 255);
	for (i = 0; i < 16; i++)
	{
		unsigned c = mmb_ibm_colour(i);
		int dr = r - (int)((c >> 16) & 255);
		int dg = g - (int)((c >> 8) & 255);
		int db = b - (int)(c & 255);
		int d = dr * dr + dg * dg + db * db;
		if (d < bd)
		{
			bd = d;
			best = i;
		}
	}
	return best;
}

static void sm_be32(unsigned char *p, unsigned v)
{
	p[0] = (unsigned char)(v >> 24);
	p[1] = (unsigned char)(v >> 16);
	p[2] = (unsigned char)(v >> 8);
	p[3] = (unsigned char)v;
}

static unsigned sm_adler32(const unsigned char *p, unsigned n)
{
	unsigned a = 1, b = 0, i;
	for (i = 0; i < n; i++)
	{
		a = (a + p[i]) % 65521u;
		b = (b + a) % 65521u;
	}
	return (b << 16) | a;
}

/* zlib stream with stored (uncompressed) deflate blocks. */
static unsigned char *sm_zlib_stored(const unsigned char *raw, unsigned n,
				     unsigned *outn)
{
	const unsigned maxblk = 65535;
	unsigned blocks = n ? (n + maxblk - 1) / maxblk : 1;
	unsigned cap = 2 + blocks * 5 + n + 4;
	unsigned char *out = G.plat->alloc(cap);
	unsigned pos = 0, off = 0, ad;

	if (!out)
		return 0;
	out[pos++] = 0x78;
	out[pos++] = 0x01;
	do
	{
		unsigned blk = n - off;
		unsigned bfinal;
		if (blk > maxblk)
			blk = maxblk;
		bfinal = (off + blk >= n) ? 1u : 0u;
		out[pos++] = (unsigned char)bfinal;
		out[pos++] = (unsigned char)(blk & 0xFF);
		out[pos++] = (unsigned char)(blk >> 8);
		out[pos++] = (unsigned char)((~blk) & 0xFF);
		out[pos++] = (unsigned char)(((~blk) >> 8) & 0xFF);
		if (blk)
			memcpy(out + pos, raw + off, blk);
		pos += blk;
		off += blk;
	} while (off < n);
	ad = sm_adler32(raw, n);
	out[pos++] = (unsigned char)(ad >> 24);
	out[pos++] = (unsigned char)(ad >> 16);
	out[pos++] = (unsigned char)(ad >> 8);
	out[pos++] = (unsigned char)ad;
	*outn = pos;
	return out;
}

static unsigned sm_chunk(unsigned char *dst, const char *type,
			 const unsigned char *data, unsigned len)
{
	sm_be32(dst, len);
	memcpy(dst + 4, type, 4);
	if (len && data)
		memcpy(dst + 8, data, len);
	sm_be32(dst + 8 + len, mmb_crc32(dst + 4, len + 4));
	return 12 + len;
}

static int sm_write_png(const char *path, int w, int h)
{
	unsigned rawlen, zlen = 0, total, pos;
	unsigned char *raw, *z, *png, ihdr[13];
	unsigned i, j, c;
	int rc = -1;

	if (w <= 0 || h <= 0 || w > 4096 || h > 4096)
		return -1;
	rawlen = (unsigned)h * (1 + (unsigned)w * 4);
	raw = G.plat->alloc(rawlen);
	if (!raw)
		return -1;
	for (j = 0; j < (unsigned)h; j++)
	{
		unsigned char *row = raw + j * (1 + (unsigned)w * 4);
		row[0] = 0;
		for (i = 0; i < (unsigned)w; i++)
		{
			unsigned v, rgb = 0, a = 0;
			unsigned cc = i / (unsigned)SM.cell;
			unsigned cr = j / (unsigned)SM.cell;
			unsigned lx = i % (unsigned)SM.cell;
			unsigned ly = j % (unsigned)SM.cell;
			c = cr * (unsigned)SM.cols + cc;
			if (c < (unsigned)SM.ncells && cr < (unsigned)SM.rows)
				v = SM.pix[c][ly * SM.cell + lx];
			else
				v = SM_TRANS;
			if (v < SM_TRANS)
			{
				rgb = mmb_ibm_colour((int)v);
				a = 255;
			}
			row[1 + i * 4 + 0] = (unsigned char)((rgb >> 16) & 255);
			row[1 + i * 4 + 1] = (unsigned char)((rgb >> 8) & 255);
			row[1 + i * 4 + 2] = (unsigned char)(rgb & 255);
			row[1 + i * 4 + 3] = (unsigned char)a;
		}
	}
	z = sm_zlib_stored(raw, rawlen, &zlen);
	G.plat->free(raw);
	if (!z)
		return -1;
	total = 8 + (12 + 13) + (12 + zlen) + 12;
	png = G.plat->alloc(total);
	if (!png)
	{
		G.plat->free(z);
		return -1;
	}
	pos = 0;
	memcpy(png, "\x89PNG\r\n\x1a\n", 8);
	pos = 8;
	sm_be32(ihdr, (unsigned)w);
	sm_be32(ihdr + 4, (unsigned)h);
	ihdr[8] = 8;
	ihdr[9] = 6; /* RGBA */
	ihdr[10] = 0;
	ihdr[11] = 0;
	ihdr[12] = 0;
	pos += sm_chunk(png + pos, "IHDR", ihdr, 13);
	pos += sm_chunk(png + pos, "IDAT", z, zlen);
	pos += sm_chunk(png + pos, "IEND", 0, 0);
	G.plat->free(z);
	if (mmb_vfs_write(path, png, pos, 0) == 0)
		rc = 0;
	G.plat->free(png);
	return rc;
}

static int sm_write_json(const char *path)
{
	char js[512];
	int n = 0, i;
	n += sprintf(js + n, "{\"source\":\"%s\",\"charset\":\"",
		     sm_base(SM.path));
	for (i = 0; SM_CHARSET[i] && n < (int)sizeof(js) - 12; i++)
	{
		if (SM_CHARSET[i] == '"' || SM_CHARSET[i] == '\\')
			js[n++] = '\\';
		js[n++] = SM_CHARSET[i];
	}
	n += sprintf(js + n,
		     "\",\"offsetX\":0,\"offsetY\":0,\"charWidth\":%d,"
		     "\"charHeight\":%d,\"charsPerRow\":%d,\"bgColour\":0}",
		     SM.cell, SM.cell, SM.cols);
	if (mmb_vfs_write(path, js, (unsigned)n, 0) != 0)
		return -1;
	return 0;
}

static void sm_save(void)
{
	int w = SM.cols * SM.cell;
	int h = SM.rows * SM.cell;

	if (sm_write_png(SM.path, w, h) != 0)
	{
		strncpy(SM.status, "Save failed", sizeof(SM.status) - 1);
		return;
	}
	if (SM.mode == SM_FONT)
	{
		char jp[160];
		char *s, *dot = 0;
		strncpy(jp, SM.path, sizeof(jp) - 1);
		jp[sizeof(jp) - 1] = 0;
		for (s = jp; *s; s++)
			if (*s == '.')
				dot = s;
		if (dot && dot > jp)
			strcpy(dot, ".json");
		else
			strncat(jp, ".json", sizeof(jp) - strlen(jp) - 1);
		if (sm_write_json(jp) != 0)
		{
			strncpy(SM.status, "Font JSON failed",
				sizeof(SM.status) - 1);
			SM.status[sizeof(SM.status) - 1] = 0;
			return;
		}
	}
	SM.dirty = 0;
	sprintf(SM.status, "Saved %s", SM.path);
}

static void sm_load(void)
{
	int sz, w = 0, h = 0, i, j, c;
	unsigned got = 0;
	unsigned char *file;
	uint32_t *pix = 0;

	sz = mmb_vfs_size(SM.path);
	if (sz <= 0)
		return;
	file = G.plat->alloc((unsigned)sz + 1);
	if (!file)
		return;
	if (mmb_vfs_read(SM.path, file, (unsigned)sz, &got) != 0 ||
	    mmb_png_decode_rgba(file, got, &pix, &w, &h) != 0)
	{
		G.plat->free(file);
		if (pix)
			G.plat->free(pix);
		strncpy(SM.status, "Load failed", sizeof(SM.status) - 1);
		return;
	}
	G.plat->free(file);
	if (SM.mode == SM_SPRITE)
	{
		if (w == h && w >= 1 && w <= SM_MAX_CELL)
			SM.cell = w;
		SM.cols = 1;
		SM.rows = 1;
		SM.ncells = 1;
	}
	else
	{
		int ic = w / SM.cell;
		int ir = h / SM.cell;
		if (ic < 1 || ir < 1)
		{
			G.plat->free(pix);
			strncpy(SM.status, "Sheet too small", sizeof(SM.status) - 1);
			return;
		}
		if (SM.mode == SM_FONT)
		{
			SM.cols = ic;
			SM.ncells = (int)strlen(SM_CHARSET);
			SM.rows = (SM.ncells + SM.cols - 1) / SM.cols;
		}
		else
		{
			if (ic > SM_MAX_CELLS)
				ic = SM_MAX_CELLS;
			if (ir > SM_MAX_CELLS)
				ir = SM_MAX_CELLS;
			SM.cols = ic;
			SM.rows = ir;
			SM.ncells = ic * ir;
			if (SM.ncells > SM_MAX_CELLS)
				SM.ncells = SM_MAX_CELLS;
		}
	}
	for (c = 0; c < SM.ncells; c++)
	{
		int cc = c % SM.cols;
		int cr = c / SM.cols;
		for (j = 0; j < SM.cell; j++)
			for (i = 0; i < SM.cell; i++)
			{
				int sx = cc * SM.cell + i;
				int sy = cr * SM.cell + j;
				unsigned v;
				if (sx >= w || sy >= h)
					v = 0;
				else
					v = pix[sy * w + sx];
				SM.pix[c][j * SM.cell + i] =
					((v >> 24) == 0)
						? SM_TRANS
						: (unsigned char)sm_nearest(v & 0xFFFFFFu);
			}
	}
	G.plat->free(pix);
}

static void sm_redraw(void)
{
	int cols, rows, cx, cy, i, x, y;
	char line[112];

	if (!G.plat || !G.plat->tui_glyph)
		return;
	cols = tui_cols();
	rows = tui_rows();
	tui_begin();
	mmb_editor_apply_tui_palette();
	tui_clear(TUI_BRWHITE, TUI_BRBLACK);

	sprintf(line, "SPRITE %s  %s  %dx%d%s",
		SM.mode == SM_FONT ? "FONT"
				   : (SM.mode == SM_SHEET ? "SHEET" : "EDIT"),
		SM.label[0] ? SM.label : "untitled", SM.cell, SM.cell,
		SM.dirty ? " *" : "  ");
	tui_pad(0, 0, line, cols, TUI_BRCYAN, TUI_BRBLACK);

	cx = 2;
	cy = 2;
	for (y = 0; y < SM.cell; y++)
		for (x = 0; x < SM.cell; x++)
		{
			unsigned char v = SM.pix[SM.frame][y * SM.cell + x];
			int ch = '.', fg = TUI_BRBLACK, bg = TUI_BLACK;
			if (v < SM_TRANS)
			{
				ch = ' ';
				fg = TUI_WHITE;
				bg = (int)v;
			}
			tui_put(cx + x, cy + y, ch, fg, bg);
		}
	if (SM.cx >= 0 && SM.cx < SM.cell && SM.cy >= 0 && SM.cy < SM.cell)
		tui_cursor(cx + SM.cx, cy + SM.cy, 1);

	{
		int px = cx + SM.cell + 3;
		int py = 2;
		sprintf(line, "CELL  %dx%d", SM.cell, SM.cell);
		tui_puts(px, py++, line, TUI_WHITE, TUI_BRBLACK);
		sprintf(line, "FRAME %d/%d", SM.frame + 1, SM.ncells);
		tui_puts(px, py++, line, TUI_WHITE, TUI_BRBLACK);
		sprintf(line, "COLOUR %d%s", SM.colour,
			SM.colour >= SM_TRANS ? " TRANS" : "");
		tui_puts(px, py++, line, TUI_BRYELLOW, TUI_BRBLACK);
		for (i = 0; i < 16; i++)
		{
			tui_put(px + i, py, ' ', TUI_WHITE, i);
			if (i == SM.colour)
				tui_put(px + i, py, '_', TUI_BRWHITE, i);
		}
		py += 2;
		tui_puts(px, py++, "Arrows move cursor", TUI_WHITE, TUI_BRBLACK);
		tui_puts(px, py++, "Space/Enter set pixel", TUI_WHITE, TUI_BRBLACK);
		tui_puts(px, py++, "0-9 colour  c [ ] cycle", TUI_WHITE, TUI_BRBLACK);
		tui_puts(px, py++, "t transparent  BS erase", TUI_WHITE, TUI_BRBLACK);
		tui_puts(px, py++, "f flip H   v flip V", TUI_WHITE, TUI_BRBLACK);
		tui_puts(px, py++, "b clear    x fill", TUI_WHITE, TUI_BRBLACK);
		tui_puts(px, py++, "n/p scrub  s save", TUI_WHITE, TUI_BRBLACK);
		tui_puts(px, py++, "Esc/Alt+X quit", TUI_WHITE, TUI_BRBLACK);
	}

	{
		int fy = cy + SM.cell + 2;
		if (fy < rows - 1)
		{
			tui_puts(2, fy - 1, "Frames", TUI_BRCYAN, TUI_BRBLACK);
			for (i = 0; i < SM.ncells && 2 + i < cols - 1; i++)
			{
				int any = 0, k;
				for (k = 0; k < SM.cell * SM.cell; k++)
					if (SM.pix[i][k] < SM_TRANS)
					{
						any = 1;
						break;
					}
				tui_put(2 + i, fy, any ? '#' : '.',
					i == SM.frame ? TUI_BLACK : TUI_WHITE,
					i == SM.frame ? TUI_BRYELLOW : TUI_BRBLACK);
			}
		}
	}

	tui_pad(0, rows - 1, SM.status, cols, TUI_BRYELLOW, TUI_BRBLACK);
	tui_flush();
}

static void sm_leave(void)
{
	tui_end();
	mmb_gfx_cls(G.gfx.bg);
	G.home_prompt = 0;
	memset(&SM, 0, sizeof(SM));
	mmb_console_write("\r\n");
	mmb_console_write(mmb_prompt());
}

static void sm_move(int dx, int dy)
{
	SM.cx += dx;
	SM.cy += dy;
	if (SM.cx < 0)
		SM.cx = 0;
	if (SM.cy < 0)
		SM.cy = 0;
	if (SM.cx >= SM.cell)
		SM.cx = SM.cell - 1;
	if (SM.cy >= SM.cell)
		SM.cy = SM.cell - 1;
}

static void sm_toggle(void)
{
	unsigned char *p = &SM.pix[SM.frame][SM.cy * SM.cell + SM.cx];
	*p = (*p == SM.colour) ? SM_TRANS : (unsigned char)SM.colour;
	SM.dirty = 1;
}

static void sm_cycle(int d)
{
	int v = SM.colour;
	if (v > SM_TRANS)
		v = SM_TRANS;
	v += d;
	if (v < 0)
		v = SM_TRANS;
	if (v > SM_TRANS)
		v = 0;
	SM.colour = v;
}

static void sm_flip(int horiz)
{
	int x, y, a, b;
	unsigned char tmp;
	for (y = 0; y < SM.cell; y++)
		for (x = 0; x < SM.cell; x++)
		{
			a = y * SM.cell + x;
			b = horiz ? y * SM.cell + (SM.cell - 1 - x)
				  : (SM.cell - 1 - y) * SM.cell + x;
			if (b > a)
			{
				tmp = SM.pix[SM.frame][a];
				SM.pix[SM.frame][a] = SM.pix[SM.frame][b];
				SM.pix[SM.frame][b] = tmp;
			}
		}
	SM.dirty = 1;
}

static void sm_clear(void)
{
	memset(SM.pix[SM.frame], SM_TRANS, (unsigned)(SM.cell * SM.cell));
	SM.dirty = 1;
}

static void sm_fill(void)
{
	memset(SM.pix[SM.frame], SM.colour, (unsigned)(SM.cell * SM.cell));
	SM.dirty = 1;
}

static void sm_frame(int d)
{
	SM.frame += d;
	if (SM.frame < 0)
		SM.frame = SM.ncells - 1;
	if (SM.frame >= SM.ncells)
		SM.frame = 0;
}

static int sm_escape(char c)
{
	if (SM.esc_state == SM_ESC_GOT)
	{
		if (c == '[')
		{
			SM.esc_state = SM_ESC_CSI;
			return 1;
		}
		if (c == 'O')
		{
			SM.esc_state = SM_ESC_SS3;
			return 1;
		}
		SM.esc_state = SM_ESC_NONE;
		return 0;
	}
	if (SM.esc_state == SM_ESC_SS3)
	{
		SM.esc_state = SM_ESC_NONE;
		return 1;
	}
	if (SM.esc_state == SM_ESC_CSI)
	{
		SM.esc_state = SM_ESC_NONE;
		if (c == 'A')
			sm_move(0, -1);
		else if (c == 'B')
			sm_move(0, 1);
		else if (c == 'C')
			sm_move(1, 0);
		else if (c == 'D')
			sm_move(-1, 0);
		return 1;
	}
	return 0;
}

static void sm_handle(char c)
{
	if (c == ' ' || c == '\r' || c == '\n')
		sm_toggle();
	else if (c == 'c' || c == ']')
		sm_cycle(1);
	else if (c == '[')
		sm_cycle(-1);
	else if (c == 't')
		SM.colour = SM_TRANS;
	else if (c >= '0' && c <= '9')
		SM.colour = c - '0';
	else if (c == 'f')
		sm_flip(1);
	else if (c == 'v')
		sm_flip(0);
	else if (c == 'b')
		sm_clear();
	else if (c == 'x')
		sm_fill();
	else if (c == 'n')
		sm_frame(1);
	else if (c == 'p')
		sm_frame(-1);
	else if (c == 's')
		sm_save();
	else if (c == 8 || c == 127)
		SM.colour = SM_TRANS;
}

const char *mmb_sprite_edit_key(char c)
{
	G.outn = 0;
	G.out[0] = 0;
	if (!SM.active)
		return G.out;
	if (SM.alt_pend)
	{
		SM.alt_pend = 0;
		if (c == 'x' || c == 'X')
		{
			sm_leave();
			return G.out;
		}
	}
	if ((unsigned char)c == 1)
	{
		SM.alt_pend = 1;
		return G.out;
	}
	if (SM.esc_state && sm_escape(c))
	{
		if (SM.active)
			sm_redraw();
		return G.out;
	}
	if (c == 27)
	{
		SM.esc_state = SM_ESC_GOT;
		SM.esc_at = mmb_now_ms();
		return G.out;
	}
	if (c == 24)
	{
		sm_leave();
		return G.out;
	}
	sm_handle(c);
	if (SM.active)
		sm_redraw();
	return G.out;
}

void mmb_sprite_edit_poll(void)
{
	if (!SM.active)
		return;
	if (SM.esc_state == SM_ESC_GOT &&
	    mmb_now_ms() - SM.esc_at >= SM_ESC_IDLE_MS)
	{
		SM.esc_state = SM_ESC_NONE;
		sm_leave();
	}
}

int mmb_in_sprite_edit(void)
{
	return SM.active;
}

static void sm_make_path(char *dst, int dstsz, const char *in, int font)
{
	char tmp[160];

	if (font && !strchr(in, ':') && !strchr(in, '/'))
		sprintf(tmp, "A:/fonts/%s", in);
	else
	{
		strncpy(tmp, in, sizeof(tmp) - 1);
		tmp[sizeof(tmp) - 1] = 0;
	}
	if (!strchr(sm_base(tmp), '.'))
		strncat(tmp, ".png", sizeof(tmp) - strlen(tmp) - 1);
	mmb_vfs_resolve(tmp, dst, dstsz);
}

static void sm_open(const char *in, int mode, int cell, int cols, int rows)
{
	memset(&SM, 0, sizeof(SM));
	memset(SM.pix, SM_TRANS, sizeof(SM.pix));
	SM.active = 1;
	SM.mode = mode;
	SM.cell = (cell >= 1 && cell <= SM_MAX_CELL) ? cell : 8;
	if (mode == SM_FONT)
	{
		SM.cols = SM.cell == 16 ? 20 : 40;
		SM.ncells = (int)strlen(SM_CHARSET);
		SM.rows = (SM.ncells + SM.cols - 1) / SM.cols;
	}
	else if (mode == SM_SHEET)
	{
		SM.cols = (cols >= 1 && cols <= SM_MAX_CELLS) ? cols : 4;
		SM.rows = (rows >= 1 && rows <= SM_MAX_CELLS) ? rows : 4;
		if (SM.cols * SM.rows > SM_MAX_CELLS)
			SM.rows = SM_MAX_CELLS / SM.cols;
		if (SM.rows < 1)
			SM.rows = 1;
		SM.ncells = SM.cols * SM.rows;
	}
	else
	{
		SM.cols = 1;
		SM.rows = 1;
		SM.ncells = 1;
	}
	SM.colour = 15;
	sm_make_path(SM.path, sizeof(SM.path), in, mode == SM_FONT);
	strncpy(SM.label, sm_base(SM.path), sizeof(SM.label) - 1);
	sm_load();
	if (!SM.status[0])
		strncpy(SM.status, "s save   Esc quit", sizeof(SM.status) - 1);
	SM.status[sizeof(SM.status) - 1] = 0;
	mmb_hw_cursor(0);
	sm_redraw();
}

static void sprite_edit_cmd(int mode)
{
	char name[128];
	int cell = 8, cols = 4, rows = 4;
	mmb_val v;

	mmb_skip_sp();
	if (*G.p == 0 || *G.p == ':' || *G.p == '\'')
		mmb_syntax();
	v = mmb_expr();
	if (v.type != T_STR)
		mmb_syntax();
	strncpy(name, v.s, sizeof(name) - 1);
	name[sizeof(name) - 1] = 0;
	mmb_skip_sp();
	if (*G.p == ',')
	{
		G.p++;
		cell = (int)mmb_as_int(mmb_expr());
	}
	if (mode == SM_SHEET)
	{
		mmb_skip_sp();
		if (*G.p == ',')
		{
			G.p++;
			cols = (int)mmb_as_int(mmb_expr());
		}
		mmb_skip_sp();
		if (*G.p == ',')
		{
			G.p++;
			rows = (int)mmb_as_int(mmb_expr());
		}
	}
	sm_open(name, mode, cell, cols, rows);
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
	if (mmb_match("EDIT"))
	{
		sprite_edit_cmd(SM_SPRITE);
		return;
	}
	if (mmb_match("SHEET"))
	{
		sprite_edit_cmd(SM_SHEET);
		return;
	}
	if (mmb_match("FONT"))
	{
		sprite_edit_cmd(SM_FONT);
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
				int p = order_pos(ix);
				if (p >= 0)
				{
					restore_from_top_to(p);
					order_remove(ix);
					show_from(p);
				}
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
