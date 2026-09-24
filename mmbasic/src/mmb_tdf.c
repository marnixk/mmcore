#include "mmb_tdf.h"

#include <string.h>

/* TheDraw .TDF parser and glyph stamp.
 *
 * Mirrors the decoder used by the rest of the tree: the file magic is
 * 0x13 "TheDraw FONTS file" 0x1A, font records start at 20, glyph offsets
 * are 94 little-endian 16-bit values (0xFFFF = undefined), and glyph data
 * holds a (width, height) pair followed by CP437 cells terminated by NUL.
 * Outline fonts encode box-drawing cells as A-O / @ / & and colour fonts
 * store a (character, attribute) byte pair for every cell.
 */

int mmb_tdf_parse(const unsigned char *buf, unsigned n, int index, mmb_tdf *f)
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
			for (j = 0; j < 94; j++)
			{
				unsigned o = f->off[j];
				int h;
				if (o == 0xFFFF || o + 1 >= f->data_len)
					continue;
				h = f->data[o + 1];
				if (h > f->max_height && h <= 16)
					f->max_height = h;
			}
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

/* Count the font records in buf/n, i.e. how many variations mmb_tdf_parse()
 * can address. The walk stops at the first record that fails, so a file with
 * a truncated or garbage tail still reports its good leading variations.
 */
int mmb_tdf_count(const unsigned char *buf, unsigned n)
{
	unsigned pos = 20;
	int i;

	if (n < 233 || buf[0] != 0x13)
		return 0;
	if (memcmp(buf + 1, "TheDraw FONTS file", 18) != 0 || buf[19] != 0x1A)
		return 0;

	for (i = 0;; i++)
	{
		unsigned bs, ds;
		if (pos + 45 > n)
			return i;
		if (memcmp(buf + pos, "\x55\xaa\x00\xff", 4) != 0)
			return i;
		bs = (unsigned)buf[pos + 23] | ((unsigned)buf[pos + 24] << 8);
		ds = (i == 0) ? 233u : pos + 213u;
		if (ds + bs > n)
			bs = (ds < n) ? n - ds : 0;

		if (i == 0)
			pos = 233 + bs;
		else
			pos = ds + bs;
		if (pos + 4 <= n && memcmp(buf + pos, "\x55\xaa\x00\xff", 4) != 0)
			pos++;
	}
}

/* Map an Outline-font letter to its CP437 box-drawing code. */
static int mmb_outline_map(int c)
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

int mmb_tdf_stamp(const mmb_tdf *f, int code, int x, int y, int fg, int bg,
		  mmb_tdf_cell_fn cb, void *ctx)
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
		int cf = fg, cb_bg = bg;
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
		if (f->type == MMB_TDF_COLOR)
		{
			int attr;
			if (p >= f->data_len)
				break;
			attr = f->data[p++];
			cb_bg = (attr >> 4) & 7;
			cf = attr & 15;
		}
		else if (f->type == MMB_TDF_OUTLINE)
		{
			int m = mmb_outline_map(c);
			if (m < 0)
			{
				col++;
				continue;
			}
			c = m;
		}
		if (c == '@' || c == '&')
		{
			col++;
			continue;
		}
		if (cb)
			cb(ctx, x + col, y + row, c, cf, cb_bg);
		col++;
	}
	return adv;
}
