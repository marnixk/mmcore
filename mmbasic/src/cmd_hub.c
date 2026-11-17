/*
 * HUB screens (#1526): the full-screen pixel-art UI for the built-in HUB
 * command, drawn on top of the engine in hub.c (#1525).
 *
 * The look is a pixel-art demoscene diskmag: dark navy dither, bevelled
 * chrome panels, purple/magenta accents with cyan highlights and chunky
 * integer-scaled fonts. Every region is laid out in multiples of an integer
 * pixel unit p (hub_layout.c) so the art stays crisp at any resolution.
 *
 * The screen takeover and restore follow JUKE: save the mode/depth and font
 * scale, raise a low palette to 16-bit, then hand the console back through
 * mmb_gfx_reset_console() on exit.
 */
#include "mmb_priv.h"
#include "frontend.h"
#include "session.h"
#include "hub.h"
#include "hub_layout.h"
#include "pkg_info.h"
#include "app_category.h"

#include <stdio.h>
#include <string.h>

#define HUB_BG        0x0A0E1Fu
#define HUB_BG2       0x0D1228u
#define HUB_PANEL     0x12162Bu
#define HUB_PANEL2    0x171C35u
#define HUB_BORDER_D  0x3A3F5Cu
#define HUB_BORDER_L  0xC8CCE0u
#define HUB_PURPLE    0x7B3FE4u
#define HUB_PURPLE_D  0x3A1A6Eu
#define HUB_MAGENTA   0xE040FBu
#define HUB_CYAN      0x29D3F0u
#define HUB_CYAN_D    0x14657Au
#define HUB_LIME      0x8BE04Au
#define HUB_LIME_D    0x3E6A20u
#define HUB_ORANGE    0xF0A030u
#define HUB_ORANGE_D  0x6E4510u
#define HUB_RED       0xFF4A4Au
#define HUB_RED_D     0x6E1E1Eu
#define HUB_TEXT      0xE8E8F0u
#define HUB_DIM       0x8890A8u
#define HUB_GREY      0x4A5068u
#define HUB_BRONZE    0xB07030u

#define HUB_ESC_IDLE_MS 60
#define HUB_LOAD_DELAY_MS 700
#define HUB_FRAME_MS 45
#define HUB_ASSET_MS 25

enum {
	HUB_ST_LOADING = 0,
	HUB_ST_LIST,
	HUB_ST_DETAIL,
	HUB_ST_INSTALL,
	HUB_ST_INSTALL_FAIL,
	HUB_ST_REMOVE
};

typedef struct hub_img {
	char *rel;      /* owned copy; NULL for the empty slot */
	uint32_t *pix;  /* RGBA, 0 when the fetch/decode failed */
	int w, h;
} hub_img;

typedef struct hub_box {
	hub_rect r;
	int value;
} hub_box;

typedef struct hub_ui {
	int active;
	int saved_mode, saved_bits, saved_font_scale;
	int w, h, p;
	hub_layout L;

	int state;       /* HUB_ST_* */
	int prev_state;  /* screen under a modal */
	int started;     /* ms when the loading screen opened */
	int load_started;
	int load_done;
	int icon_stage;  /* icons left to fetch on the loading screen */
	int icon_total;
	int err;
	char errmsg[192];

	int tab;         /* -1 = ALL, else an app category index */
	int *ids;        /* app indices in display order */
	int nids, idcap;
	int sel;
	int list_top;

	int detail_shot;
	int about_scroll;

	int remove_focus; /* 0 = YES, 1 = NO */
	int disp_mode;

	int inst_index;
	unsigned inst_done, inst_total;
	int inst_checksum; /* 0 .., 1 OK, 2 FAILED */
	int inst_cancel;   /* cancel requested while the transfer blocks */
	int inst_pump;     /* guard: the install input pump is not re-entrant */
	char inst_target[256];

	int mx, my, buttons, wheel;

	int esc;
	unsigned esc_at, esc_arg;
	unsigned last_frame, last_asset, last_load;

	int dirty;
	int layout_w, layout_h;
} hub_ui;

static hub_ui U;

/* Record a cancel request for the in-flight install/update transfer. While
 * hub_install() blocks in the HTTP client the modal must keep its state, so
 * this only raises a flag the progress callback observes; hub_start_install()
 * unwinds to the previous screen once the transfer has been abandoned. */
static void hub_request_install_cancel(void)
{
	if (U.state != HUB_ST_INSTALL || U.inst_cancel)
		return;
	U.inst_cancel = 1;
	U.dirty = 1;
}

static void hub_install_pump(void);

static hub_img *s_img;
static int s_nimg, s_imgcap;

static hub_box s_tabbox[32];
static int s_ntabbox;
static hub_box s_rowbox[256];
static int s_nrowbox;
static hub_box s_shotbox[32];
static int s_nshotbox;
static hub_rect s_mainbtn;
static int s_has_mainbtn;
static hub_rect s_modal_yes, s_modal_no, s_modal_cancel, s_modal_ok;
static int s_has_yesno, s_has_cancel, s_has_ok;

/* Tab labels, in fixed category order (ALL first). */
typedef struct hub_tab {
	int cat;
	char name[20];
} hub_tab;
static hub_tab s_tabs[MMB_APP_CATEGORY_COUNT + 1];
static int s_ntabs;

/* ---- text ------------------------------------------------------------ */

static void hub_text_n(int x, int y, const char *s, unsigned n, unsigned col,
		       int scale)
{
	int cx = x, i;

	if (scale < 1)
		scale = 1;
	for (i = 0; i < (int)n; i++)
	{
		unsigned char ch = (unsigned char)s[i];
		int row, colbit;

		if (ch == '\n' || ch == '\r')
			continue;
		for (row = 0; row < 16; row++)
		{
			unsigned char bits = mmb_cp437_8x16[ch * 16u + row];

			for (colbit = 0; colbit < 8; colbit++)
			{
				if (bits & (unsigned char)(0x80u >> colbit))
				{
					int sy, sx;

					for (sy = 0; sy < scale; sy++)
						for (sx = 0; sx < scale; sx++)
							mmb_gfx_plot(x + i * 8 * scale +
									     colbit * scale + sx,
								     y + row * scale + sy,
								     col);
				}
			}
		}
		cx = x + i * 8 * scale;
	}
	(void)cx;
}

static int hub_text_w(const char *s, int scale)
{
	if (!s)
		return 0;
	return (int)strlen(s) * 8 * scale;
}

static void hub_text(int x, int y, const char *s, unsigned col, int scale)
{
	if (!s)
		return;
	hub_text_n(x, y, s, (unsigned)strlen(s), col, scale);
}

static void hub_text_r(int right, int y, const char *s, unsigned col,
		       int scale)
{
	int w = hub_text_w(s, scale);

	hub_text(right - w, y, s, col, scale);
}

/* Draw at most maxw pixels, cutting to an ASCII ellipsis. */
static void hub_text_trunc(int x, int y, const char *s, unsigned col,
			   int scale, int maxw)
{
	int maxc, n;

	if (!s)
		return;
	if (scale < 1)
		scale = 1;
	if (maxw <= 0)
		return;
	maxc = maxw / (8 * scale);
	n = (int)strlen(s);
	if (n <= maxc)
	{
		hub_text_n(x, y, s, (unsigned)n, col, scale);
		return;
	}
	if (maxc <= 3)
	{
		hub_text_n(x, y, s, (unsigned)(maxc > 0 ? maxc : 0), col, scale);
		return;
	}
	hub_text_n(x, y, s, (unsigned)(maxc - 3), col, scale);
	hub_text(x + (maxc - 3) * 8 * scale, y, "...", col, scale);
}

/* ---- big title font (the built-in TNR bitmaps) ----------------------- */

static const unsigned char *hub_tnr(int scale, unsigned ch, int *gw, int *gh,
				    int *rowb)
{
	if (ch < 32 || ch > 126)
		ch = '?';
	ch -= 32;
	if (scale == 2)
	{
		*gw = 16;
		*gh = 32;
		*rowb = 2;
		return mmb_tnr_16x32 + ch * 32 * 2;
	}
	if (scale == 3)
	{
		*gw = 24;
		*gh = 48;
		*rowb = 3;
		return mmb_tnr_24x48 + ch * 48 * 3;
	}
	if (scale == 4)
	{
		*gw = 32;
		*gh = 64;
		*rowb = 4;
		return mmb_tnr_32x64 + ch * 64 * 4;
	}
	return 0;
}

static int hub_big_w(const char *s, int scale)
{
	return (int)strlen(s) * 8 * scale;
}

static void hub_big_text(int x, int y, const char *s, unsigned col,
			 int scale)
{
	const char *q;
	int cx = x;

	if (!s)
		return;
	if (scale < 2)
	{
		hub_text(x, y, s, col, scale < 1 ? 1 : scale);
		return;
	}
	for (q = s; *q; q++)
	{
		unsigned ch = (unsigned char)*q;
		const unsigned char *g;
		int gw, gh, rowb, gx, gy;

		if (ch == '\n' || ch == '\r')
			continue;
		g = hub_tnr(scale, ch, &gw, &gh, &rowb);
		if (!g)
		{
			if (ch != ' ')
				hub_text(cx, y, "?", col, scale < 1 ? 1 : scale);
		}
		else
		{
			for (gy = 0; gy < gh; gy++)
				for (gx = 0; gx < gw; gx++)
				{
					unsigned char bits =
						g[gy * rowb + gx / 8];

					if (bits & (unsigned char)(0x80 >>
								   (gx % 8)))
						mmb_gfx_plot(cx + gx, y + gy,
							     col);
				}
		}
		cx += 8 * scale;
	}
}

static void hub_big_shadow(int x, int y, const char *s, unsigned front,
			   unsigned shade, int scale, int maxw)
{
	int n = (int)strlen(s);
	int maxc = maxw / (8 * scale);

	if (n > maxc)
		n = maxc;
	if (n <= 0)
		return;
	/* Shadow first, then the chrome face. */
	{
		char buf[128];
		int i;

		if (n > (int)sizeof(buf) - 1)
			n = (int)sizeof(buf) - 1;
		for (i = 0; i < n; i++)
			buf[i] = s[i];
		buf[n] = 0;
		hub_big_text(x + 2, y + 2, buf, shade, scale);
		hub_big_text(x, y, buf, front, scale);
	}
}

/* ---- word wrapping --------------------------------------------------- */

static int hub_wrap_cols(int width, int scale)
{
	int cols = width / (8 * scale);

	return cols < 1 ? 1 : cols;
}

