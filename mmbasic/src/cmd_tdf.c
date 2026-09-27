#include "mmb_priv.h"

#include "mmb_tdf.h"

#include <string.h>

/* Native TheDraw (.TDF) font commands (#866, #867).
 *
 * Replaces ramdisk/lib/TDF.BAS: the whole file is read into memory once and
 * parsed with the shared mmb_tdf decoder, and a banner is stamped cell by cell
 * into a single console write instead of one LOCATE/PRINT per glyph cell.
 *
 * A small pool of font instances is kept so a program can draw from more than
 * one font at once. `slot` is the pool index (0-based); an omitted slot uses
 * the active font, and an omitted slot on LOAD/NAMED defaults to slot 0 and
 * replaces whatever it held (CMM2 behaviour, #884).
 */

#define TDF_MAX_FONTS 4

typedef struct {
	mmb_tdf font;
	unsigned char *buf;
	unsigned size;
	int open;
	int variants;
	int variant;
} tdf_slot;

static tdf_slot g_tdf[TDF_MAX_FONTS];
static int g_tdf_active;

/* Console output for one stamp, flushed in a few writes. A cell sequence is at
 * most reset + fg + bg + cursor + one byte (~40), so a small buffer keeps the
 * present count low without a large BSS array. */
#define TDF_OUT_CAP 4096
static char s_tdf_out[TDF_OUT_CAP];

typedef struct {
	int colour;
	int have_colour;
	int last_fg, last_bg;
	int exp_x, exp_y, have_exp;
	int cols, rows;
	char *buf;
	int len;
	/* When set, stamp decoded cells into the active graphics write target
	 * with mmb_gfx_glyph_cell instead of emitting console ANSI. */
	int gfx;
	int box_x0, box_y0, box_x1, box_y1;
} tdf_render;

static int tdf_slot_open(int s)
{
	return s >= 0 && s < TDF_MAX_FONTS && g_tdf[s].open;
}

static void tdf_clear_slot(int s)
{
	if (s < 0 || s >= TDF_MAX_FONTS)
		return;
	if (g_tdf[s].buf && G.plat && G.plat->free)
		G.plat->free(g_tdf[s].buf);
	memset(&g_tdf[s], 0, sizeof(g_tdf[s]));
}

static int tdf_name_eq(const char *a, const char *b)
{
	while (*a && *b)
	{
		char ca = *a++, cb = *b++;
		if (ca >= 'a' && ca <= 'z')
			ca = (char)(ca - 32);
		if (cb >= 'a' && cb <= 'z')
			cb = (char)(cb - 32);
		if (ca != cb)
			return 0;
	}
	return *a == 0 && *b == 0;
}

/* Read path$ fully, select variant `index` (or the first record named
 * `want_name` when non-NULL) and install it into `slot`. Errors longjmp. */
static void tdf_load_file(const char *path, int index, const char *want_name,
			  int slot)
{
	mmb_tdf f;
	unsigned char *buf;
	unsigned got = 0;
	int sz, variants, i;

	sz = mmb_vfs_size(path);
	if (sz < 20)
		mmb_error("TDF: file not found or too small");
	if (!G.plat || !G.plat->alloc || !G.plat->free)
		mmb_error("TDF: no allocator");
	buf = (unsigned char *)G.plat->alloc((unsigned)sz + 1);
	if (!buf)
		mmb_error("TDF: out of memory");
	if (mmb_vfs_read(path, buf, (unsigned)sz, &got) != 0)
	{
		G.plat->free(buf);
		mmb_error("TDF: cannot read file");
	}
	buf[got] = 0;

	variants = mmb_tdf_count(buf, got);
	if (want_name)
	{
		int found = -1;
		for (i = 0; i < variants; i++)
		{
			mmb_tdf tmp;
			if (mmb_tdf_parse(buf, got, i, &tmp) != 0)
				continue;
			if (tdf_name_eq(tmp.name, want_name))
			{
				found = i;
				break;
			}
		}
		if (found < 0)
		{
			G.plat->free(buf);
			mmb_error("TDF: font not found");
		}
		index = found;
	}
	if (index < 0 || (variants > 0 && index >= variants))
	{
		G.plat->free(buf);
		mmb_error("TDF: no such variant");
	}
	if (mmb_tdf_parse(buf, got, index, &f) != 0)
	{
		G.plat->free(buf);
		mmb_error("TDF: not a TheDraw font");
	}

	tdf_clear_slot(slot);
	g_tdf[slot].font = f;
	g_tdf[slot].buf = buf;
	g_tdf[slot].size = got;
	g_tdf[slot].open = 1;
	g_tdf[slot].variants = variants > 0 ? variants : 1;
	g_tdf[slot].variant = index;
}

