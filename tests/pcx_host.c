/* Host test for the PCX codec in mmbasic/src/pcx.c (#631).
 *
 * Compiles the real codec against a tiny G/alloc shim and checks:
 *   - 8-bit indexed encode -> decode round-trips raster and palette,
 *   - RLE runs, literal 0xC0+ bytes, and the 256-colour trailer decode,
 *   - 24-bit three-plane RLE decodes to the expected RGB pixels,
 *   - malformed headers, overlong runs and truncation are rejected,
 *   - the default VGA palette is written when no palette is supplied.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mmb_priv.h"
#include "pcx.h"

mmb_globals_t G;

static int fails;

static void check(int cond, const char *what)
{
	if (!cond)
	{
		printf("FAIL: %s\n", what);
		fails++;
	}
}

static void *host_alloc(unsigned n)
{
	return malloc(n ? n : 1);
}

static void host_free(void *p)
{
	free(p);
}

static mmb_plat_t s_plat = { host_alloc, host_free };

unsigned mmb_rgb_pack(int r, int g, int b)
{
	return ((unsigned)r << 16) | ((unsigned)g << 8) | (unsigned)b;
}

static void put_u16(unsigned char *p, unsigned v)
{
	p[0] = (unsigned char)(v & 0xFFu);
	p[1] = (unsigned char)((v >> 8) & 0xFFu);
}

static void build_header(unsigned char *d, int w, int h, int planes,
			 unsigned bpl, int enc)
{
	memset(d, 0, 128);
	d[0] = 0x0A;
	d[1] = 5;
	d[2] = (unsigned char)enc;
	d[3] = 8;
	put_u16(d + 4, 0); /* xmin */
	put_u16(d + 6, 0); /* ymin */
	put_u16(d + 8, (unsigned)(w - 1));
	put_u16(d + 10, (unsigned)(h - 1));
	d[65] = (unsigned char)planes;
	put_u16(d + 66, bpl);
	d[68] = 1;
}

/* RLE-encode one literal byte the way a PCX writer must. */
static unsigned put_lit(unsigned char *o, unsigned char v)
{
	if (v >= 0xC0u)
	{
		o[0] = 0xC1u;
		o[1] = v;
		return 2;
	}
	o[0] = v;
	return 1;
}

static void test_indexed_roundtrip(void)
{
	unsigned char idx[64], pal[768], rpal[768];
	unsigned char *enc = 0, *dec = 0;
	unsigned elen = 0;
	int w = 0, h = 0, ncol = 0, i;

	for (i = 0; i < 768; i++)
		pal[i] = (unsigned char)((i * 7) & 0xFF);
	for (i = 0; i < 64; i++)
		idx[i] = (unsigned char)((i * 3) & 0xFF);

	check(mmb_pcx_encode_indexed(idx, 8, 8, pal, &enc, &elen) == 0, "encode 8x8");
	check(enc && elen > 128, "encoded stream has a header");
	check(mmb_pcx_decode_indexed(enc, elen, &dec, &w, &h, rpal, &ncol) == 0,
	      "decode 8x8");
	check(w == 8 && h == 8, "decoded dimensions");
	check(ncol == 256, "decoded colour count");
	check(dec && memcmp(idx, dec, 64) == 0, "raster round-trips");
	check(memcmp(pal, rpal, 768) == 0, "palette round-trips");
	free(enc);
	free(dec);
}

/* A single 6-byte padded row mixing a run with a literal 0xC5 byte. */
static void test_indexed_rle(void)
{
	unsigned char f[128 + 6 + 1 + 768];
	unsigned char rpal[768];
	unsigned char *pix = 0;
	uint32_t *rgba = 0;
	int w = 0, h = 0, ncol = 0, i;
	unsigned o;

	build_header(f, 5, 1, 1, 6, 1);
	o = 128;
	f[o++] = 0xC4u; /* run of 4 */
	f[o++] = 0x05u;
	f[o++] = 0xC1u; /* literal 0xC5 (a byte >= 0xC0 must be escaped) */
	f[o++] = 0xC5u;
	f[o++] = 0x00u; /* pad to bytes_per_line */
	f[o] = 0x0Cu;
	for (i = 0; i < 768; i++)
		f[o + 1 + i] = 0;

	check(mmb_pcx_decode_indexed(f, o + 1 + 768, &pix, &w, &h, rpal, &ncol) == 0,
	      "decode run stream");
	check(w == 5 && h == 1, "run image dimensions");
	check(pix && pix[0] == 5 && pix[1] == 5 && pix[2] == 5 && pix[3] == 5 &&
		      pix[4] == 0xC5,
	      "run expands and the escaped literal survives");
	free(pix);

	/* And via RGBA: the palette maps index 5 to (10,20,30). */
	rpal[5 * 3] = 10;
	rpal[5 * 3 + 1] = 20;
	rpal[5 * 3 + 2] = 30;
	memcpy(f + o + 1 + 5 * 3, rpal + 5 * 3, 3);
	check(mmb_pcx_decode_rgba(f, o + 1 + 768, &rgba, &w, &h) == 0,
	      "decode run rgba");
	check(rgba && (rgba[0] & 0xFFFFFFu) == mmb_rgb_pack(10, 20, 30),
	      "palette maps the index");
	free(rgba);
}

