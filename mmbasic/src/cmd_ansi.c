#include "mmb_priv.h"
#include "tui.h"

/* TheDraw-class ANSI art editor.
 *
 * A CP437 80x25 canvas of (char, fg, bg) cells. The full-screen editor lets
 * you draw with printable characters and CP437 paint glyphs, cycle the 16
 * VGA colours, draw lines and flood-fill, stamp text using a TheDraw .TDF
 * font, and load/save standard .ANS art (SAUCE is tolerated on load and can
 * be appended with ANSI SAUCE).
 */

#define AN_COLS 80
#define AN_ROWS 25

#define AN_ESC_NONE 0
#define AN_ESC_GOT  1
#define AN_ESC_CSI  2
#define AN_ESC_SS3  3
#define AN_ESC_IDLE_MS 60

#define AN_GLYPH_MAX_W 40
#define AN_GLYPH_MAX_H 16

/* TheDraw font types. */
#define AN_TDF_OUTLINE 0
#define AN_TDF_BLOCK   1
#define AN_TDF_COLOR   2

typedef struct {
	unsigned char ch;
	unsigned char fg;
	unsigned char bg;
} an_cell;

typedef struct {
	char name[24];
	int type;
	int spacing;
	int glyph_count;
	const unsigned char *buf;
	const unsigned char *data;
	unsigned data_len;
	unsigned short off[94];
} an_tdf;

typedef struct {
	int active;
	char path[160];
	char label[96];
	int cx, cy;
	int fg, bg;
	int paint;      /* index into AN_PAINT */
	int text_mode;  /* stamp TheDraw glyphs instead of cells */
	int line_mode;
	int anchor_x, anchor_y, have_anchor;
	int dirty;
	int esc_state, esc_at, alt_pend;
	char status[96];
	char font_label[96];
	int have_font;
	an_tdf tdf;
	unsigned char *font_buf;
} an_state;

static an_cell AN[AN_ROWS][AN_COLS];
static an_state A;

static const unsigned char AN_PAINT[] = {
	0xDB, 0xB2, 0xB1, 0xB0, 0x20, 0xDC, 0xDF, 0xFE, 0x0F, 0x04, 0x03
};
#define AN_PAINT_N ((int)(sizeof(AN_PAINT) / sizeof(AN_PAINT[0])))

/* ------------------------------------------------------------------ */
/* TheDraw .TDF parsing                                                */
/* ------------------------------------------------------------------ */

static int an_tdf_parse(const unsigned char *buf, unsigned n, int index, an_tdf *f)
{
	unsigned pos = 20;
	int i, j;

	if (n < 233 || buf[0] != 0x13)
		return -1;
	if (memcmp(buf + 1, "TheDraw FONTS file", 18) != 0 || buf[19] != 0x1A)
		return -1;

	for (i = 0;; i++)
	{
		unsigned nl, bs, ds;
		if (pos + 45 > n)
			return -1;
		if (memcmp(buf + pos, "\x55\xaa\x00\xff", 4) != 0)
			return -1;
		nl = buf[pos + 4];
		if (nl > 12)
			nl = 12;
		bs = (unsigned)buf[pos + 23] | ((unsigned)buf[pos + 24] << 8);
		ds = (i == 0) ? 233u : pos + 213u;
		if (ds + bs > n)
			bs = (ds < n) ? n - ds : 0;

		if (i == index)
		{
			memset(f, 0, sizeof(*f));
			memcpy(f->name, buf + pos + 5, nl);
			f->name[nl] = 0;
			f->type = buf[pos + 21];
			f->spacing = buf[pos + 22];
			for (j = 0; j < 94; j++)
			{
				f->off[j] = (unsigned short)(buf[pos + 25 + 2 * j] |
					((unsigned)buf[pos + 26 + 2 * j] << 8));
				if (f->off[j] != 0xFFFF)
					f->glyph_count++;
			}
			f->buf = buf;
			f->data = buf + ds;
			f->data_len = bs;
			return 0;
		}

		/* First-font data is at 233; later headers sit after the
		 * previous block. The block size includes the terminating
		 * NUL, so the next marker is at ds + bs (accept either the
		 * spec's extra +1 or not). */
		if (i == 0)
			pos = 233 + bs;
		else
			pos = ds + bs;
		if (pos + 4 <= n && memcmp(buf + pos, "\x55\xaa\x00\xff", 4) != 0)
			pos++;
	}
}

/* Map an Outline-font letter to its CP437 box-drawing code. */
static int an_outline_map(int c)
{
	switch (c)
	{
	case 'A': return 205;
	case 'B': return 196;
	case 'C': return 179;
	case 'D': return 186;
	case 'E': return 213;
	case 'F': return 187;
	case 'G': return 214;
	case 'H': return 191;
	case 'I': return 200;
	case 'J': return 190;
	case 'K': return 192;
	case 'L': return 189;
	case 'M': return 181;
	case 'N': return 199;
	case 'O': return 247;
	default: return -1;
	}
}

