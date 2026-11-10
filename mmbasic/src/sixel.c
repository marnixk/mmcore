/* Streaming sixel decoder.  See sixel.h and docs/analysis/term-sixel.md.
 *
 * One image at a time, no payload buffering.  The caller provides the target
 * plane and the pack hook; this file only understands the sixel grammar.
 */
#include "sixel.h"

#include <string.h>

/* VT340 default palette, as libsixel ships it.  Registers 16..255 start
 * black.  The table is reloaded for every image so definitions never leak
 * from one image into the next. */
static const unsigned s_vt340[16] = {
	0x000000u, 0x3333CCu, 0xCC2121u, 0x33CC33u,
	0xCC33CCu, 0x33CCCCu, 0xCCCC33u, 0x777777u,
	0x444444u, 0x000099u, 0x990000u, 0x009900u,
	0x990099u, 0x009999u, 0x999900u, 0xCCCCCCu
};

static uint16_t six_pack(sixel_image *s, unsigned rgb)
{
	return s->pack ? s->pack(rgb) : 0;
}

/* DEC HLS: hue 0 is blue, not red.  Rotate into the standard HSL wheel and
 * use the usual sector formula.  Saturation 0 is grey at L*255/100. */
static unsigned six_hls_to_rgb(int hue, int lum, int sat)
{
	double h, l, sl, c, x, m, r, g, b, hp, f, d;
	int sector, q;

	if (hue < 0)
		hue = 0;
	hue %= 360;
	if (lum < 0)
		lum = 0;
	if (lum > 100)
		lum = 100;
	if (sat < 0)
		sat = 0;
	if (sat > 100)
		sat = 100;

	h = (double)((hue + 240) % 360);
	l = (double)lum / 100.0;
	sl = (double)sat / 100.0;
	d = l * 2.0 - 1.0;
	if (d < 0.0)
		d = -d;
	c = (1.0 - d) * sl;
	hp = h / 60.0;
	q = (int)hp;
	f = hp - 2.0 * (double)(q / 2);
	d = f - 1.0;
	if (d < 0.0)
		d = -d;
	x = c * (1.0 - d);
	sector = q % 6;
	r = g = b = 0.0;
	switch (sector)
	{
	case 0: r = c; g = x; break;
	case 1: r = x; g = c; break;
	case 2: g = c; b = x; break;
	case 3: g = x; b = c; break;
	case 4: r = x; b = c; break;
	default: r = c; b = x; break;
	}
	m = l - c / 2.0;
	r += m;
	g += m;
	b += m;
	{
		int ri = (int)(r * 255.0 + 0.5);
		int gi = (int)(g * 255.0 + 0.5);
		int bi = (int)(b * 255.0 + 0.5);

		if (ri < 0) ri = 0;
		if (ri > 255) ri = 255;
		if (gi < 0) gi = 0;
		if (gi > 255) gi = 255;
		if (bi < 0) bi = 0;
		if (bi > 255) bi = 255;
		return ((unsigned)ri << 16) | ((unsigned)gi << 8) | (unsigned)bi;
	}
}

static int six_pct(int v)
{
	if (v < 0)
		v = 0;
	if (v > 100)
		v = 100;
	return v * 255 / 100;
}

static void six_put(sixel_image *s, int x, int y, uint16_t c);

static void six_fill(sixel_image *s, int x, int y, int w, int h, uint16_t c)
{
	int i, j;

	for (j = 0; j < h; j++)
		for (i = 0; i < w; i++)
			six_put(s, x + i, y + j, c);
}

static void six_put(sixel_image *s, int x, int y, uint16_t c)
{
	/* A failed plane allocation still runs the grammar, but writes
	 * nothing.  The caller discards the image and the payload is never
	 * printed as text. */
	if (!s->pix)
		return;
	if (x < s->ox || y < s->oy || x >= s->ox + s->maxw ||
	    y >= s->oy + s->maxh || x >= s->width || y >= s->height)
		return;
	s->pix[y * s->stride + x] = c;
	if (y + 1 > s->bottom)
		s->bottom = y + 1;
	if (x + 1 > s->right)
		s->right = x + 1;
	if (s->cover && s->cell_w > 0 && s->cell_h > 0)
	{
		int cc = x / s->cell_w;
		int cr = y / s->cell_h;

		if (cr >= 0 && cc >= 0)
			s->cover[cr * s->cover_cols + cc] = 1;
	}
}