/* Copy an expression as a filesystem path. The string arena can be reused by a
 * later expression, so the copy must happen before the next mmb_expr(). */
static void tdf_take_path(char *dst, int n)
{
	mmb_val v;

	mmb_skip_sp();
	if (*G.p == 0 || *G.p == ':' || *G.p == '\'')
		mmb_syntax();
	v = mmb_expr();
	if (v.type != T_STR)
		mmb_syntax();
	if (n > 0)
	{
		strncpy(dst, v.s ? v.s : "", (size_t)n - 1);
		dst[n - 1] = 0;
	}
}

static void tdf_cmd_load(void)
{
	char path[128];
	int variant = -1, slot = -1;

	tdf_take_path(path, (int)sizeof(path));
	mmb_skip_sp();
	if (*G.p == ',')
	{
		G.p++;
		variant = (int)mmb_as_int(mmb_expr());
		mmb_skip_sp();
		if (*G.p == ',')
		{
			G.p++;
			slot = (int)mmb_as_int(mmb_expr());
		}
	}
	if (variant < 0)
		variant = 0;
	if (slot < 0)
		slot = 0;
	if (slot >= TDF_MAX_FONTS)
		mmb_error("TDF: bad slot");
	tdf_load_file(path, variant, 0, slot);
	g_tdf_active = slot;
}

static void tdf_cmd_named(void)
{
	char path[128];
	char name[MMB_MAX_NAME];
	mmb_val v;
	int slot = -1;

	tdf_take_path(path, (int)sizeof(path));
	mmb_skip_sp();
	if (*G.p != ',')
		mmb_syntax();
	G.p++;
	v = mmb_expr();
	if (v.type != T_STR)
		mmb_syntax();
	strncpy(name, v.s ? v.s : "", sizeof(name) - 1);
	name[sizeof(name) - 1] = 0;
	mmb_skip_sp();
	if (*G.p == ',')
	{
		G.p++;
		slot = (int)mmb_as_int(mmb_expr());
	}
	if (slot < 0)
		slot = 0;
	if (slot >= TDF_MAX_FONTS)
		mmb_error("TDF: bad slot");
	tdf_load_file(path, 0, name, slot);
	g_tdf_active = slot;
}

static void tdf_cmd_use(void)
{
	int slot;
	mmb_skip_sp();
	if (*G.p == 0 || *G.p == ':' || *G.p == '\'' || *G.p == ',')
		mmb_syntax();
	slot = (int)mmb_as_int(mmb_expr());
	if (!tdf_slot_open(slot))
		mmb_error("TDF: no font loaded");
	g_tdf_active = slot;
}

static void tdf_cmd_close(void)
{
	int slot = -1;

	mmb_skip_sp();
	if (*G.p != 0 && *G.p != ':' && *G.p != '\'' && *G.p != ',')
		slot = (int)mmb_as_int(mmb_expr());
	if (slot < 0)
		slot = g_tdf_active;
	if (slot >= 0 && slot < TDF_MAX_FONTS)
		tdf_clear_slot(slot);
	if (slot == g_tdf_active)
	{
		int i, next = 0;
		for (i = 0; i < TDF_MAX_FONTS; i++)
			if (g_tdf[i].open)
			{
				next = i;
				break;
			}
		g_tdf_active = next;
	}
}

/* ---- batched rendering --------------------------------------------------- */

static void tdf_flush(tdf_render *r)
{
	if (r->len <= 0)
		return;
	r->buf[r->len] = 0;
	mmb_console_write(r->buf);
	r->len = 0;
}

static void tdf_putc(tdf_render *r, char c)
{
	r->buf[r->len++] = c;
}

static void tdf_puts(tdf_render *r, const char *s)
{
	while (*s)
		tdf_putc(r, *s++);
}

static void tdf_put_int(tdf_render *r, int v)
{
	char tmp[8];
	int i = 0;
	if (v <= 0)
	{
		tdf_putc(r, '0');
		return;
	}
	while (v > 0 && i < 8)
	{
		tmp[i++] = (char)('0' + v % 10);
		v /= 10;
	}
	while (i > 0)
		tdf_putc(r, tmp[--i]);
}