/* Render one glyph into the canvas at (x, y). Returns the advance in cells
 * (glyph width + letter spacing; spacing alone when undefined). */
static int an_tdf_stamp(const an_tdf *f, int code, int x, int y, int fg, int bg)
{
	unsigned o, p;
	int maxw, maxh, row, col, adv;

	if (code == 32)
		return 1 + f->spacing;
	if (code < 33 || code > 126)
		return 0;
	o = f->off[code - 33];
	if (o == 0xFFFF)
		return 1 + f->spacing;
	if ((unsigned)o + 2 > f->data_len)
		return 1 + f->spacing;
	p = o;
	maxw = f->data[p];
	maxh = f->data[p + 1];
	p += 2;
	if (maxw < 1)
		maxw = 1;
	if (maxh < 1 || maxh > 16)
		maxh = 1;
	adv = maxw + f->spacing;
	row = 0;
	col = 0;
	while (p < f->data_len)
	{
		int c = f->data[p];
		int cf = fg, cb = bg;
		if (c == 0)
			break;
		p++;
		if (c == 13)
		{
			row++;
			col = 0;
			if (row >= maxh)
				break;
			continue;
		}
		if (f->type == AN_TDF_COLOR)
		{
			int attr;
			if (p >= f->data_len)
				break;
			attr = f->data[p++];
			cb = (attr >> 4) & 7;
			cf = attr & 15;
		}
		else if (f->type == AN_TDF_OUTLINE)
		{
			int m = an_outline_map(c);
			if (m < 0)
			{
				if (c == '@' || c == '&')
				{
					col++;
					continue;
				}
				col++;
				continue;
			}
			c = m;
		}
		if (c == '@')
		{
			col++;
			continue;
		}
		if (c == '&')
		{
			/* Descender marker: skip without drawing. */
			col++;
			continue;
		}
		if (x + col >= 0 && x + col < AN_COLS && y + row >= 0 && y + row < AN_ROWS)
		{
			an_cell *cell = &AN[y + row][x + col];
			cell->ch = (unsigned char)c;
			cell->fg = (unsigned char)cf;
			cell->bg = (unsigned char)cb;
		}
		col++;
	}
	return adv;
}

static void an_tdf_free(an_state *st)
{
	if (st->font_buf)
	{
		if (G.plat && G.plat->free)
			G.plat->free(st->font_buf);
		st->font_buf = 0;
	}
	st->have_font = 0;
	memset(&st->tdf, 0, sizeof(st->tdf));
}

static int an_tdf_load(an_state *st, const char *path, const char *name)
{
	unsigned char *buf;
	int sz, index, i;
	unsigned got = 0;
	int count = 0;

	sz = mmb_vfs_size(path);
	if (sz < 233)
		return -1;
	buf = G.plat->alloc((unsigned)sz + 1);
	if (!buf)
		return -1;
	if (mmb_vfs_read(path, buf, (unsigned)sz, &got) != 0)
	{
		G.plat->free(buf);
		return -1;
	}
	/* Count fonts so a name can select one. */
	{
		an_tdf probe;
		for (i = 0; an_tdf_parse(buf, got, i, &probe) == 0; i++)
			count++;
	}
	if (count < 1)
	{
		G.plat->free(buf);
		return -1;
	}
	index = 0;
	if (name && name[0])
	{
		an_tdf probe;
		index = -1;
		for (i = 0; i < count; i++)
			if (an_tdf_parse(buf, got, i, &probe) == 0 &&
			    mmb_keyword_eq(probe.name, name))
			{
				index = i;
				break;
			}
		if (index < 0)
			index = 0;
	}
	an_tdf_free(st);
	if (an_tdf_parse(buf, got, index, &st->tdf) != 0)
	{
		G.plat->free(buf);
		return -1;
	}
	st->font_buf = buf;
	st->have_font = 1;
	return 0;
}

/* Pick a font by name, or the first font when name is empty/not found. */
static int an_tdf_find(const unsigned char *buf, unsigned n, const char *name,
		       an_tdf *f)
{
	an_tdf probe;
	int i;

	if (name && name[0])
	{
		for (i = 0; an_tdf_parse(buf, n, i, &probe) == 0; i++)
			if (mmb_keyword_eq(probe.name, name))
				return an_tdf_parse(buf, n, i, f);
	}
	return an_tdf_parse(buf, n, 0, f);
}

/* ------------------------------------------------------------------ */
/* Canvas helpers                                                      */
/* ------------------------------------------------------------------ */

static void an_clear_canvas(void)
{
	int x, y;
	for (y = 0; y < AN_ROWS; y++)
		for (x = 0; x < AN_COLS; x++)
		{
			AN[y][x].ch = 0;
			AN[y][x].fg = 7;
			AN[y][x].bg = 0;
		}
}

