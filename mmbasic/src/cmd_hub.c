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
#define HUB_LOAD_HOLD_MS 800
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
static int s_tab_scale;
static hub_rect s_modal_x;

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
			  int skip, int maxlines, unsigned col, int scale)
{
	const char *q = s;
	int line = 0, cx = 0;

	if (scale < 1)
		scale = 1;

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
				hub_text_n(x + cx * 8 * scale,
					   y + (line - skip) * lineh,
					   word, (unsigned)take, col, scale);
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

/* Listing titles fill about 40% of the row. Body copy, chips, badges and
 * the button label sit one step down so they stay readable beside them. */
static int hub_title_scale(void)
{
	int p = U.p < 1 ? 1 : U.p;
	int row = U.L.row_h > 16 ? U.L.row_h : 56 * p;
	int s = row * 2 / 5 / 16;

	if (s < 1)
		s = 1;
	if (s > 4)
		s = 4;
	/* Take the next size when the row still holds a title and a summary. */
	if (s < 4 && 16 * (s + 1) + 16 * s + 6 * p <= row)
		s++;
	return s;
}

static int hub_body_scale(void)
{
	int s = hub_title_scale() - 1;

	if (s < 1)
		s = 1;
	return s;
}

static int hub_ui_scale(void)
{
	return hub_body_scale();
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
	if (t > 12)
		t = 12;
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
		mmb_gfx_line(tx, ty, trx, try_, glow_t, t + t / 2);
		mmb_gfx_line(tx, ty, tlx, tly, glow_t, t + t / 2);
		mmb_gfx_line(tlx, tly, blx, bly, glow_l, t + t / 2);
		mmb_gfx_line(blx, bly, bx, by, glow_l, t + t / 2);
		mmb_gfx_line(trx, try_, brx, bry, glow_r, t + t / 2);
		mmb_gfx_line(brx, bry, bx, by, glow_r, t + t / 2);
		mmb_gfx_line(mx, my, bx, by, glow_t, t);
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

static void hub_app_icon(hub_app *a, hub_rect r, int greyed, int framed)
{
	hub_img *im = a ? hub_img_find(a->icon) : 0;
	int p = U.p < 2 ? 1 : U.p;

	hub_fill(r, 0x0B0F22);
	if (im && im->pix)
		hub_draw_image(im->pix, im->w, im->h, r);
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
	int i, scale, longest, avail, n;

	{
		int pw = U.w / 640 < U.h / 360 ? U.w / 640 : U.h / 360;

		if (pw < 1)
			pw = 1;
		U.p = pw;
	}
	scale = U.p >= 2 ? 2 : 1;
	n = s_ntabs > 0 ? s_ntabs : 1;
	avail = U.w - 8 * (U.p < 1 ? 1 : U.p);
	for (;;)
	{
		int floor = U.p >= 2 ? 2 : 1;

		longest = 0;
		for (i = 0; i < s_ntabs; i++)
		{
			int w = hub_text_w(s_tabs[i].name, scale);

			if (w > longest)
				longest = w;
		}
		/* Stay at the larger size even when the labels wrap to two rows. */
		if (scale <= floor || n * (longest + 8 * U.p) <= avail)
			break;
		scale--;
	}
	if (scale < 1)
		scale = 1;
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
	hub_fill(r, 0x10131C);
	mmb_gfx_box(r.x, r.y, r.w, r.h, 0xC8CCD8, p, -1);
	mmb_gfx_box(r.x + p, r.y + p, r.w - 2 * p, r.h - 2 * p, 0x3A3E4C, p, -1);
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
		if (i < fill)
		{
			if (gradient)
				col = hub_lerp(0xE000C8, 0x00E4EC, i, nseg - 1);
			else
				col = hi;
		}
		else
			col = lo ? lo : 0x1A1E2A;
		hub_fill(s, col);
		if (i < fill)
			hub_hline(s.x, s.y, s.w,
				  hub_lerp(col, 0xFFFFFF, 1, 3), 1);
	}
}

/* Install dialog: one lime bar with diagonal stripes. */
static void hub_stripebar(hub_rect r, int pct)
{
	int p = U.p < 1 ? 1 : U.p;
	int pad = 3 * p;
	int inner_w, fill_w, x, y;

	if (pct < 0)
		pct = 0;
	if (pct > 100)
		pct = 100;
	hub_fill(r, 0x10131C);
	mmb_gfx_box(r.x, r.y, r.w, r.h, 0x8A9098, p, -1);
	inner_w = r.w - 2 * pad;
	if (inner_w < 1)
		return;
	fill_w = inner_w * pct / 100;
	if (pct > 0 && fill_w < 2 * p)
		fill_w = 2 * p;
	if (fill_w > 0)
	{
		hub_rect f = { r.x + pad, r.y + pad, fill_w, r.h - 2 * pad };

		hub_fill(f, 0x3CBA34);
		hub_hline(f.x, f.y, f.w, 0xB6F08A, 1);
		for (y = f.y; y < f.y + f.h; y++)
		{
			for (x = f.x; x < f.x + f.w; x++)
			{
				if (((x + y) / (p < 1 ? 1 : p)) % 6 < 2)
					mmb_gfx_plot(x, y, 0x1E6E18);
			}
		}
	}
}

/* ---- loading screen -------------------------------------------------- */

static void hub_paint_loading(void)
{
	int p = U.p < 1 ? 1 : U.p;
	hub_rect bar;
	const char *title = "MMCORE SOFTWARE HUB";
	int title_scale = p < 2 ? 1 : (p > 3 ? 3 : p);
	int cube_s = U.h * 46 / 100;
	int cube_h, title_h, bar_h, block, top, cy, ty, tx, tw;
	int pct;
	int i;

	if (cube_s > U.w * 32 / 100)
		cube_s = U.w * 32 / 100;
	if (cube_s < 72)
		cube_s = 72;
	/* The cube is about as tall as it is wide, plus the sparkle. */
	cube_h = cube_s + cube_s / 5;
	title_h = 16 * title_scale;
	bar_h = 26 * p;
	block = cube_h + 12 * p + title_h + 12 * p + bar_h;
	top = (U.h - block) / 2;
	if (top < 4 * p)
		top = 4 * p;

	mmb_gfx_fill_rect(0, 0, U.w, U.h, 0x05060C);
	cy = top + cube_s / 5 + cube_s / 2;
	hub_cube(U.w / 2, cy, cube_s, 1);

	tw = hub_text_w(title, title_scale);
	tx = (U.w - tw) / 2;
	ty = top + cube_h + 10 * p;
	hub_text(tx, ty, title, 0xC060F0, title_scale);
	for (i = 0; i < 12; i++)
	{
		int dot = 2 * p;
		int gap = 6 * p;
		int ry = ty + title_h / 2 - dot / 2;
		int lx = tx - 10 * p - (12 - i) * gap;
		int rx = tx + tw + 10 * p + i * gap;
		unsigned lc = hub_lerp(0x7010A0, 0xC060F0, i, 11);
		unsigned rc = hub_lerp(0xC060F0, 0x20D0E8, i, 11);

		if (lx > 8 * p)
			mmb_gfx_fill_rect(lx, ry, dot, dot, lc);
		if (rx + dot < U.w - 8 * p)
			mmb_gfx_fill_rect(rx, ry, dot, dot, rc);
	}

	bar.w = U.w * 58 / 100;
	if (bar.w > 380 * p)
		bar.w = 380 * p;
	if (bar.w < 80 * p)
		bar.w = U.w - 16 * p;
	bar.h = bar_h;
	bar.x = (U.w - bar.w) / 2;
	bar.y = ty + title_h + 14 * p;

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

/* Filled pill. Returns the width drawn, or 0 when `text` is empty. */
static int hub_chip(int x, int y, const char *text, unsigned col,
		    unsigned fill, int scale)
{
	int p = U.p < 1 ? 1 : U.p;
	int w, h, t;

	if (!hub_has(text))
		return 0;
	if (scale < 1)
		scale = 1;
	w = hub_text_w(text, scale) + 8 * p;
	h = 16 * scale + 4 * p;
	t = p < 2 ? 1 : p;
	hub_fill((hub_rect){ x, y, w, h }, fill);
	mmb_gfx_box(x, y, w, h, col, t, -1);
	hub_hline(x + t, y + t, w - 2 * t, hub_lerp(fill, 0xFFFFFF, 1, 4), 1);
	hub_text(x + 4 * p, y + (h - 16 * scale) / 2, text, col, scale);
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
	int greyed, scale, tscale, x, ty, lim;
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
	r.w = U.L.list.w - 6 * p;
	r.h = h - 2 * p;
	if (r.h < 8)
		return;
	greyed = a->state == HUB_NEEDS_NEWER;
	tscale = hub_title_scale();
	scale = hub_body_scale();
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
		bw = hub_text_w(badge, scale) + 10 * p;
		bh = 16 * scale + 6 * p;
		if (bh > r.h - 4 * p)
			bh = r.h - 4 * p;
	}
	meta_w = hub_text_w(author, scale);
	if (hub_text_w(sz, scale) > meta_w)
		meta_w = hub_text_w(sz, scale);
	if (bw)
		meta_w += bw + 6 * p;
	x = icon.x + icon.w + 6 * p;
	lim = r.x + r.w - 6 * p - meta_w;
	if (lim < x + 16)
		lim = r.x + r.w - 6 * p;
	{
		int block = 16 * tscale +
			    (summary[0] ? 3 * p + 16 * scale : 0);

		ty = r.y + (narrow && selected ? 2 * p : (r.h - block) / 2);
	}
	if (ty < r.y + p)
		ty = r.y + p;
	hub_text_trunc(x, ty, title, greyed ? HUB_GREY : HUB_TEXT, tscale,
		       lim - x);
	if (summary[0])
		hub_text_trunc(x, ty + 16 * tscale + 2 * p, summary,
			       greyed ? HUB_GREY : HUB_DIM, scale, lim - x);

	if (selected && narrow)
	{
		int iw = r.w - 10 * p;
		int ih = iw * 9 / 16;
		hub_rect ir, frame;
		const char *rel = hub_preview_rel(a);
		hub_img *im = hub_img_find(rel);

		if (ih > U.L.list.h * 42 / 100)
			ih = U.L.list.h * 42 / 100;
		frame.x = r.x + 4 * p;
		frame.y = ty + 16 * tscale +
			  (summary[0] ? 16 * scale + 4 * p : 0) + 6 * p;
		frame.w = iw;
		frame.h = ih;
		if (frame.y + frame.h > r.y + r.h - 2 * p)
			frame.h = r.y + r.h - 2 * p - frame.y;
		if (frame.h > 16 * p)
		{
			hub_ornate_frame(frame, 0x0B0F22);
			ir.x = frame.x + 8 * p;
			ir.y = frame.y + 8 * p;
			ir.w = frame.w - 16 * p;
			ir.h = frame.h - 16 * p;
			if (ir.w > 0 && ir.h > 0)
			{
				hub_fill(ir, 0x070910);
				if (im && im->pix)
					hub_draw_image_cover(im->pix, im->w, im->h, ir);
				else
					hub_cube(ir.x + ir.w / 2, ir.y + ir.h / 2,
						 ir.h * 2 / 3, 1);
			}
			hub_frame_chrome(frame);
		}
	}

	bx = r.x + r.w - 4 * p - bw;
	by = r.y + (r.h - bh) / 2;
	if (narrow && selected)
		by = r.y + 4 * p;
	if (bw > 0)
		hub_badge(bx, by, bw, bh, badge, bfill, bcol);
	{
		int mx = (bw ? bx : r.x + r.w) - 6 * p;
		int my = by + (bh - (sz[0] && author[0] ? 16 * scale * 2 + p :
				      16 * scale)) / 2;
		unsigned col = greyed ? HUB_GREY : HUB_DIM;

		if (my < r.y + 2 * p)
			my = r.y + 2 * p;
		if (author[0])
		{
			hub_text_r(mx, my, author, col, scale);
			my += 16 * scale + p;
		}
		if (sz[0])
			hub_text_r(mx, my, sz, col, scale);
	}
}

static void hub_paint_listing(void)
{
	hub_app *cur = hub_cur();
	hub_rect hdr = U.L.header;
	int p = U.p < 1 ? 1 : U.p;
	int i, y, mark;

	hub_dither_bg();

	hub_bevel(hdr, 0x101428);
	hub_cube(hdr.x + 14 * p, hdr.y + hdr.h / 2, hdr.h * 3 / 4, 0);
	mark = p >= 3 ? 3 : (p >= 2 ? 2 : 1);
	{
		int ty = hdr.y + (hdr.h - 16 * mark) / 2;
		int tx = hdr.x + hdr.h;

		if (ty < hdr.y)
			ty = hdr.y;
		hub_text(tx, ty, "mmcore", HUB_TEXT, mark);
		hub_text(tx + hub_text_w("mmcore ", mark), ty, "HUB",
			 HUB_MAGENTA, mark);
	}

	hub_fill(U.L.content, HUB_PANEL);
	mmb_gfx_box(U.L.content.x, U.L.content.y, U.L.content.w, U.L.content.h,
		    HUB_BORDER_D, p, -1);
	hub_hline(U.L.content.x, U.L.content.y, U.L.content.w, HUB_BORDER_L, p);

	{
		int cols = U.L.tabs_per_row > 0 ? U.L.tabs_per_row : 1;
		int gap = U.L.tab_gap;
		int tw = U.L.tab_w;
		int scale = s_tab_scale > 0 ? s_tab_scale : 1;

		if (s_ntabs > 0)
		{
			int nfit = U.L.tab_rows == 1 ? s_ntabs : cols;

			tw = (U.L.tabs.w - (nfit - 1) * gap) / nfit;
			if (tw < 8)
				tw = 8;
		}
		for (i = 0; i < s_ntabs; i++)
		{
			int row = i / cols;
			int col = i % cols;
			{
				int tx = U.L.tabs.x + col * (tw + gap);
				int ty = U.L.tabs.y + row * (U.L.tab_h + gap);
				int sel = s_tabs[i].cat == U.tab;
				hub_rect tr = { tx, ty, tw, U.L.tab_h };

				if (i < (int)(sizeof(s_tabbox) / sizeof(s_tabbox[0])))
				{
					s_tabbox[s_ntabbox].r = tr;
					s_tabbox[s_ntabbox].value = s_tabs[i].cat;
					s_ntabbox++;
				}
				if (sel)
				{
					int foot_y = tr.y + tr.h;
					int foot_h = U.L.content.y + p - foot_y;
					int k, rad = 3 * p;

					if (foot_h < 1)
						foot_h = 1;
					hub_fill(tr, HUB_PANEL);
					hub_fill((hub_rect){ tr.x, foot_y, tr.w, foot_h },
						 HUB_PANEL);
					hub_hline(tr.x, tr.y, tr.w, HUB_MAGENTA, p < 2 ? 1 : 2);
					hub_vline(tr.x, tr.y, tr.h + foot_h, HUB_BORDER_L, p);
					hub_vline(tr.x + tr.w - p, tr.y, tr.h + foot_h,
						  HUB_BORDER_D, p);
					for (k = 0; k <= rad; k++)
					{
						mmb_gfx_plot(tr.x - k, foot_y + foot_h - p,
							     HUB_BORDER_L);
						mmb_gfx_plot(tr.x + tr.w - 1 + k,
							     foot_y + foot_h - p,
							     HUB_BORDER_D);
					}
					hub_text(tr.x + (tr.w - hub_text_w(s_tabs[i].name,
									     scale)) / 2,
						 tr.y + (tr.h - 16 * scale) / 2,
						 s_tabs[i].name, HUB_MAGENTA, scale);
				}
				else
				{
					hub_text(tr.x + (tr.w - hub_text_w(s_tabs[i].name,
									     scale)) / 2,
						 tr.y + (tr.h - 16 * scale) / 2,
						 s_tabs[i].name, HUB_DIM, scale);
				}
			}
		}
	}

	y = U.L.list.y + 2 * p;
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
		hub_rect frame, ir;
		hub_img *im = 0;
		const char *rel = 0;
		int ty;
		int frame_h;

		int tscale = hub_title_scale();
		int bscale = hub_body_scale();
		int cap;

		pr.x += 4 * p;
		pr.y += 4 * p;
		pr.w -= 8 * p;
		pr.h -= 8 * p;
		/* Title, author and two lines of summary sit under the shot. */
		cap = 16 * tscale + 16 * bscale * 3 + 16 * p;
		if (cap > pr.h / 2)
			cap = pr.h / 2;
		hub_ornate_frame(pr, 0x101428);
		frame.x = pr.x + 8 * p;
		frame.y = pr.y + 8 * p;
		frame.w = pr.w - 16 * p;
		frame_h = pr.h - cap - 12 * p;
		if (frame_h < 24 * p)
			frame_h = 24 * p;
		if (frame.y + frame_h > pr.y + pr.h - cap)
			frame_h = pr.y + pr.h - cap - frame.y;
		frame.h = frame_h;
		ir.x = frame.x + 2 * p;
		ir.y = frame.y + 2 * p;
		ir.w = frame.w - 4 * p;
		ir.h = frame.h - 4 * p;
		hub_fill(ir, 0x070910);
		if (cur)
			rel = hub_preview_rel(cur);
		im = hub_img_find(rel);
		if (im && im->pix && ir.w > 0 && ir.h > 0)
			hub_draw_image_cover(im->pix, im->w, im->h, ir);
		else if (cur && ir.w > 0)
			hub_cube(ir.x + ir.w / 2, ir.y + ir.h / 2,
				 ir.h * 2 / 3, 1);
		hub_frame_chrome(pr);

		ty = frame.y + frame.h + 6 * p;
		if (cur && ty < pr.y + pr.h - 8 * p)
		{
			char line[120];
			char shown[96];

			hub_upper(hub_has(cur->title) ? cur->title : cur->id,
				  shown, sizeof(shown));
			if (hub_has(cur->version))
				snprintf(line, sizeof(line), "%s v%s", shown,
					 cur->version);
			else
				snprintf(line, sizeof(line), "%s", shown);
			hub_text_trunc(pr.x + 12 * p, ty, line, HUB_TEXT, tscale,
				       pr.w - 24 * p);
			ty += 16 * tscale + 3 * p;
			if (hub_has(cur->author) && ty < pr.y + pr.h - 16)
			{
				snprintf(line, sizeof(line), "by %s", cur->author);
				hub_text_trunc(pr.x + 12 * p, ty, line, HUB_DIM,
					       bscale, pr.w - 24 * p);
				ty += 16 * bscale + 3 * p;
			}
			if (hub_has(cur->summary) && ty < pr.y + pr.h - 8)
				hub_wrap_draw(pr.x + 12 * p, ty, cur->summary,
					      hub_wrap_cols(pr.w - 24 * p, bscale),
					      16 * bscale, 0,
					      (pr.y + pr.h - ty - 6 * p) /
					      (16 * bscale),
					      HUB_DIM, bscale);
		}
	}
}

