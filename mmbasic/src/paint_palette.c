/*
 * PAINT - palette strip and FG/BG selection (#635).
 *
 * Implements the palette half of the frozen paint.h API:
 *
 *   - the fixed default VGA 256 palette (EGA 16, a 16-step grey ramp, a
 *     uniform 6x6x6 RGB cube, then eight black entries). It is not editable
 *     and is deliberately not the old app's IBM16 + grey + 6x6x6-cube build
 *     (non-uniform cube, trailing primaries).
 *   - the bottom swatch strip: 4 rows x 64 swatches of 8x8 screen pixels,
 *     laid out by the scaffold in paint.h.
 *   - the FG/BG indicator in the 32x32 cell at the left end of the band.
 *   - mouse hit-testing: the strip and the indicator.
 *   - FG/BG selection: left button picks FG, right button picks BG, and a
 *     click on the indicator swaps them.
 *
 * These are strong definitions; they override the weak stubs at the bottom of
 * cmd_paint.c at link time.
 */
#include "paint.h"

/* ---- fixed default VGA 256 palette ------------------------------------- */

static unsigned s_pal[256];
static int s_ready;

/*
 * The default VGA palette as initialised by the VGA BIOS:
 *
 *   0..15    EGA/CGA 16
 *   16..31   16 shades of grey (6-bit BIOS values, expanded to 8-bit)
 *   32..247  6x6x6 RGB cube
 *   248..255 black
 *
 * No setter is exposed: the app always uses this table.
 */
static void pt_palette_build(void)
{
	static const unsigned ega[16] = {
		0x000000u, 0x0000AAu, 0x00AA00u, 0x00AAAAu,
		0xAA0000u, 0xAA00AAu, 0xAA5500u, 0xAAAAAAu,
		0x555555u, 0x5555FFu, 0x55FF55u, 0x55FFFFu,
		0xFF5555u, 0xFF55FFu, 0xFFFF55u, 0xFFFFFFu
	};
	/* BIOS default grey ramp in 6-bit DAC values. */
	static const int grey6[16] = {
		0, 5, 8, 11, 14, 17, 20, 24,
		28, 32, 36, 40, 45, 50, 56, 63
	};
	/* Uniform cube axis levels. */
	static const int lvl[6] = { 0, 51, 102, 153, 204, 255 };
	int i, r, g, b;

	if (s_ready)
		return;

	for (i = 0; i < 16; i++)
		s_pal[i] = ega[i];

	for (i = 0; i < 16; i++)
	{
		unsigned v = (unsigned)((grey6[i] << 2) | (grey6[i] >> 4));
		s_pal[16 + i] = (v << 16) | (v << 8) | v;
	}

	i = 32;
	for (r = 0; r < 6; r++)
		for (g = 0; g < 6; g++)
			for (b = 0; b < 6; b++)
				s_pal[i++] = ((unsigned)lvl[r] << 16) |
					     ((unsigned)lvl[g] << 8) |
					     (unsigned)lvl[b];

	for (i = 248; i < 256; i++)
		s_pal[i] = 0x000000u;

	s_ready = 1;
}

void pt_palette_init(void)
{
	pt_palette_build();
}

unsigned pt_palette_rgb(int idx)
{
	if (idx < 0)
		idx = 0;
	if (idx > 255)
		idx = 255;
	pt_palette_build();
	return s_pal[idx];
}

/* ---- FG/BG indicator --------------------------------------------------- */

/* Classic two overlapping squares: BG behind, FG in front. */
#define PT_IND_BG_X 2
#define PT_IND_BG_Y 2
#define PT_IND_SQ_W 18
#define PT_IND_SQ_H 18
#define PT_IND_FG_X 13
#define PT_IND_FG_Y 13

static void pt_framed_square(int x, int y, int w, int h, unsigned rgb,
			     unsigned edge)
{
	pt_fill_rect(x, y, w, h, rgb);
	pt_fill_rect(x, y, w, 1, edge);
	pt_fill_rect(x, y + h - 1, w, 1, edge);
	pt_fill_rect(x, y, 1, h, edge);
	pt_fill_rect(x + w - 1, y, 1, h, edge);
}

static void pt_indicator_draw(void)
{
	unsigned edge = 0x00FFFFFFu;

	/* Cell mount, BG square behind, FG square in front. */
	pt_fill_rect(PT_IND_X, PT_IND_Y, PT_IND_W, PT_IND_H, 0x00303030u);
	pt_framed_square(PT_IND_X + PT_IND_BG_X, PT_IND_Y + PT_IND_BG_Y,
			 PT_IND_SQ_W, PT_IND_SQ_H, pt_palette_rgb(PT.bg), edge);
	pt_framed_square(PT_IND_X + PT_IND_FG_X, PT_IND_Y + PT_IND_FG_Y,
			 PT_IND_SQ_W, PT_IND_SQ_H, pt_palette_rgb(PT.fg), edge);

	/* Outer frame around the whole cell. */
	pt_fill_rect(PT_IND_X, PT_IND_Y, PT_IND_W, 1, edge);
	pt_fill_rect(PT_IND_X, PT_IND_Y + PT_IND_H - 1, PT_IND_W, 1, edge);
	pt_fill_rect(PT_IND_X, PT_IND_Y, 1, PT_IND_H, edge);
	pt_fill_rect(PT_IND_X + PT_IND_W - 1, PT_IND_Y, 1, PT_IND_H, edge);
}

/* ---- drawing ----------------------------------------------------------- */

void pt_palette_draw(void)
{
	int r, c;

	pt_palette_build();

	/* Band behind the swatches and the indicator. */
	pt_fill_rect(0, PT_PAL_Y, PT_W, PT_PAL_H, 0x00101010u);

	for (r = 0; r < PT_PAL_ROWS; r++)
		for (c = 0; c < PT_PAL_COLS; c++)
		{
			int idx = r * PT_PAL_COLS + c;
			pt_fill_rect(PT_PAL_X + c * PT_PAL_SW,
				     PT_PAL_Y + r * PT_PAL_SW,
				     PT_PAL_SW, PT_PAL_SW, s_pal[idx]);
		}

	pt_indicator_draw();
}

/* ---- hit-testing ------------------------------------------------------- */

int pt_palette_hit(int sx, int sy, int *idx)
{
	int c, r;

	if (sx < PT_PAL_X || sy < PT_PAL_Y ||
	    sx >= PT_PAL_X + PT_PAL_W || sy >= PT_PAL_Y + PT_PAL_H)
		return 0;
	c = (sx - PT_PAL_X) / PT_PAL_SW;
	r = (sy - PT_PAL_Y) / PT_PAL_SW;
	if (c < 0 || c >= PT_PAL_COLS || r < 0 || r >= PT_PAL_ROWS)
		return 0;
	if (idx)
		*idx = r * PT_PAL_COLS + c;
	return 1;
}

int pt_palette_indicator_hit(int sx, int sy)
{
	return sx >= PT_IND_X && sy >= PT_IND_Y &&
	       sx < PT_IND_X + PT_IND_W && sy < PT_IND_Y + PT_IND_H;
}

/* ---- selection --------------------------------------------------------- */

void pt_palette_select(int idx, int button)
{
	if (idx < 0)
		idx = 0;
	if (idx > 255)
		idx = 255;
	if (button == PT_BTN_RIGHT)
		PT.bg = idx;
	else
		PT.fg = idx;
	pt_request_redraw();
}

void pt_palette_swap(void)
{
	int t = PT.fg;

	PT.fg = PT.bg;
	PT.bg = t;
	pt_request_redraw();
}