static void an_put(int x, int y, int ch, int fg, int bg)
{
	if (x < 0 || x >= AN_COLS || y < 0 || y >= AN_ROWS)
		return;
	AN[y][x].ch = (unsigned char)ch;
	AN[y][x].fg = (unsigned char)fg;
	AN[y][x].bg = (unsigned char)bg;
}

static void an_line(an_state *st, int x0, int y0, int x1, int y1)
{
	int dx = (x1 - x0) < 0 ? x0 - x1 : x1 - x0;
	int ady = (y1 - y0) < 0 ? y0 - y1 : y1 - y0;
	int sx = x0 < x1 ? 1 : -1;
	int sy = y0 < y1 ? 1 : -1;
	int dy = -ady;
	int err = dx + dy;
	int ch = st->text_mode ? ' ' : AN_PAINT[st->paint];

	for (;;)
	{
		an_put(x0, y0, ch, st->fg, st->bg);
		if (x0 == x1 && y0 == y1)
			break;
		{
			int e2 = 2 * err;
			if (e2 >= dy)
			{
				err += dy;
				x0 += sx;
			}
			if (e2 <= dx)
			{
				err += dx;
				y0 += sy;
			}
		}
	}
	st->dirty = 1;
}

static void an_flood(an_state *st, int x, int y)
{
	static int stack[AN_COLS * AN_ROWS];
	int sp = 0;
	unsigned char tch, tfg, tbg;
	int want = st->text_mode ? ' ' : AN_PAINT[st->paint];

	if (x < 0 || x >= AN_COLS || y < 0 || y >= AN_ROWS)
		return;
	tch = AN[y][x].ch;
	tfg = AN[y][x].fg;
	tbg = AN[y][x].bg;
	if (tch == (unsigned char)want && tfg == (unsigned char)st->fg &&
	    tbg == (unsigned char)st->bg)
		return;
	stack[sp++] = y * AN_COLS + x;
	while (sp > 0)
	{
		int c = stack[--sp];
		int cy = c / AN_COLS, cx = c % AN_COLS;
		if (cy < 0 || cy >= AN_ROWS || cx < 0 || cx >= AN_COLS)
			continue;
		if (AN[cy][cx].ch != tch || AN[cy][cx].fg != tfg ||
		    AN[cy][cx].bg != tbg)
			continue;
		AN[cy][cx].ch = (unsigned char)want;
		AN[cy][cx].fg = (unsigned char)st->fg;
		AN[cy][cx].bg = (unsigned char)st->bg;
		if (sp + 4 < AN_COLS * AN_ROWS)
		{
			stack[sp++] = c - 1;
			stack[sp++] = c + 1;
			stack[sp++] = c - AN_COLS;
			stack[sp++] = c + AN_COLS;
		}
	}
	st->dirty = 1;
}

/* ------------------------------------------------------------------ */
/* .ANS load / save                                                    */
/* ------------------------------------------------------------------ */

typedef struct {
	int state;
	int narg, args[8], priv;
	int row, col, fg, bg, bold, inv;
	int sav_row, sav_col;
} an_parser;

static int an_nearest16(int r, int g, int b)
{
	static const int vga[16][3] = {
		{ 0, 0, 0 }, { 170, 0, 0 }, { 0, 170, 0 }, { 170, 85, 0 },
		{ 0, 0, 170 }, { 170, 0, 170 }, { 0, 170, 170 }, { 170, 170, 170 },
		{ 85, 85, 85 }, { 255, 85, 85 }, { 85, 255, 85 }, { 255, 255, 85 },
		{ 85, 85, 255 }, { 255, 85, 255 }, { 85, 255, 255 }, { 255, 255, 255 }
	};
	int i, best = 0;
	long best_d = -1;
	for (i = 0; i < 16; i++)
	{
		long dr = r - vga[i][0], dg = g - vga[i][1], db = b - vga[i][2];
		long d = dr * dr + dg * dg + db * db;
		if (best_d < 0 || d < best_d)
		{
			best_d = d;
			best = i;
		}
	}
	return best;
}

static int an_256(int n)
{
	int r, g, b, v;
	if (n < 0)
		n = 0;
	if (n < 16)
		return n;
	if (n < 232)
	{
		n -= 16;
		b = n % 6;
		g = (n / 6) % 6;
		r = n / 36;
		r = r ? r * 40 + 55 : 0;
		g = g ? g * 40 + 55 : 0;
		b = b ? b * 40 + 55 : 0;
		return an_nearest16(r, g, b);
	}
	v = 8 + (n - 232) * 10;
	if (v > 255)
		v = 255;
	return an_nearest16(v, v, v);
}

