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
#include "hub_chrome.h"
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
#define HUB_LOAD_HOLD_MS 800
#define HUB_FRAME_MS 45
#define HUB_ASSET_MS 25

enum {
	HUB_ST_LOADING = 0,
	HUB_ST_LIST,
	HUB_ST_DETAIL,
	HUB_ST_INSTALL,
	HUB_ST_INSTALL_FAIL,
	HUB_ST_REMOVE,
	HUB_ST_INSTALL_DONE
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
	int offline;     /* listing came from the on-disk cache (#1544) */
	int offline_note;
	unsigned offline_note_at;

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
static int s_tab_scale;
static int s_clip;
static int s_cx0, s_cy0, s_cx1, s_cy1;
static hub_rect s_modal_x;
static int s_list_rpad; /* scrollbar width taken from the rows */

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
						{
							int px = x + i * 8 * scale +
								 colbit * scale + sx;
							int py = y + row * scale + sy;

							if (s_clip &&
							    (px < s_cx0 || py < s_cy0 ||
							     px >= s_cx1 || py >= s_cy1))
								continue;
							mmb_gfx_plot(px, py, col);
						}
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

/* Nearest-neighbour scale of the 8x16 face to an arbitrary cell. Used for
 * the detail paragraph, which sits between the 8px and 16px sizes. */
static void hub_text_cell(int x, int y, const char *s, unsigned n,
			  unsigned col, int gw, int gh)
{
	unsigned i;

	if (!s || gw < 1 || gh < 1)
		return;
	for (i = 0; i < n; i++)
	{
		unsigned char ch = (unsigned char)s[i];
		int row, colbit;

		if (ch == '\n' || ch == '\r')
			continue;
		for (row = 0; row < 16; row++)
		{
			unsigned char bits = mmb_cp437_8x16[ch * 16u + row];
			int y0 = y + row * gh / 16;
			int y1 = y + (row + 1) * gh / 16;

			for (colbit = 0; colbit < 8; colbit++)
			{
				int x0, x1, px, py;

				if (!(bits & (unsigned char)(0x80u >> colbit)))
					continue;
				x0 = x + (int)i * gw + colbit * gw / 8;
				x1 = x + (int)i * gw + (colbit + 1) * gw / 8;
				for (py = y0; py < y1; py++)
					for (px = x0; px < x1; px++)
					{
						if (s_clip &&
						    (px < s_cx0 || py < s_cy0 ||
						     px >= s_cx1 || py >= s_cy1))
							continue;
						mmb_gfx_plot(px, py, col);
					}
			}
		}
	}
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
			  int skip, int maxlines, unsigned col, int scale,
			  int gw, int gh)
{
	const char *q = s;
	int line = 0, cx = 0;
	int cw;

	if (scale < 1)
		scale = 1;
	if (gw < 1)
	{
		gw = 8 * scale;
		gh = 16 * scale;
	}
	if (gh < 1)
		gh = gw * 2;
	cw = gw;

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
			{
				if (hub_chrome_has("ui-font") &&
				    cw == hub_chrome_ui_w("M", 1))
				{
					char buf[160];
					int n = take;

					if (n > (int)sizeof(buf) - 1)
						n = (int)sizeof(buf) - 1;
					memcpy(buf, word, (size_t)n);
					buf[n] = 0;
					hub_chrome_ui(buf, x + cx * cw,
						      y + (line - skip) * lineh,
						      col, 1);
				}
				else if (cw == 8 * scale && gh == 16 * scale)
					hub_text_n(x + cx * cw,
						   y + (line - skip) * lineh,
						   word, (unsigned)take, col,
						   scale);
				else
					hub_text_cell(x + cx * cw,
						      y + (line - skip) * lineh,
						      word, (unsigned)take, col,
						      gw, gh);
			}
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

/* Optional clip, used while a tab strip scrolls past the panel edge. */
static void hub_clip_set(int x0, int y0, int x1, int y1)
{
	s_clip = 1;
	s_cx0 = x0;
	s_cy0 = y0;
	s_cx1 = x1;
	s_cy1 = y1;
	hub_chrome_clip(x0, y0, x1, y1);
}

static void hub_clip_clear(void)
{
	s_clip = 0;
	hub_chrome_unclip();
}

static int hub_clip_rect(hub_rect *r)
{
	if (!r)
		return 0;
	if (s_clip)
	{
		if (r->x < s_cx0)
		{
			r->w -= s_cx0 - r->x;
			r->x = s_cx0;
		}
		if (r->y < s_cy0)
		{
			r->h -= s_cy0 - r->y;
			r->y = s_cy0;
		}
		if (r->x + r->w > s_cx1)
			r->w = s_cx1 - r->x;
		if (r->y + r->h > s_cy1)
			r->h = s_cy1 - r->y;
	}
	return r->w > 0 && r->h > 0;
}

static void hub_fill(hub_rect r, unsigned col)
{
	if (hub_clip_rect(&r))
		mmb_gfx_fill_rect(r.x, r.y, r.w, r.h, col);
}

static void hub_hline(int x, int y, int w, unsigned col, int t)
{
	hub_rect r;

	if (t < 1)
		t = 1;
	r.x = x;
	r.y = y;
	r.w = w;
	r.h = t;
	hub_fill(r, col);
}

static void hub_vline(int x, int y, int h, unsigned col, int t)
{
	hub_rect r;

	if (t < 1)
		t = 1;
	r.x = x;
	r.y = y;
	r.w = t;
	r.h = h;
	hub_fill(r, col);
}

static int hub_has(const char *s)
{
	return s && s[0];
}

static void hub_upper(const char *s, char *out, int n)
{
	int i = 0;

	if (!s)
		s = "";
	if (n < 1)
		return;
	for (; s[i] && i < n - 1; i++)
	{
		unsigned char c = (unsigned char)s[i];

		if (c >= 'a' && c <= 'z')
			c = (unsigned char)(c - 32);
		out[i] = (char)c;
	}
	out[i] = 0;
}

/* The listing title is twice the summary. At 720p that is 32px over 16px,
 * which is what fits six rows. Detail body copy uses the same step so a
 * paragraph still fits above the button. */
static int hub_body_scale(void)
{
	return U.p >= 3 ? 2 : 1;
}

static int hub_title_scale(void)
{
	return hub_body_scale() * 2;
}

static int hub_ui_scale(void)
{
	return hub_body_scale();
}

/* UI face from A:/hub/ui-font. zoom is an extra integer scale. The CP437
 * face is the fallback when the sheet is missing. */
static int hub_ui_h(int zoom)
{
	int h;

	if (zoom < 1)
		zoom = 1;
	h = hub_chrome_ui_h();
	if (h > 0)
		return h * zoom;
	return 16 * zoom;
}

static int hub_ui_w(const char *s, int zoom)
{
	if (zoom < 1)
		zoom = 1;
	if (hub_chrome_has("ui-font"))
		return hub_chrome_ui_w(s, zoom);
	return hub_text_w(s, zoom);
}

static void hub_label(int x, int y, const char *s, unsigned col, int zoom)
{
	if (!s)
		return;
	if (zoom < 1)
		zoom = 1;
	if (hub_chrome_ui(s, x, y, col, zoom))
		return;
	hub_text(x, y, s, col, zoom);
}

static void hub_ui_r(int right, int y, const char *s, unsigned col, int zoom)
{
	hub_label(right - hub_ui_w(s, zoom), y, s, col, zoom);
}

static void hub_ui_trunc(int x, int y, const char *s, unsigned col,
			   int zoom, int maxw)
{
	char buf[160];
	int adv, maxc, n;

	if (!s)
		return;
	if (zoom < 1)
		zoom = 1;
	adv = hub_ui_w("M", zoom);
	if (adv < 1)
		adv = 8 * zoom;
	if (maxw <= 0)
		return;
	maxc = maxw / adv;
	n = (int)strlen(s);
	if (n > maxc)
	{
		if (maxc < 0)
			maxc = 0;
		if (maxc > (int)sizeof(buf) - 1)
			maxc = (int)sizeof(buf) - 1;
		if (maxc > 3)
		{
			memcpy(buf, s, (size_t)(maxc - 3));
			memcpy(buf + maxc - 3, "...", 4);
		}
		else
		{
			memcpy(buf, s, (size_t)maxc);
			buf[maxc] = 0;
		}
		s = buf;
	}
	hub_label(x, y, s, col, zoom);
}

static unsigned hub_lerp(unsigned a, unsigned b, int t, int n)
{
	int ar, ag, ab, br, bg, bb;

	if (n < 1)
		n = 1;
	if (t < 0)
		t = 0;
	if (t > n)
		t = n;
	ar = (int)((a >> 16) & 255);
	ag = (int)((a >> 8) & 255);
	ab = (int)(a & 255);
	br = (int)((b >> 16) & 255);
	bg = (int)((b >> 8) & 255);
	bb = (int)(b & 255);
	return (unsigned)((ar + (br - ar) * t / n) << 16) |
	       (unsigned)((ag + (bg - ag) * t / n) << 8) |
	       (unsigned)(ab + (bb - ab) * t / n);
}

/* Chunkier display type: the CP437 bitmap, a vertical gradient and a slight
 * italic lean. Used for the detail title. */
static void hub_grad_text(int x, int y, const char *s, unsigned top,
			  unsigned bot, int scale, int slant, unsigned shade)
{
	const char *q;
	int pass, passes;

	if (!s || scale < 1)
		return;
	passes = shade ? 2 : 1;
	for (pass = 0; pass < passes; pass++)
	{
		int cx = x;

		for (q = s; *q; q++)
		{
			unsigned char ch = (unsigned char)*q;
			int row, colbit;
			int outline = scale >= 4 ? 3 : (scale >= 2 ? 2 : 1);
			int bold = scale >= 2 ? (scale + 1) / 3 : 0;

			if (ch == '\n' || ch == '\r')
				continue;
			for (row = 0; row < 16; row++)
			{
				unsigned char bits = mmb_cp437_8x16[ch * 16u + row];
				unsigned col = hub_lerp(top, bot, row, 15);
				int lean = slant ? (15 - row) * scale / 5 : 0;
				int sy, sx;

				for (colbit = 0; colbit < 8; colbit++)
				{
					int px, py;

					if (!(bits & (unsigned char)(0x80u >> colbit)))
						continue;
					for (sy = 0; sy < scale; sy++)
					{
						for (sx = 0; sx < scale; sx++)
						{
							px = cx + colbit * scale + sx + lean;
							py = y + row * scale + sy;
							if (pass == 0 && shade)
							{
								int oy, ox;

								for (oy = -outline; oy <= outline; oy++)
									for (ox = -outline; ox <= outline; ox++)
										if (ox || oy)
											mmb_gfx_plot(px + ox, py + oy, shade);
							}
							else
							{
								mmb_gfx_plot(px, py, col);
								if (bold)
								{
									mmb_gfx_plot(px + bold, py, col);
									mmb_gfx_plot(px, py + bold, col);
								}
							}
						}
					}
				}
			}
			cx += 8 * scale;
		}
	}
}

static void hub_bevel(hub_rect r, unsigned fill)
{
	int t = U.p < 2 ? 1 : U.p;

	hub_fill(r, fill);
	hub_hline(r.x, r.y, r.w, HUB_BORDER_L, t);
	hub_vline(r.x, r.y, r.h, HUB_BORDER_L, t);
	hub_hline(r.x, r.y + r.h - t, r.w, HUB_BORDER_D, t);
	hub_vline(r.x + r.w - t, r.y, r.h, HUB_BORDER_D, t);
}

static void hub_dither_bg(void)
{
	int x, y, step = 4 * U.p;

	if (step < 3)
		step = 3;
	mmb_gfx_fill_rect(0, 0, U.w, U.h, 0x07091A);
	for (y = 0; y < U.h; y += step)
		for (x = 0; x < U.w; x += step)
			if ((((x + y) / step) & 3) == 0)
				mmb_gfx_plot(x, y, 0x14182E);
}

static void hub_face(int x0, int y0, int x1, int y1, int x2, int y2,
		     int x3, int y3, unsigned col)
{
	int xs[4], ys[4];

	xs[0] = x0; ys[0] = y0;
	xs[1] = x1; ys[1] = y1;
	xs[2] = x2; ys[2] = y2;
	xs[3] = x3; ys[3] = y3;
	mmb_gfx_fill_polygon(xs, ys, 4, col);
}

static void hub_window(int x0, int y0, int x1, int y1, int x2, int y2,
		       int x3, int y3, unsigned col, unsigned edge, int t)
{
	int xs[4], ys[4], nx[4], ny[4];
	int i, cx = 0, cy = 0;

	xs[0] = x0; ys[0] = y0;
	xs[1] = x1; ys[1] = y1;
	xs[2] = x2; ys[2] = y2;
	xs[3] = x3; ys[3] = y3;
	for (i = 0; i < 4; i++)
	{
		cx += xs[i];
		cy += ys[i];
	}
	cx /= 4;
	cy /= 4;
	for (i = 0; i < 4; i++)
	{
		nx[i] = xs[i] + (cx - xs[i]) * 28 / 100;
		ny[i] = ys[i] + (cy - ys[i]) * 28 / 100;
	}
	if (col)
		mmb_gfx_fill_polygon(nx, ny, 4, col);
	if (t > 0)
	{
		for (i = 0; i < 4; i++)
			mmb_gfx_line(nx[i], ny[i], nx[(i + 1) & 3],
				     ny[(i + 1) & 3], edge, t);
	}
}

/* Isometric cube shared by the header mark and the loading screen.
 * `size` is the width. `neon` draws the thick magenta/cyan wireframe. */
static void hub_cube(int cx, int cy, int size, int neon)
{
	int hw = size / 2;
	int rise = size / 4;
	int vh = size * 50 / 100;
	int total, y0, t, core;
	int tx, ty, trx, try_, tlx, tly, mx, my, brx, bry, blx, bly, bx, by;
	unsigned et, el, er, glow_l, glow_r, glow_t;

	if (hw < 3)
		hw = 3;
	if (rise < 2)
		rise = 2;
	if (vh < 4)
		vh = 4;
	total = 2 * rise + vh;
	y0 = cy - total / 2;
	t = size / 16;
	if (t < 1)
		t = 1;
	if (t > 10)
		t = 10;
	core = t > 3 ? t / 2 : 1;
	tx = cx;
	ty = y0;
	trx = cx + hw;
	try_ = y0 + rise;
	tlx = cx - hw;
	tly = y0 + rise;
	mx = cx;
	my = y0 + 2 * rise;
	brx = cx + hw;
	bry = y0 + rise + vh;
	blx = cx - hw;
	bly = y0 + rise + vh;
	bx = cx;
	by = y0 + total;
	if (neon)
	{
		et = 0xF6F8FF;
		el = 0xE040FB;
		er = 0x29D3F0;
		glow_t = 0x6A78A0;
		glow_l = 0xA020A8;
		glow_r = 0x1088A0;
		/* Hollow wireframe: inner frame on each face, no fill, so
		 * the back edges stay visible. */
		hub_window(tx, ty, trx, try_, mx, my, tlx, tly, 0,
			   0xC8D4F0, core);
		hub_window(tlx, tly, mx, my, bx, by, blx, bly, 0, el, core);
		hub_window(mx, my, trx, try_, brx, bry, bx, by, 0, er, core);
	}
	else
	{
		et = 0xF4F6FF;
		el = 0xC8CCD8;
		er = 0x9AA0B4;
		glow_t = glow_l = glow_r = 0;
		hub_face(tx, ty, trx, try_, mx, my, tlx, tly, 0x4A5270);
		hub_face(tlx, tly, mx, my, bx, by, blx, bly, 0x343C58);
		hub_face(mx, my, trx, try_, brx, bry, bx, by, 0x242C48);
	}
	if (neon && t > 1)
	{
		/* A 3px dimmer stroke past the bright core, then a 2px
		 * step, so the wireframe reads as a glow rather than a
		 * hard line. */
		int halo = t + 6;
		int mid = t + 3;
		unsigned mid_t = 0xC8D4FF;
		unsigned mid_l = 0xE060FF;
		unsigned mid_r = 0x40E8FF;

		mmb_gfx_line(tx, ty, trx, try_, glow_t, halo);
		mmb_gfx_line(tx, ty, tlx, tly, glow_t, halo);
		mmb_gfx_line(tlx, tly, blx, bly, glow_l, halo);
		mmb_gfx_line(blx, bly, bx, by, glow_l, halo);
		mmb_gfx_line(trx, try_, brx, bry, glow_r, halo);
		mmb_gfx_line(brx, bry, bx, by, glow_r, halo);
		mmb_gfx_line(mx, my, bx, by, glow_t, halo);
		mmb_gfx_line(tx, ty, trx, try_, mid_t, mid);
		mmb_gfx_line(tx, ty, tlx, tly, mid_t, mid);
		mmb_gfx_line(tlx, tly, blx, bly, mid_l, mid);
		mmb_gfx_line(blx, bly, bx, by, mid_l, mid);
		mmb_gfx_line(trx, try_, brx, bry, mid_r, mid);
		mmb_gfx_line(brx, bry, bx, by, mid_r, mid);
		mmb_gfx_line(mx, my, bx, by, mid_t, mid);
	}
	mmb_gfx_line(tx, ty, trx, try_, et, t);
	mmb_gfx_line(tx, ty, tlx, tly, et, t);
	mmb_gfx_line(tlx, tly, blx, bly, el, t);
	mmb_gfx_line(blx, bly, bx, by, el, t);
	mmb_gfx_line(trx, try_, brx, bry, er, t);
	mmb_gfx_line(brx, bry, bx, by, er, t);
	mmb_gfx_line(tx, ty, mx, my, et, core + 1);
	mmb_gfx_line(tlx, tly, mx, my, el, core);
	mmb_gfx_line(trx, try_, mx, my, er, core);
	mmb_gfx_line(mx, my, bx, by, et, t);
	if (neon && core > 1)
	{
		mmb_gfx_line(tx, ty, trx, try_, 0xFFFFFF, core);
		mmb_gfx_line(tx, ty, tlx, tly, 0xFFFFFF, core);
		mmb_gfx_line(mx, my, bx, by, 0xFFFFFF, core);
	}
	if (neon && size >= 80)
	{
		int s = size / 6;
		int i;
		static const int dx[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };
		static const int dy[8] = { -1, -1, 0, 1, 1, 1, 0, -1 };
		int len[8];
		unsigned col[8] = {
			0xFFFFFF, 0xE8C0FF, 0xFFFFFF, 0xB0F4FF,
			0xFFFFFF, 0xB0F4FF, 0xFFFFFF, 0xE8C0FF
		};

		len[0] = s;
		len[1] = s * 2 / 3;
		len[2] = s * 5 / 6;
		len[3] = s / 3;
		len[4] = s / 4;
		len[5] = s / 3;
		len[6] = s * 5 / 6;
		len[7] = s * 2 / 3;
		for (i = 0; i < 8; i++)
			mmb_gfx_line(tx, ty, tx + dx[i] * len[i],
				     ty + dy[i] * len[i], col[i],
				     (i & 1) ? 1 : 2);
		mmb_gfx_fill_rect(tx - 2, ty - 2, 5, 5, 0xFFFFFF);
		mmb_gfx_fill_rect(tx - 1, ty - 6, 3, 13, 0xFFFFFF);
		mmb_gfx_fill_rect(tx - 6, ty - 1, 13, 3, 0xFFFFFF);
	}
}

/* Filled rect in a corner-local frame. sx/sy are +1 or -1. */
static void hub_fill_rel(int x, int y, int sx, int sy, int rx, int ry,
			 int rw, int rh, unsigned col)
{
	int px, py;

	if (rw < 1 || rh < 1)
		return;
	px = sx > 0 ? x + rx : x - rx - rw + 1;
	py = sy > 0 ? y + ry : y - ry - rh + 1;
	mmb_gfx_fill_rect(px, py, rw, rh, col);
}

/* One bronze bracket, mirrored onto all four corners. No stars. */
static void hub_corner(int x, int y, int sx, int sy, int arm)
{
	int p = U.p < 1 ? 1 : U.p;
	int thick = 3 * p;
	int riv = 2 * p + 1;
	int inset;

	if (thick < 3)
		thick = 3;
	if (arm < thick * 3)
		arm = thick * 3;
	hub_fill_rel(x, y, sx, sy, 0, 0, arm, thick, 0xE0A44A);
	hub_fill_rel(x, y, sx, sy, 0, 0, thick, arm, 0xE0A44A);
	hub_fill_rel(x, y, sx, sy, 0, 0, arm, p, 0xF6D090);
	hub_fill_rel(x, y, sx, sy, 0, 0, p, arm, 0xF6D090);
	hub_fill_rel(x, y, sx, sy, 0, thick - p, arm, p, 0x8A5820);
	hub_fill_rel(x, y, sx, sy, thick - p, 0, p, arm, 0x8A5820);
	inset = thick + p;
	hub_fill_rel(x, y, sx, sy, inset, inset, arm - inset - p, p + 1,
		     HUB_PURPLE);
	hub_fill_rel(x, y, sx, sy, inset, inset, p + 1, arm - inset - p,
		     HUB_PURPLE);
	hub_fill_rel(x, y, sx, sy, p, p, riv, riv, 0xFFF0C8);
	hub_fill_rel(x, y, sx, sy, arm - riv - p, p, riv, riv, HUB_CYAN);
	hub_fill_rel(x, y, sx, sy, p, arm - riv - p, riv, riv, HUB_MAGENTA);
}

static void hub_frame_chrome(hub_rect r)
{
	int p = U.p < 1 ? 1 : U.p;

	if (hub_chrome_frame(r.x, r.y, r.w, r.h))
		return;
	int arm = 26 * p;
	int outer = 2 * p + 1;
	int inset;

	if (r.w < 8 || r.h < 8)
		return;
	if (arm > r.w / 4)
		arm = r.w / 4;
	if (arm > r.h / 4)
		arm = r.h / 4;
	if (arm < 14)
		arm = 14;
	mmb_gfx_box(r.x, r.y, r.w, r.h, 0xC89848, outer, -1);
	inset = outer;
	mmb_gfx_box(r.x + inset, r.y + inset, r.w - 2 * inset, r.h - 2 * inset,
		    0x6A30C0, p + 1, -1);
	inset += p + 1;
	mmb_gfx_box(r.x + inset, r.y + inset, r.w - 2 * inset, r.h - 2 * inset,
		    0x3A2858, p, -1);
	hub_corner(r.x, r.y, 1, 1, arm);
	hub_corner(r.x + r.w - 1, r.y, -1, 1, arm);
	hub_corner(r.x, r.y + r.h - 1, 1, -1, arm);
	hub_corner(r.x + r.w - 1, r.y + r.h - 1, -1, -1, arm);
}

static void hub_ornate_frame(hub_rect r, unsigned fill)
{
	if (r.w < 8 || r.h < 8)
		return;
	hub_fill(r, fill);
	hub_frame_chrome(r);
}

static hub_img *hub_img_find(const char *rel);
static void hub_draw_image_fit(uint32_t *pix, int iw, int ih, hub_rect r);

/* Frame edge to the start of the clear area: the rim plus a margin wide
 * enough that the inner corner cut never reaches content. */
static int hub_frame_inset(void)
{
	int p = U.p < 1 ? 1 : U.p;
	int band = hub_chrome_frame_band();

	if (band > 0)
		return band + HUB_FRAME_PAD * p;
	return 7 * p + 2;
}

static hub_rect hub_inset(hub_rect r, int d)
{
	r.x += d;
	r.y += d;
	r.w -= 2 * d;
	r.h -= 2 * d;
	if (r.w < 0)
		r.w = 0;
	if (r.h < 0)
		r.h = 0;
	return r;
}

/* Picture well: a 1p line and the picture fitted inside it. */
static void hub_picture_well(hub_rect well, hub_img *im, int cube)
{
	int p = U.p < 1 ? 1 : U.p;
	hub_rect img;

	if (well.w < 4 || well.h < 4)
		return;
	mmb_gfx_box(well.x, well.y, well.w, well.h, 0x2C3150, p, -1);
	img = hub_inset(well, p);
	hub_fill(img, 0x070910);
	if (im && im->pix)
		hub_draw_image_fit(im->pix, im->w, im->h, img);
	else if (cube)
		hub_cube(img.x + img.w / 2, img.y + img.h / 2, img.h * 2 / 3, 1);
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

/* `about` and `changes` are either prose or a catalogue path
 * (info/<id>/about.txt). A path has a slash and no whitespace. */
#define HUB_DOC_MAX 8
#define HUB_DOC_BYTES 16384

typedef struct hub_doc {
	char *rel;
	char *text;
} hub_doc;

static hub_doc s_doc[HUB_DOC_MAX];
static int s_ndoc;

static int hub_ends_txt(const char *s)
{
	int n = (int)strlen(s);

	if (n < 4)
		return 0;
	s += n - 4;
	return s[0] == '.' &&
	       (s[1] == 't' || s[1] == 'T') &&
	       (s[2] == 'x' || s[2] == 'X') &&
	       (s[3] == 't' || s[3] == 'T');
}

/* A catalogue text file, not prose and not an image. hero.png also has a
 * slash and no spaces, so the suffix is what keeps it on the image path. */
static int hub_is_docpath(const char *s)
{
	const char *p;

	if (!hub_has(s) || !hub_ends_txt(s))
		return 0;
	for (p = s; *p; p++)
	{
		if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
			return 0;
	}
	return strchr(s, '/') != 0;
}

static hub_doc *hub_doc_find(const char *rel)
{
	int i;

	for (i = 0; i < s_ndoc; i++)
		if (s_doc[i].rel && strcmp(s_doc[i].rel, rel) == 0)
			return &s_doc[i];
	return 0;
}

/* Inline prose is returned as-is. A path yields the fetched text, or
 * `fallback` until that fetch finishes (so the path itself is not drawn). */
static const char *hub_doc_body(const char *field, const char *fallback)
{
	hub_doc *d;

	if (!fallback)
		fallback = "";
	if (!hub_is_docpath(field))
		return hub_has(field) ? field : fallback;
	d = hub_doc_find(field);
	if (d && d->text && d->text[0])
		return d->text;
	return fallback;
}

static int hub_doc_try(const char *rel)
{
	hub_doc *d;
	unsigned char *buf = 0;
	unsigned n = 0, keep;
	char err[96];
	char *text;

	if (!hub_is_docpath(rel) || hub_doc_find(rel) || s_ndoc >= HUB_DOC_MAX)
		return 0;
	d = &s_doc[s_ndoc];
	memset(d, 0, sizeof(*d));
	d->rel = G.plat->alloc(strlen(rel) + 1);
	if (!d->rel)
		return 0;
	memcpy(d->rel, rel, strlen(rel) + 1);
	s_ndoc++;
	text = G.plat->alloc(1);
	if (text)
		text[0] = 0;
	if (hub_fetch(rel, &buf, &n, err, sizeof(err)) == 0 && buf && n)
	{
		keep = n > HUB_DOC_BYTES ? HUB_DOC_BYTES : n;
		if (text)
			G.plat->free(text);
		text = G.plat->alloc(keep + 1);
		if (text)
		{
			memcpy(text, buf, keep);
			text[keep] = 0;
		}
	}
	d->text = text;
	if (buf)
		G.plat->free(buf);
	return 1;
}

static void hub_docs_free(void)
{
	int i;

	for (i = 0; i < s_ndoc; i++)
	{
		if (s_doc[i].rel)
			G.plat->free(s_doc[i].rel);
		if (s_doc[i].text)
			G.plat->free(s_doc[i].text);
		s_doc[i].rel = 0;
		s_doc[i].text = 0;
	}
	s_ndoc = 0;
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

/* Whole picture, centred. An integer scale keeps pixel art on the grid.
 * Anything that does not fit at 1:1 is an area downsample, not a crop. */
static void hub_draw_image_fit(uint32_t *pix, int iw, int ih, hub_rect r)
{
	int n, dw, dh, ox, oy, x, y;

	if (!pix || iw < 1 || ih < 1 || r.w < 1 || r.h < 1)
		return;
	n = r.w / iw;
	if (r.h / ih < n)
		n = r.h / ih;
	if (n >= 1)
	{
		dw = iw * n;
		dh = ih * n;
		ox = r.x + (r.w - dw) / 2;
		oy = r.y + (r.h - dh) / 2;
		for (y = 0; y < dh; y++)
		{
			int sy = y / n;

			for (x = 0; x < dw; x++)
			{
				uint32_t c = pix[sy * iw + x / n];

				if (c >> 24)
					mmb_gfx_plot(ox + x, oy + y, c & 0xFFFFFFu);
			}
		}
		return;
	}
	dw = r.w;
	dh = (int)((long)r.w * ih / iw);
	if (dh > r.h || dh < 1)
	{
		dh = r.h;
		dw = (int)((long)r.h * iw / ih);
		if (dw < 1)
			dw = 1;
	}
	if (dh < 1)
		dh = 1;
	ox = r.x + (r.w - dw) / 2;
	oy = r.y + (r.h - dh) / 2;
	for (y = 0; y < dh; y++)
	{
		int sy0 = (int)((long)y * ih / dh);
		int sy1 = (int)((long)(y + 1) * ih / dh);

		if (sy1 <= sy0)
			sy1 = sy0 + 1;
		if (sy1 > ih)
			sy1 = ih;
		for (x = 0; x < dw; x++)
		{
			int sx0 = (int)((long)x * iw / dw);
			int sx1 = (int)((long)(x + 1) * iw / dw);
			unsigned sr = 0, sg = 0, sb = 0, npx = 0;
			int sy, sx;

			if (sx1 <= sx0)
				sx1 = sx0 + 1;
			if (sx1 > iw)
				sx1 = iw;
			for (sy = sy0; sy < sy1; sy++)
			{
				for (sx = sx0; sx < sx1; sx++)
				{
					uint32_t c = pix[sy * iw + sx];

					if (!(c >> 24))
						continue;
					sr += (c >> 16) & 255u;
					sg += (c >> 8) & 255u;
					sb += c & 255u;
					npx++;
				}
			}
			if (!npx)
				continue;
			mmb_gfx_plot(ox + x, oy + y,
				     ((sr / npx) << 16) | ((sg / npx) << 8) |
				     (sb / npx));
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

static void hub_app_icon(hub_app *a, hub_rect r, int greyed, int framed)
{
	hub_img *im = a ? hub_img_find(a->icon) : 0;
	int p = U.p < 2 ? 1 : U.p;

	hub_fill(r, 0x0B0F22);
	if (im && im->pix && framed == 2)
	{
		int s = 1;
		int dw, dh, ox, oy, yy, xx;

		while ((s + 1) * im->w <= r.w - 4 * p &&
		       (s + 1) * im->h <= r.h - 4 * p)
			s++;
		dw = im->w * s;
		dh = im->h * s;
		ox = r.x + (r.w - dw) / 2;
		oy = r.y + (r.h - dh) / 2;
		for (yy = 0; yy < dh; yy++)
		{
			int sy = yy / s;

			for (xx = 0; xx < dw; xx++)
			{
				uint32_t c = im->pix[sy * im->w + xx / s];

				if (!(c >> 24))
					continue;
				mmb_gfx_plot(ox + xx, oy + yy, c & 0xFFFFFFu);
			}
		}
	}
	else if (im && im->pix)
		hub_draw_image(im->pix, im->w, im->h, r);
	else if (hub_chrome_has("icon-cube"))
		hub_chrome_blit("icon-cube",
				r.x + (r.w - hub_chrome_w("icon-cube")) / 2,
				r.y + (r.h - hub_chrome_h("icon-cube")) / 2);
	else
		hub_cube(r.x + r.w / 2, r.y + r.h / 2, r.w * 3 / 4, 0);
	if (framed)
	{
		int t = p < 2 ? 1 : 2;

		mmb_gfx_box(r.x, r.y, r.w, r.h, 0xC8A060, t, -1);
		if (r.w > 4 * t && r.h > 4 * t)
			mmb_gfx_box(r.x + t, r.y + t, r.w - 2 * t, r.h - 2 * t,
				    0x6A30C0, t, -1);
	}
	if (greyed)
	{
		int x, y;

		for (y = r.y; y < r.y + r.h; y += 2)
			for (x = r.x + (y & 1); x < r.x + r.w; x += 2)
				mmb_gfx_plot(x, y, 0x12162A);
	}
}

/* The left viewer and its strip are screenshots only. The hero, when the
 * index has one, is the picture at the top of the right column. */
static int hub_gallery_count(hub_app *a)
{
	if (!a || a->nshots < 0)
		return 0;
	return a->nshots;
}

static const char *hub_gallery_rel(hub_app *a, int index)
{
	if (!a || index < 0 || index >= a->nshots)
		return 0;
	if (a->shots[index] && a->shots[index][0])
		return a->shots[index];
	return 0;
}

static const char *hub_preview_rel(hub_app *a)
{
	if (!a)
		return 0;
	if (hub_has(a->hero))
		return a->hero;
	if (a->nshots > 0 && a->shots[0] && a->shots[0][0])
		return a->shots[0];
	if (hub_has(a->thumb))
		return a->thumb;
	return 0;
}

static const char *hub_detail_rel(hub_app *a)
{
	const char *rel;

	if (!a)
		return 0;
	rel = hub_gallery_rel(a, U.detail_shot);
	if (rel)
		return rel;
	if (a->nshots > 0 && a->shots[0] && a->shots[0][0])
		return a->shots[0];
	if (hub_has(a->thumb))
		return a->thumb;
	return 0;
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
	int i, scale, longest, avail, n;

	{
		int pw = U.w / 640 < U.h / 360 ? U.w / 640 : U.h / 360;

		if (pw < 1)
			pw = 1;
		U.p = pw;
	}
	/* Small labels, one size. The strip scrolls instead of growing. */
	scale = 1;
	n = s_ntabs > 0 ? s_ntabs : 1;
	avail = U.w - 8 * (U.p < 1 ? 1 : U.p);
	longest = 0;
	for (i = 0; i < s_ntabs; i++)
	{
		int w = hub_text_w(s_tabs[i].name, scale);

		if (w > longest)
			longest = w;
	}
	(void)n;
	(void)avail;
	s_tab_scale = scale;
	tab_w = longest + 8 * U.p;
	hub_layout_compute(&U.L, U.w, U.h, s_ntabs, tab_w);
	U.p = U.L.p;
	U.layout_w = U.w;
	U.layout_h = U.h;
}

/* ---- progress bar ---------------------------------------------------- */

/* Loading bar: bevelled track, segments fading magenta to cyan. `hi`/`lo`
 * override the gradient (the error state passes red). */
static void hub_segbar(hub_rect r, int pct, unsigned hi, unsigned lo)
{
	int p = U.p < 1 ? 1 : U.p;
	int pad = 3 * p;
	int inner = r.w - 2 * pad;
	int segw = 12 * p;
	int gap = 2 * p;
	int nseg, i, fill;
	int gradient = (hi == 0);

	if (inner < 8 * p)
		inner = 8 * p;
	nseg = inner / (segw + gap);
	if (nseg < 8)
		nseg = 8;
	if (nseg > 18)
		nseg = 18;
	segw = (inner - (nseg - 1) * gap) / nseg;
	if (segw < 3 * p)
		segw = 3 * p;
	if (pct < 0)
		pct = 0;
	if (pct > 100)
		pct = 100;
	fill = nseg * pct / 100;
	if (pct > 0 && fill < 1)
		fill = 1;
	{
		int rad = r.h / 3;

		if (rad < 4)
			rad = 4;
		if (rad > r.h / 2)
			rad = r.h / 2;
		if (rad > 8 * p)
			rad = 8 * p;
		/* Dark track. Unfilled segments are not painted, so the
		 * empty part stays this colour. */
		mmb_gfx_rbox(r.x, r.y, r.w, r.h, rad, 0x080A12, 1, 0x080A12);
		mmb_gfx_rbox(r.x, r.y, r.w, r.h, rad, 0xC8CCD8, p + 1, -1);
		mmb_gfx_rbox(r.x + p + 1, r.y + p + 1, r.w - 2 * (p + 1),
			     r.h - 2 * (p + 1), rad > p + 1 ? rad - p - 1 : 1,
			     0x3A3E4C, p, -1);
	}
	for (i = 0; i < nseg; i++)
	{
		hub_rect s;
		unsigned col;

		s.x = r.x + pad + i * (segw + gap);
		s.y = r.y + pad;
		s.w = segw;
		s.h = r.h - 2 * pad;
		if (s.h < 2 || s.x + s.w > r.x + r.w - pad)
			break;
		if (i >= fill)
			continue;
		if (gradient)
			col = hub_lerp(0xE000C8, 0x00E4EC, i, nseg - 1);
		else
			col = hi ? hi : lo;
		hub_fill(s, col);
		hub_hline(s.x, s.y, s.w, hub_lerp(col, 0xFFFFFF, 1, 3), 1);
	}
}

/* Width of a block bar that holds whole blocks only and fits in `maxw`. */
static int hub_blockbar_w(int maxw)
{
	int p = U.p < 1 ? 1 : U.p;
	int pitch = (HUB_BAR_SEG + HUB_BAR_GAP) * p;
	int inset = HUB_BAR_INSET * p;
	int fw = hub_chrome_w("bar-fill");
	int n;

	if (fw < 1 || pitch < 1)
		return maxw;
	n = (fw + HUB_BAR_GAP * p) / pitch;
	if (n * pitch - HUB_BAR_GAP * p + 2 * inset > maxw)
		n = (maxw - 2 * inset + HUB_BAR_GAP * p) / pitch;
	if (n < 1)
		n = 1;
	return n * pitch - HUB_BAR_GAP * p + 2 * inset;
}

/* Loading / install progress: the bar-frame 9-slice at its own height and
 * whole blocks from bar-fill. The blocks are exactly the inner height, so
 * nothing is drawn over the rim at any step. */
static int hub_blockbar(hub_rect bar, int pct)
{
	int p = U.p < 1 ? 1 : U.p;
	int pitch = (HUB_BAR_SEG + HUB_BAR_GAP) * p;
	int inset = HUB_BAR_INSET * p;
	int fh, fw, n, k, show;

	if (!hub_chrome_has("bar-frame") || !hub_chrome_has("bar-fill"))
		return 0;
	bar.h = hub_chrome_h("bar-frame");
	if (!hub_chrome_slice("bar-frame", bar.x, bar.y, bar.w, bar.h, 16))
		return 0;
	fw = hub_chrome_w("bar-fill");
	fh = hub_chrome_h("bar-fill");
	if (pct < 0)
		pct = 0;
	if (pct > 100)
		pct = 100;
	n = (bar.w - 2 * inset + HUB_BAR_GAP * p) / pitch;
	if (n * pitch > fw + HUB_BAR_GAP * p)
		n = (fw + HUB_BAR_GAP * p) / pitch;
	k = (n * pct + 50) / 100;
	if (pct > 0 && k < 1)
		k = 1;
	if (k < 1 || fh < 1)
		return 1;
	show = k * pitch - HUB_BAR_GAP * p;
	/* Blocks keep their place in the gradient as the bar grows. */
	hub_chrome_clip(bar.x + inset, bar.y + inset, bar.x + inset + show,
			bar.y + inset + fh);
	hub_chrome_blit("bar-fill", bar.x + inset, bar.y + inset);
	hub_chrome_unclip();
	return 1;
}

/* ---- loading screen -------------------------------------------------- */

static void hub_paint_loading(void)
{
	int p = U.p < 1 ? 1 : U.p;
	hub_rect bar;
	const char *title = "MMCORE SOFTWARE HUB";
	int title_n = 19;
	/* Wider than the 8x16 face so the wordmark matches the mockup's
	 * width (about a third of the screen) while staying one step up. */
	int gw = (U.w * 32 / 100 + title_n - 1) / title_n;
	int gh;
	/* About a quarter larger than the old centred cube, kept just inside
	 * the mockup once the glow is included. */
	int cube_s = U.h * 52 / 100;
	int spark, spark_top, cy, ty, tx, tw;
	int bar_h, pct;
	int i;

	if (gw < 8 * p)
		gw = 8 * p;
	if (gw > 40)
		gw = 40;
	gh = gw * 3 / 2;
	if (gh < 16)
		gh = 16;
	if (cube_s > U.w * 40 / 100)
		cube_s = U.w * 40 / 100;
	if (cube_s < 90)
		cube_s = 90;
	bar_h = 28 * p;
	/* Top of the sparkle sits near 8% of the screen; the cube hangs
	 * below it instead of being centred with the title. */
	spark = cube_s / 6;
	spark_top = U.h * 8 / 100;

	mmb_gfx_fill_rect(0, 0, U.w, U.h, 0x05060C);
	if (hub_chrome_has("loading-cube"))
	{
		int cw = hub_chrome_w("loading-cube");
		int cx = (U.w - cw) / 2;
		int ty_img;
		int twd;

		/* The crop has a little air above the sparkle, so 4% puts the
		 * star near 8% of the screen. Title and bar sit on the mockup's
		 * bands rather than under the glow. */
		ty_img = U.h * 4 / 100;
		hub_chrome_blit("loading-cube", cx, ty_img);
		twd = hub_chrome_w("load-title");
		ty = U.h * 74 / 100;
		if (twd > 0)
			hub_chrome_blit("load-title", (U.w - twd) / 2, ty);
		else
		{
			tw = title_n * gw;
			tx = (U.w - tw) / 2;
			hub_text_cell(tx, ty, title, (unsigned)title_n, 0xC060F0, gw, gh);
		}
		bar.w = hub_blockbar_w(U.w - 16 * p);
		bar.h = hub_chrome_h("bar-frame");
		if (bar.h < 8 * p)
			bar.h = bar_h;
		bar.x = (U.w - bar.w) / 2;
		bar.y = U.h * 82 / 100;
		goto loading_pct;
	}
	cy = spark_top + spark + cube_s / 2;
	hub_cube(U.w / 2, cy, cube_s, 1);

	tw = title_n * gw;
	tx = (U.w - tw) / 2;
	ty = cy + cube_s / 2 + 18 * p;
	hub_text_cell(tx, ty, title, (unsigned)title_n, 0xC060F0, gw, gh);
	for (i = 0; i < 12; i++)
	{
		int dot = 2 * p + 1;
		int gap = 6 * p;
		int ry = ty + gh / 2 - dot / 2;
		int lx = tx - 10 * p - (12 - i) * gap;
		int rx = tx + tw + 10 * p + i * gap;
		unsigned lc = hub_lerp(0x7010A0, 0xC060F0, i, 11);
		unsigned rc = hub_lerp(0xC060F0, 0x20D0E8, i, 11);

		if (lx > 8 * p)
			mmb_gfx_fill_rect(lx, ry, dot, dot, lc);
		if (rx + dot < U.w - 8 * p)
			mmb_gfx_fill_rect(rx, ry, dot, dot, rc);
	}

	bar.w = U.w * 57 / 100;
	if (bar.w < 80 * p)
		bar.w = U.w - 16 * p;
	if (bar.w > U.w - 16 * p)
		bar.w = U.w - 16 * p;
	bar.h = bar_h;
	bar.x = (U.w - bar.w) / 2;
	bar.y = ty + gh + 10 * p;

loading_pct:
	if (U.state != HUB_ST_LOADING)
		return;
	if (!U.load_started)
	{
		unsigned el = mmb_now_ms() - (unsigned)U.started;

		pct = el >= HUB_LOAD_DELAY_MS ? 42 :
		      (int)(el * 42 / HUB_LOAD_DELAY_MS);
	}
	else if (U.err)
		pct = 55;
	else if (U.load_done)
		pct = 100;
	else if (U.icon_total > 0)
	{
		int done = U.icon_total - U.icon_stage;

		pct = 60 + done * 35 / U.icon_total;
	}
	else
		pct = 55;
	if (U.err)
		hub_segbar(bar, pct, HUB_RED, HUB_RED_D);
	else if (hub_blockbar(bar, pct))
		;
	else
		hub_segbar(bar, pct, 0, 0);

	if (U.err)
	{
		int scale = hub_ui_scale();
		int rw = hub_text_w(U.errmsg, scale);
		int rx = (U.w - rw) / 2;
		int ry = bar.y + bar.h + 10 * p;

		if (rx < 8 * p)
			rx = 8 * p;
		hub_text_trunc(rx, ry, U.errmsg, HUB_RED, scale, U.w - 2 * rx);
	}
}

/* ---- badges ---------------------------------------------------------- */

static const char *hub_state_badge(hub_app *a, unsigned *col,
				   unsigned *fill, char *buf, int bufsz)
{
	*col = 0xFFFFFF;
	*fill = HUB_PANEL2;
	if (!a)
		return "";
	switch (a->state)
	{
	case HUB_INSTALLED:
		*col = 0xFFFFFF;
		*fill = 0x2E9A32;
		return "INSTALLED";
	case HUB_UPDATE:
		*col = 0xFFFFFF;
		*fill = 0xD07018;
		return "UPDATE";
	case HUB_NEEDS_NEWER:
		*col = 0xFFFFFF;
		*fill = 0xD02828;
		if (hub_has(a->minver))
			snprintf(buf, (size_t)bufsz, "NEEDS v%s", a->minver);
		else
			snprintf(buf, (size_t)bufsz, "NEEDS");
		return buf;
	default:
		return "";
	}
}

/* Rounded pill, integer radius. Returns the width drawn, or 0 when empty. */
static int hub_chip(int x, int y, const char *text, unsigned col,
		    unsigned fill, int scale)
{
	int p = U.p < 1 ? 1 : U.p;
	int w, h, t, rad, yy, xx;

	if (!hub_has(text))
		return 0;
	if (scale < 1)
		scale = 1;
	w = hub_ui_w(text, 1) + 10 * p;
	h = hub_ui_h(1) + 6 * p;
	t = p < 2 ? 1 : 2;
	rad = 4 * p;
	if (rad * 2 >= h)
		rad = h / 2;
	for (yy = 0; yy < h; yy++)
		for (xx = 0; xx < w; xx++)
		{
			int cx = xx < rad ? rad : (xx >= w - rad ? w - 1 - rad : xx);
			int cy = yy < rad ? rad : (yy >= h - rad ? h - 1 - rad : yy);
			int dx = xx - cx, dy = yy - cy;
			int edge;

			if (dx * dx + dy * dy > rad * rad)
				continue;
			edge = 0;
			if (xx < t || yy < t || xx >= w - t || yy >= h - t)
				edge = 1;
			else
			{
				int ix = xx < rad ? rad : (xx >= w - rad ? w - 1 - rad : xx);
				int iy = yy - t < rad ? rad :
					 (yy - t >= h - rad ? h - 1 - rad : yy - t);
				int ex = xx - ix, ey = (yy - t) - iy;

				if (ex * ex + ey * ey > rad * rad)
					edge = 1;
			}
			mmb_gfx_plot(x + xx, y + yy, edge ? col : fill);
		}
	hub_label(x + 5 * p, y + (h - hub_ui_h(1)) / 2, text, col, 1);
	return w;
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
		/* Title block, the framed picture and a bottom margin; the
		 * same sums hub_draw_row uses to place the frame. */
		int p = U.p < 1 ? 1 : U.p;
		int iw = U.L.list.w - 6 * p - s_list_rpad - 10 * p;
		int ih = iw * 9 / 16;

		if (ih > U.L.list.h * 42 / 100)
			ih = U.L.list.h * 42 / 100;
		h = 2 * p + 2 * p + hub_ui_h(2) + hub_ui_h(1) + 10 * p + ih +
		    8 * p;
		if (h < U.L.row_h)
			h = U.L.row_h;
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

/* Outlined pill: dark fill, coloured border, large label in the same colour. */
static void hub_badge(int x, int y, int w, int h, const char *text,
		      unsigned fill, unsigned ink)
{
	int p = U.p < 1 ? 1 : U.p;
	int t = p < 2 ? 1 : 2;
	int scale = hub_body_scale();

	(void)ink;
	hub_fill((hub_rect){ x, y, w, h }, 0x101428);
	mmb_gfx_box(x, y, w, h, fill, t, -1);
	if (w > 4 * t && h > 4 * t)
		mmb_gfx_box(x + t, y + t, w - 2 * t, h - 2 * t,
			    hub_lerp(fill, 0xFFFFFF, 1, 3), 1, -1);
	if (16 * scale > h - 2)
		scale = 1;
	hub_text(x + (w - hub_text_w(text, scale)) / 2,
		 y + (h - 16 * scale) / 2, text, fill, scale);
}

static void hub_draw_row(int i, int y, int h, int selected)
{
	hub_app *a = hub_get(U.ids[i]);
	hub_rect r, icon;
	int p = U.p < 1 ? 1 : U.p;
	int greyed, x, ty, lim;
	char title[96], summary[160], author[64], sz[32], badgebuf[64];
	const char *badge;
	unsigned bcol, bfill;
	int narrow = U.L.mode == HUB_LAYOUT_NARROW;
	int bw = 0, bh = 0, bx, by;
	int meta_w = 0;

	if (!a)
		return;
	r.x = U.L.list.x + 3 * p;
	r.y = y + p;
	r.w = U.L.list.w - 6 * p - s_list_rpad;
	r.h = h - 2 * p;
	if (r.h < 8)
		return;
	greyed = a->state == HUB_NEEDS_NEWER;
	if (selected)
	{
		hub_fill(r, 0x16304A);
		mmb_gfx_box(r.x, r.y, r.w, r.h, HUB_CYAN, p < 2 ? 1 : 2, -1);
	}
	else
		hub_hline(r.x + 4 * p, r.y + r.h - 1, r.w - 8 * p, 0x1A2036, 1);

	icon.h = (narrow && selected ? U.L.row_h : r.h) - 4 * p;
	if (icon.h < 12)
		icon.h = 12;
	if (icon.h > r.h - 4 * p)
		icon.h = r.h - 4 * p;
	icon.w = icon.h;
	if (icon.w > r.w / 4)
		icon.w = icon.h = r.w / 4;
	icon.x = r.x + 3 * p;
	icon.y = r.y + (narrow && selected ? 2 * p : (r.h - icon.h) / 2);
	hub_app_icon(a, icon, greyed, 1);

	hub_upper(hub_has(a->title) ? a->title : a->id, title, sizeof(title));
	summary[0] = 0;
	if (hub_has(a->summary))
		snprintf(summary, sizeof(summary), "%s", a->summary);
	author[0] = 0;
	if (hub_has(a->author))
		snprintf(author, sizeof(author), "%s", a->author);
	sz[0] = 0;
	if (a->size > 0)
		hub_size_text(a->size, sz, sizeof(sz));
	badge = hub_state_badge(a, &bcol, &bfill, badgebuf, sizeof(badgebuf));
	if (badge && badge[0])
	{
		bw = hub_ui_w(badge, 1) + 12 * p;
		bh = hub_ui_h(1) + 8 * p;
		if (bh > r.h - 4 * p)
			bh = r.h - 4 * p;
	}
	meta_w = hub_ui_w(author, 1);
	if (hub_ui_w(sz, 1) > meta_w)
		meta_w = hub_ui_w(sz, 1);
	if (bw)
		meta_w += bw + 6 * p;
	x = icon.x + icon.w + 6 * p;
	lim = r.x + r.w - 6 * p - meta_w;
	if (lim < x + 16)
		lim = r.x + r.w - 6 * p;
	{
		int block = hub_ui_h(2) +
			    (summary[0] ? 2 * p + hub_ui_h(1) : 0);

		ty = r.y + (narrow && selected ? 2 * p : (r.h - block) / 2);
	}
	if (ty < r.y + p)
		ty = r.y + p;
	hub_ui_trunc(x, ty, title, greyed ? HUB_GREY : HUB_TEXT, 2, lim - x);
	if (summary[0])
		hub_ui_trunc(x, ty + hub_ui_h(2) + 2 * p, summary,
			       greyed ? HUB_GREY : HUB_DIM, 1, lim - x);

	if (selected && narrow)
	{
		int iw = r.w - 10 * p;
		int ih = iw * 9 / 16;
		hub_rect frame;
		const char *rel = hub_preview_rel(a);
		hub_img *im = hub_img_find(rel);

		if (ih > U.L.list.h * 42 / 100)
			ih = U.L.list.h * 42 / 100;
		frame.x = x;
		frame.y = ty + hub_ui_h(2) +
			  (summary[0] ? hub_ui_h(1) + 4 * p : 0) + 6 * p;
		frame.w = iw;
		frame.h = ih;
		if (frame.y + frame.h > r.y + r.h - 2 * p)
			frame.h = r.y + r.h - 2 * p - frame.y;
		{
			/* As wide as a 16:9 picture at this height, under the
			 * title column. */
			int in = 2 * (hub_frame_inset() + p);
			int fw = (frame.h - in) * 16 / 9 + in;

			if (fw < frame.w && fw > 32 * p)
				frame.w = fw;
			if (frame.x + frame.w > r.x + r.w - 4 * p)
				frame.w = r.x + r.w - 4 * p - frame.x;
		}
		if (frame.h > 16 * p)
		{
			hub_ornate_frame(frame, 0x0B0E20);
			hub_picture_well(hub_inset(frame, hub_frame_inset()), im, 1);
			hub_frame_chrome(frame);
		}
	}

	bx = r.x + r.w - 4 * p - bw;
	by = r.y + (r.h - bh) / 2;
	if (narrow && selected)
		by = r.y + 4 * p;
	if (bw > 0)
	{
		if (hub_chrome_badge(badge, bx, by, bw, bh))
			hub_label(bx + (bw - hub_ui_w(badge, 1)) / 2,
				 by + (bh - hub_ui_h(1)) / 2,
				 badge, bfill, 1);
		else
			hub_badge(bx, by, bw, bh, badge, bfill, bcol);
	}
	{
		int mx = (bw ? bx : r.x + r.w) - 6 * p;
		int stack = (sz[0] && author[0]) ? hub_ui_h(1) * 2 + p :
			    hub_ui_h(1);
		int my = by + (bh - stack) / 2;
		unsigned col = greyed ? HUB_GREY : HUB_DIM;

		if (my < r.y + 2 * p)
			my = r.y + 2 * p;
		if (author[0])
		{
			hub_ui_r(mx, my, author, col, 1);
			my += hub_ui_h(1) + p;
		}
		if (sz[0])
			hub_ui_r(mx, my, sz, col, 1);
	}
}

/* Draw row i cut at `limit` and fade it towards the panel colour, so the
 * list shows there is more below. Pixels under the limit are put back. */
static void hub_partial_row(int i, int y, int h, int limit)
{
	int x0 = U.L.list.x + U.p, w = U.L.list.w - 2 * U.p;
	int y1 = y + h > U.h ? U.h : y + h;
	int sh = y1 - limit;
	uint32_t *keep = 0;
	int xx, yy;
	unsigned pr = (HUB_PANEL >> 16) & 255u, pg = (HUB_PANEL >> 8) & 255u,
		 pb = HUB_PANEL & 255u;

	if (limit <= y || w < 1)
		return;
	if (sh > 0)
		keep = G.plat->alloc(sizeof(uint32_t) * (unsigned)(w * sh));
	if (sh > 0 && !keep)
		return;
	for (yy = 0; yy < sh; yy++)
		for (xx = 0; xx < w; xx++)
			keep[yy * w + xx] = mmb_gfx_get(x0 + xx, limit + yy);
	hub_draw_row(i, y, h, 0);
	for (yy = 0; yy < sh; yy++)
		for (xx = 0; xx < w; xx++)
			mmb_gfx_plot(x0 + xx, limit + yy, keep[yy * w + xx]);
	if (keep)
		G.plat->free(keep);
	for (yy = y; yy < limit; yy++)
	{
		/* 25% at the top of the strip to 90% at the cut. */
		unsigned a = 64u + (unsigned)((yy - y) * 166 / (limit - y));

		for (xx = 0; xx < w; xx++)
		{
			unsigned c = mmb_gfx_get(x0 + xx, yy);
			unsigned r = (c >> 16) & 255u, g = (c >> 8) & 255u,
				 b = c & 255u;

			r = (r * (256u - a) + pr * a) >> 8;
			g = (g * (256u - a) + pg * a) >> 8;
			b = (b * (256u - a) + pb * a) >> 8;
			mmb_gfx_plot(x0 + xx, yy, (r << 16) | (g << 8) | b);
		}
	}
}

/* Wide listing preview: one ornate frame, the picture well at the top of
 * its clear area and the caption under it on the same left edge. Every line
 * is truncated to the clear width and the summary stops at the bottom. */
static void hub_draw_preview(hub_rect pr, hub_app *cur)
{
	int p = U.p < 1 ? 1 : U.p;
	hub_rect clear, well;
	int lh = hub_ui_h(1);
	int gw = hub_ui_w("M", 1);
	int cap_min, ty, bottom;
	char line[128], shown[96];

	if (lh < 8)
		lh = 16;
	if (gw < 1)
		gw = 8;
	pr = hub_inset(pr, 4 * p);
	if (pr.w < 40 * p || pr.h < 40 * p)
		return;
	hub_ornate_frame(pr, 0x0B0E20);
	clear = hub_inset(pr, hub_frame_inset());
	/* Title, byline and two summary lines must fit under the picture. */
	cap_min = 6 * p + hub_ui_h(2) + 4 * p + (lh + 3 * p) +
		  (lh + 4 * p) * 2 + 3 * p;
	well = clear;
	well.h = (clear.w - 2 * p) * 9 / 16 + 2 * p;
	if (well.h > clear.h - cap_min)
	{
		well.h = clear.h - cap_min;
		if (well.h < 24 * p)
			well.h = 24 * p;
	}
	hub_picture_well(well, cur ? hub_img_find(hub_preview_rel(cur)) : 0,
			 cur != 0);
	hub_frame_chrome(pr);
	if (!cur)
		return;
	ty = well.y + well.h + 6 * p;
	bottom = clear.y + clear.h;
	hub_upper(hub_has(cur->title) ? cur->title : cur->id, shown,
		  sizeof(shown));
	{
		/* Name at zoom 2 when it fits the clear width, then the version
		 * and author on one quieter line. */
		int zoom = hub_ui_w(shown, 2) <= clear.w ? 2 : 1;

		if (ty + hub_ui_h(zoom) <= bottom)
		{
			hub_ui_trunc(clear.x, ty, shown, 0xFFFFFF, zoom, clear.w);
			ty += hub_ui_h(zoom) + 4 * p;
		}
	}
	line[0] = 0;
	if (hub_has(cur->version) && hub_has(cur->author))
		snprintf(line, sizeof(line), "v%s  by %s", cur->version,
			 cur->author);
	else if (hub_has(cur->version))
		snprintf(line, sizeof(line), "v%s", cur->version);
	else if (hub_has(cur->author))
		snprintf(line, sizeof(line), "by %s", cur->author);
	if (line[0] && ty + lh <= bottom)
	{
		hub_ui_trunc(clear.x, ty, line, 0xC890F0, 1, clear.w);
		ty += lh + 3 * p;
	}
	if (hub_has(cur->summary))
	{
		int step = lh + 4 * p;
		int rows;
		int cols = clear.w / gw;

		ty += 3 * p;
		rows = (bottom - ty + 4 * p) / step;
		if (rows > 0 && cols > 0)
			hub_wrap_draw(clear.x, ty, cur->summary, cols, step, 0,
				      rows, 0xA8B0C8, 1, gw, lh);
	}
}

static void hub_paint_listing(void)
{
	hub_app *cur = hub_cur();
	hub_rect hdr = U.L.header;
	int p = U.p < 1 ? 1 : U.p;
	int i, y, mark;

	hub_dither_bg();

	hub_fill(hdr, 0x101428);
	hub_hline(hdr.x, hdr.y, hdr.w, 0x2A3058, p < 2 ? 1 : 2);
	hub_hline(hdr.x, hdr.y + hdr.h - p, hdr.w, 0xC8A060, p < 2 ? 1 : 2);
	if (hub_chrome_has("header-cube") && hub_chrome_has("wordmark"))
	{
		int ch = hub_chrome_h("header-cube");
		int cw = hub_chrome_w("header-cube");
		int wh = hub_chrome_h("wordmark");
		int cy = hdr.y + (hdr.h - ch) / 2;
		int wy = hdr.y + (hdr.h - wh) / 2;

		if (cy < hdr.y)
			cy = hdr.y;
		if (wy < hdr.y)
			wy = hdr.y;
		hub_chrome_blit("header-cube", hdr.x + 6 * p, cy);
		hub_chrome_blit("wordmark", hdr.x + 6 * p + cw + 4 * p, wy);
	}
	else
	{
		mark = 1;
		while (mark < 4 && 16 * (mark + 1) <= hdr.h - 8 * p)
			mark++;
		hub_cube(hdr.x + hdr.h / 2, hdr.y + hdr.h / 2, hdr.h * 3 / 4, 0);
		{
			int ty = hdr.y + (hdr.h - 16 * mark) / 2;
			int tx = hdr.x + hdr.h + 4 * p;

			if (ty < hdr.y)
				ty = hdr.y;
			hub_text(tx, ty, "mmcore", HUB_TEXT, mark);
			hub_text(tx + 1, ty, "mmcore", HUB_TEXT, mark);
			hub_text(tx + hub_text_w("mmcore ", mark), ty, "HUB",
				 HUB_MAGENTA, mark);
			hub_text(tx + hub_text_w("mmcore ", mark) + 1, ty, "HUB",
				 HUB_MAGENTA, mark);
		}
	}

	if (U.offline)
	{
		int scale = 1;
		int w = hub_ui_w("OFFLINE", scale) + 10 * p;
		int h = hub_ui_h(scale) + 6 * p;
		int x = hdr.x + hdr.w - w - 6 * p;
		int y = hdr.y + (hdr.h - h) / 2;

		if (x > hdr.x)
			hub_chip(x, y, "OFFLINE", 0xFFD080, 0x2A1A08, scale);
	}

	hub_fill(U.L.content, HUB_PANEL);
	mmb_gfx_box(U.L.content.x, U.L.content.y, U.L.content.w, U.L.content.h,
		    HUB_BORDER_D, p, -1);
	hub_hline(U.L.content.x, U.L.content.y, U.L.content.w, 0x50407E, p);

	{
		int gap = U.L.tab_gap;
		int scale = 1;
		int widths[16];
		int n = s_ntabs;
		int total = 0, scroll = 0, view, sel = 0;
		int x;

		if (n > 16)
			n = 16;
		s_tab_scale = scale;
		for (i = 0; i < n; i++)
		{
			widths[i] = hub_ui_w(s_tabs[i].name, 1) + 14 * p;
			if (widths[i] < 24 * p)
				widths[i] = 24 * p;
			total += widths[i];
			if (s_tabs[i].cat == U.tab)
				sel = i;
		}
		if (n > 1)
			total += (n - 1) * gap;
		view = U.L.tabs.w - 8 * p;
		if (n > 0 && total <= view)
		{
			int extra = view - total;
			int cap = 8 * p;
			int add = extra / n;

			/* Leftover pixels are a few per tab when all ten fit.
			 * A catalogue with only ALL and GAMES would otherwise
			 * stretch each bracket across half the screen. */
			if (add > cap)
				add = cap;
			for (i = 0; i < n; i++)
				widths[i] += add;
			scroll = 0;
		}
		else if (n > 0)
		{
			int sx = 0, sel_x = 0, sel_w = widths[0];

			for (i = 0; i < n; i++)
			{
				if (i == sel)
				{
					sel_x = sx;
					sel_w = widths[i];
				}
				sx += widths[i] + gap;
			}
			if (sel_x + sel_w > view)
				scroll = sel_x + sel_w - view;
			if (sel_x < scroll)
				scroll = sel_x;
			if (scroll > total - view)
				scroll = total - view;
			if (scroll < 0)
				scroll = 0;
		}
		hub_clip_set(U.L.tabs.x, U.L.tabs.y,
			     U.L.tabs.x + U.L.tabs.w,
			     U.L.content.y + p + 2);
		x = U.L.tabs.x + 4 * p - scroll;
		for (i = 0; i < n; i++)
		{
			int selon = s_tabs[i].cat == U.tab;
			int tw = widths[i];
			const char *art = selon ? "tab-on" : "tab";
			int art_h = hub_chrome_h(art);
			int line = U.L.content.y;
			int body_h;
			hub_rect tr, hit;

			/* The tab art ends on the panel's top line. The active tab
			 * carries one extra row that covers the line, so it opens
			 * into the panel. Labels centre in the part above it. */
			if (art_h < 8)
				art_h = selon ? 24 * p + p : 20 * p;
			body_h = selon ? art_h - p : art_h;
			tr = (hub_rect){ x, line - body_h, tw, art_h };
			hit = (hub_rect){ x, line - body_h, tw, body_h };
			if (hit.x < U.L.tabs.x)
			{
				hit.w -= U.L.tabs.x - hit.x;
				hit.x = U.L.tabs.x;
			}
			if (hit.x + hit.w > U.L.tabs.x + U.L.tabs.w)
				hit.w = U.L.tabs.x + U.L.tabs.w - hit.x;
			if (hit.w > 0 &&
			    i < (int)(sizeof(s_tabbox) / sizeof(s_tabbox[0])))
			{
				s_tabbox[s_ntabbox].r = hit;
				s_tabbox[s_ntabbox].value = s_tabs[i].cat;
				s_ntabbox++;
			}
			if (!hub_chrome_slice(art, tr.x, tr.y, tr.w, tr.h, 8))
			{
				hub_fill((hub_rect){ tr.x, tr.y, tr.w, body_h },
					 selon ? HUB_PANEL : 0x0D1022);
				mmb_gfx_box(tr.x, tr.y, tr.w, body_h + (selon ? p : 0),
					    selon ? HUB_MAGENTA : HUB_BORDER_D,
					    p, -1);
				if (selon)
					hub_fill((hub_rect){ tr.x + p, line,
							     tr.w - 2 * p, p },
						 HUB_PANEL);
			}
			hub_label(tr.x + (tr.w - hub_ui_w(s_tabs[i].name, 1)) / 2,
				  tr.y + (body_h - hub_ui_h(1)) / 2,
				  s_tabs[i].name, selon ? 0xE070FF : HUB_DIM, 1);
			x += tw + gap;
		}
		hub_clip_clear();
	}

	{
		int limit = U.L.list.y + U.L.list.h - p;
		int total = 0, above = 0, shown = 0;

		/* A scrollbar when the rows do not all fit; rows give up its
		 * width so the meta column never runs under it. */
		for (i = 0; i < U.nids; i++)
		{
			int h = hub_row_h(i, i == U.sel);

			total += h;
			if (i < U.list_top)
				above += h;
		}
		s_list_rpad = total > U.L.list.h - 3 * p ? 10 * p : 0;
		y = U.L.list.y + 2 * p;
		for (i = U.list_top; i < U.nids; i++)
		{
			int h = hub_row_h(i, i == U.sel);
			int vis = h;

			if (y + h > limit)
			{
				/* Next row peeks out under the last full one and
				 * fades into the panel. */
				vis = limit - y;
				if (vis < 6 * p)
					break;
				hub_partial_row(i, y, h, limit);
			}
			else
			{
				hub_draw_row(i, y, h, i == U.sel);
				shown += h;
			}
			if (i < (int)(sizeof(s_rowbox) / sizeof(s_rowbox[0])))
			{
				s_rowbox[s_nrowbox].r = (hub_rect){ U.L.list.x, y,
								     U.L.list.w, vis };
				s_rowbox[s_nrowbox].value = i;
				s_nrowbox++;
			}
			y += h;
			if (vis < h)
				break;
		}
		if (s_list_rpad && total > 0)
		{
			int tx = U.L.list.x + U.L.list.w - s_list_rpad;
			int ty = U.L.list.y + 3 * p;
			int th = U.L.list.h - 6 * p;
			int thumb = th * shown / total;
			int pos = th * above / total;

			if (thumb < 12 * p)
				thumb = 12 * p;
			if (pos + thumb > th)
				pos = th - thumb;
			if (!hub_chrome_scroll(tx, ty, s_list_rpad - 2 * p, th, pos,
					       thumb))
			{
				hub_fill((hub_rect){ tx + 3 * p, ty, 2 * p, th },
					 0x2A3048);
				hub_fill((hub_rect){ tx + 3 * p, ty + pos, 2 * p,
						     thumb }, 0x8A90A8);
			}
		}
	}

	if (U.L.mode == HUB_LAYOUT_WIDE)
		hub_draw_preview(U.L.preview, cur);
}

/* ---- detail ---------------------------------------------------------- */

static const char *hub_button_label(hub_app *a, unsigned *col,
				    unsigned *fill, char *buf, int bufsz)
{
	*col = 0x2E9A32;
	*fill = 1;
	if (!a)
		return "";
	/* Cached browsing is read-only: INSTALL and UPDATE need the network,
	 * so they use the grey NEEDS form while offline (#1544). */
	if (U.offline &&
	    (a->state == HUB_NOT_INSTALLED || a->state == HUB_UPDATE))
	{
		*col = 0x4A5068;
		*fill = 0;
		return a->state == HUB_UPDATE ? "UPDATE" : "INSTALL";
	}
	switch (a->state)
	{
	case HUB_INSTALLED:
		*col = 0x1490C0;
		return "RUN";
	case HUB_UPDATE:
		*col = 0xE07020;
		return "UPDATE";
	case HUB_NEEDS_NEWER:
		*col = 0x4A5068;
		*fill = 0;
		if (hub_has(a->minver))
			snprintf(buf, (size_t)bufsz, "NEEDS v%s", a->minver);
		else
			snprintf(buf, (size_t)bufsz, "NEEDS");
		return buf;
	default:
		return "INSTALL";
	}
}

/* White bitmap label with a 1px dark ring and a 2px drop shadow.
 * The cap is the closest zoom to about 40% of the button. */
static void hub_button_caption(hub_rect b, const char *label, int enabled)
{
	int cap1 = hub_ui_h(1) * 7 / 9;
	int zoom, best, bestd, tw, th, x, y;
	unsigned ink = enabled ? 0xFFFFFFu : 0xC8C8D0u;
	unsigned shade = enabled ? 0x140800u : 0x202028u;

	if (cap1 < 1)
		cap1 = 1;
	best = 1;
	bestd = 100000;
	for (zoom = 1; zoom <= 4; zoom++)
	{
		int cap = cap1 * zoom;
		int d;

		if (hub_ui_w(label, zoom) > b.w - 8)
			continue;
		if (cap > b.h * 55 / 100)
			continue;
		d = cap - b.h * 40 / 100;
		if (d < 0)
			d = -d;
		if (d < bestd)
		{
			bestd = d;
			best = zoom;
		}
	}
	tw = hub_ui_w(label, best);
	th = hub_ui_h(best);
	x = b.x + (b.w - tw) / 2;
	y = b.y + (b.h - th) / 2;
	hub_label(x + 2, y + 2, label, shade, best);
	hub_label(x - 1, y, label, shade, best);
	hub_label(x + 1, y, label, shade, best);
	hub_label(x, y - 1, label, shade, best);
	hub_label(x, y + 1, label, shade, best);
	hub_label(x - 1, y - 1, label, shade, best);
	hub_label(x + 1, y - 1, label, shade, best);
	hub_label(x - 1, y + 1, label, shade, best);
	hub_label(x + 1, y + 1, label, shade, best);
	hub_label(x, y, label, ink, best);
}

static void hub_action_button(hub_rect b, const char *label, unsigned accent,
			      int enabled)
{
	int p = U.p < 1 ? 1 : U.p;
	int t = p < 2 ? 1 : 2;
	unsigned fill = enabled ? hub_lerp(0x000000, accent, 2, 3) : 0x2A2E3A;
	unsigned top = enabled ? hub_lerp(accent, 0xFFFFFF, 1, 2) : 0x6A7080;
	unsigned bot = enabled ? hub_lerp(accent, 0x000000, 1, 2) : 0x14161C;
	int scale = 1;
	const char *art = 0;

	while (scale < 4 && 16 * (scale + 1) + 8 * p <= b.h)
		scale++;
	/* INSTALL and RUN are the mockup bevel. UPDATE is the gold form.
	 * NEEDS is the grey form. The slice keeps the rim and stretches the
	 * face; the height stays the asset's own pixels. */
	if (label && strncmp(label, "NEEDS", 5) == 0)
		art = "button-needs";
	else if (!enabled && label &&
		 (strcmp(label, "INSTALL") == 0 || strcmp(label, "UPDATE") == 0))
		art = "button-needs";
	else if (enabled && label && strcmp(label, "UPDATE") == 0)
		art = "button-update";
	else if (enabled && label &&
		 (strcmp(label, "INSTALL") == 0 || strcmp(label, "RUN") == 0))
		art = "button";
	else if (enabled && label &&
		 (strcmp(label, "CANCEL") == 0 || strcmp(label, "OK") == 0))
		art = "button-needs";
	if (art && hub_chrome_has(art))
	{
		int bh = hub_chrome_h(art);
		hub_rect face = b;

		if (bh > 8 && bh < b.h)
		{
			face.h = bh;
			face.y = b.y + (b.h - bh) / 2;
		}
		if (hub_chrome_slice(art, face.x, face.y, face.w, face.h, 18))
		{
			hub_button_caption(face, label, enabled);
			return;
		}
	}
	hub_fill(b, fill);
	hub_hline(b.x, b.y, b.w, top, t);
	hub_vline(b.x, b.y, b.h, top, t);
	hub_hline(b.x, b.y + b.h - t, b.w, bot, t);
	hub_vline(b.x + b.w - t, b.y, b.h, bot, t);
	mmb_gfx_box(b.x + 2 * p, b.y + 2 * p, b.w - 4 * p, b.h - 4 * p,
		    top, p < 2 ? 1 : p, -1);
	if (16 * scale + 4 > b.h)
		scale = 1;
	{
		int tx = b.x + (b.w - hub_text_w(label, scale)) / 2;
		int ty = b.y + (b.h - 16 * scale) / 2;
		unsigned ink = enabled ? 0xFFFFFF : HUB_GREY;

		hub_text(tx, ty, label, ink, scale);
		if (scale >= 2)
			hub_text(tx + 1, ty, label, ink, scale);
	}
}

static void hub_draw_main_button(hub_rect area)
{
	hub_app *a = hub_cur();
	int p = U.p < 1 ? 1 : U.p;
	int bw = area.w / 3;
	hub_rect b;
	char buf[64];
	const char *label;
	unsigned accent, enabled;

	/* The mockup button is about half the text column, which reads as
	 * a large block under the paragraph. */
	if (bw < area.w / 2)
		bw = area.w / 2;
	if (bw > area.w * 56 / 100)
		bw = area.w * 56 / 100;
	if (bw > area.w - 4 * p)
		bw = area.w - 4 * p;
	if (bw < 20)
		bw = area.w;
	b.w = bw;
	b.h = area.h > 8 ? area.h : 16;
	if (b.h < 16)
		b.h = area.h > 0 ? area.h : 16;
	b.x = area.x + (area.w - b.w) / 2;
	b.y = area.y + (area.h - b.h) / 2;
	label = hub_button_label(a, &accent, &enabled, buf, sizeof(buf));
	hub_action_button(b, label, accent, enabled);
	s_mainbtn = b;
	s_has_mainbtn = 1;
}

static void hub_draw_about(hub_rect r, hub_app *a, int scroll)
{
	int p = U.p < 1 ? 1 : U.p;
	/* The UI face at zoom 1 is about 44 characters across this column. */
	int gw = hub_chrome_has("ui-font") ? hub_chrome_ui_w("M", 1) :
		 ((U.p >= 3 ? 16 : 8) * 3 / 2);
	int gh = hub_chrome_has("ui-font") ? hub_chrome_ui_h() : gw * 2;
	int lh = gh + (hub_chrome_has("ui-font") ? 8 * p :
		       11 * (U.p < 1 ? 1 : U.p) / 2);
	int cols = (r.w - 16 * p) / gw;
	int face = hub_chrome_has("ui-font");
	const char *body = hub_doc_body(a ? a->about : 0,
					a && hub_has(a->summary) ? a->summary : "");
	const char *changes = hub_doc_body(a ? a->changes : 0, "");
	int body_lines, ch_lines;
	int head, ch_at, lines, rows;
	int maxscroll;
	int i;

	if (cols < 8)
		cols = 8;
	body_lines = hub_wrap_lines(body, cols);
	ch_lines = changes[0] ? hub_wrap_lines(changes, cols) : 0;
	head = changes[0] ? body_lines + 1 : -1;
	ch_at = changes[0] ? body_lines + 2 : -1;
	lines = body_lines + (changes[0] ? 2 + ch_lines : 0);
	rows = lh > 0 ? r.h / lh : 0;
	if (rows < 1)
		rows = 1;
	maxscroll = lines - rows;
	if (maxscroll < 0)
		maxscroll = 0;
	if (scroll > maxscroll)
		scroll = maxscroll;
	if (scroll < 0)
		scroll = 0;
	if (a)
		U.about_scroll = scroll;
	for (i = 0; i < rows; i++)
	{
		int line = scroll + i;
		int y = r.y + i * lh;

		if (line < body_lines && body[0])
		{
			hub_wrap_draw(r.x, y, body, cols, lh, line, 1,
				      HUB_TEXT, 1, gw, gh);
			if (!face)
				hub_wrap_draw(r.x + 1, y, body, cols, lh, line, 1,
					      HUB_TEXT, 1, gw, gh);
		}
		else if (line == head)
		{
			if (face)
				hub_label(r.x, y, "WHAT'S NEW", HUB_ORANGE, 1);
			else
				hub_text_cell(r.x, y, "WHAT'S NEW", 9, HUB_ORANGE,
					      gw, gh);
		}
		else if (ch_at >= 0 && line >= ch_at &&
			 line < ch_at + ch_lines)
			hub_wrap_draw(r.x, y, changes, cols, lh,
				      line - ch_at, 1, HUB_DIM, 1, gw, gh);
	}
	if (r.h > 8 * p)
	{
		int track = r.h - 2 * p;
		int thumb = lines > 0 ? track * rows / lines : track;
		int pos = lines > 0 ? track * scroll / lines : 0;

		if (thumb < 8 * p)
			thumb = 8 * p;
		if (thumb > track)
			thumb = track;
		if (!hub_chrome_scroll(r.x + r.w - 4 * p, r.y, 4 * p, track,
				       pos, thumb))
		{
			mmb_gfx_fill_rect(r.x + r.w - 2 * p, r.y, 2 * p, track,
					  0x2A3048);
			mmb_gfx_fill_rect(r.x + r.w - 2 * p, r.y + pos, 2 * p,
					  thumb, 0x8A90A8);
		}
	}
}

static void hub_draw_framed(hub_rect frame, const char *rel, int cube)
{
	hub_ornate_frame(frame, 0x0B0E20);
	hub_picture_well(hub_inset(frame, hub_frame_inset()),
			 hub_img_find(rel), cube);
}

/* One line in the UI face, zoom 2 when the column is wide enough. */
static int hub_hero_title_zoom(hub_app *a, int width)
{
	const char *title;
	int p = U.p < 1 ? 1 : U.p;

	title = a && hub_has(a->title) ? a->title : (a ? a->id : "");
	if (title && hub_ui_w(title, 2) <= width - 8 * p)
		return 2;
	return 1;
}

/* Hero at the top of the right column (under the thumbnails when the
 * screen is narrow). The picture's frame is the column width and the
 * picture inside it is 16:9. A short screen keeps that ratio and centres
 * a narrower frame rather than squashing it. */
static void hub_place_hero(hub_layout *L, hub_app *a)
{
	int p = L->p < 1 ? 1 : L->p;
	int gap = L->gap > 0 ? L->gap : 4 * p;
	int narrow = L->mode == HUB_LAYOUT_NARROW;
	int inset = hub_frame_inset() + p;
	int rx, rw, y, bottom, title_h, chips_h, button_h, hero_h;
	int min_about, zoom, inner_w;

	rx = narrow ? L->detail.x : L->detail_shot.x + L->detail_shot.w + gap;
	rw = narrow ? L->detail.w : L->detail.x + L->detail.w - rx;
	if (rw < 32)
		rw = 32;
	zoom = hub_hero_title_zoom(a, rw);
	title_h = hub_ui_h(zoom) + 4 * p;
	chips_h = 28 * p;
	button_h = narrow ? 44 * p : 48 * p;
	min_about = hub_ui_h(1) * 2 + 4 * p;
	bottom = L->detail.y + L->detail.h;
	inner_w = rw - 2 * inset;
	if (inner_w < 16)
		inner_w = 16;
	hero_h = inner_w * 9 / 16 + 2 * inset;

	if (narrow)
	{
		int thumbs_h = L->detail_thumbs.h;
		int room, shot_h, inner_h, frame_w;

		/* Two framed 16:9 pictures, one under the other. A full-width
		 * pair does not fit, so each frame is only as wide as 16:9
		 * allows and sits in the middle of the panel. */
		room = bottom - L->detail.y - thumbs_h - title_h - chips_h -
		       button_h - min_about - hub_ui_h(1) - 5 * gap - p;
		if (room < 64 * p)
			room = 64 * p;
		shot_h = room * 40 / 100;
		if (shot_h < 24 * p)
			shot_h = 24 * p;
		inner_h = shot_h - 2 * inset;
		if (inner_h < 16)
			inner_h = 16;
		inner_w = inner_h * 16 / 9;
		frame_w = inner_w + 2 * inset;
		if (frame_w > L->detail.w)
		{
			frame_w = L->detail.w;
			inner_w = frame_w - 2 * inset;
			if (inner_w < 16)
				inner_w = 16;
			shot_h = inner_w * 9 / 16 + 2 * inset;
		}
		L->detail_shot.x = L->detail.x + (L->detail.w - frame_w) / 2;
		L->detail_shot.w = frame_w;
		L->detail_shot.h = shot_h;
		L->detail_thumbs.y = L->detail.y + shot_h + p;
		y = L->detail_thumbs.y + thumbs_h + gap;
		room = bottom - y - title_h - chips_h - button_h - min_about -
		       hub_ui_h(1) - 4 * gap;
		rw = L->detail.w;
		inner_w = rw - 2 * inset;
		if (inner_w < 16)
			inner_w = 16;
		hero_h = inner_w * 9 / 16 + 2 * inset;
		if (hero_h > room && room > 24 * p)
		{
			inner_h = room - 2 * inset;
			if (inner_h < 16)
				inner_h = 16;
			inner_w = inner_h * 16 / 9;
			hero_h = inner_h + 2 * inset;
			rw = inner_w + 2 * inset;
		}
		rx = L->detail.x + (L->detail.w - rw) / 2;
	}
	else
	{
		int room;

		y = L->detail.y;
		room = bottom - y - title_h - chips_h - button_h - min_about -
		       4 * gap - 8 * p;
		if (hero_h > room && room > 24 * p)
		{
			int inner_h = room - 2 * inset;

			if (inner_h < 16)
				inner_h = 16;
			inner_w = inner_h * 16 / 9;
			if (inner_w > rw - 2 * inset)
				inner_w = rw - 2 * inset;
			hero_h = inner_h + 2 * inset;
			rw = inner_w + 2 * inset;
			rx = L->detail_shot.x + L->detail_shot.w + gap +
			     ((L->detail.x + L->detail.w) -
			      (L->detail_shot.x + L->detail_shot.w + gap) - rw) / 2;
		}
	}
	if (hero_h < 16)
		hero_h = 16;
	L->detail_title.x = rx;
	L->detail_title.y = y;
	L->detail_title.w = rw;
	L->detail_title.h = hero_h;
	y += hero_h + gap;
	L->detail_chips.x = rx;
	L->detail_chips.y = y + title_h;
	L->detail_chips.w = rw;
	L->detail_chips.h = chips_h;
	if (narrow)
	{
		/* The pictures are centred. The title, chips and paragraph
		 * still use the full panel so the line length stays readable. */
		L->detail_chips.x = L->detail.x;
		L->detail_chips.w = L->detail.w;
		L->detail_button.x = L->detail.x;
		L->detail_button.y = L->detail_chips.y + chips_h + gap;
		L->detail_button.w = L->detail.w;
		L->detail_button.h = button_h;
		L->detail_about.x = L->detail.x;
		L->detail_about.y = L->detail_button.y + button_h + gap;
		L->detail_about.w = L->detail.w;
		L->detail_about.h = bottom - L->detail_about.y;
	}
	else
	{
		L->detail_button.x = rx;
		L->detail_button.w = rw;
		L->detail_button.h = button_h;
		L->detail_button.y = bottom - button_h - 4 * p;
		L->detail_about.x = rx;
		L->detail_about.y = L->detail_chips.y + chips_h + gap;
		L->detail_about.w = rw;
		L->detail_about.h = L->detail_button.y - gap - L->detail_about.y;
	}
	if (L->detail_about.h < 1)
		L->detail_about.h = 1;
}

static void hub_draw_detail(void)
{
	hub_app *a = hub_cur();
	hub_layout *L = &U.L;
	int p = U.p < 1 ? 1 : U.p;
	hub_rect shot;
	int narrow;

	if (a && hub_has(a->hero))
	{
		hub_layout_now();
		hub_place_hero(L, a);
	}
	shot = L->detail_shot;
	narrow = L->mode == HUB_LAYOUT_NARROW;

	hub_dither_bg();
	hub_fill(L->detail, 0x0C1024);
	mmb_gfx_box(L->detail.x, L->detail.y, L->detail.w, L->detail.h,
		    0x2A3050, p, -1);

	hub_draw_framed(shot, hub_detail_rel(a), a != 0);

	s_nshotbox = 0;
	if (a && hub_gallery_count(a) > 1)
	{
		int n = hub_gallery_count(a);
		int avail = L->detail_thumbs.w - 4 * p;
		int th = L->detail_thumbs.h - 4 * p;
		int tw2 = th * 16 / 9;
		int i, x;

		if (th < 6)
			th = L->detail_thumbs.h;
		if (tw2 < 8)
			tw2 = 8;
		if (n * (tw2 + 3 * p) > avail || th < 10)
		{
			int dw = 7 * p;
			int total = n * (dw + 3 * p);
			int cy = L->detail_thumbs.y + L->detail_thumbs.h / 2;

			x = L->detail_thumbs.x + (L->detail_thumbs.w - total) / 2;
			for (i = 0; i < n; i++)
			{
				int on = i == U.detail_shot;

				mmb_gfx_fill_rect(x + i * (dw + 3 * p),
						  cy - dw / 2, dw, dw,
						  on ? HUB_CYAN : 0x3A4060);
			}
		}
		else
		{
			int total = n * (tw2 + 3 * p) - 3 * p;

			x = L->detail_thumbs.x +
			    (L->detail_thumbs.w - total) / 2;
			for (i = 0; i < n; i++)
			{
				hub_rect tr = { x, L->detail_thumbs.y + 2 * p,
						tw2, th };
				hub_img *ti = hub_img_find(hub_gallery_rel(a, i));

				hub_fill(tr, 0x070910);
				if (ti && ti->pix)
					hub_draw_image(ti->pix, ti->w, ti->h, tr);
				mmb_gfx_box(tr.x, tr.y, tr.w, tr.h,
					    i == U.detail_shot ? HUB_CYAN :
					    0x3A4068,
					    i == U.detail_shot ? 2 : 1, -1);
				if (i < (int)(sizeof(s_shotbox) / sizeof(s_shotbox[0])))
				{
					s_shotbox[s_nshotbox].r = tr;
					s_shotbox[s_nshotbox].value = i;
					s_nshotbox++;
				}
				x += tw2 + 3 * p;
			}
		}
	}

	if (a && hub_has(a->hero))
	{
		const char *title = hub_has(a->title) ? a->title : a->id;
		int zoom = hub_hero_title_zoom(a, L->detail_chips.w);
		int th = hub_ui_h(zoom);
		int ty = L->detail_chips.y - (th + 4 * p);

		hub_draw_framed(L->detail_title, a->hero, 0);
		if (ty < L->detail_title.y + L->detail_title.h)
			ty = L->detail_title.y + L->detail_title.h + 2 * p;
		hub_ui_trunc(L->detail_chips.x, ty, title, 0xFFFFFF, zoom,
			     L->detail_chips.w);
	}
	else if (a)
	{
		char upper[96];
		char line1[64], line2[64];
		int two = 0;
		int scale = 8;
		int lines, block_h, icon_s;
		int tx, ty, ix, iy;
		char *sp;

		if (U.p < 2)
			scale = 4;
		hub_upper(hub_has(a->title) ? a->title : a->id, upper,
			  sizeof(upper));
		line2[0] = 0;
		snprintf(line1, sizeof(line1), "%s", upper);
		sp = strchr(line1, ' ');
		if (sp && sp != line1 && sp[1])
		{
			snprintf(line2, sizeof(line2), "%s", sp + 1);
			*sp = 0;
			two = 1;
		}
		lines = two ? 2 : 1;
		if (hub_chrome_has("title-font"))
		{
			int lh = hub_chrome_title_h();
			int gap = 2 * p;
			int w1 = hub_chrome_title_w(line1, 0);
			int w2 = two ? hub_chrome_title_w(line2, 1) : 0;
			int avail;
			hub_img *icon_im = hub_img_find(a->icon);
			int isrc = (icon_im && icon_im->w > 0) ? icon_im->w : 32;
			int isrch = (icon_im && icon_im->h > 0) ? icon_im->h : isrc;
			int iscale = 1;

			block_h = two ? lh * 2 + gap : lh;
			/* Integer-scaled app icon, framed tight, as tall as the slot. */
			while ((iscale + 1) * isrc + 8 * p <= L->detail_title.h &&
			       (iscale + 1) * isrch + 8 * p <= L->detail_title.h)
				iscale++;
			for (;;)
			{
				icon_s = isrc * iscale + 8 * p;
				if (icon_s > L->detail_title.h)
					icon_s = L->detail_title.h;
				avail = L->detail_title.w - icon_s - 12 * p;
				if (avail >= 16 && w1 <= avail &&
				    (!two || w2 <= avail))
					break;
				if (iscale <= 1)
					goto title_procedural;
				iscale--;
			}
			if (icon_s > L->detail_title.h)
				icon_s = L->detail_title.h;
			if (narrow)
			{
				ix = L->detail_title.x;
				tx = ix + icon_s + 8 * p;
			}
			else
			{
				ix = L->detail_title.x + L->detail_title.w - icon_s;
				tx = L->detail_title.x;
			}
			ty = L->detail_title.y + 2 * p;
			if (ty + block_h > L->detail_title.y + L->detail_title.h)
				ty = L->detail_title.y;
			iy = ty + (block_h - icon_s) / 2;
			hub_app_icon(a, (hub_rect){ ix, iy, icon_s, icon_s },
				     a->state == HUB_NEEDS_NEWER, 2);
			hub_chrome_title(line1, tx, ty, 0);
			if (two)
			{
				int x2 = tx;

				if (w2 < w1)
					x2 = tx + (w1 - w2) / 2;
				hub_chrome_title(line2, x2, ty + lh + gap, 1);
			}
		}
		else
		{
		title_procedural:
		/* The icon is as tall as the two title lines and sits at the
		 * right; the words fill the column to its left. */
		while (scale > 1)
		{
			int bh = 16 * scale * lines + (lines - 1) * 2 * p;
			int mw = L->detail_title.w - bh - 12 * p;

			if (bh <= L->detail_title.h && mw > 16 &&
			    hub_text_w(line1, scale) <= mw &&
			    (!two || hub_text_w(line2, scale) <= mw))
				break;
			scale--;
		}
		block_h = 16 * scale * lines + (lines - 1) * 2 * p;
		icon_s = block_h;
		if (icon_s > L->detail_title.h)
			icon_s = L->detail_title.h;
		if (narrow)
		{
			ix = L->detail_title.x;
			tx = ix + icon_s + 8 * p;
		}
		else
		{
			ix = L->detail_title.x + L->detail_title.w - icon_s;
			tx = L->detail_title.x;
		}
		ty = L->detail_title.y + (L->detail_title.h - block_h) / 2;
		if (ty < L->detail_title.y)
			ty = L->detail_title.y;
		iy = ty + (block_h - icon_s) / 2;
		hub_app_icon(a, (hub_rect){ ix, iy, icon_s, icon_s },
			     a->state == HUB_NEEDS_NEWER, 2);
		hub_grad_text(tx, ty, line1, 0xFFFFFF, 0xB0B6C8, scale, 1,
			      0x120818);
		if (two)
		{
			int y2 = ty + 16 * scale + 2 * p;
			int x2 = tx;
			int w1 = hub_text_w(line1, scale);
			int w2 = hub_text_w(line2, scale);

			if (w2 < w1)
				x2 = tx + (w1 - w2) / 2;
			hub_grad_text(x2, y2, line2, 0xF0D0FF, 0x6820C8,
				      scale, 1, 0x140820);
		}
		}
	}

	if (a)
	{
		int x = L->detail_chips.x;
		int y = L->detail_chips.y;
		int maxx = L->detail_chips.x + L->detail_chips.w - 4 * p;
		int scale = U.p >= 2 ? 2 : 1;
		char buf[96];

		if (hub_has(a->author))
		{
			snprintf(buf, sizeof(buf), "by %s", a->author);
			x += hub_chip(x, y, buf, 0xE0B0FF, 0x24143A, scale) +
			     4 * p;
		}
		if (hub_has(a->version) && x < maxx)
		{
			snprintf(buf, sizeof(buf), "v%s", a->version);
			x += hub_chip(x, y, buf, HUB_CYAN, 0x0C2430, scale) +
			     4 * p;
		}
		if (a->size > 0 && x < maxx)
		{
			hub_size_text(a->size, buf, sizeof(buf));
			x += hub_chip(x, y, buf, HUB_ORANGE, 0x2A1A08, scale) +
			     4 * p;
		}
		if (hub_has(a->category) && x < maxx)
			x += hub_chip(x, y, a->category, 0xD090F0, 0x24143A,
				      scale) + 4 * p;
		if (a->state == HUB_NEEDS_NEWER && x < maxx)
		{
			if (hub_has(a->minver))
				snprintf(buf, sizeof(buf), "NEEDS MMCORE v%s",
					 a->minver);
			else
				snprintf(buf, sizeof(buf), "NEEDS MMCORE");
			hub_chip(x, y, buf, HUB_RED, 0x3A1010, scale);
		}
		(void)maxx;
	}

	hub_draw_about(L->detail_about, a, U.about_scroll);
	hub_draw_main_button(L->detail_button);

	if (U.offline_note)
	{
		int scale = 1;
		int w = hub_ui_w("OFFLINE - CONNECT TO INSTALL", scale) + 10 * p;
		int h = hub_ui_h(scale) + 6 * p;
		int x = L->detail.x + (L->detail.w - w) / 2;
		int y = L->detail_button.y - h - 4 * p;

		if (x < L->detail.x + p)
			x = L->detail.x + p;
		if (y < L->detail.y)
			y = L->detail.y;
		hub_chip(x, y, "OFFLINE - CONNECT TO INSTALL",
			 0xFFD080, 0x2A1A08, scale);
	}
}

/* ---- modals ---------------------------------------------------------- */

static void hub_modal_rect(hub_rect *m, int wunits, int hunits)
{
	int p = U.p < 1 ? 1 : U.p;
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

/* Install dialog. One framed panel in the HUB's own art: the app icon and
 * name, a status line, the block bar from the loading screen and a single
 * plate button. `mode` 0 is the transfer, 1 a failure, 2 done. */
/* Place a one-zoom label by its visible ink: left >= 0 puts the first ink
 * column there, otherwise right puts the column after the last ink there. */
static void hub_label_edges(int left, int right, int y, const char *s,
			    unsigned col)
{
	int l = 0, r = 0;

	if (!s || !*s)
		return;
	if (hub_chrome_has("ui-font"))
		hub_chrome_ui_bearing(s, 1, &l, &r);
	if (left >= 0)
		hub_label(left - l, y, s, col, 1);
	else
		hub_label(right + r - hub_ui_w(s, 1), y, s, col, 1);
}

static void hub_paint_install_modal(int mode)
{
	hub_app *a = hub_get(U.inst_index);
	int p = U.p < 1 ? 1 : U.p;
	int lh = hub_ui_h(1);
	int btn_h = hub_chrome_h("button-needs");
	int icon_s = 32 * p;
	int bar_h = hub_chrome_h("bar-frame");
	hub_rect m, c, bar, b;
	char name[96], buf[192], right[64];
	const char *status;
	unsigned scol;
	int y, pct, rim_l, rim_r;

	if (lh < 8)
		lh = 16;
	if (btn_h < 16 * p)
		btn_h = 28 * p;
	if (bar_h < 8 * p)
		bar_h = 26 * p;
	m.w = 420 * p;
	if (m.w > U.w - 16 * p)
		m.w = U.w - 16 * p;
	m.h = 2 * hub_frame_inset() + icon_s + 8 * p + p + 10 * p + lh +
	      6 * p + bar_h + 8 * p + lh + 12 * p + btn_h;
	if (m.h > U.h - 16 * p)
		m.h = U.h - 16 * p;
	m.x = (U.w - m.w) / 2;
	m.y = (U.h - m.h) / 2;
	hub_ornate_frame(m, 0x0E1128);
	c = hub_inset(m, hub_frame_inset());

	/* Header: icon, name and the version / author line. */
	if (a)
		hub_app_icon(a, (hub_rect){ c.x, c.y, icon_s, icon_s }, 0, 1);
	hub_upper(a ? (hub_has(a->title) ? a->title : a->id) : "", name,
		  sizeof(name));
	{
		int tx = c.x + icon_s + 8 * p;
		int zoom = hub_ui_w(name, 2) <= c.x + c.w - tx ? 2 : 1;
		int th = hub_ui_h(zoom) + 2 * p + lh;
		int ty = c.y + (icon_s - th) / 2;

		if (ty < c.y)
			ty = c.y;
		hub_ui_trunc(tx, ty, name, 0xFFFFFF, zoom, c.x + c.w - tx);
		buf[0] = 0;
		if (a && hub_has(a->version) && hub_has(a->author))
			snprintf(buf, sizeof(buf), "v%s  by %s", a->version,
				 a->author);
		else if (a && hub_has(a->version))
			snprintf(buf, sizeof(buf), "v%s", a->version);
		else if (a && hub_has(a->author))
			snprintf(buf, sizeof(buf), "by %s", a->author);
		hub_ui_trunc(tx, ty + hub_ui_h(zoom) + 2 * p, buf, HUB_DIM, 1,
			     c.x + c.w - tx);
	}
	y = c.y + icon_s + 8 * p;
	hub_hline(c.x, y, c.w, 0x3A2E66, p);
	y += p + 10 * p;

	/* Status line: what is happening on the left, the numbers right. */
	right[0] = 0;
	pct = U.inst_total ? (int)((uint64_t)U.inst_done * 100u /
				    (unsigned)U.inst_total) : 0;
	if (mode == 1)
	{
		status = "FAILED";
		scol = HUB_RED;
		pct = U.inst_total ? pct : 0;
	}
	else if (mode == 2)
	{
		status = "DONE";
		scol = HUB_LIME;
		pct = 100;
		snprintf(right, sizeof(right), "%u KB",
			 (U.inst_total + 1023) / 1024);
	}
	else
	{
		unsigned kb = (U.inst_done + 1023) / 1024;
		unsigned total = (U.inst_total + 1023) / 1024;

		if (U.inst_total && U.inst_done >= U.inst_total)
		{
			status = "INSTALLING";
			scol = HUB_CYAN;
		}
		else
		{
			status = a && a->state == HUB_UPDATE ? "UPDATING" :
				 "DOWNLOADING";
			scol = HUB_LIME;
		}
		snprintf(right, sizeof(right), "%u / %u KB", kb, total);
	}
	/* The labels hang off the bar's silver rim: status and target start at
	 * its left edge, the size ends at its right edge (by visible ink). */
	bar.w = hub_blockbar_w(c.w);
	bar.h = bar_h;
	bar.x = c.x + (c.w - bar.w) / 2;
	bar.y = y + lh + 6 * p;
	rim_l = bar.x + HUB_BAR_OUTLINE * p;
	rim_r = bar.x + bar.w - HUB_BAR_OUTLINE * p;
	hub_label_edges(rim_l, -1, y, status, scol);
	if (right[0])
		hub_label_edges(-1, rim_r, y, right, HUB_TEXT);
	y = bar.y;
	if (!hub_blockbar(bar, pct))
		hub_segbar(bar, pct, mode == 1 ? HUB_RED : HUB_LIME,
			   mode == 1 ? HUB_RED_D : HUB_LIME_D);
	y += bar_h + 8 * p;

	if (mode == 1)
		snprintf(buf, sizeof(buf), "%s",
			 U.errmsg[0] ? U.errmsg : "INSTALL FAILED");
	else
		snprintf(buf, sizeof(buf), "%s%s", mode == 2 ? "SAVED TO " : "",
			 U.inst_target[0] ? U.inst_target : "");
	{
		int l = 0, r = 0;

		if (hub_chrome_has("ui-font"))
			hub_chrome_ui_bearing(buf, 1, &l, &r);
		hub_ui_trunc(rim_l - l, y, buf, mode == 1 ? HUB_RED : HUB_DIM,
			     1, rim_r - rim_l + l);
	}

	/* One plate button, centred on the bottom of the clear area. */
	b.w = 120 * p;
	if (b.w > c.w)
		b.w = c.w;
	b.h = btn_h;
	b.x = c.x + (c.w - b.w) / 2;
	b.y = c.y + c.h - b.h;
	s_modal_x = (hub_rect){ 0, 0, 0, 0 };
	if (mode == 0)
	{
		hub_action_button(b, "CANCEL", 0xA0A8B8, 1);
		s_modal_cancel = b;
		s_has_cancel = 1;
	}
	else
	{
		hub_action_button(b, mode == 2 ? "RUN" : "OK", 0x6A7088, 1);
		s_modal_ok = b;
		s_has_ok = 1;
	}
}

static void hub_paint_remove_modal(void)
{
	hub_app *a = hub_get(U.ids[U.sel < U.nids ? U.sel : 0]);
	hub_rect m;
	int p = U.p < 1 ? 1 : U.p;
	char buf[192];
	int bw = 72 * p, bh = 26 * p, y;
	int scale = hub_ui_scale();

	hub_modal_rect(&m, 340, 130);
	hub_ornate_frame(m, 0x101428);
	snprintf(buf, sizeof(buf), "REMOVE %s?",
		 a ? (hub_has(a->title) ? a->title : a->id) : "");
	hub_text_trunc(m.x + 14 * p, m.y + 16 * p, buf, HUB_TEXT, scale,
		       m.w - 28 * p);
	y = m.y + m.h - bh - 14 * p;
	s_modal_yes.x = m.x + m.w / 2 - bw - 8 * p;
	s_modal_yes.y = y;
	s_modal_yes.w = bw;
	s_modal_yes.h = bh;
	s_modal_no.x = m.x + m.w / 2 + 8 * p;
	s_modal_no.y = y;
	s_modal_no.w = bw;
	s_modal_no.h = bh;
	s_has_yesno = 1;
	hub_action_button(s_modal_yes, "YES",
			  U.remove_focus == 0 ? 0x2E9A32 : 0x3A4058,
			  1);
	hub_action_button(s_modal_no, "NO",
			  U.remove_focus == 1 ? 0xC87820 : 0x3A4058,
			  1);
}

/* Darken the screen under a dialog with dark scanlines: every other row
 * is filled, which keeps the pixel grid and costs one fill per row. */
static void hub_dim_backdrop(void)
{
	int p = U.p < 1 ? 1 : U.p;
	int y;

	for (y = 0; y < U.h; y += 2 * p)
		mmb_gfx_fill_rect(0, y, U.w, p, 0x04050A);
}

static void hub_paint(void)
{
	hub_chrome_load(U.p < 1 ? 1 : U.p);
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
	case HUB_ST_INSTALL_DONE:
		if (U.prev_state == HUB_ST_DETAIL)
			hub_draw_detail();
		else
			hub_paint_listing();
		hub_dim_backdrop();
		hub_paint_install_modal(U.state == HUB_ST_INSTALL_FAIL ? 1 :
					U.state == HUB_ST_INSTALL_DONE ? 2 : 0);
		break;
	case HUB_ST_REMOVE:
		if (U.prev_state == HUB_ST_DETAIL)
			hub_draw_detail();
		else
			hub_paint_listing();
		hub_dim_backdrop();
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
	hub_docs_free();
	hub_chrome_free();
	hub_free_ids();
	hub_free();
	if (U.saved_mode != G.gfx.mode || U.saved_bits != G.gfx.bits)
		mmb_gfx_set_mode(U.saved_mode, U.saved_bits);
	G.gfx.font_scale = U.saved_font_scale;
	mmb_gfx_reset_console(1);
}

/* The dialog shows the same path hub_install() will write. */
static int hub_install_target_path(int index, char *out, int outsz)
{
	char *t = hub_target_path(index);

	out[0] = 0;
	if (!t)
		return -1;
	snprintf(out, (size_t)outsz, "%s", t);
	G.plat->free(t);
	return 0;
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
	hub_install_target_path(U.inst_index, U.inst_target,
				sizeof(U.inst_target));
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
	U.inst_done = U.inst_total;
	/* The dialog stays up showing DONE; its RUN button (or Enter) starts
	 * the app and Esc returns to the screen underneath. */
	U.state = HUB_ST_INSTALL_DONE;
	U.dirty = 1;
	hub_rebuild_ids();
	hub_scroll_sel();
	hub_frame();
}

static void hub_activate(void);

static void hub_install_done_close(int run)
{
	U.state = U.prev_state;
	U.dirty = 1;
	if (run)
		hub_activate();
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
	{
		if (U.offline)
		{
			/* Read-only cached mode: say OFFLINE instead of starting a
			 * download that is certain to fail (#1544). */
			U.offline_note = 1;
			U.offline_note_at = mmb_now_ms();
			U.dirty = 1;
			return;
		}
		hub_start_install();
	}
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
		if (hub_has(cur->hero) && !hub_img_find(cur->hero))
			return cur->hero;
		for (i = 0; i < hub_gallery_count(cur) && i < 8; i++)
		{
			rel = hub_gallery_rel(cur, i);
			if (rel && !hub_img_find(rel))
				return rel;
		}
		if (hub_is_docpath(cur->about) && !hub_doc_find(cur->about))
			return cur->about;
		if (hub_is_docpath(cur->changes) && !hub_doc_find(cur->changes))
			return cur->changes;
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
	U.offline = 0;
	U.offline_note = 0;
	if (hub_load_index(err, sizeof(err)) != 0)
	{
		U.err = 1;
		snprintf(U.errmsg, sizeof(U.errmsg), "%s",
			 err[0] ? err : "OFFLINE");
		U.load_done = 1;
		hub_frame();
		return;
	}
	U.offline = hub_is_offline();
	hub_build_tabs();
	{
		int i;

		U.tab = -1;
		U.sel = 0;
		U.list_top = 0;
		hub_layout_now();
		U.icon_total = hub_count() < 6 ? hub_count() : 6;
		U.icon_stage = U.icon_total;
		hub_frame();
		for (i = 0; i < U.icon_total; i++)
		{
			hub_app *a = hub_get(i);

			if (a && a->icon && a->icon[0])
				hub_img_try(a->icon);
			if (U.icon_stage > 0)
				U.icon_stage--;
			hub_frame();
		}
		hub_rebuild_ids();
		hub_scroll_sel();
	}
	U.load_done = 1;
	U.icon_stage = 0;
	U.last_load = mmb_now_ms();
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
	case HUB_ST_INSTALL_DONE:
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

/* Re-run the index fetch from cached browsing. If it still fails the cache
 * opens again; when it succeeds the refreshed catalogue replaces it. */
static void hub_retry_online(void)
{
	U.err = 0;
	U.offline = 0;
	U.offline_note = 0;
	U.load_started = 0;
	U.load_done = 0;
	U.started = (int)mmb_now_ms();
	U.state = HUB_ST_LOADING;
	U.dirty = 1;
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
		    (hub_rect_contains(&s_modal_cancel, ms.x, ms.y) ||
		     hub_rect_contains(&s_modal_x, ms.x, ms.y)))
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
		if (U.err && (c == '\r' || c == '\n' ||
			      c == 'r' || c == 'R'))
		{
			hub_retry_online();
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
	case HUB_ST_INSTALL_DONE:
		if (c == '\r' || c == '\n')
			hub_install_done_close(1);
		handled = 1;
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
		if (U.offline && (c == 'r' || c == 'R'))
		{
			hub_retry_online();
			handled = 1;
		}
		else if (c == '\r' || c == '\n')
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
		if (U.offline && (c == 'r' || c == 'R'))
		{
			hub_retry_online();
			handled = 1;
		}
		else if (c == '\r' || c == '\n')
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

			if (a && hub_gallery_count(a) > 0)
			{
				U.detail_shot = (U.detail_shot + 1) %
						hub_gallery_count(a);
				U.dirty = 1;
			}
		}
		else if (code == 'D')
		{
			hub_app *a = hub_cur();

			if (a && hub_gallery_count(a) > 0)
			{
				int n = hub_gallery_count(a);

				U.detail_shot = (U.detail_shot + n - 1) % n;
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
		if (s_has_cancel &&
		    (hub_rect_contains(&s_modal_cancel, x, y) ||
		     hub_rect_contains(&s_modal_x, x, y)))
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
	if (U.state == HUB_ST_INSTALL_DONE)
	{
		if (s_has_ok && hub_rect_contains(&s_modal_ok, x, y))
			hub_install_done_close(1);
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

	if (U.offline_note && now - U.offline_note_at >= 2500)
	{
		U.offline_note = 0;
		U.dirty = 1;
	}

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
	/* Resample the clock. `now` was taken before the index fetch, so
	 * subtracting last_load from it underflows and skips the hold. */
	if (U.state == HUB_ST_LOADING && U.load_done && !U.err &&
	    mmb_now_ms() - U.last_load >= HUB_LOAD_HOLD_MS)
	{
		U.state = HUB_ST_LIST;
		U.dirty = 1;
	}

	if (U.active && U.state != HUB_ST_LOADING && now - U.last_asset >=
	    HUB_ASSET_MS)
	{
		const char *rel = hub_need_asset();

		if (rel)
		{
			U.last_asset = now;
			if (hub_is_docpath(rel))
			{
				if (hub_doc_try(rel))
					U.dirty = 1;
			}
			else if (hub_img_try(rel))
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