/* ---- detail ---------------------------------------------------------- */

static const char *hub_button_label(hub_app *a, unsigned *col,
				    unsigned *fill, char *buf, int bufsz)
{
	*col = 0x2E9A32;
	*fill = 1;
	if (!a)
		return "";
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

static void hub_action_button(hub_rect b, const char *label, unsigned accent,
			      int enabled)
{
	int p = U.p < 1 ? 1 : U.p;
	int t = p < 2 ? 1 : 2;
	unsigned fill = enabled ? hub_lerp(0x000000, accent, 2, 3) : 0x2A2E3A;
	unsigned top = enabled ? hub_lerp(accent, 0xFFFFFF, 1, 2) : 0x6A7080;
	unsigned bot = enabled ? hub_lerp(accent, 0x000000, 1, 2) : 0x14161C;
	int scale = hub_body_scale();

	hub_fill(b, fill);
	hub_hline(b.x, b.y, b.w, top, t);
	hub_vline(b.x, b.y, b.h, top, t);
	hub_hline(b.x, b.y + b.h - t, b.w, bot, t);
	hub_vline(b.x + b.w - t, b.y, b.h, bot, t);
	mmb_gfx_box(b.x + 2 * p, b.y + 2 * p, b.w - 4 * p, b.h - 4 * p,
		    top, p < 2 ? 1 : p, -1);
	if (16 * scale + 4 > b.h)
		scale = 1;
	hub_text(b.x + (b.w - hub_text_w(label, scale)) / 2,
		 b.y + (b.h - 16 * scale) / 2, label,
		 enabled ? 0xFFFFFF : HUB_GREY, scale);
}

static void hub_draw_main_button(hub_rect area)
{
	hub_app *a = hub_cur();
	int p = U.p < 1 ? 1 : U.p;
	int bw = area.w * 70 / 100;
	hub_rect b;
	char buf[64];
	const char *label;
	unsigned accent, enabled;

	{
		int ls = hub_body_scale();
		const char *probe;
		char pbuf[64];
		unsigned ac, en;

		probe = hub_button_label(a, &ac, &en, pbuf, sizeof(pbuf));
		bw = hub_text_w(probe, ls) + 24 * p;
		(void)ac;
		(void)en;
	}
	if (bw < 72 * p)
		bw = 72 * p;
	if (bw > area.w * 55 / 100)
		bw = area.w * 55 / 100;
	if (bw > area.w - 4 * p)
		bw = area.w - 4 * p;
	if (bw < 20)
		bw = area.w;
	b.w = bw;
	b.h = 16 * hub_body_scale() + 10 * p;
	if (b.h > area.h)
		b.h = area.h > 0 ? area.h : b.h;
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
	int scale = hub_ui_scale();
	int lh = 16 * scale + 4 * (U.p < 1 ? 1 : U.p);
	int cols = hub_wrap_cols(r.w - 8 * p, scale);
	const char *body = (a && hub_has(a->about)) ? a->about :
			   (a && hub_has(a->summary) ? a->summary : "");
	const char *changes = (a && hub_has(a->changes)) ? a->changes : "";
	int body_lines = hub_wrap_lines(body, cols);
	int ch_lines = changes[0] ? hub_wrap_lines(changes, cols) : 0;
	int head = changes[0] ? body_lines + 1 : -1;
	int ch_at = changes[0] ? body_lines + 2 : -1;
	int lines = body_lines + (changes[0] ? 2 + ch_lines : 0);
	int rows = lh > 0 ? r.h / lh : 0;
	int maxscroll;
	int i;

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
			hub_wrap_draw(r.x, y, body, cols, lh, line, 1,
				      HUB_TEXT, scale);
		else if (line == head)
			hub_text(r.x, y, "WHAT'S NEW", HUB_ORANGE, scale);
		else if (ch_at >= 0 && line >= ch_at &&
			 line < ch_at + ch_lines)
			hub_wrap_draw(r.x, y, changes, cols, lh,
				      line - ch_at, 1, HUB_DIM, scale);
	}
	if (maxscroll > 0 && r.h > 4 * p)
	{
		int track = r.h - 2 * p;
		int thumb = track * rows / (lines > 0 ? lines : 1);
		int pos = lines > 0 ? track * scroll / lines : 0;

		if (thumb < 8 * p)
			thumb = 8 * p;
		if (thumb > track)
			thumb = track;
		mmb_gfx_fill_rect(r.x + r.w - p, r.y, p, track, 0x2A3048);
		mmb_gfx_fill_rect(r.x + r.w - p, r.y + pos, p, thumb, 0x8A90A8);
	}
}