static void an_parser_put(an_parser *p, int ch)
{
	if (p->row < 0 || p->row >= AN_ROWS)
		return;
	if (p->col < 0)
		p->col = 0;
	if (p->col >= AN_COLS)
	{
		p->col = 0;
		p->row++;
		if (p->row >= AN_ROWS)
			return;
	}
	{
		int fg = p->fg, bg = p->bg;
		if (p->bold && fg < 8)
			fg += 8;
		if (p->inv)
		{
			int t = fg;
			fg = bg;
			bg = t;
		}
		an_put(p->col, p->row, ch, fg & 15, bg & 7);
	}
	p->col++;
}

static void an_parser_sgr(an_parser *p)
{
	int i, n = p->narg > 0 ? p->narg : 1;
	if (p->narg == 0)
		p->args[0] = 0;
	for (i = 0; i < n; i++)
	{
		int v = p->args[i];
		if (v == 0)
		{
			p->fg = 7;
			p->bg = 0;
			p->bold = 0;
			p->inv = 0;
		}
		else if (v == 1)
			p->bold = 1;
		else if (v == 22)
			p->bold = 0;
		else if (v == 7)
			p->inv = 1;
		else if (v == 27)
			p->inv = 0;
		else if (v >= 30 && v <= 37)
			p->fg = v - 30;
		else if (v >= 90 && v <= 97)
			p->fg = v - 90 + 8;
		else if (v >= 40 && v <= 47)
			p->bg = v - 40;
		else if (v >= 100 && v <= 107)
			p->bg = v - 100 + 8;
		else if (v == 39)
			p->fg = 7;
		else if (v == 49)
			p->bg = 0;
		else if ((v == 38 || v == 48) && i + 2 < n && p->args[i + 1] == 5)
		{
			int c = an_256(p->args[i + 2]);
			if (v == 38)
				p->fg = c;
			else
				p->bg = c;
			i += 2;
		}
	}
}

static int an_arg(an_parser *p, int i, int dflt)
{
	if (i < p->narg && p->args[i] > 0)
		return p->args[i];
	return dflt;
}

static void an_parser_csi(an_parser *p, int cmd)
{
	int n = an_arg(p, 0, 1);
	if (cmd == 'A')
		p->row -= n;
	else if (cmd == 'B')
		p->row += n;
	else if (cmd == 'C')
		p->col += n;
	else if (cmd == 'D')
		p->col -= n;
	else if (cmd == 'G')
		p->col = n - 1;
	else if (cmd == 'd')
		p->row = n - 1;
	else if (cmd == 'H' || cmd == 'f')
	{
		p->row = an_arg(p, 0, 1) - 1;
		p->col = an_arg(p, 1, 1) - 1;
	}
	else if (cmd == 'J')
	{
		int mode = p->narg ? p->args[0] : 0;
		int r, c, r0 = 0, r1 = AN_ROWS, c0 = 0, c1 = AN_COLS;
		if (mode == 0)
		{
			r0 = p->row;
			c0 = p->col;
		}
		else if (mode == 1)
		{
			r1 = p->row + 1;
			c1 = p->col + 1;
		}
		if (mode == 2 || mode == 3)
		{
			r0 = 0;
			r1 = AN_ROWS;
			c0 = 0;
			c1 = AN_COLS;
			p->row = 0;
			p->col = 0;
		}
		for (r = r0; r < r1 && r < AN_ROWS; r++)
			for (c = (r == r0 ? c0 : 0); c < c1 && c < AN_COLS; c++)
				if (r >= 0 && c >= 0)
					an_put(c, r, ' ', p->fg, p->bg);
	}
	else if (cmd == 'K')
	{
		int mode = p->narg ? p->args[0] : 0;
		int c, c0 = 0, c1 = AN_COLS;
		if (mode == 0)
			c0 = p->col;
		else if (mode == 1)
			c1 = p->col + 1;
		for (c = c0; c < c1 && c < AN_COLS; c++)
			if (c >= 0)
				an_put(c, p->row, ' ', p->fg, p->bg);
	}
	else if (cmd == 'm')
		an_parser_sgr(p);
	else if (cmd == 's')
	{
		p->sav_row = p->row;
		p->sav_col = p->col;
	}
	else if (cmd == 'u')
	{
		p->row = p->sav_row;
		p->col = p->sav_col;
	}
	if (p->row < 0)
		p->row = 0;
	if (p->col < 0)
		p->col = 0;
	if (p->row >= AN_ROWS)
		p->row = AN_ROWS - 1;
	if (p->col >= AN_COLS)
		p->col = AN_COLS - 1;
}