static int hub_wrap_lines(const char *s, int cols)
{
	int lines = 1, cx = 0;
	const char *q = s;

	if (!s || !*s)
		return 0;
	while (*q)
	{
		int wl;

		if (*q == '\n')
		{
			lines++;
			cx = 0;
			q++;
			continue;
		}
		wl = 0;
		while (*q && *q != ' ' && *q != '\n')
		{
			q++;
			wl++;
		}
		if (*q == ' ')
			q++;
		if (cx > 0)
		{
			if (cx + 1 + wl <= cols)
			{
				cx += 1 + wl;
				continue;
			}
			if (wl <= cols)
			{
				lines++;
				cx = wl;
				continue;
			}
			lines++;
			cx = 0;
		}
		while (wl > cols)
		{
			wl -= cols;
			cx = cols;
			lines++;
			cx = 0;
		}
		cx = wl;
	}
	return lines;
}

/* Draw wrapped text, skipping `skip` lines and stopping at `maxlines`. */
static void hub_wrap_draw(int x, int y, const char *s, int cols, int lineh,
			  int skip, int maxlines, unsigned col)
{
	const char *q = s;
	int line = 0, cx = 0;

	if (!s || !*s)
		return;
	while (*q)
	{
		const char *word;
		int wl;

		if (line >= skip + maxlines)
			break;
		if (*q == '\n')
		{
			line++;
			cx = 0;
			q++;
			continue;
		}
		word = q;
		wl = 0;
		while (*q && *q != ' ' && *q != '\n')
		{
			q++;
			wl++;
		}
		if (*q == ' ')
			q++;
		if (cx > 0)
		{
			if (cx + 1 + wl <= cols)
				cx++;
			else
			{
				line++;
				cx = 0;
			}
		}
		while (wl > 0)
		{
			int avail = cols - cx;
			int take = wl > avail ? avail : wl;

			if (take <= 0)
			{
				line++;
				cx = 0;
				continue;
			}
			if (line >= skip && line < skip + maxlines)
				hub_text_n(x + cx * 8, y + (line - skip) * lineh,
					   word, (unsigned)take, col, 1);
			word += take;
			wl -= take;
			cx += take;
			if (wl > 0)
			{
				line++;
				cx = 0;
			}
		}
	}
}


/* ---- colours / primitives -------------------------------------------- */

static void hub_fill(hub_rect r, unsigned col)
{
	if (r.w > 0 && r.h > 0)
		mmb_gfx_fill_rect(r.x, r.y, r.w, r.h, col);
}

static void hub_hline(int x, int y, int w, unsigned col, int t)
{
	if (t < 1)
		t = 1;
	mmb_gfx_fill_rect(x, y, w, t, col);
}

static void hub_vline(int x, int y, int h, unsigned col, int t)
{
	if (t < 1)
		t = 1;
	mmb_gfx_fill_rect(x, y, t, h, col);
}

static void hub_bevel(hub_rect r, unsigned fill)
{
	int t = U.p;

	hub_fill(r, fill);
	hub_hline(r.x, r.y, r.w, HUB_BORDER_L, t);
	hub_vline(r.x, r.y, r.h, HUB_BORDER_L, t);
	hub_hline(r.x, r.y + r.h - t, r.w, HUB_BORDER_D, t);
	hub_vline(r.x + r.w - t, r.y, r.h, HUB_BORDER_D, t);
	mmb_gfx_box(r.x, r.y, r.w, r.h, HUB_BG2, t, -1);
}

static void hub_dither_bg(void)
{
	int x, y, step = 6 * U.p;

	mmb_gfx_fill_rect(0, 0, U.w, U.h, HUB_BG);
	for (y = 0; y < U.h; y += step)
		for (x = (y / step % 2) * step; x < U.w; x += step * 2)
			mmb_gfx_fill_rect(x, y, U.p, U.p, HUB_BG2);
}

static void hub_glow(hub_rect r, unsigned col)
{
	int t = U.p;

	mmb_gfx_box(r.x - 2 * t, r.y - 2 * t, r.w + 4 * t, r.h + 4 * t,
		    col, t, -1);
	mmb_gfx_box(r.x - t, r.y - t, r.w + 2 * t, r.h + 2 * t,
		    HUB_CYAN_D, t, -1);
}

/* ---- the cube logo --------------------------------------------------- */

static void hub_cube(int cx, int cy, int size)
{
	int hw = size / 2;
	int hh = size * 55 / 100;
	int xs[4], ys[4];
	int ax = cx, ay = cy - hh;
	int bx = cx + hw, by = cy - hh / 2;
	int cxx = cx + hw, cyy = cy + hh / 2;
	int dx = cx, dy = cy + hh;
	int ex = cx - hw, ey = cy + hh / 2;
	int fx = cx - hw, fy = cy - hh / 2;
	int e = U.p < 2 ? 1 : 2;

	/* Isometric cube: top, left and right faces with neon edges. */
	xs[0] = ax; ys[0] = ay; xs[1] = bx; ys[1] = by;
	xs[2] = cx; ys[2] = cy; xs[3] = fx; ys[3] = fy;
	mmb_gfx_fill_polygon(xs, ys, 4, 0x1B2340);
	xs[0] = fx; ys[0] = fy; xs[1] = cx; ys[1] = cy;
	xs[2] = dx; ys[2] = dy; xs[3] = ex; ys[3] = ey;
	mmb_gfx_fill_polygon(xs, ys, 4, 0x241148);
	xs[0] = cx; ys[0] = cy; xs[1] = bx; ys[1] = by;
	xs[2] = cxx; ys[2] = cyy; xs[3] = dx; ys[3] = dy;
	mmb_gfx_fill_polygon(xs, ys, 4, 0x0C3546);

	mmb_gfx_line(ax, ay, bx, by, HUB_BORDER_L, e);
	mmb_gfx_line(ax, ay, fx, fy, HUB_BORDER_L, e);
	mmb_gfx_line(bx, by, cxx, cyy, HUB_CYAN, e);
	mmb_gfx_line(fx, fy, ex, ey, HUB_MAGENTA, e);
	mmb_gfx_line(cxx, cyy, dx, dy, HUB_CYAN, e);
	mmb_gfx_line(ex, ey, dx, dy, HUB_PURPLE, e);
	mmb_gfx_line(cx, cy, ax, ay, HUB_BORDER_L, e);
	mmb_gfx_line(cx, cy, dx, dy, HUB_PURPLE, e);
	mmb_gfx_line(cx, cy, bx, by, HUB_CYAN_D, e);
	mmb_gfx_line(cx, cy, fx, fy, HUB_PURPLE_D, e);
}

/* ---- ornaments ------------------------------------------------------- */

static void hub_ornament(int x, int y, int sx, int sy)
{
	int p = U.p;

	/* An L of bronze and purple with a small ring, mirrored by sx/sy. */
	mmb_gfx_line(x, y, x + sx * 26 * p, y, HUB_BRONZE, p < 2 ? 1 : 2);
	mmb_gfx_line(x, y, x, y + sy * 26 * p, HUB_BRONZE, p < 2 ? 1 : 2);
	mmb_gfx_line(x + sx * 3 * p, y + sy * 8 * p,
		     x + sx * 18 * p, y + sy * 8 * p, HUB_PURPLE,
		     p < 2 ? 1 : 2);
	mmb_gfx_line(x + sx * 8 * p, y + sy * 3 * p,
		     x + sx * 8 * p, y + sy * 18 * p, HUB_PURPLE,
		     p < 2 ? 1 : 2);
	mmb_gfx_circle(x + sx * 13 * p, y + sy * 3 * p, 4 * p,
		       HUB_MAGENTA, p < 2 ? 1 : 2, -1);
	mmb_gfx_circle(x + sx * 3 * p, y + sy * 13 * p, 4 * p,
		       HUB_CYAN, p < 2 ? 1 : 2, -1);
}

static void hub_ornate_frame(hub_rect r, unsigned fill)
{
	int p = U.p, c = 10 * p;

	hub_bevel(r, fill);
	mmb_gfx_box(r.x + 3 * p, r.y + 3 * p, r.w - 6 * p, r.h - 6 * p,
		    HUB_PURPLE_D, p, -1);
	hub_ornament(r.x, r.y, 1, 1);
	hub_ornament(r.x + r.w - 1, r.y, -1, 1);
	hub_ornament(r.x, r.y + r.h - 1, 1, -1);
	hub_ornament(r.x + r.w - 1, r.y + r.h - 1, -1, -1);
	(void)c;
}

/* ---- images ---------------------------------------------------------- */

static hub_img *hub_img_find(const char *rel)
{
	int i;

	if (!rel || !*rel)
		return 0;
	for (i = 0; i < s_nimg; i++)
		if (s_img[i].rel && strcmp(s_img[i].rel, rel) == 0)
			return &s_img[i];
	return 0;
}

static hub_img *hub_img_slot(const char *rel)
{
	hub_img *e;
	int cap;
	unsigned n = (unsigned)strlen(rel);

	if (s_nimg >= s_imgcap)
	{
		hub_img *nv;

		cap = s_imgcap ? s_imgcap * 2 : 16;
		nv = G.plat->alloc(sizeof(*nv) * (unsigned)cap);
		if (!nv)
			return 0;
		if (s_img)
		{
			memcpy(nv, s_img, sizeof(*nv) * (unsigned)s_nimg);
			G.plat->free(s_img);
		}
		s_img = nv;
		s_imgcap = cap;
	}
	e = &s_img[s_nimg++];
	memset(e, 0, sizeof(*e));
	e->rel = G.plat->alloc(n + 1);
	if (!e->rel)
	{
		s_nimg--;
		return 0;
	}
	memcpy(e->rel, rel, n + 1);
	return e;
}

static void hub_images_free(void)
{
	int i;

	for (i = 0; i < s_nimg; i++)
	{
		if (s_img[i].rel)
			G.plat->free(s_img[i].rel);
		if (s_img[i].pix)
			G.plat->free(s_img[i].pix);
	}
	if (s_img)
		G.plat->free(s_img);
	s_img = 0;
	s_nimg = 0;
	s_imgcap = 0;
}

/* Fetch and decode one asset (lazily, one per poll tick). Returns 1 when it
 * made an attempt, 0 when the asset was already known. */
static int hub_img_try(const char *rel)
{
	hub_img *e;
	unsigned char *buf = 0;
	unsigned n = 0;
	char err[96];

	if (!rel || !*rel)
		return 0;
	if (hub_img_find(rel))
		return 0;
	e = hub_img_slot(rel);
	if (!e)
		return 0;
	if (hub_fetch(rel, &buf, &n, err, sizeof(err)) == 0 && buf && n)
	{
		uint32_t *pix = 0;
		int w = 0, h = 0;

		if (mmb_png_decode_rgba(buf, n, &pix, &w, &h) == 0 && pix)
		{
			e->pix = pix;
			e->w = w;
			e->h = h;
		}
	}
	if (buf)
		G.plat->free(buf);
	return 1;
}

