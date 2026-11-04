/* Streaming sixel decoder (docs/analysis/term-sixel.md).
 *
 * The decoder owns the sixel grammar and writes native pixels straight into a
 * caller-supplied buffer.  It never buffers the DCS payload: TERM feeds one
 * byte at a time as it arrives.  Colour registers are packed once, through the
 * caller's pack hook, so a sixel pixel is the same value a glyph stores on the
 * current platform (mmb_pix_store in TERM; a test pack in the host tests).
 */
#ifndef MMB_SIXEL_H
#define MMB_SIXEL_H

#include <stdint.h>

typedef uint16_t (*sixel_pack_fn)(unsigned rgb888);

typedef struct {
	uint16_t *pix;     /* top-left pixel of the target buffer */
	int stride;        /* pixels per target row */
	int width;         /* target width (pane pixels) */
	int height;        /* target height (pane pixels) */
	int ox, oy;        /* image origin inside the target */
	int p1, p2, p3;    /* DCS P1;P2;P3 (P1/P3 ignored) */
	uint16_t bg;       /* packed background captured at q */
	sixel_pack_fn pack;
	unsigned char *cover; /* optional per-cell coverage bytes */
	int cover_cols;
	int cell_w, cell_h;   /* cell size for coverage */
} sixel_config;

typedef enum {
	SIXEL_DATA = 0,
	SIXEL_HASH,
	SIXEL_REPEAT,
	SIXEL_RASTER
} sixel_parse_state;

typedef struct {
	uint16_t *pix;
	int stride;
	int width, height;
	int ox, oy;
	int p2;
	uint16_t bg;
	sixel_pack_fn pack;

	unsigned char *cover;
	int cover_cols;
	int cell_w, cell_h;

	uint16_t pal[256];
	int reg;

	int x, y;          /* current pixel position (target coordinates) */
	int maxw, maxh;    /* clip box in image coordinates */
	int have_raster;   /* both Ph and Pv were supplied */
	int bottom, right; /* touched extent + 1, for the caller's dirty box */

	sixel_parse_state st;
	int par[6];
	int npar;
	uint32_t cur;
	int have_cur;

	int advance_h;     /* cursor advance, in pixels, after finish */
	int painted_w;
	int painted_h;
} sixel_image;

void sixel_begin(sixel_image *s, const sixel_config *cfg);
void sixel_feed(sixel_image *s, unsigned char b);
void sixel_finish(sixel_image *s);

#endif
