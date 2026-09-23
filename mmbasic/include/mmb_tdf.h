#ifndef MMB_TDF_H
#define MMB_TDF_H

/* Shared TheDraw (.TDF) font decoder.
 *
 * Used by the FILES .TDF preview; the format is the one documented for the
 * ramdisk fonts (A:/fonts/tdf/) and ramdisk/lib/TDF.BAS. The decoder is pure
 * C: the caller owns the file buffer and keeps it alive for as long as the
 * parsed font is used (the font points into it).
 */

#define MMB_TDF_OUTLINE 0
#define MMB_TDF_BLOCK   1
#define MMB_TDF_COLOR   2

typedef struct {
	char name[24];
	int type;
	int spacing;
	int glyph_count;
	int max_height;
	const unsigned char *buf;
	const unsigned char *data;
	unsigned data_len;
	unsigned short off[94];
} mmb_tdf;

/* Parse font `index` out of the whole file in buf/n. Returns 0 on success,
 * -1 when the header magic is wrong or the index is past the last font. */
int mmb_tdf_parse(const unsigned char *buf, unsigned n, int index, mmb_tdf *f);

/* Write one CP437 cell. x/y are screen cells; implementations clip. */
typedef void (*mmb_tdf_cell_fn)(void *ctx, int x, int y, int ch, int fg, int bg);

/* Render glyph code at cell (x, y) using fg/bg (colour fonts override them
 * from the glyph's attribute). Returns the column advance in cells. A space
 * (32), an undefined glyph, and an out-of-range code advance safely instead
 * of dereferencing a missing glyph. */
int mmb_tdf_stamp(const mmb_tdf *f, int code, int x, int y, int fg, int bg,
		  mmb_tdf_cell_fn cb, void *ctx);

#endif