static void hub_draw_image(uint32_t *pix, int iw, int ih, hub_rect r)
{
	int dw, dh, dx, dy;

	if (!pix || iw <= 0 || ih <= 0 || r.w <= 0 || r.h <= 0)
		return;
	dw = r.w;
	dh = (int)((long)dw * ih / iw);
	if (dh > r.h)
	{
		dh = r.h;
		dw = (int)((long)dh * iw / ih);
	}
	if (dw < 1 || dh < 1)
		return;
	dx = r.x + (r.w - dw) / 2;
	dy = r.y + (r.h - dh) / 2;
	for (int y = 0; y < dh; y++)
	{
		int sy = (int)((long)y * ih / dh);

		for (int x = 0; x < dw; x++)
		{
			int sx = (int)((long)x * iw / dw);
			uint32_t c = pix[sy * iw + sx];

			if (!(c >> 24))
				continue;
			mmb_gfx_plot(dx + x, dy + y, c & 0xFFFFFFu);
		}
	}
}

static void hub_draw_image_cover(uint32_t *pix, int iw, int ih, hub_rect r)
{
	int sx0, sy0, sw, sh, x, y;

	if (!pix || iw <= 0 || ih <= 0 || r.w <= 0 || r.h <= 0)
		return;
	if ((long)iw * r.h >= (long)ih * r.w)
	{
		/* Source is wider than the box: crop the sides. */
		sh = ih;
		sw = (int)((long)ih * r.w / r.h);
		if (sw < 1)
			sw = 1;
		sx0 = (iw - sw) / 2;
		sy0 = 0;
	}
	else
	{
		sw = iw;
		sh = (int)((long)iw * r.h / r.w);
		if (sh < 1)
			sh = 1;
		sx0 = 0;
		sy0 = (ih - sh) / 2;
	}
	for (y = 0; y < r.h; y++)
	{
		int sy = sy0 + (int)((long)y * sh / r.h);

		for (x = 0; x < r.w; x++)
		{
			int sx = sx0 + (int)((long)x * sw / r.w);
			uint32_t c = pix[sy * iw + sx];

			if (!(c >> 24))
				continue;
			mmb_gfx_plot(r.x + x, r.y + y, c & 0xFFFFFFu);
		}
	}
}

static void hub_app_icon(hub_app *a, hub_rect r, int greyed)
{
	hub_img *im = a ? hub_img_find(a->icon) : 0;

	hub_fill(r, 0x0B0F22);
	if (im && im->pix)
		hub_draw_image(im->pix, im->w, im->h, r);
	else
		hub_cube(r.x + r.w / 2, r.y + r.h / 2, r.w * 3 / 4);
	mmb_gfx_box(r.x, r.y, r.w, r.h, HUB_BORDER_D, U.p, -1);
	if (greyed)
	{
		/* Desaturate cheaply: hatch the framed icon with the greyed tone. */
		int step = 3 * U.p;

		for (int y = r.y; y < r.y + r.h; y += step)
			hub_hline(r.x, y, r.w, HUB_GREY, U.p);
	}
}

static const char *hub_preview_rel(hub_app *a)
{
	if (!a)
		return 0;
	if (a->thumb && a->thumb[0])
		return a->thumb;
	if (a->nshots > 0 && a->shots[0] && a->shots[0][0])
		return a->shots[0];
	return 0;
}

static const char *hub_detail_rel(hub_app *a)
{
	if (!a)
		return 0;
	if (a->nshots > 0 && U.detail_shot >= 0 && U.detail_shot < a->nshots &&
	    a->shots[U.detail_shot] && a->shots[U.detail_shot][0])
		return a->shots[U.detail_shot];
	return hub_preview_rel(a);
}

/* ---- tabs ------------------------------------------------------------ */

static void hub_build_tabs(void)
{
	int cat, n = 0;

	s_tabs[n].cat = -1;
	strcpy(s_tabs[n].name, "ALL");
	n++;
	for (cat = 0; cat < MMB_APP_CATEGORY_COUNT; cat++)
	{
		int i, count = 0;

		for (i = 0; i < hub_count(); i++)
		{
			hub_app *a = hub_get(i);

			if (a && a->cat_index == cat)
				count++;
		}
		if (!count)
			continue;
		s_tabs[n].cat = cat;
		strncpy(s_tabs[n].name, mmb_app_category_name(cat),
			sizeof(s_tabs[n].name) - 1);
		s_tabs[n].name[sizeof(s_tabs[n].name) - 1] = 0;
		n++;
	}
	s_ntabs = n;
}

static void hub_rebuild_ids(void)
{
	int total = hub_count();
	int n;

	if (total > U.idcap || !U.ids)
	{
		int cap = total > 8 ? total : 8;
		int *nv = G.plat->alloc(sizeof(int) * (unsigned)cap);

		if (!nv)
			return;
		if (U.ids)
			G.plat->free(U.ids);
		U.ids = nv;
		U.idcap = cap;
	}
	n = hub_filter(U.tab, U.ids, U.idcap);
	U.nids = n > U.idcap ? U.idcap : n;
	if (U.sel >= U.nids)
		U.sel = U.nids > 0 ? U.nids - 1 : 0;
	if (U.sel < 0)
		U.sel = 0;
	if (U.list_top > U.sel)
		U.list_top = U.sel;
}

static hub_app *hub_cur(void)
{
	if (U.sel < 0 || U.sel >= U.nids || !U.ids)
		return 0;
	return hub_get(U.ids[U.sel]);
}

static void hub_layout_now(void)
{
	int tab_w = 0;
	int i;

	for (i = 0; i < s_ntabs; i++)
	{
		int w = hub_text_w(s_tabs[i].name, 1) + 6 * U.p;

		if (w > tab_w)
			tab_w = w;
	}
	hub_layout_compute(&U.L, U.w, U.h, s_ntabs, tab_w);
	U.p = U.L.p;
	U.layout_w = U.w;
	U.layout_h = U.h;
}

/* ---- progress bar ---------------------------------------------------- */

static void hub_segbar(hub_rect r, int pct, unsigned hi, unsigned lo)
{
	int p = U.p;
	int inner = r.w - 6 * p;
	int segw = 10 * p;
	int nseg, i, fill;

	if (inner < 6 * p)
		inner = 6 * p;
	nseg = inner / (segw + 2 * p);
	if (nseg < 1)
		nseg = 1;
	segw = (inner - (nseg - 1) * 2 * p) / nseg;
	if (segw < 3 * p)
		segw = 3 * p;
	fill = nseg * pct / 100;
	hub_bevel(r, 0x0B0F22);
	for (i = 0; i < nseg; i++)
	{
		hub_rect s;
		unsigned col = i < fill ? hi : lo;

		s.x = r.x + 3 * p + i * (segw + 2 * p);
		s.y = r.y + 3 * p;
		s.w = segw;
		s.h = r.h - 6 * p;
		if (s.x + s.w > r.x + r.w - 3 * p)
			break;
		hub_fill(s, col);
		hub_hline(s.x, s.y, s.w, col == hi ? HUB_TEXT : HUB_BORDER_D,
			  p);
		hub_hline(s.x, s.y + s.h - p, s.w,
			  col == hi ? HUB_PURPLE_D : HUB_BG2, p);
	}
}

/* ---- loading screen -------------------------------------------------- */

static void hub_paint_loading(void)
{
	int p = U.p, y, title_scale;
	hub_rect bar;
	const char *title = "MMCORE SOFTWARE HUB";
	int tw, tx;
	int pct;

	hub_dither_bg();
	hub_cube(U.w / 2, U.h / 2 - 70 * p, 90 * p);

	title_scale = p >= 2 ? 3 : 2;
	tw = hub_big_w(title, title_scale);
	tx = (U.w - tw) / 2;
	y = U.h / 2 + 45 * p;
	hub_big_text(tx, y, title, HUB_MAGENTA, title_scale);
	/* Thin dotted rules either side of the title. */
	for (int side = 0; side < 2; side++)
	{
		int ry = y + 10 * title_scale;
		int start = side ? tx + tw + 8 * p : tx - 8 * p;

		if (!side)
		{
			/* right aligned dots */
			int x;

			for (x = start - 4 * p; x > start - 40 * p && x > 10 * p;
			     x -= 6 * p)
				mmb_gfx_fill_rect(x, ry, 2 * p, 2 * p,
						  HUB_PURPLE);
		}
		else
		{
			int x;

			for (x = start; x < start + 40 * p && x < U.w - 10 * p;
			     x += 6 * p)
				mmb_gfx_fill_rect(x, ry, 2 * p, 2 * p,
						  HUB_PURPLE);
		}
	}

	bar.w = U.w * 60 / 100;
	if (bar.w > 420 * p)
		bar.w = 420 * p;
	bar.h = 18 * p;
	bar.x = (U.w - bar.w) / 2;
	bar.y = y + 34 * p;

	if (U.state != HUB_ST_LOADING)
		return;
	if (!U.load_started)
	{
		unsigned el = mmb_now_ms() - (unsigned)U.started;

		pct = el >= HUB_LOAD_DELAY_MS ? 42 :
		      (int)(el * 42 / HUB_LOAD_DELAY_MS);
	}
	else if (!U.load_done)
		pct = 55;
	else if (U.err)
		pct = 55;
	else
	{
		int done = U.icon_total - U.icon_stage;

		pct = 60 + (U.icon_total ? done * 35 / U.icon_total : 35);
	}
	hub_segbar(bar, pct, U.err ? HUB_RED : HUB_LIME,
		   U.err ? HUB_RED_D : HUB_LIME_D);

	if (U.err)
	{
		int rw = hub_text_w(U.errmsg, p >= 2 ? 2 : 1);
		int rx = (U.w - rw) / 2;
		int ry = bar.y + bar.h + 12 * p;

		if (rx < 8 * p)
			rx = 8 * p;
		hub_text_trunc(rx, ry, U.errmsg, HUB_RED, p >= 2 ? 2 : 1,
			       U.w - 2 * rx);
	}
}

/* ---- badges ---------------------------------------------------------- */

static const char *hub_state_badge(hub_app *a, unsigned *col,
				   unsigned *fill, char *buf, int bufsz)
{
	*col = HUB_TEXT;
	*fill = HUB_PANEL2;
	if (!a)
		return buf;
	switch (a->state)
	{
	case HUB_INSTALLED:
		*col = HUB_LIME;
		*fill = 0x1B3010;
		return "INSTALLED";
	case HUB_UPDATE:
		*col = HUB_ORANGE;
		*fill = 0x3A2408;
		return "UPDATE";
	case HUB_NEEDS_NEWER:
		*col = HUB_RED;
		*fill = 0x3A0E0E;
		snprintf(buf, (size_t)bufsz, "NEEDS v%s",
			 a->minver && a->minver[0] ? a->minver : "?");
		return buf;
	default:
		return "";
	}
}

static void hub_chip(int x, int y, const char *text, unsigned col,
		     unsigned fill, int scale)
{
	int p = U.p;
	int w = hub_text_w(text, scale) + 8 * p;
	int h = 16 * scale + 4 * p;

	hub_fill((hub_rect){ x, y, w, h }, fill);
	mmb_gfx_box(x, y, w, h, col, p, -1);
	hub_text(x + 4 * p, y + (h - 16 * scale) / 2, text, col, scale);
}