static void tdf_room(tdf_render *r, int need)
{
	if (r->len + need >= TDF_OUT_CAP)
		tdf_flush(r);
}

static void tdf_render_cell(void *ctx, int x, int y, int ch, int fg, int bg)
{
	tdf_render *r = (tdf_render *)ctx;

	if (!ch)
		return;
	if (x < 0 || y < 0 || x >= r->cols || y >= r->rows)
		return;
	if (r->gfx)
	{
		/* Page target: cell blit at the matching 8x16 pixel cell. Colour
		 * fonts carry per-cell fg/bg; the others use the current graphics
		 * pen/paper, as the console path leaves the terminal colour in
		 * place. */
		unsigned c_fg = r->colour ? mmb_ibm_colour(fg & 15) : G.gfx.fg;
		unsigned c_bg = r->colour ? mmb_ibm_colour(bg & 7) : G.gfx.bg;
		mmb_gfx_glyph_cell(x * mmb_print_font_w(),
				   y * mmb_print_font_h(), (unsigned)ch, c_fg,
				   c_bg);
		if (r->box_x1 < 0)
		{
			r->box_x0 = r->box_x1 = x;
			r->box_y0 = r->box_y1 = y;
		}
		else
		{
			if (x < r->box_x0)
				r->box_x0 = x;
			if (y < r->box_y0)
				r->box_y0 = y;
			if (x > r->box_x1)
				r->box_x1 = x;
			if (y > r->box_y1)
				r->box_y1 = y;
		}
		r->last_fg = fg;
		r->last_bg = bg;
		r->have_colour = 1;
		return;
	}
	tdf_room(r, 48);
	if (r->colour && (!r->have_colour || fg != r->last_fg || bg != r->last_bg))
	{
		/* Circle only honours single-parameter SGR, so reset then fg then
		 * bg; the VGA nibbles map through the standard IBM palette. */
		tdf_puts(r, "\x1b[0m\x1b[");
		tdf_put_int(r, mmb_console_ansi_code(mmb_ibm_colour(fg & 15), 1));
		tdf_puts(r, "m\x1b[");
		tdf_put_int(r, mmb_console_ansi_code(mmb_ibm_colour(bg & 7), 0));
		tdf_puts(r, "m");
		r->last_fg = fg;
		r->last_bg = bg;
		r->have_colour = 1;
	}
	if (!r->have_exp || x != r->exp_x || y != r->exp_y)
	{
		tdf_puts(r, "\x1b[");
		tdf_put_int(r, y + 1);
		tdf_putc(r, ';');
		tdf_put_int(r, x + 1);
		tdf_putc(r, 'H');
	}
	tdf_putc(r, (char)ch);
	r->exp_x = x + 1;
	r->exp_y = y;
	r->have_exp = 1;
}

/* Columns TDF PRINT would advance for s$ using `f`, without drawing. */
static int tdf_string_width(const mmb_tdf *f, const char *s)
{
	int i, w = 0;
	int gap = f->spacing > 1 ? f->spacing : 1;

	for (i = 0; s[i]; i++)
	{
		int c = (unsigned char)s[i];
		int idx = (c >= 33 && c <= 126) ? c - 33 : -1;
		if (idx >= 0 && f->off[idx] != 0xFFFF)
			w += mmb_tdf_stamp(f, c, 0, 0, 0, 0, 0, 0);
		else
			w += gap;
	}
	return w;
}

/* True when TDF PRINT must draw through the graphics layer instead of the
 * console: a raw framebuffer write, or a soft page that is not the page
 * currently on screen. The ANSI path can only paint the live console, so an
 * offscreen write there would leak onto the visible frame (#883). */
static int tdf_gfx_target(void)
{
	if (mmb_gfx_writing_fb())
		return 1;
	return G.gfx.pages > 0 && G.gfx.write_page != G.gfx.display_page;
}