static void an_parser_feed(an_parser *p, int b)
{
	if (p->state == 0)
	{
		if (b == 27)
		{
			p->state = 1;
			return;
		}
		if (b == '\r')
		{
			p->col = 0;
			return;
		}
		if (b == '\n' || b == 11)
		{
			p->row++;
			p->col = 0;
			return;
		}
		if (b == 8)
		{
			if (p->col > 0)
				p->col--;
			return;
		}
		if (b == '\t')
		{
			int nx = (p->col / 8 + 1) * 8;
			p->col = nx;
			return;
		}
		if (b < 32)
			return;
		an_parser_put(p, b);
		return;
	}
	if (p->state == 1)
	{
		if (b == '[')
		{
			int i;
			p->state = 2;
			p->narg = 0;
			p->priv = 0;
			for (i = 0; i < 8; i++)
				p->args[i] = 0;
			return;
		}
		if (b == ']')
		{
			p->state = 3;
			return;
		}
		p->state = 0;
		return;
	}
	if (p->state == 3)
	{
		if (b == 7)
			p->state = 0;
		else if (b == 27)
			p->state = 4;
		return;
	}
	if (p->state == 4)
	{
		p->state = 0;
		return;
	}
	/* CSI parameter bytes. */
	if (b == '?')
	{
		p->priv = 1;
		return;
	}
	if (b >= '0' && b <= '9')
	{
		if (p->narg == 0)
			p->narg = 1;
		p->args[p->narg - 1] = p->args[p->narg - 1] * 10 + (b - '0');
		return;
	}
	if (b == ';')
	{
		if (p->narg < 8)
			p->narg++;
		return;
	}
	if ((b >= 'A' && b <= 'Z') || (b >= 'a' && b <= 'z'))
	{
		if (!p->priv)
			an_parser_csi(p, b);
		p->state = 0;
		return;
	}
	p->state = 0;
}

/* Strip a trailing SAUCE record and return the effective art length. */
static unsigned an_strip_sauce(const unsigned char *buf, unsigned n)
{
	if (n >= 128 && memcmp(buf + n - 128, "SAUCE00", 7) == 0)
	{
		unsigned end = n - 128;
		if (end > 0 && buf[end - 1] == 0x1A)
			end--;
		return end;
	}
	if (n > 0 && buf[n - 1] == 0x1A)
		return n - 1;
	return n;
}

static int an_load(const char *path)
{
	int sz;
	unsigned got = 0, end, i;
	unsigned char *buf;
	an_parser p;

	an_clear_canvas();
	sz = mmb_vfs_size(path);
	if (sz <= 0)
		return -1;
	buf = G.plat->alloc((unsigned)sz + 1);
	if (!buf)
		return -1;
	if (mmb_vfs_read(path, buf, (unsigned)sz, &got) != 0)
	{
		G.plat->free(buf);
		return -1;
	}
	end = an_strip_sauce(buf, got);
	memset(&p, 0, sizeof(p));
	p.fg = 7;
	for (i = 0; i < end; i++)
		an_parser_feed(&p, buf[i]);
	G.plat->free(buf);
	return 0;
}

static void an_sgr_emit(char *dst, int *n, int fg, int bg)
{
	int f = (fg & 15), b = (bg & 7);
	if (f < 8)
		*n += sprintf(dst + *n, "\x1b[%d;%dm", 30 + f, 40 + b);
	else
		*n += sprintf(dst + *n, "\x1b[%d;%dm", 90 + (f - 8), 40 + b);
}

static int an_save(const char *path)
{
	unsigned cap = (unsigned)AN_ROWS * (AN_COLS * 20u + 16u) + 16u;
	unsigned char *out = G.plat->alloc(cap);
	int n = 0, x, y;
	int rc;
	int pf = -1, pb = -1;

	if (!out)
		return -1;
	n += sprintf((char *)out + n, "\x1b[0m");
	for (y = 0; y < AN_ROWS; y++)
	{
		pf = pb = -1;
		for (x = 0; x < AN_COLS; x++)
		{
			an_cell *c = &AN[y][x];
			int ch = c->ch ? c->ch : ' ';
			if (c->fg != pf || c->bg != pb)
			{
				an_sgr_emit((char *)out, &n, c->fg, c->bg);
				pf = c->fg;
				pb = c->bg;
			}
			out[n++] = (unsigned char)ch;
		}
		n += sprintf((char *)out + n, "\x1b[0m\r\n");
		if ((unsigned)n > cap - 32)
			break;
	}
	rc = mmb_vfs_write(path, out, (unsigned)n, 0);
	G.plat->free(out);
	return rc == 0 ? 0 : -1;
}

static void an_sauce_append(const char *path)
{
	/* Minimal SAUCE v00 record: 128 bytes after a 0x1A EOF marker. */
	unsigned char rec[128];
	int sz = mmb_vfs_size(path);
	unsigned got = 0;
	unsigned char *buf;
	int missing_eof = 1;

	if (sz <= 0)
		return;
	buf = G.plat->alloc((unsigned)sz + 1);
	if (!buf)
		return;
	if (mmb_vfs_read(path, buf, (unsigned)sz, &got) == 0)
	{
		unsigned end = an_strip_sauce(buf, got);
		if (end > 0 && buf[end - 1] == 0x1A)
			missing_eof = 0;
	}
	G.plat->free(buf);
	memset(rec, 0, sizeof(rec));
	memcpy(rec, "SAUCE00", 7);
	memcpy(rec + 7, "mmcore ANSI editor", 18);
	rec[35] = 'M';
	rec[36] = 'M';
	rec[37] = 'B';
	rec[42] = '1';
	rec[43] = '.';
	rec[44] = '0';
	rec[90] = 80;
	rec[92] = 25;
	if (missing_eof)
	{
		unsigned char eof = 0x1A;
		mmb_vfs_write(path, &eof, 1, 1);
	}
	mmb_vfs_write(path, rec, sizeof(rec), 1);
}