static void hub_size_text(long size, char *out, int outsz)
{
	if (size <= 0)
	{
		out[0] = '-';
		out[1] = 0;
		return;
	}
	if (size >= 1024L * 1024L)
		snprintf(out, (size_t)outsz, "%ld.%ld MB",
			 size / (1024L * 1024L),
			 (size / (102L * 1024L)) % 10);
	else
		snprintf(out, (size_t)outsz, "%ld KB",
			 (size + 1023) / 1024);
}

/* ---- listing --------------------------------------------------------- */

static int hub_row_h(int idx, int selected)
{
	int h = U.L.row_h;

	if (selected && U.L.mode == HUB_LAYOUT_NARROW)
	{
		int iw = U.L.list.w - 8 * U.p;
		int ih = iw * 9 / 16;

		if (ih > U.L.list.h * 45 / 100)
			ih = U.L.list.h * 45 / 100;
		h += ih + 28 * U.p;
	}
	(void)idx;
	return h;
}

static void hub_scroll_sel(void)
{
	int i, guard;

	if (U.nids <= 0)
	{
		U.sel = 0;
		U.list_top = 0;
		return;
	}
	if (U.sel < 0)
		U.sel = 0;
	if (U.sel >= U.nids)
		U.sel = U.nids - 1;
	if (U.list_top > U.sel)
		U.list_top = U.sel;
	if (U.list_top < 0)
		U.list_top = 0;
	for (guard = 0; guard < U.nids + 2; guard++)
	{
		int y = 0;

		for (i = U.list_top; i <= U.sel; i++)
			y += hub_row_h(i, i == U.sel);
		if (y <= U.L.list.h - 4 * U.p)
			break;
		U.list_top++;
	}
}

static void hub_draw_row(int i, int y, int h, int selected)
{
	hub_app *a = hub_get(U.ids[i]);
	hub_rect r, icon;
	int p = U.p;
	int greyed;
	int x, ty;
	char buf[64], badgebuf[64];
	const char *badge;
	unsigned bcol, bfill;
	int narrow = U.L.mode == HUB_LAYOUT_NARROW;

	if (!a)
		return;
	r.x = U.L.list.x + 2 * p;
	r.y = y;
	r.w = U.L.list.w - 4 * p;
	r.h = h - 2 * p;
	greyed = a->state == HUB_NEEDS_NEWER;

	if (selected)
	{
		hub_glow(r, HUB_CYAN_D);
		hub_fill(r, 0x1A2142);
	}
	else
		hub_fill(r, (i & 1) ? HUB_PANEL2 : HUB_PANEL);
	mmb_gfx_box(r.x, r.y, r.w, r.h, selected ? HUB_CYAN : HUB_BORDER_D,
		    selected ? (p < 2 ? 1 : 2) : p, -1);

	icon.w = icon.h = 32 * p;
	icon.x = r.x + 6 * p;
	icon.y = r.y + (r.h - icon.h) / 2;
	if (selected && narrow)
		icon.y = r.y + 6 * p;
	hub_app_icon(a, icon, greyed);

	x = icon.x + icon.w + 8 * p;
	ty = icon.y;
	if (!narrow)
		ty = r.y + 10 * p;
	{
		int lim = r.x + r.w - 8 * p - (narrow ? 0 : 100 * p);

		hub_text_trunc(x, ty, a->title ? a->title : a->id,
			       greyed ? HUB_GREY : HUB_TEXT, p, lim - x);
		hub_text_trunc(x, ty + 16 * p + 2 * p,
			       a->summary ? a->summary : "",
			       greyed ? HUB_GREY : HUB_DIM, p >= 2 ? 1 : 1,
			       lim - x);
	}

	if (selected && narrow)
	{
		int iw = r.w - 12 * p;
		int ih = iw * 9 / 16;
		hub_rect ir;
		const char *rel = hub_preview_rel(a);
		hub_img *im = hub_img_find(rel);

		if (ih > U.L.list.h * 45 / 100)
			ih = U.L.list.h * 45 / 100;
		ir.x = r.x + 6 * p;
		ir.y = ty + 34 * p;
		ir.w = iw;
		ir.h = ih;
		hub_fill(ir, 0x0B0F22);
		if (im && im->pix)
			hub_draw_image(im->pix, im->w, im->h, ir);
		else
		{
			hub_fill(ir, 0x0B0F22);
			hub_app_icon(a, (hub_rect){ ir.x + ir.w / 2 - 16 * p,
						    ir.y + ir.h / 2 - 16 * p,
						    32 * p, 32 * p }, 0);
		}
		mmb_gfx_box(ir.x - p, ir.y - p, ir.w + 2 * p, ir.h + 2 * p,
			    HUB_PURPLE_D, p, -1);
	}

	/* Right side: author and size, with the state badge above them. */
	badge = hub_state_badge(a, &bcol, &bfill, badgebuf, sizeof(badgebuf));
	if (badge[0])
	{
		int scale = 1;
		int bw = hub_text_w(badge, scale) + 8 * p;
		int bh = 16 * scale + 2 * p;
		int bx = r.x + r.w - bw - 6 * p;
		int by = r.y + 3 * p;

		hub_fill((hub_rect){ bx, by, bw, bh }, bfill);
		mmb_gfx_box(bx, by, bw, bh, bcol, p, -1);
		hub_text(bx + 4 * p, by + (bh - 16 * scale) / 2, badge, bcol,
			 scale);
	}
	{
		int scale = 1;
		char linebuf[96];
		int ay = r.y + r.h - 6 * p - 16 * scale;

		if (p == 1 && badge[0] && ay < r.y + 22 * p)
			ay = r.y + 22 * p;
		hub_size_text(a->size, buf, sizeof(buf));
		snprintf(linebuf, sizeof(linebuf), "%s - %s",
			 a->author && a->author[0] ? a->author : "unknown", buf);
		hub_text_r(r.x + r.w - 6 * p, ay, linebuf,
			   greyed ? HUB_GREY : HUB_DIM, scale);
	}
}

static void hub_paint_listing(void)
{
	hub_app *cur = hub_cur();
	hub_rect hdr = U.L.header;
	int p = U.p;
	int i, y;

	hub_dither_bg();

	/* Header: small cube + mmcore HUB. */
	hub_bevel(hdr, HUB_PANEL);
	hub_cube(hdr.x + 16 * p, hdr.y + hdr.h / 2, 18 * p);
	hub_text(hdr.x + 32 * p, hdr.y + (hdr.h - 16 * p) / 2, "mmcore",
		 HUB_TEXT, p);
	hub_text(hdr.x + 32 * p + hub_text_w("mmcore ", p), hdr.y +
		 (hdr.h - 16 * p) / 2, "HUB", HUB_MAGENTA, p);

	/* Content panel first: the selected tab's open foot is painted over its
	 * top border so the tab blends into the panel. */
	hub_bevel(U.L.content, HUB_PANEL);
	mmb_gfx_box(U.L.content.x + p, U.L.content.y + p,
		    U.L.content.w - 2 * p, U.L.content.h - 2 * p,
		    HUB_BORDER_D, p, -1);

	/* Tabs. */
	for (i = 0; i < s_ntabs; i++)
	{
		int row = i / U.L.tabs_per_row;
		int col = i % U.L.tabs_per_row;
		int tx = U.L.tabs.x + col * (U.L.tab_w + U.L.tab_gap);
		int ty = U.L.tabs.y + row * (U.L.tab_h + U.L.tab_gap);
		int sel = s_tabs[i].cat == U.tab;
		hub_rect tr = { tx, ty, U.L.tab_w, U.L.tab_h };
		unsigned fill = sel ? HUB_PANEL : HUB_PANEL2;
		int scale = 1;

		if (i < (int)(sizeof(s_tabbox) / sizeof(s_tabbox[0])))
		{
			s_tabbox[s_ntabbox].r = tr;
			s_tabbox[s_ntabbox].value = s_tabs[i].cat;
			s_ntabbox++;
		}
		hub_fill(tr, fill);
		if (sel)
		{
			/* Open at the bottom: the selected tab shares the content
			 * panel's top border and has inverted bottom corners. */
			int foot_y = tr.y + tr.h;
			int foot_h = U.L.content.y + p - foot_y;
			int r = 3 * p;
			int k;

			if (foot_h < 1)
				foot_h = 1;
			hub_fill((hub_rect){ tr.x, foot_y, tr.w, foot_h },
				 HUB_PANEL);
			hub_hline(tr.x, tr.y, tr.w, HUB_BORDER_L, p);
			hub_vline(tr.x, tr.y, tr.h + foot_h, HUB_BORDER_L, p);
			hub_vline(tr.x + tr.w - p, tr.y, tr.h + foot_h,
				  HUB_BORDER_D, p);
			/* Inverted corners: sweep outwards into the panel border. */
			for (k = 0; k <= r; k++)
			{
				int yy = foot_y + foot_h - p - k;

				mmb_gfx_plot(tr.x + k, yy, HUB_BORDER_L);
				mmb_gfx_plot(tr.x + tr.w - 1 - k, yy,
					     HUB_BORDER_D);
			}
			hub_text(tr.x + (tr.w - hub_text_w(s_tabs[i].name,
							   scale)) / 2,
				 tr.y + (tr.h - 16 * scale) / 2, s_tabs[i].name,
				 HUB_MAGENTA, scale);
		}
		else
		{
			mmb_gfx_box(tr.x, tr.y, tr.w, tr.h, HUB_BORDER_D, p,
				    -1);
			hub_text(tr.x + (tr.w - hub_text_w(s_tabs[i].name,
							   scale)) / 2,
				 tr.y + (tr.h - 16 * scale) / 2, s_tabs[i].name,
				 HUB_DIM, scale);
		}
	}

	/* Rows. */
	y = U.L.list.y + 3 * p;
	for (i = U.list_top; i < U.nids; i++)
	{
		int h = hub_row_h(i, i == U.sel);

		if (y + h > U.L.list.y + U.L.list.h - p)
			break;
		hub_draw_row(i, y, h, i == U.sel);
		if (i < (int)(sizeof(s_rowbox) / sizeof(s_rowbox[0])))
		{
			s_rowbox[s_nrowbox].r = (hub_rect){ U.L.list.x, y,
							     U.L.list.w, h };
			s_rowbox[s_nrowbox].value = i;
			s_nrowbox++;
		}
		y += h;
	}

	if (U.L.mode == HUB_LAYOUT_WIDE)
	{
		hub_rect pr = U.L.preview;
		hub_rect ir;
		hub_img *im;
		const char *rel = 0;
		int ty;

		hub_ornate_frame(pr, HUB_PANEL);
		ir.x = pr.x + 8 * p;
		ir.y = pr.y + 8 * p;
		ir.w = pr.w - 16 * p;
		ir.h = (pr.h * 45 / 100);
		if (ir.h > ir.w * 9 / 16)
			ir.h = ir.w * 9 / 16;
		hub_fill(ir, 0x0B0F22);
		if (cur)
			rel = hub_preview_rel(cur);
		im = hub_img_find(rel);
		if (im && im->pix)
			hub_draw_image(im->pix, im->w, im->h, ir);
		else if (cur)
			hub_app_icon(cur, (hub_rect){ ir.x + ir.w / 2 -
						      24 * p,
						      ir.y + ir.h / 2 -
						      24 * p,
						      48 * p, 48 * p }, 0);
		mmb_gfx_box(ir.x - p, ir.y - p, ir.w + 2 * p, ir.h + 2 * p,
			    HUB_PURPLE_D, p, -1);

		ty = ir.y + ir.h + 8 * p;
		if (cur)
		{
			char vbuf[32];
			char line[96];

			hub_text_trunc(pr.x + 10 * p, ty,
				       cur->title ? cur->title : cur->id,
				       HUB_TEXT, p, pr.w - 20 * p);
			snprintf(vbuf, sizeof(vbuf), "v%s",
				 cur->version && cur->version[0] ?
				 cur->version : "?");
			hub_text(pr.x + 10 * p, ty + 18 * p, vbuf, HUB_CYAN,
				 p >= 2 ? 1 : 1);
			snprintf(line, sizeof(line), "by %s",
				 cur->author && cur->author[0] ?
				 cur->author : "unknown");
			hub_text(pr.x + 10 * p, ty + 34 * p, line, HUB_DIM, 1);
			hub_wrap_draw(pr.x + 10 * p, ty + 54 * p,
				      cur->summary ? cur->summary : "",
				      hub_wrap_cols(pr.w - 20 * p, 1), 16,
				      0, (pr.y + pr.h - ty - 60 * p) / 16,
				      HUB_DIM);
		}
	}
}