/* One sixel character: six vertical pixels in the active register.
 * bit 0 is the top pixel.  A 0 bit paints the captured background unless P2
 * is 1 (transparent). */
static void six_char(sixel_image *s, unsigned v)
{
	int bit;

	for (bit = 0; bit < 6; bit++)
	{
		if (v & (1u << bit))
			six_put(s, s->x, s->y + bit, s->pal[s->reg]);
		else if (s->p2 != 1)
			six_put(s, s->x, s->y + bit, s->bg);
	}
	s->x++;
}

static void six_params_reset(sixel_image *s)
{
	int i;

	s->npar = 0;
	s->cur = 0;
	s->have_cur = 0;
	for (i = 0; i < 6; i++)
		s->par[i] = 0;
}

static void six_push_param(sixel_image *s)
{
	if (s->npar < 6)
		s->par[s->npar] = (int)s->cur;
	s->npar++;
	s->cur = 0;
	s->have_cur = 0;
}

static void six_digit(sixel_image *s, unsigned char b)
{
	uint32_t d = (uint32_t)(b - '0');

	if (s->cur > (0xFFFFFFFFu - d) / 10u)
		s->cur = 0xFFFFFFFFu;
	else
		s->cur = s->cur * 10u + d;
	s->have_cur = 1;
}

static void six_finish_hash(sixel_image *s)
{
	int pc, pu;

	if (s->have_cur)
		six_push_param(s);
	pc = s->npar > 0 ? s->par[0] : 0;
	if (pc < 0)
		pc = 0;
	if (pc > 255)
		pc = 255;
	s->reg = pc;
	if (s->npar >= 2)
	{
		pu = s->par[1];
		if (pu == 2)
		{
			int r = six_pct(s->npar > 2 ? s->par[2] : 0);
			int g = six_pct(s->npar > 3 ? s->par[3] : 0);
			int b = six_pct(s->npar > 4 ? s->par[4] : 0);

			s->pal[pc] = six_pack(s, ((unsigned)r << 16) |
						  ((unsigned)g << 8) |
						  (unsigned)b);
		}
		else if (pu == 1)
		{
			int h = s->npar > 2 ? s->par[2] : 0;
			int l = s->npar > 3 ? s->par[3] : 0;
			int sat = s->npar > 4 ? s->par[4] : 0;

			s->pal[pc] = six_pack(s, six_hls_to_rgb(h, l, sat));
		}
	}
}

static void six_finish_raster(sixel_image *s)
{
	int have_w = 0, have_h = 0;
	int bx = s->x - s->ox;   /* image start relative to the clip origin */
	int by = s->y - s->oy;
	int fw = 0, fh = 0;

	if (s->have_cur)
		six_push_param(s);
	if (bx < 0)
		bx = 0;
	if (by < 0)
		by = 0;
	if (s->npar >= 3 && s->par[2] > 0)
	{
		int w = s->par[2];

		if (w > s->width - s->ox - bx)
			w = s->width - s->ox - bx;
		if (w < 0)
			w = 0;
		s->maxw = bx + w;
		fw = w;
		have_w = 1;
	}
	if (s->npar >= 4 && s->par[3] > 0)
	{
		int h = s->par[3];

		if (h > s->height - s->oy - by)
			h = s->height - s->oy - by;
		if (h < 0)
			h = 0;
		/* The clip box is image-relative.  TERM may start the image
		 * below the top of the pane, so include that offset or the
		 * whole raster is clipped away. */
		s->maxh = by + h;
		s->advance_h = h;
		fh = h;
		have_h = 1;
	}
	s->have_raster = have_w && have_h;
	/* Pan/Pad are pixel aspect, ignored: HDMI pixels are square. */
	if (s->have_raster && s->p2 != 1)
		six_fill(s, s->x, s->y, fw, fh, s->bg);
}