static void tdf_cmd_print(void)
{
	char *text;
	mmb_val v;
	const mmb_tdf *f;
	tdf_render r;
	int x, y, slot = -1, i, cx, tlen;

	mmb_skip_sp();
	if (*G.p == 0 || *G.p == ':' || *G.p == '\'')
		mmb_syntax();
	x = (int)mmb_as_int(mmb_expr());
	mmb_skip_sp();
	if (*G.p != ',')
		mmb_syntax();
	G.p++;
	y = (int)mmb_as_int(mmb_expr());
	mmb_skip_sp();
	if (*G.p != ',')
		mmb_syntax();
	G.p++;
	v = mmb_expr();
	if (v.type != T_STR)
		mmb_syntax();
	tlen = v.s ? (int)strlen(v.s) : 0;
	if (!G.plat || !G.plat->alloc)
		mmb_error("TDF: no allocator");
	text = (char *)G.plat->alloc((unsigned)tlen + 1);
	if (!text)
		mmb_error("TDF: out of memory");
	memcpy(text, v.s ? v.s : "", (size_t)tlen + 1);
	mmb_skip_sp();
	if (*G.p == ',')
	{
		G.p++;
		slot = (int)mmb_as_int(mmb_expr());
	}
	if (slot < 0)
		slot = g_tdf_active;
	if (!tdf_slot_open(slot))
	{
		G.plat->free(text);
		mmb_error("TDF: no font loaded");
	}

	f = &g_tdf[slot].font;
	r.colour = (f->type == MMB_TDF_COLOR);
	r.have_colour = 0;
	r.last_fg = -1;
	r.last_bg = -1;
	r.exp_x = 0;
	r.exp_y = 0;
	r.have_exp = 0;
	r.gfx = tdf_gfx_target();
	r.box_x0 = r.box_y0 = r.cols = r.rows = 0;
	r.box_x1 = r.box_y1 = -1;
	if (r.gfx)
	{
		int pw = mmb_gfx_writing_fb() ? G.gfx.fb_w : G.gfx.w;
		int ph = mmb_gfx_writing_fb() ? G.gfx.fb_h : G.gfx.h;
		r.cols = pw / mmb_print_font_w();
		r.rows = ph / mmb_print_font_h();
	}
	else
	{
		r.cols = (G.plat && G.plat->video_cols) ? G.plat->video_cols() : 80;
		r.rows = (G.plat && G.plat->video_rows) ? G.plat->video_rows() : 25;
	}
	if (r.cols < 1)
		r.cols = 80;
	if (r.rows < 1)
		r.rows = 25;
	r.buf = s_tdf_out;
	r.len = 0;

	cx = x;
	for (i = 0; i < tlen; i++)
	{
		int c = (unsigned char)text[i];
		int idx = (c >= 33 && c <= 126) ? c - 33 : -1;
		if (idx >= 0 && f->off[idx] != 0xFFFF)
			cx += mmb_tdf_stamp(f, c, cx, y, 0, 0, tdf_render_cell, &r);
		else
			cx += f->spacing > 1 ? f->spacing : 1;
	}

	if (r.gfx)
	{
		/* Page target: flush the stamped AABB so an overlay write lands on
		 * the displayed page; a hidden page or the raw framebuffer needs no
		 * present. */
		if (r.box_x1 >= 0 && !mmb_gfx_writing_fb())
		{
			mmb_gfx_dirty_add(r.box_x0 * mmb_print_font_w(),
					  r.box_y0 * mmb_print_font_h(),
					  (r.box_x1 - r.box_x0 + 1) *
						  mmb_print_font_w(),
					  (r.box_y1 - r.box_y0 + 1) *
						  mmb_print_font_h());
			mmb_gfx_present_if(MMB_PAGE_CUR);
		}
	}
	else
	{
		/* Leave the terminal and the tracked cursor at the end column, and
		 * the console colour where the last colour cell left it (as TDF.BAS
		 * did). */
		tdf_room(&r, 24);
		tdf_puts(&r, "\x1b[");
		tdf_put_int(&r, y + 1);
		tdf_putc(&r, ';');
		tdf_put_int(&r, cx + 1);
		tdf_putc(&r, 'H');
		tdf_flush(&r);
	}
	if (r.colour && r.have_colour)
	{
		G.gfx.fg = mmb_ibm_colour(r.last_fg & 15);
		G.gfx.bg = mmb_ibm_colour(r.last_bg & 7);
	}
	mmb_print_cursor_goto(cx * mmb_print_font_w(), y * mmb_print_font_h());
	G.plat->free(text);
}

void mmb_cmd_tdf(void)
{
	if (mmb_match("LOAD"))
		tdf_cmd_load();
	else if (mmb_match("NAMED"))
		tdf_cmd_named();
	else if (mmb_match("USE"))
		tdf_cmd_use();
	else if (mmb_match("PRINT"))
		tdf_cmd_print();
	else if (mmb_match("CLOSE"))
		tdf_cmd_close();
	else
		mmb_syntax();
}