/* ---- detail ---------------------------------------------------------- */

static const char *hub_button_label(hub_app *a, unsigned *col,
				    unsigned *fill, char *buf, int bufsz)
{
	*col = HUB_TEXT;
	*fill = HUB_PANEL2;
	if (!a)
		return "";
	switch (a->state)
	{
	case HUB_INSTALLED:
		*col = HUB_CYAN;
		*fill = 0x0C2A36;
		return "RUN";
	case HUB_UPDATE:
		*col = HUB_ORANGE;
		*fill = 0x3A2408;
		return "UPDATE";
	case HUB_NEEDS_NEWER:
		*col = HUB_GREY;
		*fill = HUB_PANEL;
		snprintf(buf, (size_t)bufsz, "NEEDS v%s",
			 a->minver && a->minver[0] ? a->minver : "?");
		return buf;
	default:
		*col = HUB_LIME;
		*fill = 0x17300C;
		return "INSTALL";
	}
}

static void hub_draw_main_button(hub_rect area)
{
	hub_app *a = hub_cur();
	int p = U.p;
	int bw = area.w * 62 / 100;
	int bh = 26 * p;
	hub_rect b;
	char buf[64];
	const char *label;
	unsigned col, fill;
	int scale = p >= 2 ? 2 : 1;

	if (bw < 90 * p)
		bw = 90 * p;
	if (bw > area.w - 8 * p)
		bw = area.w - 8 * p;
	b.x = area.x + (area.w - bw) / 2;
	b.y = area.y + (area.h - bh) / 2;
	b.w = bw;
	b.h = bh;
	label = hub_button_label(a, &col, &fill, buf, sizeof(buf));
	hub_fill(b, fill);
	mmb_gfx_box(b.x, b.y, b.w, b.h, col, p, -1);
	mmb_gfx_box(b.x + p, b.y + p, b.w - 2 * p, b.h - 2 * p,
		    HUB_BORDER_D, p, -1);
	hub_text(b.x + (b.w - hub_text_w(label, scale)) / 2,
		 b.y + (b.h - 16 * scale) / 2, label, col, scale);
	s_mainbtn = b;
	s_has_mainbtn = 1;
}

static void hub_draw_about(hub_rect r, hub_app *a, int scroll)
{
	int p = U.p;
	int lh = 16;
	int cols = hub_wrap_cols(r.w - 6 * p, 1);
	const char *text = (a && a->about && a->about[0]) ? a->about :
			   (a ? a->summary : "");
	int lines = hub_wrap_lines(text ? text : "", cols);
	int rows = r.h / lh;
	int maxscroll = lines - rows;

	if (maxscroll < 0)
		maxscroll = 0;
	if (scroll > maxscroll)
		scroll = maxscroll;
	if (scroll < 0)
		scroll = 0;
	if (a)
		U.about_scroll = scroll;
	if (text && text[0])
		hub_wrap_draw(r.x, r.y, text, cols, lh, scroll, rows, HUB_TEXT);
	if (maxscroll > 0)
	{
		/* Thin overflow indicator on the right edge only. */
		int track = r.h - 2 * p;
		int thumb = track * rows / lines;
		int pos = track * scroll / lines;

		if (thumb < 8 * p)
			thumb = 8 * p;
		mmb_gfx_fill_rect(r.x + r.w - p, r.y + p, p, track,
				  HUB_BORDER_D);
		mmb_gfx_fill_rect(r.x + r.w - p, r.y + p + pos, p, thumb,
				  HUB_CYAN);
	}
}

static void hub_draw_detail(void)
{
	hub_app *a = hub_cur();
	hub_layout *L = &U.L;
	int p = U.p;
	hub_rect shot = L->detail_shot;
	hub_rect img;
	hub_img *im;
	const char *rel;
	int scale;

	hub_dither_bg();
	hub_bevel(L->detail, HUB_PANEL);

	/* Framed screenshot with the same ornament on all four corners. */
	hub_ornate_frame(shot, HUB_PANEL2);
	img.x = shot.x + 7 * p;
	img.y = shot.y + 7 * p;
	img.w = shot.w - 14 * p;
	img.h = shot.h - 14 * p;
	hub_fill(img, 0x0B0F22);
	rel = hub_detail_rel(a);
	im = hub_img_find(rel);
	if (im && im->pix)
		hub_draw_image_cover(im->pix, im->w, im->h, img);
	else if (a)
		hub_app_icon(a, (hub_rect){ img.x + img.w / 2 - 32 * p,
					    img.y + img.h / 2 - 32 * p,
					    64 * p, 64 * p }, 0);
	mmb_gfx_box(img.x - p, img.y - p, img.w + 2 * p, img.h + 2 * p,
		    HUB_PURPLE_D, p, -1);

	/* Shot thumbnails or dots. */
	s_nshotbox = 0;
	if (a && a->nshots > 0)
	{
		int n = a->nshots;
		int avail = L->detail_thumbs.w - 4 * p;
		int th = L->detail_thumbs.h - 2 * p;
		int tw2 = th * 16 / 9;
		int i, x = L->detail_thumbs.x;

		if (tw2 < 8 * p)
			tw2 = 8 * p;
		if (n * (tw2 + 3 * p) > avail)
		{
			/* Dots when there is no room. */
			int dots = n;
			int dw = 8 * p;
			int total = dots * (dw + 2 * p);

			x = L->detail_thumbs.x +
			    (L->detail_thumbs.w - total) / 2;
			for (i = 0; i < dots; i++)
				mmb_gfx_circle(x + i * (dw + 2 * p) + dw / 2,
					       L->detail_thumbs.y +
					       L->detail_thumbs.h / 2,
					       3 * p,
					       i == U.detail_shot ? HUB_CYAN :
					       HUB_BORDER_D, p, -1);
			mmb_gfx_circle(x + U.detail_shot * (dw + 2 * p) +
				       dw / 2,
				       L->detail_thumbs.y +
				       L->detail_thumbs.h / 2, 3 * p, HUB_CYAN,
				       p, -1);
		}
		else
		{
			for (i = 0; i < n; i++)
			{
				hub_rect tr = { x, L->detail_thumbs.y, tw2, th };
				hub_img *ti = hub_img_find(a->shots[i]);

				hub_fill(tr, 0x0B0F22);
				if (ti && ti->pix)
					hub_draw_image(ti->pix, ti->w, ti->h,
						       tr);
				mmb_gfx_box(tr.x, tr.y, tr.w, tr.h,
					    i == U.detail_shot ? HUB_CYAN :
					    HUB_BORDER_D,
					    i == U.detail_shot ? (p < 2 ? 1 :
								  2) : p, -1);
				if (i < (int)(sizeof(s_shotbox) /
					      sizeof(s_shotbox[0])))
				{
					s_shotbox[s_nshotbox].r = tr;
					s_shotbox[s_nshotbox].value = i;
					s_nshotbox++;
				}
				x += tw2 + 3 * p;
			}
		}
	}

	/* Title and icon. */
	scale = p >= 3 ? 4 : (p >= 2 ? 3 : 2);
	img.w = 32 * p;
	img.h = 32 * p;
	img.x = L->detail_title.x + L->detail_title.w - img.w - 4 * p;
	img.y = L->detail_title.y + 2 * p;
	if (!(a && a->icon && a->icon[0]))
	{
		img.x = L->detail_title.x;
		img.w = L->detail_title.w;
		img.y = L->detail_title.y;
		img.h = L->detail_title.h;
	}
	if (a)
		hub_app_icon(a, img, a->state == HUB_NEEDS_NEWER);
	{
		int maxw = L->detail_title.w - (a && a->icon && a->icon[0] ?
						img.w + 10 * p : 0);
		const char *title = a ? (a->title ? a->title : a->id) : "";

		hub_big_shadow(L->detail_title.x,
			       L->detail_title.y +
			       (L->detail_title.h - 16 * scale) / 2,
			       title, HUB_TEXT, HUB_PURPLE, scale, maxw);
	}

	/* Tag chips. */
	if (a)
	{
		char vbuf[32], sbuf[32];
		int x = L->detail_chips.x;
		int y = L->detail_chips.y;
		int maxx = L->detail_chips.x + L->detail_chips.w;
		char line[96];

		snprintf(line, sizeof(line), "by %s",
			 a->author && a->author[0] ? a->author : "unknown");
		hub_chip(x, y, line, HUB_TEXT, HUB_PANEL2, 1);
		x += hub_text_w(line, 1) + 12 * p;
		snprintf(vbuf, sizeof(vbuf), "v%s",
			 a->version && a->version[0] ? a->version : "?");
		if (x + hub_text_w(vbuf, 1) + 8 * p < maxx)
		{
			hub_chip(x, y, vbuf, HUB_CYAN, HUB_PANEL2, 1);
			x += hub_text_w(vbuf, 1) + 12 * p;
		}
		hub_size_text(a->size, sbuf, sizeof(sbuf));
		if (x + hub_text_w(sbuf, 1) + 8 * p < maxx)
		{
			hub_chip(x, y, sbuf, HUB_DIM, HUB_PANEL2, 1);
			x += hub_text_w(sbuf, 1) + 12 * p;
		}
		if (a->category && a->category[0] &&
		    x + hub_text_w(a->category, 1) + 8 * p < maxx)
		{
			hub_chip(x, y, a->category, HUB_PURPLE, HUB_PANEL2, 1);
			x += hub_text_w(a->category, 1) + 12 * p;
		}
		if (a->state == HUB_NEEDS_NEWER)
		{
			char nb[64];
			int nw;

			snprintf(nb, sizeof(nb), "NEEDS MMCORE v%s",
				 a->minver && a->minver[0] ? a->minver : "?");
			nw = hub_text_w(nb, 1) + 8 * p;
			if (x + nw > maxx)
			{
				x = maxx - nw;
				y += L->detail_chips.h;
			}
			hub_chip(x, y, nb, HUB_RED, 0x3A0E0E, 1);
		}
	}

	hub_draw_about(L->detail_about, a, U.about_scroll);
	hub_draw_main_button(L->detail_button);
}