/* ------------------------------------------------------------------ */
/* Editor TUI                                                          */
/* ------------------------------------------------------------------ */

static const char *an_base(const char *p)
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

static void an_redraw(void)
{
	int cols, rows, x, y, total = AN_PAINT_N;
	char line[112];

	if (!G.plat || !G.plat->tui_glyph)
		return;
	cols = tui_cols();
	rows = tui_rows();
	tui_begin();
	tui_set_palette(0);
	tui_clear(TUI_BRWHITE, TUI_BRBLACK);
	for (y = 0; y < AN_ROWS; y++)
		for (x = 0; x < AN_COLS; x++)
		{
			an_cell *c = &AN[y][x];
			int ch = c->ch ? c->ch : ' ';
			tui_put(x, y, ch, c->fg, c->bg);
		}
	if (A.cx >= 0 && A.cx < AN_COLS && A.cy >= 0 && A.cy < AN_ROWS)
		tui_cursor(A.cx, A.cy, 1);

	if (cols > AN_COLS + 2)
	{
		int px = AN_COLS + 2, py = 0;
		sprintf(line, "ANSI %s%s", A.text_mode ? "TEXT" : "DRAW",
			A.line_mode ? " LINE" : "");
		tui_puts(px, py++, line, TUI_BRCYAN, TUI_BRBLACK);
		sprintf(line, "FG %d  BG %d", A.fg, A.bg);
		tui_puts(px, py++, line, TUI_BRYELLOW, TUI_BRBLACK);
		for (x = 0; x < 16 && px + x < cols; x++)
		{
			tui_put(px + x, py, ' ', TUI_WHITE, x);
			if (x == A.fg)
				tui_put(px + x, py, '_', TUI_BRWHITE, x);
		}
		py++;
		sprintf(line, "PAINT %d/%d", A.paint + 1, total);
		tui_puts(px, py++, line, TUI_WHITE, TUI_BRBLACK);
		tui_put(px, py, AN_PAINT[A.paint], TUI_WHITE, TUI_BRBLACK);
		py += 2;
		if (A.have_font)
		{
			sprintf(line, "FONT %s", A.tdf.name);
			tui_puts(px, py++, line, TUI_BRCYAN, TUI_BRBLACK);
		}
		else
		{
			tui_puts(px, py++, "FONT none", TUI_BRBLACK, TUI_BRBLACK);
		}
		py++;
		tui_puts(px, py++, "type chars", TUI_WHITE, TUI_BRBLACK);
		tui_puts(px, py++, "Space paint", TUI_WHITE, TUI_BRBLACK);
		tui_puts(px, py++, "Arrows move", TUI_WHITE, TUI_BRBLACK);
		tui_puts(px, py++, "a+c/a+b col", TUI_WHITE, TUI_BRBLACK);
		tui_puts(px, py++, "a+p paint", TUI_WHITE, TUI_BRBLACK);
		tui_puts(px, py++, "a+t text", TUI_WHITE, TUI_BRBLACK);
		tui_puts(px, py++, "a+l line", TUI_WHITE, TUI_BRBLACK);
		tui_puts(px, py++, "f flood fill", TUI_WHITE, TUI_BRBLACK);
		tui_puts(px, py++, "a+s save", TUI_WHITE, TUI_BRBLACK);
		tui_puts(px, py++, "Esc quit", TUI_WHITE, TUI_BRBLACK);
	}
	tui_pad(0, rows - 1, A.status, cols, TUI_BRYELLOW, TUI_BRBLACK);
	tui_flush();
}

static void an_leave(void)
{
	tui_end();
	mmb_gfx_cls(G.gfx.bg);
	G.home_prompt = 0;
	an_tdf_free(&A);
	memset(&A, 0, sizeof(A));
	mmb_console_write("\r\n");
	mmb_console_write(mmb_prompt());
}

static void an_set_pos(int x, int y)
{
	int ox = A.cx, oy = A.cy;
	A.cx = x;
	A.cy = y;
	if (A.cx < 0)
		A.cx = 0;
	if (A.cy < 0)
		A.cy = 0;
	if (A.cx >= AN_COLS)
		A.cx = AN_COLS - 1;
	if (A.cy >= AN_ROWS)
		A.cy = AN_ROWS - 1;
	if (A.line_mode && (ox != A.cx || oy != A.cy))
		an_line(&A, ox, oy, A.cx, A.cy);
}

static void an_move(int dx, int dy)
{
	an_set_pos(A.cx + dx, A.cy + dy);
}