static void test_24bit(void)
{
	unsigned char f[128 + 16];
	uint32_t *rgba = 0;
	int w = 0, h = 0;
	unsigned o = 128;

	/* One row, two pixels, three planes: R=10,40 G=20,50 B=30,60. */
	build_header(f, 2, 1, 3, 2, 1);
	o += put_lit(f + o, 10);
	o += put_lit(f + o, 40);
	o += put_lit(f + o, 20);
	o += put_lit(f + o, 50);
	o += put_lit(f + o, 30);
	o += put_lit(f + o, 60);

	check(mmb_pcx_decode_rgba(f, o, &rgba, &w, &h) == 0, "decode 24-bit");
	check(w == 2 && h == 1, "24-bit dimensions");
	check(rgba && (rgba[0] & 0xFFFFFFu) == mmb_rgb_pack(10, 20, 30),
	      "24-bit pixel 0");
	check(rgba && (rgba[1] & 0xFFFFFFu) == mmb_rgb_pack(40, 50, 60),
	      "24-bit pixel 1");
	free(rgba);
}

static void test_default_palette(void)
{
	unsigned char idx[4], rpal[768];
	unsigned char *enc = 0, *dec = 0;
	unsigned elen = 0;
	int w = 0, h = 0, ncol = 0;

	memset(idx, 0, sizeof idx);
	check(mmb_pcx_encode_indexed(idx, 2, 2, 0, &enc, &elen) == 0,
	      "encode with default palette");
	check(enc && elen > 128 + 1 + 768, "trailer present");
	check(enc[elen - 769] == 0x0Cu, "256-colour trailer marker");
	check(mmb_pcx_decode_indexed(enc, elen, &dec, &w, &h, rpal, &ncol) == 0,
	      "decode default palette");
	check(rpal[0] == 0 && rpal[1] == 0 && rpal[2] == 0, "VGA 0 black");
	check(rpal[3] == 0x00 && rpal[4] == 0x00 && rpal[5] == 0xAA, "VGA 1 blue");
	free(dec);
	free(enc);
}

static void test_malformed(void)
{
	unsigned char f[128 + 16];
	unsigned char *out = 0;
	unsigned char pal[768];
	int w = 0, h = 0, ncol = 0;
	uint32_t *rgba = 0;

	/* Wrong manufacturer. */
	build_header(f, 2, 2, 1, 2, 1);
	f[0] = 0x00;
	check(mmb_pcx_decode_rgba(f, 128, &rgba, &w, &h) == -1, "bad manufacturer");

	/* Oversized dimensions. */
	build_header(f, 2, 2, 1, 2, 1);
	put_u16(f + 8, 5000 - 1);
	check(mmb_pcx_decode_rgba(f, 128, &rgba, &w, &h) == -1, "huge width");

	/* Truncated RLE stream. */
	build_header(f, 8, 1, 1, 8, 1);
	check(mmb_pcx_decode_rgba(f, 128, &rgba, &w, &h) == -1, "truncated RLE");

	/* A run that would overrun the raster. */
	build_header(f, 8, 1, 1, 8, 1);
	f[128] = 0xFFu; /* run of 63 */
	f[129] = 0x01u;
	check(mmb_pcx_decode_rgba(f, 130, &rgba, &w, &h) == -1, "run overrun");

	/* A zero-length run. */
	build_header(f, 8, 1, 1, 8, 1);
	f[128] = 0xC0u;
	f[129] = 0x01u;
	check(mmb_pcx_decode_rgba(f, 130, &rgba, &w, &h) == -1, "zero run");

	/* bytes_per_line smaller than the width. */
	build_header(f, 8, 1, 1, 4, 1);
	f[128] = 0x01;
	check(mmb_pcx_decode_indexed(f, 129, &out, &w, &h, pal, &ncol) == -1,
	      "bpl < width");
}

int main(void)
{
	G.plat = &s_plat;
	test_indexed_roundtrip();
	test_indexed_rle();
	test_24bit();
	test_default_palette();
	test_malformed();

	if (fails)
	{
		printf("%d check(s) failed\n", fails);
		return 1;
	}
	printf("all checks passed\n");
	return 0;
}