/* ---- modals ---------------------------------------------------------- */

static void hub_modal_rect(hub_rect *m, int wunits, int hunits)
{
	int p = U.p;
	int w = wunits * p;
	int h = hunits * p;

	if (w > U.w - 16 * p)
		w = U.w - 16 * p;
	if (h > U.h - 16 * p)
		h = U.h - 16 * p;
	m->x = (U.w - w) / 2;
	m->y = (U.h - h) / 2;
	m->w = w;
	m->h = h;
}

static void hub_paint_install_modal(int failed)
{
	hub_app *a = hub_get(U.inst_index);
	hub_rect m, bar;
	int p = U.p;
	int y;
	char buf[192];

	hub_modal_rect(&m, 500, 250);
	hub_ornate_frame(m, HUB_PANEL2);
	y = m.y + 14 * p;
	{
		const char *verb = a && a->state == HUB_UPDATE ? "UPDATING" :
				   "INSTALLING";
		snprintf(buf, sizeof(buf), "%s %s v%s", verb,
			 a ? (a->title ? a->title : a->id) : "",
			 a && a->version ? a->version : "");
	}
	hub_text_trunc(m.x + 12 * p, y, buf, HUB_TEXT, p >= 2 ? 2 : 1,
		       m.w - 24 * p);
	y += 30 * p;

	if (!failed)
	{
		unsigned kb = (U.inst_done + 1023) / 1024;
		unsigned total = (U.inst_total + 1023) / 1024;

		snprintf(buf, sizeof(buf), "DOWNLOADING  %u / %u KB", kb,
			 total);
		hub_text(m.x + 16 * p, y, buf, HUB_TEXT, 1);
		y += 22 * p;
		bar.x = m.x + 16 * p;
		bar.y = y;
		bar.w = m.w - 32 * p;
		bar.h = 18 * p;
		hub_segbar(bar, U.inst_total ?
			   (int)((uint64_t)U.inst_done * 100u /
				 (unsigned)U.inst_total) : 0,
			   HUB_LIME, HUB_LIME_D);
		y += 26 * p;

		snprintf(buf, sizeof(buf), "CHECKSUM   %s",
			 U.inst_checksum == 1 ? "OK" :
			 U.inst_checksum == 2 ? "FAILED" :
			 (U.inst_done && U.inst_total &&
			  U.inst_done >= U.inst_total ? "..." : "..."));
		hub_text(m.x + 16 * p, y, buf,
			 U.inst_checksum == 2 ? HUB_RED :
			 U.inst_checksum == 1 ? HUB_LIME : HUB_TEXT, 1);
		y += 22 * p;
		snprintf(buf, sizeof(buf), "TARGET   %s", U.inst_target);
		hub_text_trunc(m.x + 16 * p, y, buf, HUB_DIM, 1,
			       m.w - 32 * p);
		y += 28 * p;
		{
			int bw = 100 * p, bh = 24 * p;
			hub_rect b = { m.x + (m.w - bw) / 2, y, bw, bh };

			hub_fill(b, HUB_PANEL);
			mmb_gfx_box(b.x, b.y, b.w, b.h, HUB_BORDER_L, p, -1);
			hub_text(b.x + (b.w - hub_text_w("CANCEL", 1)) / 2,
				 b.y + (b.h - 16) / 2, "CANCEL", HUB_TEXT, 1);
			s_modal_cancel = b;
			s_has_cancel = 1;
		}
	}
	else
	{
		snprintf(buf, sizeof(buf), "%s",
			 U.errmsg[0] ? U.errmsg : "INSTALL FAILED");
		hub_text_trunc(m.x + 16 * p, y, buf, HUB_RED, 1,
			       m.w - 32 * p);
		y += 34 * p;
		{
			int bw = 80 * p, bh = 24 * p;
			hub_rect b = { m.x + (m.w - bw) / 2, y, bw, bh };

			hub_fill(b, HUB_PANEL);
			mmb_gfx_box(b.x, b.y, b.w, b.h, HUB_BORDER_L, p, -1);
			hub_text(b.x + (b.w - hub_text_w("OK", 1)) / 2,
				 b.y + (b.h - 16) / 2, "OK", HUB_TEXT, 1);
			s_modal_ok = b;
			s_has_ok = 1;
		}
	}
}

static void hub_paint_remove_modal(void)
{
	hub_app *a = hub_get(U.ids[U.sel < U.nids ? U.sel : 0]);
	hub_rect m;
	int p = U.p;
	char buf[192];
	int bw = 84 * p, bh = 24 * p, y;

	hub_modal_rect(&m, 360, 150);
	hub_ornate_frame(m, HUB_PANEL2);
	snprintf(buf, sizeof(buf), "REMOVE %s?", a ? (a->title ? a->title :
						      a->id) : "");
	hub_text_trunc(m.x + 12 * p, m.y + 16 * p, buf, HUB_TEXT,
		       p >= 2 ? 2 : 1, m.w - 24 * p);
	y = m.y + m.h - 34 * p;
	s_modal_yes.x = m.x + m.w / 2 - bw - 10 * p;
	s_modal_yes.y = y;
	s_modal_yes.w = bw;
	s_modal_yes.h = bh;
	s_modal_no.x = m.x + m.w / 2 + 10 * p;
	s_modal_no.y = y;
	s_modal_no.w = bw;
	s_modal_no.h = bh;
	s_has_yesno = 1;
	{
		int focus[2] = { 0, 1 };
		hub_rect *br[2] = { &s_modal_yes, &s_modal_no };
		const char *labels[2] = { "YES", "NO" };

		for (int i = 0; i < 2; i++)
		{
			int on = U.remove_focus == focus[i];

			hub_fill(*br[i], on ? 0x17300C : HUB_PANEL);
			mmb_gfx_box(br[i]->x, br[i]->y, br[i]->w, br[i]->h,
				    on ? HUB_LIME : HUB_BORDER_L, p, -1);
			hub_text(br[i]->x + (br[i]->w -
					     hub_text_w(labels[i], 1)) / 2,
				 br[i]->y + (br[i]->h - 16) / 2, labels[i],
				 on ? HUB_LIME : HUB_TEXT, 1);
		}
	}
}

static void hub_paint(void)
{
	switch (U.state)
	{
	case HUB_ST_LOADING:
		hub_paint_loading();
		break;
	case HUB_ST_LIST:
		hub_paint_listing();
		break;
	case HUB_ST_DETAIL:
		hub_draw_detail();
		break;
	case HUB_ST_INSTALL:
	case HUB_ST_INSTALL_FAIL:
		if (U.prev_state == HUB_ST_DETAIL)
			hub_draw_detail();
		else
			hub_paint_listing();
		hub_paint_install_modal(U.state == HUB_ST_INSTALL_FAIL);
		break;
	case HUB_ST_REMOVE:
		if (U.prev_state == HUB_ST_DETAIL)
			hub_draw_detail();
		else
			hub_paint_listing();
		hub_paint_remove_modal();
		break;
	}
}

static void hub_frame(void)
{
	if (!U.active)
		return;
	if (g_console != mmb_console_visible())
		return;
	G.gfx.write_page = 0;
	G.gfx.write_fb = 0;
	G.gfx.display_page = 0;
	s_ntabbox = 0;
	s_nrowbox = 0;
	s_nshotbox = 0;
	s_has_mainbtn = 0;
	s_has_yesno = 0;
	s_has_cancel = 0;
	s_has_ok = 0;
	hub_paint();
	mmb_gfx_dirty_add(0, 0, U.w, U.h);
	mmb_gfx_present();
	U.dirty = 0;
	U.last_frame = mmb_now_ms();
}

/* ---- lifecycle ------------------------------------------------------- */

static void hub_free_ids(void)
{
	if (U.ids)
		G.plat->free(U.ids);
	U.ids = 0;
	U.nids = 0;
	U.idcap = 0;
}

static void hub_leave(void)
{
	if (!U.active)
		return;
	U.active = 0;
	hub_images_free();
	hub_free_ids();
	hub_free();
	if (U.saved_mode != G.gfx.mode || U.saved_bits != G.gfx.bits)
		mmb_gfx_set_mode(U.saved_mode, U.saved_bits);
	G.gfx.font_scale = U.saved_font_scale;
	mmb_gfx_reset_console(1);
}

static int hub_install_target_path(hub_app *a, char *out, int outsz)
{
	const char *p = G.opt.app_path;

	out[0] = 0;
	if (!a)
		return -1;
	if (a->installed_path && a->installed_path[0])
	{
		snprintf(out, (size_t)outsz, "%s", a->installed_path);
		return 0;
	}
	while (p && *p)
	{
		const char *e = p;
		int dn;
		char dir[160];

		while (*e && *e != ';')
			e++;
		dn = (int)(e - p);
		if (dn > (int)sizeof(dir) - 1)
			dn = (int)sizeof(dir) - 1;
		memcpy(dir, p, (unsigned)dn);
		dir[dn] = 0;
		p = *e ? e + 1 : e;
		{
			char *q = dir;

			while (*q == ' ' || *q == '\t')
				q++;
			if (q != dir)
				memmove(dir, q, strlen(q) + 1);
		}
		if (!dir[0])
			continue;
		if (dir[strlen(dir) - 1] == '/')
			snprintf(out, (size_t)outsz, "%s%s.APP", dir,
				 a->id ? a->id : "");
		else
			snprintf(out, (size_t)outsz, "%s/%s.APP", dir,
				 a->id ? a->id : "");
		return 0;
	}
	return -1;
}

static int hub_install_progress(unsigned done, unsigned total, void *ctx)
{
	(void)ctx;
	hub_install_pump();
	if (total && U.inst_total != total)
		U.inst_total = total;
	U.inst_done = done;
	U.dirty = 1;
	hub_frame();
	return U.inst_cancel ? 1 : 0;
}