/* ---- dotted functions ---------------------------------------------------- */

/* Parse an optional argument list; `slot` is resolved to the active slot when
 * the caller omits it. */
static int tdf_fn_args(mmb_val *a, int maxn, int *n)
{
	*n = 0;
	mmb_skip_sp();
	if (*G.p == '(')
	{
		G.p++;
		mmb_skip_sp();
		if (*G.p != ')')
		{
			for (;;)
			{
				if (*n >= maxn)
					mmb_syntax();
				a[(*n)++] = mmb_expr();
				mmb_skip_sp();
				if (*G.p == ',')
				{
					G.p++;
					continue;
				}
				break;
			}
		}
		mmb_expect(')');
	}
	return *n;
}

static const mmb_tdf *tdf_fn_font(int slot)
{
	if (!tdf_slot_open(slot))
		mmb_error("TDF: no font loaded");
	return &g_tdf[slot].font;
}

void mmb_tdf_fn_width(mmb_val *out)
{
	mmb_val a[2];
	int n, slot;
	tdf_fn_args(a, 2, &n);
	if (n < 1 || a[0].type != T_STR)
		mmb_syntax();
	slot = n >= 2 ? (int)mmb_as_int(a[1]) : g_tdf_active;
	*out = mmb_int_val(tdf_string_width(tdf_fn_font(slot), a[0].s));
}

void mmb_tdf_fn_name(mmb_val *out)
{
	mmb_val a[2];
	int n, slot;
	tdf_fn_args(a, 2, &n);
	slot = n >= 1 ? (int)mmb_as_int(a[0]) : g_tdf_active;
	*out = mmb_str_val(tdf_fn_font(slot)->name);
}

void mmb_tdf_fn_type(mmb_val *out)
{
	mmb_val a[2];
	int n, slot;
	tdf_fn_args(a, 2, &n);
	slot = n >= 1 ? (int)mmb_as_int(a[0]) : g_tdf_active;
	*out = mmb_int_val(tdf_fn_font(slot)->type);
}

void mmb_tdf_fn_spacing(mmb_val *out)
{
	mmb_val a[2];
	int n, slot;
	tdf_fn_args(a, 2, &n);
	slot = n >= 1 ? (int)mmb_as_int(a[0]) : g_tdf_active;
	*out = mmb_int_val(tdf_fn_font(slot)->spacing);
}

void mmb_tdf_fn_height(mmb_val *out)
{
	mmb_val a[2];
	int n, slot;
	tdf_fn_args(a, 2, &n);
	slot = n >= 1 ? (int)mmb_as_int(a[0]) : g_tdf_active;
	*out = mmb_int_val(tdf_fn_font(slot)->max_height);
}

void mmb_tdf_fn_variants(mmb_val *out)
{
	mmb_val a[2];
	int n, slot;
	tdf_fn_args(a, 2, &n);
	slot = n >= 1 ? (int)mmb_as_int(a[0]) : g_tdf_active;
	tdf_fn_font(slot);
	*out = mmb_int_val(g_tdf[slot].variants);
}

void mmb_tdf_fn_variant(mmb_val *out)
{
	mmb_val a[2];
	int n, slot;
	tdf_fn_args(a, 2, &n);
	slot = n >= 1 ? (int)mmb_as_int(a[0]) : g_tdf_active;
	tdf_fn_font(slot);
	*out = mmb_int_val(g_tdf[slot].variant);
}

void mmb_tdf_fn_variantname(mmb_val *out)
{
	mmb_val a[3];
	int n, slot, index;
	mmb_tdf tmp;
	const mmb_tdf *f;

	tdf_fn_args(a, 3, &n);
	if (n < 1)
		mmb_syntax();
	index = (int)mmb_as_int(a[0]);
	slot = n >= 2 ? (int)mmb_as_int(a[1]) : g_tdf_active;
	f = tdf_fn_font(slot);
	if (index < 0 || index >= g_tdf[slot].variants ||
	    mmb_tdf_parse(g_tdf[slot].buf, g_tdf[slot].size, index, &tmp) != 0)
	{
		(void)f;
		mmb_error("TDF: no such variant");
	}
	*out = mmb_str_val(tmp.name);
}