static void hub_draw_detail(void)
{
	hub_app *a = hub_cur();
	hub_layout *L = &U.L;
	int p = U.p < 1 ? 1 : U.p;
	hub_rect shot = L->detail_shot;
	hub_rect img;
	hub_img *im;
	const char *rel;
	int narrow = L->mode == HUB_LAYOUT_NARROW;

	hub_dither_bg();
	hub_fill(L->detail, 0x0C1024);
	mmb_gfx_box(L->detail.x, L->detail.y, L->detail.w, L->detail.h,
		    0x2A3050, p, -1);

	hub_ornate_frame(shot, 0x101428);
	img.x = shot.x + 12 * p;
	img.y = shot.y + 12 * p;
	img.w = shot.w - 24 * p;
	img.h = shot.h - 24 * p;
	if (img.w < 4)
		img.w = 4;
	if (img.h < 4)
		img.h = 4;
	hub_fill(img, 0x070910);
	rel = hub_detail_rel(a);
	im = hub_img_find(rel);
	if (im && im->pix)
		hub_draw_image_cover(im->pix, im->w, im->h, img);
	else if (a)
		hub_cube(img.x + img.w / 2, img.y + img.h / 2,
			 img.h * 2 / 3, 1);
	hub_frame_chrome(shot);

	s_nshotbox = 0;
	if (a && a->nshots > 1)
	{
		int n = a->nshots;
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
				hub_img *ti = hub_img_find(a->shots[i]);

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

	if (a)
	{
		char upper[96];
		char line1[64], line2[64];
		int two = 0;
		int scale = U.p >= 2 ? U.p * 2 + 1 : 3;
		int lines, block_h, icon_s, text_w, maxw;
		int tx, ty, ix, iy;
		char *sp;

		if (scale > 8)
			scale = 8;
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
		icon_s = L->detail_title.h * 70 / 100;
		if (icon_s > 48 * p)
			icon_s = 48 * p;
		if (icon_s < 16 * p)
			icon_s = 16 * p;
		if (icon_s > L->detail_title.h - 2 * p)
			icon_s = L->detail_title.h - 2 * p;
		maxw = L->detail_title.w - icon_s - 12 * p;
		if (maxw < 16)
			maxw = L->detail_title.w;
		while (scale > 1 &&
		       (16 * scale * lines + (lines - 1) * 2 * p >
			L->detail_title.h ||
			hub_text_w(line1, scale) > maxw ||
			(two && hub_text_w(line2, scale) > maxw)))
			scale--;
		block_h = 16 * scale * lines + (lines - 1) * 2 * p;
		text_w = hub_text_w(line1, scale);
		if (two && hub_text_w(line2, scale) > text_w)
			text_w = hub_text_w(line2, scale);
		if (narrow)
		{
			ix = L->detail_title.x;
			tx = ix + icon_s + 8 * p;
		}
		else
		{
			int pair = text_w + 8 * p + icon_s;
			int left = L->detail_title.x +
				   (L->detail_title.w - pair) / 2;

			if (left < L->detail_title.x)
				left = L->detail_title.x;
			tx = left;
			ix = left + text_w + 8 * p;
		}
		ty = L->detail_title.y + (L->detail_title.h - block_h) / 2;
		if (ty < L->detail_title.y)
			ty = L->detail_title.y;
		iy = ty + (block_h - icon_s) / 2;
		hub_app_icon(a, (hub_rect){ ix, iy, icon_s, icon_s },
			     a->state == HUB_NEEDS_NEWER, 0);
		hub_grad_text(tx, ty, line1, 0xFFFFFF, 0xB0B6C8, scale, 1,
			      0x120818);
		if (two)
		{
			int y2 = ty + 16 * scale + 2 * p;

			hub_grad_text(tx, y2, line2, 0xF0D0FF, 0x6820C8,
				      scale, 1, 0x140820);
		}
	}

	if (a)
	{
		int x = L->detail_chips.x;
		int y = L->detail_chips.y;
		int maxx = L->detail_chips.x + L->detail_chips.w - 4 * p;
		int scale = hub_body_scale();
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

static void hub_rocket(int x, int y, int s)
{
	/* 16x28 grid, s pixels per cell. Nose at the top. */
	hub_fill((hub_rect){ x + 6 * s, y, 4 * s, 3 * s }, 0xD8E0F0);
	hub_fill((hub_rect){ x + 4 * s, y + 3 * s, 8 * s, 4 * s }, 0x9AA8C4);
	hub_fill((hub_rect){ x + 5 * s, y + 5 * s, 6 * s, 3 * s }, 0x29D3F0);
	hub_fill((hub_rect){ x + 3 * s, y + 7 * s, 10 * s, 10 * s }, 0x6A7A9A);
	hub_fill((hub_rect){ x + 5 * s, y + 9 * s, 6 * s, 5 * s }, 0xC8D4E8);
	hub_fill((hub_rect){ x, y + 10 * s, 3 * s, 8 * s }, 0xC87820);
	hub_fill((hub_rect){ x + 13 * s, y + 10 * s, 3 * s, 8 * s }, 0xC87820);
	hub_fill((hub_rect){ x + 1 * s, y + 12 * s, 2 * s, 4 * s }, 0xE8A040);
	hub_fill((hub_rect){ x + 13 * s, y + 12 * s, 2 * s, 4 * s }, 0xE8A040);
	hub_fill((hub_rect){ x + 6 * s, y + 17 * s, 4 * s, 4 * s }, 0xF0E060);
	hub_fill((hub_rect){ x + 5 * s, y + 20 * s, 6 * s, 4 * s }, 0xF09020);
	hub_fill((hub_rect){ x + 6 * s, y + 23 * s, 4 * s, 4 * s }, 0xE04020);
	hub_fill((hub_rect){ x + 7 * s, y + 26 * s, 2 * s, 3 * s }, 0xFFD080);
}

static void hub_floppy(int x, int y, int s)
{
	int i;

	hub_fill((hub_rect){ x, y, 16 * s, 16 * s }, 0x3A48D0);
	hub_fill((hub_rect){ x + s, y + s, 14 * s, 14 * s }, 0x2A38B0);
	hub_fill((hub_rect){ x + 2 * s, y + 2 * s, 12 * s, 5 * s }, 0xE8ECF4);
	hub_fill((hub_rect){ x + 10 * s, y + 2 * s, 3 * s, 3 * s }, 0x202838);
	hub_fill((hub_rect){ x + 4 * s, y + 9 * s, 8 * s, 5 * s }, 0x101428);
	hub_fill((hub_rect){ x + 6 * s, y + 10 * s, 4 * s, 3 * s }, 0xC0C8E0);
	for (i = 0; i < 3; i++)
	{
		int rad = (5 + i * 3) * s;
		int ax = x + 14 * s;
		int ay = y + 2 * s;

		mmb_gfx_line(ax, ay - rad, ax + rad * 2 / 3, ay - rad / 2,
			     0xD070F0, s);
		mmb_gfx_line(ax + rad * 2 / 3, ay - rad / 2, ax + rad, ay,
			     0xD070F0, s);
	}
}

static void hub_paint_install_modal(int failed)
{
	hub_app *a = hub_get(U.inst_index);
	hub_rect m, bar, b;
	int p = U.p < 1 ? 1 : U.p;
	int y, scale;
	char buf[192];
	char title[128];

	m.w = U.w * 62 / 100;
	m.h = U.h * 64 / 100;
	if (m.w < 300 * p)
		m.w = 300 * p;
	if (m.h < 200 * p)
		m.h = 200 * p;
	if (m.w > U.w - 12 * p)
		m.w = U.w - 12 * p;
	if (m.h > U.h - 12 * p)
		m.h = U.h - 12 * p;
	m.x = (U.w - m.w) / 2;
	m.y = (U.h - m.h) / 2;
	hub_ornate_frame(m, 0x101428);

	scale = p >= 2 ? 2 : 1;
	if (16 * scale + 8 > 36 * p)
		scale = 1;
	{
		const char *verb = a && a->state == HUB_UPDATE ? "UPDATING" :
				   "INSTALLING";
		char name[64];
		int th = 16 * scale;

		hub_upper(a ? (hub_has(a->title) ? a->title : a->id) : "",
			  name, sizeof(name));
		if (a && hub_has(a->version))
			snprintf(title, sizeof(title), "%s %s v%s", verb, name,
				 a->version);
		else
			snprintf(title, sizeof(title), "%s %s", verb, name);
		hub_text_trunc(m.x + (m.w - hub_text_w(title, scale)) / 2,
			       m.y + 10 * p + (22 * p - th) / 2, title,
			       HUB_TEXT, scale, m.w - 56 * p);
		hub_hline(m.x + 14 * p, m.y + 34 * p, m.w - 28 * p, 0x3A3058,
			  p);
	}
	{
		int xsz = 16 * p;

		s_modal_x.x = m.x + m.w - xsz - 14 * p;
		s_modal_x.y = m.y + 10 * p;
		s_modal_x.w = xsz;
		s_modal_x.h = xsz;
		hub_fill(s_modal_x, 0x1A1E30);
		mmb_gfx_box(s_modal_x.x, s_modal_x.y, xsz, xsz, 0xC8CCD8, p, -1);
		mmb_gfx_line(s_modal_x.x + 4 * p, s_modal_x.y + 4 * p,
			     s_modal_x.x + xsz - 4 * p,
			     s_modal_x.y + xsz - 4 * p, HUB_TEXT, p);
		mmb_gfx_line(s_modal_x.x + xsz - 4 * p, s_modal_x.y + 4 * p,
			     s_modal_x.x + 4 * p,
			     s_modal_x.y + xsz - 4 * p, HUB_TEXT, p);
	}

	y = m.y + 48 * p;
	if (!failed)
	{
		unsigned kb = (U.inst_done + 1023) / 1024;
		unsigned total = (U.inst_total + 1023) / 1024;
		int rs = p < 2 ? 1 : 3;
		int lx = m.x + 28 * p + 16 * rs;

		if (m.w > 220 * p)
			hub_rocket(m.x + 18 * p, y, rs);
		hub_text(lx, y, "DOWNLOADING", HUB_LIME, scale);
		snprintf(buf, sizeof(buf), "%u / %u KB", kb, total);
		hub_text(lx + hub_text_w("DOWNLOADING  ", scale), y, buf,
			 HUB_TEXT, scale);
		y += 16 * scale + 8 * p;
		bar.x = lx;
		bar.y = y;
		bar.w = m.x + m.w - 22 * p - lx;
		bar.h = 20 * p;
		hub_stripebar(bar, U.inst_total ?
			      (int)((uint64_t)U.inst_done * 100u /
				    (unsigned)U.inst_total) : 0);
		y += bar.h + 12 * p;
		hub_text(lx, y, "CHECKSUM", HUB_CYAN, scale);
		snprintf(buf, sizeof(buf), "%s",
			 U.inst_checksum == 1 ? "OK" :
			 U.inst_checksum == 2 ? "FAILED" : "...");
		hub_text(lx + hub_text_w("CHECKSUM  ", scale), y, buf,
			 U.inst_checksum == 2 ? HUB_RED :
			 U.inst_checksum == 1 ? HUB_LIME : HUB_TEXT, scale);
		y += 16 * scale + 8 * p;
		hub_text(lx, y, "TARGET", HUB_MAGENTA, scale);
		snprintf(buf, sizeof(buf), "%s",
			 U.inst_target[0] ? U.inst_target : "");
		hub_text_trunc(lx + hub_text_w("TARGET  ", scale), y, buf,
			       HUB_TEXT, scale,
			       m.x + m.w - 48 * p -
			       (lx + hub_text_w("TARGET  ", scale)));
		if (m.w > 240 * p)
			hub_floppy(m.x + m.w - 22 * p, m.y + m.h - 22 * p,
				   rs);
		{
			int bw = 120 * p;
			int bh = 28 * p;

			if (bw > m.w / 2)
				bw = m.w / 2;
			b.w = bw;
			b.h = bh;
			b.x = m.x + (m.w - bw) / 2;
			b.y = m.y + m.h - bh - 16 * p;
			hub_action_button(b, "CANCEL", 0xA0A8B8, 1);
			s_modal_cancel = b;
			s_has_cancel = 1;
		}
	}
	else
	{
		snprintf(buf, sizeof(buf), "%s",
			 U.errmsg[0] ? U.errmsg : "INSTALL FAILED");
		hub_text_trunc(m.x + 16 * p, y, buf, HUB_RED, scale,
			       m.w - 32 * p);
		{
			int bw = 70 * p;
			int bh = 26 * p;

			b.w = bw;
			b.h = bh;
			b.x = m.x + (m.w - bw) / 2;
			b.y = m.y + m.h - bh - 16 * p;
			hub_action_button(b, "OK", 0x6A7088, 1);
			s_modal_ok = b;
			s_has_ok = 1;
		}
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