static void hub_start_install(void)
{
	hub_app *a = hub_cur();
	char err[192];

	if (!a)
		return;
	U.inst_index = U.ids[U.sel];
	U.inst_done = 0;
	U.inst_total = a->size > 0 ? (unsigned)a->size : 0;
	U.inst_checksum = 0;
	U.inst_cancel = 0;
	U.inst_pump = 0;
	U.inst_target[0] = 0;
	hub_install_target_path(a, U.inst_target, sizeof(U.inst_target));
	U.prev_state = U.state;
	U.state = HUB_ST_INSTALL;
	U.errmsg[0] = 0;
	hub_frame();
	if (hub_install(U.inst_index, hub_install_progress, 0, err,
			sizeof(err)) != 0)
	{
		U.inst_cancel = 0;
		/* A cancel is not a failure: no error dialog, no state change,
		 * and the engine has already discarded the partial download. */
		if (err[0] && strstr(err, "CANCELLED"))
		{
			U.inst_checksum = 0;
			U.state = U.prev_state;
			U.dirty = 1;
			hub_rebuild_ids();
			hub_scroll_sel();
			hub_frame();
			return;
		}
		snprintf(U.errmsg, sizeof(U.errmsg), "%s",
			 err[0] ? err : "INSTALL FAILED");
		U.inst_checksum = 2;
		U.state = HUB_ST_INSTALL_FAIL;
		hub_frame();
		return;
	}
	U.inst_cancel = 0;
	U.inst_checksum = 1;
	U.state = U.prev_state;
	U.dirty = 1;
	hub_rebuild_ids();
	hub_scroll_sel();
	hub_frame();
}

static void hub_start_remove(void)
{
	hub_app *a = hub_cur();

	if (!a || !a->installed_path)
		return;
	U.prev_state = U.state;
	U.state = HUB_ST_REMOVE;
	U.remove_focus = 1;
	hub_frame();
}

static void hub_do_remove(void)
{
	char err[160];
	hub_app *a = hub_cur();

	if (a && hub_remove(U.ids[U.sel], err, sizeof(err)) == 0)
	{
		U.state = U.prev_state;
		hub_rebuild_ids();
		hub_scroll_sel();
		hub_frame();
	}
	else
	{
		U.state = U.prev_state;
		hub_frame();
	}
}

static void hub_run_installed(void)
{
	hub_app *a = hub_cur();
	char *cmd;
	unsigned n;

	if (!a || !a->installed_path || !a->installed_path[0])
		return;
	n = (unsigned)strlen(a->installed_path);
	cmd = G.plat->alloc(n + 16);
	if (!cmd)
		return;
	strcpy(cmd, "RUN \"");
	strcat(cmd, a->installed_path);
	strcat(cmd, "\"");
	hub_leave();
	mmb_exec_line(cmd);
	G.plat->free(cmd);
}

static void hub_activate(void)
{
	hub_app *a = hub_cur();

	if (!a)
		return;
	if (a->state == HUB_INSTALLED)
	{
		hub_run_installed();
		return;
	}
	if (a->state == HUB_NOT_INSTALLED || a->state == HUB_UPDATE)
		hub_start_install();
}

/* ---- asset loading --------------------------------------------------- */

static const char *hub_need_asset(void)
{
	hub_app *cur = hub_cur();
	int i, shown = 0;

	if (!cur)
		return 0;
	if (U.state == HUB_ST_DETAIL)
	{
		const char *rel = hub_detail_rel(cur);

		if (rel && !hub_img_find(rel))
			return rel;
		if (cur->icon && cur->icon[0] && !hub_img_find(cur->icon))
			return cur->icon;
		for (i = 0; i < cur->nshots && i < 8; i++)
			if (cur->shots[i] && cur->shots[i][0] &&
			    !hub_img_find(cur->shots[i]))
				return cur->shots[i];
		return 0;
	}
	/* Listing: the selected preview first, then the visible icons. */
	{
		const char *rel = hub_preview_rel(cur);

		if (rel && !hub_img_find(rel))
			return rel;
	}
	for (i = U.list_top; i < U.nids; i++)
	{
		hub_app *a = hub_get(U.ids[i]);
		int h = hub_row_h(i, i == U.sel);
		int y = 0, k;

		for (k = U.list_top; k < i; k++)
			y += hub_row_h(k, k == U.sel);
		if (y + h > U.L.list.h)
			break;
		if (a && a->icon && a->icon[0] && !hub_img_find(a->icon))
			return a->icon;
		if (++shown > 8)
			break;
	}
	return 0;
}

static void hub_load_index_now(void)
{
	char err[192];

	U.load_started = 1;
	U.err = 0;
	U.load_done = 0;
	if (hub_load_index(err, sizeof(err)) != 0)
	{
		U.err = 1;
		snprintf(U.errmsg, sizeof(U.errmsg), "%s",
			 err[0] ? err : "OFFLINE");
		U.load_done = 1;
		hub_frame();
		return;
	}
	hub_build_tabs();
	{
		int i;

		U.tab = -1;
		U.sel = 0;
		U.list_top = 0;
		hub_layout_now();
		U.icon_total = hub_count() < 6 ? hub_count() : 6;
		U.icon_stage = U.icon_total;
		for (i = 0; i < U.icon_stage; i++)
		{
			hub_app *a = hub_get(i);

			if (a && a->icon && a->icon[0])
				hub_img_try(a->icon);
		}
		hub_rebuild_ids();
		hub_scroll_sel();
	}
	U.load_done = 1;
	U.icon_stage = 0;
	U.state = HUB_ST_LIST;
	U.dirty = 1;
	hub_frame();
}

/* ---- keys ------------------------------------------------------------ */

static void hub_tab_move(int dir)
{
	int i, cur = 0;

	if (s_ntabs <= 0)
		return;
	for (i = 0; i < s_ntabs; i++)
		if (s_tabs[i].cat == U.tab)
			cur = i;
	cur += dir;
	if (cur < 0)
		cur = s_ntabs - 1;
	if (cur >= s_ntabs)
		cur = 0;
	U.tab = s_tabs[cur].cat;
	U.sel = 0;
	U.list_top = 0;
	U.about_scroll = 0;
	hub_rebuild_ids();
	hub_scroll_sel();
	U.dirty = 1;
}

static void hub_move(int delta)
{
	if (U.nids <= 0)
		return;
	U.sel += delta;
	if (U.sel < 0)
		U.sel = 0;
	if (U.sel >= U.nids)
		U.sel = U.nids - 1;
	U.about_scroll = 0;
	hub_scroll_sel();
	U.dirty = 1;
}

static void hub_jump_letter(char ch)
{
	int i, n = U.nids;
	char up = (ch >= 'a' && ch <= 'z') ? (char)(ch - 32) : ch;

	if (n <= 0)
		return;
	for (i = 0; i < n; i++)
	{
		int idx = (U.sel + 1 + i) % n;
		hub_app *a = hub_get(U.ids[idx]);
		char first = a && a->title && a->title[0] ? a->title[0] : 0;

		if (first >= 'a' && first <= 'z')
			first = (char)(first - 32);
		if (first == up)
		{
			U.sel = idx;
			U.about_scroll = 0;
			hub_scroll_sel();
			U.dirty = 1;
			return;
		}
	}
}

static void hub_esc_action(void)
{
	switch (U.state)
	{
	case HUB_ST_INSTALL_FAIL:
	case HUB_ST_REMOVE:
		U.state = U.prev_state;
		U.dirty = 1;
		break;
	case HUB_ST_INSTALL:
		hub_request_install_cancel();
		break;
	case HUB_ST_DETAIL:
		U.state = HUB_ST_LIST;
		U.dirty = 1;
		break;
	case HUB_ST_LIST:
	case HUB_ST_LOADING:
		hub_leave();
		break;
	}
}

/* Pump input while hub_install() blocks inside the HTTP client. The platform
 * poll delivers queued SDL/serial bytes; on the serial console they land in
 * the INKEY queue (drain them here), on the SDL console the front end feeds
 * them straight to mmb_hub_key(). Only a cancel request is observed: the
 * install modal must not change state or repaint itself before the transfer
 * has unwound (hub_start_install does the transition). */
static void hub_install_pump(void)
{
	mmb_mouse_state ms;
	int n = 0;

	if (U.inst_pump)
		return;
	U.inst_pump = 1;
	if (G.plat && G.plat->poll_input)
		G.plat->poll_input();
	/* Flush a lone Esc the screenshot-hotkey decoder buffered. */
	mmb_front_poll();
	while (n++ < 64)
	{
		int c = mmb_inkey_pop();

		if (c < 0)
			break;
		if (c == 27)
			hub_request_install_cancel();
	}
	/* A key delivered straight to the front end reaches mmb_hub_key(),
	 * which records an Esc cancel while INSTALL is active. */
	/* The CANCEL button is a mouse target too: poll the pointer and detect
	 * the release edge the same way mmb_hub_poll() does. */
	if (mmb_mouse_read(&ms) && ms.present)
	{
		int click = (U.buttons & 1) && !(ms.buttons & 1);

		U.buttons = ms.buttons;
		U.mx = ms.x;
		U.my = ms.y;
		if (click && s_has_cancel &&
		    hub_rect_contains(&s_modal_cancel, ms.x, ms.y))
			hub_request_install_cancel();
	}
	U.inst_pump = 0;
}

static int hub_key_direct(char c)
{
	int handled = 0;

	switch (U.state)
	{
	case HUB_ST_LOADING:
		if (U.err && (c == '\r' || c == '\n'))
		{
			U.started = (int)mmb_now_ms();
			U.load_started = 0;
			U.load_done = 0;
			U.err = 0;
			U.dirty = 1;
			handled = 1;
		}
		break;
	case HUB_ST_INSTALL_FAIL:
		if (c == '\r' || c == '\n')
		{
			U.state = U.prev_state;
			U.dirty = 1;
			handled = 1;
		}
		break;
	case HUB_ST_INSTALL:
		/* The transfer blocks the main loop; only Esc/CANCEL cancels it
		 * (hub_install_pump records the request). Ignore every other
		 * key so the modal cannot change state mid-download. */
		break;
	case HUB_ST_REMOVE:
		if (c == 'y' || c == 'Y')
		{
			hub_do_remove();
			handled = 1;
		}
		else if (c == 'n' || c == 'N')
		{
			U.state = U.prev_state;
			U.dirty = 1;
			handled = 1;
		}
		else if (c == '\r' || c == '\n')
		{
			if (U.remove_focus == 0)
				hub_do_remove();
			else
			{
				U.state = U.prev_state;
				U.dirty = 1;
			}
			handled = 1;
		}
		break;
	case HUB_ST_LIST:
		if (c == '\r' || c == '\n')
		{
			if (U.nids > 0)
			{
				U.state = HUB_ST_DETAIL;
				U.detail_shot = 0;
				U.about_scroll = 0;
				U.dirty = 1;
			}
			handled = 1;
		}
		else if (c == 127 || c == 8 || c == 3)
		{
			/* Del arrives as a CSI sequence; 3 is Ctrl+C. */
			if (c == 3)
				hub_esc_action();
			else
				hub_start_remove();
			handled = 1;
		}
		else if (c > 32 && c < 127)
		{
			hub_jump_letter(c);
			handled = 1;
		}
		break;
	case HUB_ST_DETAIL:
		if (c == '\r' || c == '\n')
		{
			hub_activate();
			handled = 1;
		}
		else if (c == 127 || c == 8)
		{
			hub_start_remove();
			handled = 1;
		}
		else if (c > 32 && c < 127)
			handled = 1;
		break;
	}
	return handled;
}