static void an_advance(void)
{
	A.cx++;
	if (A.cx >= AN_COLS)
	{
		A.cx = 0;
		A.cy++;
		if (A.cy >= AN_ROWS)
			A.cy = AN_ROWS - 1;
	}
}

static void an_type_char(int c)
{
	if (A.text_mode)
	{
		if (A.have_font)
		{
			int adv = an_tdf_stamp(&A.tdf, c, A.cx, A.cy, A.fg, A.bg);
			int i;
			for (i = 0; i < adv; i++)
				an_advance();
			A.dirty = 1;
		}
		return;
	}
	an_put(A.cx, A.cy, c, A.fg, A.bg);
	A.dirty = 1;
	an_advance();
}

static void an_stamp_brush(void)
{
	if (A.text_mode)
		return;
	an_put(A.cx, A.cy, AN_PAINT[A.paint], A.fg, A.bg);
	A.dirty = 1;
	an_advance();
}

static void an_cycle_colour(int which)
{
	if (which == 0)
		A.fg = (A.fg + 1) & 15;
	else
		A.bg = (A.bg + 1) & 7;
}

static void an_cycle_paint(void)
{
	A.paint = (A.paint + 1) % AN_PAINT_N;
}

static int an_escape(int c)
{
	if (A.esc_state == AN_ESC_GOT)
	{
		if (c == '[')
		{
			A.esc_state = AN_ESC_CSI;
			return 1;
		}
		if (c == 'O')
		{
			A.esc_state = AN_ESC_SS3;
			return 1;
		}
		A.esc_state = AN_ESC_NONE;
		return 0;
	}
	if (A.esc_state == AN_ESC_SS3)
	{
		A.esc_state = AN_ESC_NONE;
		return 1;
	}
	if (A.esc_state == AN_ESC_CSI)
	{
		A.esc_state = AN_ESC_NONE;
		if (c == 'A')
			an_move(0, -1);
		else if (c == 'B')
			an_move(0, 1);
		else if (c == 'C')
			an_move(1, 0);
		else if (c == 'D')
			an_move(-1, 0);
		else if (c == 'H')
			an_set_pos(0, A.cy);
		else if (c == 'F')
			an_set_pos(AN_COLS - 1, A.cy);
		return 1;
	}
	return 0;
}

static void an_alt_key(int c)
{
	if (c == 'x' || c == 'X')
	{
		an_leave();
		return;
	}
	if (c == 'c' || c == 'C')
		an_cycle_colour(0);
	else if (c == 'b' || c == 'B')
		an_cycle_colour(1);
	else if (c == 'p' || c == 'P')
		an_cycle_paint();
	else if (c == 'f' || c == 'F')
		an_flood(&A, A.cx, A.cy);
	else if (c == 's' || c == 'S')
	{
		if (an_save(A.path) == 0)
		{
			A.dirty = 0;
			sprintf(A.status, "Saved %s", A.path);
		}
		else
			strncpy(A.status, "Save failed", sizeof(A.status) - 1);
	}
	else if (c == 't' || c == 'T')
	{
		if (A.have_font)
			A.text_mode = !A.text_mode;
		else
			strncpy(A.status, "No .TDF font loaded", sizeof(A.status) - 1);
	}
	else if (c == 'l' || c == 'L')
		A.line_mode = !A.line_mode;
	else if (c >= '0' && c <= '9')
		A.fg = c - '0';
}

static void an_handle(int c)
{
	if (c == '\r' || c == '\n')
	{
		A.cx = 0;
		A.cy++;
		if (A.cy >= AN_ROWS)
			A.cy = AN_ROWS - 1;
		return;
	}
	if (c == 8)
	{
		an_move(-1, 0);
		return;
	}
	if (c == 127)
	{
		an_put(A.cx, A.cy, 0, A.fg, A.bg);
		A.dirty = 1;
		return;
	}
	if (c == ' ')
	{
		an_stamp_brush();
		return;
	}
	if (c == '\t')
	{
		an_cycle_colour(0);
		return;
	}
	if (c >= 32 && c < 127)
		an_type_char(c);
}

const char *mmb_ansi_edit_key(char c)
{
	G.outn = 0;
	G.out[0] = 0;
	if (!A.active)
		return G.out;
	if (A.alt_pend)
	{
		A.alt_pend = 0;
		an_alt_key(c);
		if (A.active)
			an_redraw();
		return G.out;
	}
	if ((unsigned char)c == 1)
	{
		A.alt_pend = 1;
		return G.out;
	}
	if (A.esc_state && an_escape(c))
	{
		if (A.active)
			an_redraw();
		return G.out;
	}
	if (c == 27)
	{
		A.esc_state = AN_ESC_GOT;
		A.esc_at = mmb_now_ms();
		return G.out;
	}
	an_handle(c);
	if (A.active)
		an_redraw();
	return G.out;
}

