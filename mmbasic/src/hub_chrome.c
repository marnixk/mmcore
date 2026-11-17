/*
 * HUB chrome sprites. The PNGs live in ramdisk/hub/ and are seeded onto A:
 * by scripts/gen_ramdisk.py. Names are "<piece>-1.png" and "<piece>-2.png";
 * the loader picks the variant for the pixel unit p and nearest-neighbour
 * scales it by an integer so the art stays on the pixel grid.
 */
#include "mmb_priv.h"
#include "hub_chrome.h"

#include <stdio.h>
#include <string.h>

#define CHROME_MAX 32

static const char *const k_names[] = {
	"loading-cube", "load-title", "header-cube", "wordmark", "icon-cube",
	"badge-installed", "badge-update", "badge-needs",
	"tab", "tab-on", "button", "bar-frame", "bar-fill", "bar-stripe",
	"corner-tl", "corner-tr", "corner-bl", "corner-br", "edge-h", "edge-v",
	"scroll-track", "scroll-thumb", "title-font", "ui-font",
	0
};

/* One cell per character, two rows (chrome then purple). */
static const char k_font[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 .,'!?:-+/()";

/* Monospace UI face. Same order as scripts/gen_hub_assets.py UI_ORDER. */
static const char k_ui[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
	"0123456789 .,'!?:-+/()%&#\"";

typedef struct chrome_img {
	const char *name;
	uint32_t *pix;
	int w, h;
	int scale;
	int variant;
	int tried;
} chrome_img;

static chrome_img s_img[CHROME_MAX];
static int s_nimg;
static int s_p;
static int s_clip;
static int s_x0, s_y0, s_x1, s_y1;

static int chrome_variant(int p)
{
	if (p >= 2 && (p % 2) == 0)
		return 2;
	return 1;
}

static int chrome_scale(int p, int variant)
{
	int s = p / variant;

	return s < 1 ? 1 : s;
}

static chrome_img *chrome_find(const char *name)
{
	int i;

	for (i = 0; i < s_nimg; i++)
		if (s_img[i].name && strcmp(s_img[i].name, name) == 0)
			return &s_img[i];
	return 0;
}

static void chrome_plot(int x, int y, unsigned rgb, unsigned a)
{
	unsigned dst, sr, sg, sb, dr, dg, db;

	if (!a)
		return;
	if (s_clip && (x < s_x0 || y < s_y0 || x >= s_x1 || y >= s_y1))
		return;
	if (a >= 250)
	{
		mmb_gfx_plot(x, y, rgb & 0xFFFFFFu);
		return;
	}
	dst = mmb_gfx_get(x, y);
	sr = (rgb >> 16) & 255u;
	sg = (rgb >> 8) & 255u;
	sb = rgb & 255u;
	dr = (dst >> 16) & 255u;
	dg = (dst >> 8) & 255u;
	db = dst & 255u;
	dr = (sr * a + dr * (255u - a)) / 255u;
	dg = (sg * a + dg * (255u - a)) / 255u;
	db = (sb * a + db * (255u - a)) / 255u;
	mmb_gfx_plot(x, y, (dr << 16) | (dg << 8) | db);
}

static void chrome_stretch(chrome_img *im, int sx0, int sy0, int sw, int sh,
			   int dx, int dy, int dw, int dh)
{
	int y, x;

	if (!im || !im->pix || sw < 1 || sh < 1 || dw < 1 || dh < 1)
		return;
	for (y = 0; y < dh; y++)
	{
		int sy = sy0 + (int)((long)y * sh / dh);

		if (sy < 0)
			sy = 0;
		if (sy >= im->h)
			sy = im->h - 1;
		for (x = 0; x < dw; x++)
		{
			int sx = sx0 + (int)((long)x * sw / dw);
			uint32_t p;

			if (sx < 0)
				sx = 0;
			if (sx >= im->w)
				sx = im->w - 1;
			p = im->pix[sy * im->w + sx];
			chrome_plot(dx + x, dy + y, p & 0xFFFFFFu, p >> 24);
		}
	}
}

static int chrome_load_one(chrome_img *im, int p)
{
	const unsigned char *data = 0;
	unsigned n = 0;
	char path[96];
	int variant = chrome_variant(p);
	int scale = chrome_scale(p, variant);
	uint32_t *pix = 0;
	int w = 0, h = 0;

	snprintf(path, sizeof(path), "A:/hub/%s-%d.png", im->name, variant);
	if (mmb_vfs_read_ptr(path, &data, &n) != 0 || !data || !n)
	{
		if (variant == 2)
		{
			variant = 1;
			scale = p < 1 ? 1 : p;
			snprintf(path, sizeof(path), "A:/hub/%s-1.png", im->name);
			if (mmb_vfs_read_ptr(path, &data, &n) != 0 || !data)
				return 0;
		}
		else
			return 0;
	}
	if (mmb_png_decode_rgba(data, n, &pix, &w, &h) != 0 || !pix)
		return 0;
	im->pix = pix;
	im->w = w;
	im->h = h;
	im->scale = scale;
	im->variant = variant;
	return 1;
}

void hub_chrome_free(void)
{
	int i;

	for (i = 0; i < s_nimg; i++)
	{
		if (s_img[i].pix)
			G.plat->free(s_img[i].pix);
		s_img[i].pix = 0;
		s_img[i].tried = 0;
		s_img[i].w = s_img[i].h = 0;
	}
	s_p = 0;
}

void hub_chrome_load(int p)
{
	int i;

	if (p < 1)
		p = 1;
	if (s_p == p && s_nimg > 0 && s_img[0].tried)
		return;
	hub_chrome_free();
	s_p = p;
	s_nimg = 0;
	for (i = 0; k_names[i]; i++)
	{
		chrome_img *im;

		if (s_nimg >= CHROME_MAX)
			break;
		im = &s_img[s_nimg++];
		memset(im, 0, sizeof(*im));
		im->name = k_names[i];
		im->tried = 1;
		chrome_load_one(im, p);
	}
}

void hub_chrome_clip(int x0, int y0, int x1, int y1)
{
	s_clip = 1;
	s_x0 = x0;
	s_y0 = y0;
	s_x1 = x1;
	s_y1 = y1;
}

void hub_chrome_unclip(void)
{
	s_clip = 0;
}

int hub_chrome_has(const char *name)
{
	chrome_img *im = chrome_find(name);

	return im && im->pix;
}

int hub_chrome_w(const char *name)
{
	chrome_img *im = chrome_find(name);

	return (im && im->pix) ? im->w * im->scale : 0;
}

int hub_chrome_h(const char *name)
{
	chrome_img *im = chrome_find(name);

	return (im && im->pix) ? im->h * im->scale : 0;
}

int hub_chrome_blit(const char *name, int x, int y)
{
	chrome_img *im = chrome_find(name);

	if (!im || !im->pix)
		return 0;
	chrome_stretch(im, 0, 0, im->w, im->h, x, y,
		       im->w * im->scale, im->h * im->scale);
	return 1;
}

int hub_chrome_fit(const char *name, int x, int y, int w, int h, int pct)
{
	chrome_img *im = chrome_find(name);
	int dw;

	if (!im || !im->pix || w < 1 || h < 1)
		return 0;
	if (pct < 0)
		pct = 0;
	if (pct > 100)
		pct = 100;
	dw = w * pct / 100;
	if (pct > 0 && dw < 1)
		dw = 1;
	if (dw < 1)
		return 1;
	/* Sample the matching left portion of the source so a partial bar
	 * keeps the segment size instead of squashing the whole strip. */
	chrome_stretch(im, 0, 0, im->w * pct / 100 > 0 ? im->w * pct / 100 : 1,
		       im->h, x, y, dw, h);
	return 1;
}

int hub_chrome_slice(const char *name, int x, int y, int w, int h, int slice2)
{
	chrome_img *im = chrome_find(name);
	int sl, dl, yy, xx;

	if (!im || !im->pix || w < 4 || h < 4)
		return 0;
	/* slice2 is in 2x pixels. A 1x asset is half of that. */
	sl = slice2 * (im->variant < 1 ? 1 : im->variant) / 2;
	if (sl < 1)
		sl = 1;
	if (sl * 2 >= im->w)
		sl = im->w / 3;
	if (sl * 2 >= im->h)
		sl = im->h / 3;
	if (sl < 1)
		sl = 1;
	dl = sl * im->scale;
	if (dl * 2 >= w)
		dl = w / 3;
	if (dl * 2 >= h)
		dl = h / 3;
	if (dl < 1)
		dl = 1;
	for (yy = 0; yy < h; yy++)
	{
		int sy, src_h;

		if (yy < dl)
		{
			sy = yy * sl / dl;
			src_h = 1;
		}
		else if (yy >= h - dl)
		{
			sy = im->h - sl + (yy - (h - dl)) * sl / dl;
			src_h = 1;
		}
		else
		{
			int mid = h - 2 * dl;

			sy = sl + (mid > 0 ? (yy - dl) * (im->h - 2 * sl) / mid : 0);
			src_h = 1;
		}
		(void)src_h;
		if (sy < 0)
			sy = 0;
		if (sy >= im->h)
			sy = im->h - 1;
		for (xx = 0; xx < w; xx++)
		{
			int sx;
			uint32_t p;

			if (xx < dl)
				sx = xx * sl / dl;
			else if (xx >= w - dl)
				sx = im->w - sl + (xx - (w - dl)) * sl / dl;
			else
			{
				int mid = w - 2 * dl;

				sx = sl + (mid > 0 ? (xx - dl) * (im->w - 2 * sl) / mid : 0);
			}
			if (sx < 0)
				sx = 0;
			if (sx >= im->w)
				sx = im->w - 1;
			p = im->pix[sy * im->w + sx];
			chrome_plot(x + xx, y + yy, p & 0xFFFFFFu, p >> 24);
		}
	}
	return 1;
}

static void chrome_tile(const char *name, int x, int y, int w, int h)
{
	chrome_img *im = chrome_find(name);
	int tw, th, yy, xx;

	if (!im || !im->pix || w < 1 || h < 1)
		return;
	tw = im->w * im->scale;
	th = im->h * im->scale;
	if (tw < 1)
		tw = 1;
	if (th < 1)
		th = 1;
	for (yy = 0; yy < h; yy += th)
		for (xx = 0; xx < w; xx += tw)
		{
			int dw = tw, dh = th;

			int sw, sh;

			if (xx + dw > w)
				dw = w - xx;
			if (yy + dh > h)
				dh = h - yy;
			sw = dw * im->w / tw;
			sh = dh * im->h / th;
			if (sw < 1)
				sw = 1;
			if (sh < 1)
				sh = 1;
			chrome_stretch(im, 0, 0, sw, sh, x + xx, y + yy, dw, dh);
		}
}

int hub_chrome_frame(int x, int y, int w, int h)
{
	int cw, ch, ew, eh;

	if (!hub_chrome_has("corner-tl") || w < 16 || h < 16)
		return 0;
	cw = hub_chrome_w("corner-tl");
	ch = hub_chrome_h("corner-tl");
	if (cw * 2 > w)
		cw = w / 3;
	if (ch * 2 > h)
		ch = h / 3;
	hub_chrome_fit("corner-tl", x, y, cw, ch, 100);
	hub_chrome_fit("corner-tr", x + w - cw, y, cw, ch, 100);
	hub_chrome_fit("corner-bl", x, y + h - ch, cw, ch, 100);
	hub_chrome_fit("corner-br", x + w - cw, y + h - ch, cw, ch, 100);
	ew = hub_chrome_w("edge-h");
	eh = hub_chrome_h("edge-h");
	if (eh < 1)
		eh = ch / 4;
	if (eh > ch)
		eh = ch;
	chrome_tile("edge-h", x + cw, y, w - 2 * cw, eh > 0 ? eh : 2);
	chrome_tile("edge-h", x + cw, y + h - (eh > 0 ? eh : 2), w - 2 * cw,
		    eh > 0 ? eh : 2);
	ew = hub_chrome_w("edge-v");
	eh = hub_chrome_h("edge-v");
	(void)eh;
	if (ew < 1)
		ew = cw / 4;
	if (ew > cw)
		ew = cw;
	chrome_tile("edge-v", x, y + ch, ew > 0 ? ew : 2, h - 2 * ch);
	chrome_tile("edge-v", x + w - (ew > 0 ? ew : 2), y + ch,
		    ew > 0 ? ew : 2, h - 2 * ch);
	(void)ew;
	return 1;
}

int hub_chrome_scroll(int x, int y, int w, int h, int thumb_y, int thumb_h)
{
	if (!hub_chrome_has("scroll-track") || h < 4)
		return 0;
	hub_chrome_slice("scroll-track", x, y, w, h, 8);
	if (thumb_h > 0 && hub_chrome_has("scroll-thumb"))
		hub_chrome_fit("scroll-thumb", x, y + thumb_y, w, thumb_h, 100);
	return 1;
}

static int font_cell(chrome_img *im, int *cw, int *ch)
{
	int n = (int)strlen(k_font);

	if (!im || !im->pix || n < 1 || im->h < 2)
		return 0;
	*cw = im->w / n;
	*ch = im->h / 2;
	return *cw > 0 && *ch > 0;
}

static int font_index(char ch)
{
	const char *p;

	if (ch >= 'a' && ch <= 'z')
		ch = (char)(ch - 'a' + 'A');
	p = strchr(k_font, ch);
	return p ? (int)(p - k_font) : -1;
}

static int font_advance(chrome_img *im, int index, int cell_w, int cell_h, int row)
{
	int x, y, maxx = 0;
	int sy0 = row * cell_h;

	for (x = cell_w - 1; x >= 0; x--)
	{
		for (y = 0; y < cell_h; y++)
		{
			uint32_t p = im->pix[(sy0 + y) * im->w + index * cell_w + x];

			if (p >> 24)
			{
				maxx = x + 2;
				return maxx * im->scale;
			}
		}
	}
	return cell_w * im->scale / 3;
}

int hub_chrome_title_h(void)
{
	chrome_img *im = chrome_find("title-font");
	int cw, ch;

	if (!font_cell(im, &cw, &ch))
		return 0;
	return ch * im->scale;
}

int hub_chrome_title_w(const char *text, int row)
{
	chrome_img *im = chrome_find("title-font");
	int cw, ch, width = 0;
	const char *s;

	if (!text || !font_cell(im, &cw, &ch))
		return 0;
	if (row < 0)
		row = 0;
	if (row > 1)
		row = 1;
	for (s = text; *s; s++)
	{
		int idx = font_index(*s);

		if (idx < 0)
			idx = font_index(' ');
		if (idx < 0)
			continue;
		width += font_advance(im, idx, cw, ch, row);
	}
	return width;
}

int hub_chrome_title(const char *text, int x, int y, int row)
{
	chrome_img *im = chrome_find("title-font");
	int cw, ch, pen = x;
	const char *s;

	if (!text || !font_cell(im, &cw, &ch))
		return 0;
	if (row < 0)
		row = 0;
	if (row > 1)
		row = 1;
	for (s = text; *s; s++)
	{
		int idx = font_index(*s);
		int adv;

		if (idx < 0)
			idx = font_index(' ');
		if (idx < 0)
			continue;
		adv = font_advance(im, idx, cw, ch, row);
		chrome_stretch(im, idx * cw, row * ch, cw, ch, pen, y,
			       cw * im->scale, ch * im->scale);
		pen += adv;
	}
	return pen - x;
}

int hub_chrome_badge(const char *label, int x, int y, int w, int h)
{
	const char *name = 0;

	if (!label || w < 4 || h < 4)
		return 0;
	if (strncmp(label, "INSTALLED", 9) == 0)
		name = "badge-installed";
	else if (strncmp(label, "UPDATE", 6) == 0)
		name = "badge-update";
	else if (strncmp(label, "NEEDS", 5) == 0)
		name = "badge-needs";
	if (!name)
		return 0;
	return hub_chrome_slice(name, x, y, w, h, 10);
}

static int ui_cell(chrome_img *im, int *cw, int *ch)
{
	int n = (int)strlen(k_ui);

	if (!im || !im->pix || n < 1 || im->h < 1)
		return 0;
	*cw = im->w / n;
	*ch = im->h;
	return *cw > 0 && *ch > 0;
}

static int ui_index(char ch)
{
	const char *p = strchr(k_ui, ch);

	return p ? (int)(p - k_ui) : -1;
}

static void chrome_stretch_tint(chrome_img *im, int sx0, int sy0, int sw, int sh,
				int dx, int dy, int dw, int dh, unsigned rgb)
{
	int y, x;

	if (!im || !im->pix || sw < 1 || sh < 1 || dw < 1 || dh < 1)
		return;
	for (y = 0; y < dh; y++)
	{
		int sy = sy0 + (int)((long)y * sh / dh);

		if (sy < 0)
			sy = 0;
		if (sy >= im->h)
			sy = im->h - 1;
		for (x = 0; x < dw; x++)
		{
			int sx = sx0 + (int)((long)x * sw / dw);
			uint32_t p;

			if (sx < 0)
				sx = 0;
			if (sx >= im->w)
				sx = im->w - 1;
			p = im->pix[sy * im->w + sx];
			chrome_plot(dx + x, dy + y, rgb, p >> 24);
		}
	}
}

int hub_chrome_ui_h(void)
{
	chrome_img *im = chrome_find("ui-font");
	int cw, ch;

	if (!ui_cell(im, &cw, &ch))
		return 0;
	return ch * im->scale;
}

int hub_chrome_ui_w(const char *text, int zoom)
{
	chrome_img *im = chrome_find("ui-font");
	int cw, ch;

	if (!text || !ui_cell(im, &cw, &ch))
		return 0;
	if (zoom < 1)
		zoom = 1;
	return (int)strlen(text) * cw * im->scale * zoom;
}

int hub_chrome_ui(const char *text, int x, int y, unsigned rgb, int zoom)
{
	chrome_img *im = chrome_find("ui-font");
	int cw, ch, pen, dw, dh;
	const char *s;

	if (!text || !ui_cell(im, &cw, &ch))
		return 0;
	if (zoom < 1)
		zoom = 1;
	dw = cw * im->scale * zoom;
	dh = ch * im->scale * zoom;
	pen = x;
	for (s = text; *s; s++)
	{
		int idx = ui_index(*s);

		if (idx < 0)
			idx = ui_index(' ');
		if (idx < 0)
			continue;
		chrome_stretch_tint(im, idx * cw, 0, cw, ch, pen, y, dw, dh, rgb);
		pen += dw;
	}
	return pen - x;
}