static void hub_arrow(int code, unsigned arg)
{
	switch (U.state)
	{
	case HUB_ST_REMOVE:
		if (code == 'C' || code == 'D' || code == 'A' || code == 'B')
		{
			U.remove_focus = !U.remove_focus;
			U.dirty = 1;
		}
		break;
	case HUB_ST_LIST:
		if (code == 'A')
			hub_move(-1);
		else if (code == 'B')
			hub_move(1);
		else if (code == 'C')
			hub_tab_move(1);
		else if (code == 'D')
			hub_tab_move(-1);
		else if (code == 'H')
		{
			U.sel = 0;
			hub_scroll_sel();
			U.dirty = 1;
		}
		else if (code == 'F')
		{
			U.sel = U.nids > 0 ? U.nids - 1 : 0;
			hub_scroll_sel();
			U.dirty = 1;
		}
		else if (code == '~' && arg == 3)
			hub_start_remove();
		else if (code == '~' && (arg == 5 || arg == 6))
		{
			int page = U.L.list.h / U.L.row_h;

			if (page < 1)
				page = 1;
			hub_move(arg == 5 ? -page : page);
		}
		else if (code == '~' && (arg == 1 || arg == 7))
		{
			U.sel = 0;
			hub_scroll_sel();
			U.dirty = 1;
		}
		else if (code == '~' && (arg == 4 || arg == 8))
		{
			U.sel = U.nids > 0 ? U.nids - 1 : 0;
			hub_scroll_sel();
			U.dirty = 1;
		}
		break;
	case HUB_ST_DETAIL:
		if (code == 'C')
		{
			hub_app *a = hub_cur();

			if (a && a->nshots > 0)
			{
				U.detail_shot = (U.detail_shot + 1) % a->nshots;
				U.dirty = 1;
			}
		}
		else if (code == 'D')
		{
			hub_app *a = hub_cur();

			if (a && a->nshots > 0)
			{
				U.detail_shot = (U.detail_shot + a->nshots - 1) %
						a->nshots;
				U.dirty = 1;
			}
		}
		else if (code == 'A')
		{
			U.about_scroll -= 1;
			if (U.about_scroll < 0)
				U.about_scroll = 0;
			U.dirty = 1;
		}
		else if (code == 'B')
		{
			U.about_scroll += 1;
			U.dirty = 1;
		}
		else if (code == '~' && (arg == 5 || arg == 6))
		{
			U.about_scroll += arg == 5 ? -6 : 6;
			if (U.about_scroll < 0)
				U.about_scroll = 0;
			U.dirty = 1;
		}
		else if (code == '~' && arg == 3)
			hub_start_remove();
		break;
	default:
		break;
	}
}

const char *mmb_hub_key(char c)
{
	if (!U.active)
		return "";
	if (U.state == HUB_ST_INSTALL)
	{
		/* The transfer blocks the main loop, so there is no idle timer
		 * to tell a lone Esc from a CSI sequence. Esc/Ctrl+C is the
		 * documented cancel gesture here, and arrows/other keys have no
		 * meaning in the modal: record the request and let the download
		 * progress callback unwind it. */
		if (c == 27 || c == 3)
			hub_request_install_cancel();
		return "";
	}
	if (U.esc)
	{
		unsigned char uc = (unsigned char)c;

		if (U.esc == 1)
		{
			if (c == '[')
			{
				U.esc = 2;
				U.esc_arg = 0;
				return "";
			}
			if (c == 'O')
			{
				U.esc = 3;
				return "";
			}
			U.esc = 0;
			hub_esc_action();
			return "";
		}
		if (U.esc == 2)
		{
			U.esc = 0;
			if (uc >= '0' && uc <= '9')
			{
				U.esc_arg = U.esc_arg * 10 + (unsigned)(uc - '0');
				U.esc = 2;
				return "";
			}
			if (c == ';')
			{
				U.esc = 2;
				return "";
			}
			hub_arrow(c, U.esc_arg);
			return "";
		}
		/* SS3: ESC O A..D */
		U.esc = 0;
		if (uc >= 0x40 && uc <= 0x7e)
			hub_arrow(c, 0);
		return "";
	}
	if (c == 27)
	{
		U.esc = 1;
		U.esc_at = mmb_now_ms();
		return "";
	}
	hub_key_direct(c);
	return "";
}

void mmb_hub_close(void)
{
	if (!U.active)
		return;
	U.esc = 0;
	hub_leave();
}

int mmb_in_hub(void)
{
	return U.active;
}

/* ---- mouse ----------------------------------------------------------- */

static int hub_box_hit(const hub_box *b, int n, int x, int y, int *value)
{
	int i;

	for (i = 0; i < n; i++)
		if (hub_rect_contains(&b[i].r, x, y))
		{
			if (value)
				*value = b[i].value;
			return 1;
		}
	return 0;
}

static void hub_mouse_click(int x, int y)
{
	int v;

	if (U.state == HUB_ST_INSTALL)
	{
		if (s_has_cancel && hub_rect_contains(&s_modal_cancel, x, y))
			hub_request_install_cancel();
		return;
	}
	if (U.state == HUB_ST_REMOVE)
	{
		if (hub_rect_contains(&s_modal_yes, x, y))
			hub_do_remove();
		else if (hub_rect_contains(&s_modal_no, x, y))
		{
			U.state = U.prev_state;
			U.dirty = 1;
		}
		else if (!hub_rect_contains(&s_modal_yes, x, y))
		{
			U.state = U.prev_state;
			U.dirty = 1;
		}
		return;
	}
	if (U.state == HUB_ST_INSTALL_FAIL)
	{
		U.state = U.prev_state;
		U.dirty = 1;
		return;
	}
	if (U.state == HUB_ST_LIST)
	{
		if (hub_box_hit(s_tabbox, s_ntabbox, x, y, &v) && v != U.tab)
		{
			U.tab = v;
			U.sel = 0;
			U.list_top = 0;
			hub_rebuild_ids();
			hub_scroll_sel();
			U.dirty = 1;
			return;
		}
		if (hub_box_hit(s_rowbox, s_nrowbox, x, y, &v))
		{
			if (v == U.sel)
			{
				if (U.nids > 0)
				{
					U.state = HUB_ST_DETAIL;
					U.detail_shot = 0;
					U.about_scroll = 0;
					U.dirty = 1;
				}
			}
			else
			{
				U.sel = v;
				U.about_scroll = 0;
				hub_scroll_sel();
				U.dirty = 1;
			}
			return;
		}
	}
	else if (U.state == HUB_ST_DETAIL)
	{
		if (hub_box_hit(s_shotbox, s_nshotbox, x, y, &v))
		{
			U.detail_shot = v;
			U.dirty = 1;
			return;
		}
		if (s_has_mainbtn && hub_rect_contains(&s_mainbtn, x, y))
		{
			hub_activate();
			return;
		}
	}
}

static void hub_mouse_wheel(int wheel)
{
	if (!wheel)
		return;
	if (U.state == HUB_ST_LIST)
		hub_move(wheel > 0 ? -3 : 3);
	else if (U.state == HUB_ST_DETAIL)
	{
		U.about_scroll += wheel > 0 ? -3 : 3;
		if (U.about_scroll < 0)
			U.about_scroll = 0;
		U.dirty = 1;
	}
}

/* ---- poll ------------------------------------------------------------ */

void mmb_hub_poll(void)
{
	unsigned now;
	mmb_mouse_state ms;

	if (!U.active)
		return;
	if (G.gfx.w != U.layout_w || G.gfx.h != U.layout_h)
	{
		U.w = G.gfx.w > 0 ? G.gfx.w : 640;
		U.h = G.gfx.h > 0 ? G.gfx.h : 360;
		hub_layout_now();
		U.dirty = 1;
	}
	now = mmb_now_ms();

	if (U.esc == 1 && now - U.esc_at >= HUB_ESC_IDLE_MS)
	{
		U.esc = 0;
		hub_esc_action();
	}

	if (mmb_mouse_read(&ms) && ms.present)
	{
		int click;

		click = (U.buttons & 1) && !(ms.buttons & 1);
		U.buttons = ms.buttons;
		U.mx = ms.x;
		U.my = ms.y;
		if (click)
			hub_mouse_click(ms.x, ms.y);
		if (ms.wheel)
			hub_mouse_wheel(ms.wheel);
	}

	if (U.state == HUB_ST_LOADING && !U.load_started &&
	    now - (unsigned)U.started >= HUB_LOAD_DELAY_MS)
		hub_load_index_now();

	if (U.active && U.state != HUB_ST_LOADING && now - U.last_asset >=
	    HUB_ASSET_MS)
	{
		const char *rel = hub_need_asset();

		if (rel)
		{
			U.last_asset = now;
			if (hub_img_try(rel))
				U.dirty = 1;
		}
	}

	if (U.active && U.state == HUB_ST_LOADING && now - U.last_frame >= 120)
		U.dirty = 1;
	if (U.active && U.dirty && now - U.last_frame >= HUB_FRAME_MS)
		hub_frame();
}

/* ---- entry ----------------------------------------------------------- */

void mmb_hub_open(void)
{
	hub_rect r;

	if (U.active)
		return;
	if (G.running)
		mmb_error("?Not available in RUN");
	memset(&U, 0, sizeof(U));
	U.saved_mode = G.gfx.mode;
	U.saved_bits = G.gfx.bits;
	U.saved_font_scale = G.gfx.font_scale;
	if (G.gfx.bits < 16)
		mmb_gfx_set_mode(G.gfx.mode, 16);
	U.w = G.gfx.w > 0 ? G.gfx.w : 640;
	U.h = G.gfx.h > 0 ? G.gfx.h : 360;
	U.p = 1;
	s_ntabs = 1;
	s_tabs[0].cat = -1;
	strcpy(s_tabs[0].name, "ALL");
	hub_layout_now();
	U.tab = -1;
	U.state = HUB_ST_LOADING;
	U.started = (int)mmb_now_ms();
	U.load_started = 0;
	U.load_done = 0;
	U.err = 0;
	U.detail_shot = 0;
	U.about_scroll = 0;
	U.active = 1;
	U.buttons = 0;
	U.last_frame = 0;
	U.last_asset = 0;
	r.x = 0;
	r.y = 0;
	r.w = U.w;
	r.h = U.h;
	(void)r;
	hub_frame();
}