static void six_repeat(sixel_image *s, uint32_t n, unsigned char b)
{
	uint32_t i;
	int avail;

	if (b < 0x3F || b > 0x7E)
		return;
	avail = s->maxw - (s->x - s->ox);
	if (avail < 0)
		avail = 0;
	if (n > (uint32_t)avail)
		n = (uint32_t)avail;
	for (i = 0; i < n; i++)
		six_char(s, (unsigned)(b - 0x3F));
}

void sixel_begin(sixel_image *s, const sixel_config *cfg)
{
	int i;

	memset(s, 0, sizeof(*s));
	s->pix = cfg->pix;
	s->stride = cfg->stride;
	s->width = cfg->width;
	s->height = cfg->height;
	s->ox = cfg->ox;
	s->oy = cfg->oy;
	s->p2 = (cfg->p2 == 1) ? 1 : 0;
	s->bg = cfg->bg;
	s->pack = cfg->pack;
	s->cover = cfg->cover;
	s->cover_cols = cfg->cover_cols;
	s->cell_w = cfg->cell_w;
	s->cell_h = cfg->cell_h;
	for (i = 0; i < 16; i++)
		s->pal[i] = six_pack(s, s_vt340[i]);
	for (i = 16; i < 256; i++)
		s->pal[i] = six_pack(s, 0);
	s->reg = 0;
	s->x = cfg->ox;
	s->y = cfg->oy;
	s->maxw = cfg->width - cfg->ox;
	s->maxh = cfg->height - cfg->oy;
	if (s->maxw < 0)
		s->maxw = 0;
	if (s->maxh < 0)
		s->maxh = 0;
	s->bottom = cfg->oy;
	s->right = cfg->ox;
	s->st = SIXEL_DATA;
	six_params_reset(s);
}

void sixel_feed(sixel_image *s, unsigned char b)
{
	if (s->st == SIXEL_HASH || s->st == SIXEL_RASTER)
	{
		if (b >= '0' && b <= '9')
		{
			six_digit(s, b);
			return;
		}
		if (b == ';')
		{
			six_push_param(s);
			return;
		}
		if (s->st == SIXEL_HASH)
			six_finish_hash(s);
		else
			six_finish_raster(s);
		s->st = SIXEL_DATA;
		six_params_reset(s);
		/* Fall through and interpret this byte as data. */
	}
	else if (s->st == SIXEL_REPEAT)
	{
		uint32_t n;

		if (b >= '0' && b <= '9')
		{
			six_digit(s, b);
			return;
		}
		n = s->cur;
		s->st = SIXEL_DATA;
		six_params_reset(s);
		if (b >= 0x3F && b <= 0x7E)
		{
			six_repeat(s, n, b);
			return;
		}
		/* Not a sixel character: drop the pending repeat and treat the
		 * byte normally. */
	}

	switch (b)
	{
	case '#':
		s->st = SIXEL_HASH;
		six_params_reset(s);
		break;
	case '!':
		s->st = SIXEL_REPEAT;
		six_params_reset(s);
		break;
	case '"':
		s->st = SIXEL_RASTER;
		six_params_reset(s);
		break;
	case '$':
		s->x = s->ox;
		break;
	case '-':
		s->y += 6;
		s->x = s->ox;
		break;
	default:
		if (b >= 0x3F && b <= 0x7E)
			six_char(s, (unsigned)(b - 0x3F));
		break;
	}
}

void sixel_finish(sixel_image *s)
{
	if (!s->have_raster)
		s->advance_h = s->bottom > s->oy ? s->bottom - s->oy : 0;
	s->painted_w = s->right > s->ox ? s->right - s->ox : 0;
	s->painted_h = s->bottom > s->oy ? s->bottom - s->oy : 0;
}