void mmb_ansi_edit_poll(void)
{
	if (!A.active)
		return;
	if (A.esc_state == AN_ESC_GOT &&
	    mmb_now_ms() - A.esc_at >= AN_ESC_IDLE_MS)
	{
		A.esc_state = AN_ESC_NONE;
		an_leave();
	}
}

int mmb_in_ansi_edit(void)
{
	return A.active;
}

static void an_make_path(char *dst, int dstsz, const char *in)
{
	char tmp[160];

	if (!in || !in[0])
		strcpy(tmp, "A:/ANSI.ANS");
	else
	{
		strncpy(tmp, in, sizeof(tmp) - 1);
		tmp[sizeof(tmp) - 1] = 0;
	}
	if (!strchr(an_base(tmp), '.'))
		strncat(tmp, ".ANS", sizeof(tmp) - strlen(tmp) - 1);
	mmb_vfs_resolve(tmp, dst, dstsz);
}

static void an_open(const char *in, const char *font)
{
	memset(&A, 0, sizeof(A));
	A.active = 1;
	A.fg = 15;
	A.bg = 0;
	A.paint = 0;
	A.cx = A.cy = 0;
	A.esc_state = AN_ESC_NONE;
	an_clear_canvas();
	an_make_path(A.path, sizeof(A.path), in);
	strncpy(A.label, an_base(A.path), sizeof(A.label) - 1);
	if (font && font[0])
	{
		char fp[160];
		mmb_vfs_resolve(font, fp, sizeof(fp));
		if (an_tdf_load(&A, fp, 0) == 0)
			strncpy(A.font_label, A.tdf.name, sizeof(A.font_label) - 1);
	}
	if (an_load(A.path) == 0)
		strncpy(A.status, "a+s save   Esc quit", sizeof(A.status) - 1);
	else
		strncpy(A.status, "new file   a+s save   Esc quit", sizeof(A.status) - 1);
	A.status[sizeof(A.status) - 1] = 0;
	mmb_hw_cursor(0);
	an_redraw();
}

static void an_cmd_edit(void)
{
	char name[128] = "";
	char font[160] = "";
	mmb_val v;

	mmb_skip_sp();
	if (*G.p && *G.p != ':' && *G.p != '\'')
	{
		v = mmb_expr();
		if (v.type != T_STR)
			mmb_syntax();
		strncpy(name, v.s, sizeof(name) - 1);
		mmb_skip_sp();
		if (*G.p == ',')
		{
			G.p++;
			v = mmb_expr();
			if (v.type != T_STR)
				mmb_syntax();
			strncpy(font, v.s, sizeof(font) - 1);
		}
	}
	an_open(name, font);
}

static void an_cmd_font(void)
{
	char path[160];
	char name[24] = "";
	mmb_val v;
	int sz;
	unsigned char *buf;
	unsigned got = 0;
	an_tdf f;
	char line[128];

	mmb_skip_sp();
	v = mmb_expr();
	if (v.type != T_STR)
		mmb_syntax();
	mmb_vfs_resolve(v.s, path, sizeof(path));
	mmb_skip_sp();
	if (*G.p == ',')
	{
		G.p++;
		v = mmb_expr();
		if (v.type == T_STR)
			strncpy(name, v.s, sizeof(name) - 1);
	}
	sz = mmb_vfs_size(path);
	if (sz < 233)
		mmb_error("?TDF");
	buf = G.plat->alloc((unsigned)sz + 1);
	if (!buf)
		mmb_error("?OUT OF MEMORY");
	if (mmb_vfs_read(path, buf, (unsigned)sz, &got) != 0 ||
	    an_tdf_find(buf, got, name, &f) != 0)
	{
		G.plat->free(buf);
		mmb_error("?TDF");
	}
	sprintf(line, "TDF %s type=%d spacing=%d glyphs=%d", f.name, f.type,
		f.spacing, f.glyph_count);
	G.plat->free(buf);
	mmb_console_write(line);
	mmb_console_write("\r\n");
}

static void an_cmd_sauce(void)
{
	char path[160];
	mmb_val v;

	mmb_skip_sp();
	v = mmb_expr();
	if (v.type != T_STR)
		mmb_syntax();
	mmb_vfs_resolve(v.s, path, sizeof(path));
	if (mmb_vfs_size(path) <= 0)
		mmb_error("?FILE");
	an_sauce_append(path);
}

void mmb_cmd_ansi(void)
{
	if (mmb_match("EDIT") || mmb_match("LOAD"))
	{
		an_cmd_edit();
		return;
	}
	if (mmb_match("FONT"))
	{
		an_cmd_font();
		return;
	}
	if (mmb_match("SAUCE"))
	{
		an_cmd_sauce();
		return;
	}
	mmb_skip_sp();
	if (*G.p == 0 || *G.p == '"' || *G.p == ':' || *G.p == '\'')
	{
		an_cmd_edit();
		return;
	}
	mmb_syntax();
}
