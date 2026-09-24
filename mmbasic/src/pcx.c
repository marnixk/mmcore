/*
 * PCX codec (#631) - ZSoft PCX version 5.
 *
 * Decode: 8-bit single-plane (indexed, with the 256-colour palette trailer)
 * and 24-bit three-plane (R,G,B) RLE images. Encode: 8-bit single-plane RLE
 * with a 256-colour trailer (the fixed VGA palette when the caller passes
 * none). Runs on both the native host and the bare-metal target, so it only
 * uses G.plat->alloc/free and plain libc string helpers.
 */
#include "mmb_priv.h"
#include "pcx.h"

#define PCX_HEADER  128
#define PCX_RLE_TAG 0xC0u

static unsigned pcx_le16(const unsigned char *p)
{
	return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

/* Expand the fixed default VGA 256 palette to 768 RGB bytes: EGA 16, a
 * 16-step grey ramp, a uniform 6x6x6 cube, then eight blacks. Mirrors the
 * PAINT palette module so saved files carry the app's own colours. */
static void pcx_vga_palette(unsigned char *pal)
{
	static const unsigned ega[16] = {
		0x000000u, 0x0000AAu, 0x00AA00u, 0x00AAAAu,
		0xAA0000u, 0xAA00AAu, 0xAA5500u, 0xAAAAAAu,
		0x555555u, 0x5555FFu, 0x55FF55u, 0x55FFFFu,
		0xFF5555u, 0xFF55FFu, 0xFFFF55u, 0xFFFFFFu
	};
	static const int grey6[16] = {
		0, 5, 8, 11, 14, 17, 20, 24,
		28, 32, 36, 40, 45, 50, 56, 63
	};
	static const int lvl[6] = { 0, 51, 102, 153, 204, 255 };
	int i, r, g, b;

	for (i = 0; i < 16; i++)
	{
		pal[i * 3] = (unsigned char)((ega[i] >> 16) & 0xFFu);
		pal[i * 3 + 1] = (unsigned char)((ega[i] >> 8) & 0xFFu);
		pal[i * 3 + 2] = (unsigned char)(ega[i] & 0xFFu);
	}
	for (i = 0; i < 16; i++)
	{
		unsigned v = (unsigned)((grey6[i] << 2) | (grey6[i] >> 4));
		pal[(16 + i) * 3] = (unsigned char)v;
		pal[(16 + i) * 3 + 1] = (unsigned char)v;
		pal[(16 + i) * 3 + 2] = (unsigned char)v;
	}
	i = 32;
	for (r = 0; r < 6; r++)
		for (g = 0; g < 6; g++)
			for (b = 0; b < 6; b++)
			{
				pal[i * 3] = (unsigned char)lvl[r];
				pal[i * 3 + 1] = (unsigned char)lvl[g];
				pal[i * 3 + 2] = (unsigned char)lvl[b];
				i++;
			}
	for (i = 248; i < 256; i++)
		pal[i * 3] = pal[i * 3 + 1] = pal[i * 3 + 2] = 0;
}

/* Validated PCX header fields. */
typedef struct {
	int w, h;
	int planes;
	int encoding;
	unsigned bpl;
} pcx_hdr;

static int pcx_parse_header(const unsigned char *d, unsigned n, pcx_hdr *out)
{
	unsigned xmin, ymin, xmax, ymax, bpl;
	int w, h;

	if (n < PCX_HEADER)
		return -1;
	if (d[0] != 0x0Au) /* manufacturer: ZSoft */
		return -1;
	if (d[2] != 0 && d[2] != 1) /* encoding: raw or RLE */
		return -1;
	if (d[3] != 8) /* only 8 bits per plane per pixel */
		return -1;

	/* Version 0-5 share the layout; 3/5 cover these formats. Reject the
	 * 2.5-era versions that use a different palette convention. */
	if (d[1] != 3 && d[1] != 5 && d[1] != 2 && d[1] != 4)
		return -1;

	if (d[65] != 1 && d[65] != 3)
		return -1;

	xmin = pcx_le16(d + 4);
	ymin = pcx_le16(d + 6);
	xmax = pcx_le16(d + 8);
	ymax = pcx_le16(d + 10);
	if (xmax < xmin || ymax < ymin)
		return -1;
	w = (int)(xmax - xmin) + 1;
	h = (int)(ymax - ymin) + 1;
	if (w <= 0 || h <= 0)
		return -1;
	if (w > MMB_PCX_MAX_DIM || h > MMB_PCX_MAX_DIM)
		return -1;
	if ((uint64_t)(unsigned)w * (unsigned)h > MMB_PCX_MAX_PIXELS)
		return -1;

	bpl = pcx_le16(d + 66);
	if (bpl < (unsigned)w || bpl > MMB_PCX_MAX_DIM)
		return -1;
	/* The RLE stream must fit the planar cap too (defends against a tiny
	 * image with an absurd bytes_per_line). */
	if ((uint64_t)(unsigned)h * (unsigned)d[65] * bpl >
	    3u * MMB_PCX_MAX_PIXELS)
		return -1;

	out->w = w;
	out->h = h;
	out->planes = d[65];
	out->encoding = d[2];
	out->bpl = bpl;
	return 0;
}

/* Decode exactly out_len planar bytes starting at `start`. `encoding` 0 means
 * a raw copy; 1 is ZSoft RLE. Fails on truncation or an RLE run that would
 * overrun the image, so a malformed file cannot read or write out of bounds. */
static int pcx_decode_data(const unsigned char *d, unsigned n, unsigned start,
			   int encoding, unsigned char *out, unsigned out_len)
{
	unsigned i = start, o = 0;

	if (encoding == 0)
	{
		if (start > n || n - start < out_len)
			return -1;
		memcpy(out, d + start, out_len);
		return 0;
	}
	while (o < out_len)
	{
		unsigned char b;
		unsigned run;

		if (i >= n)
			return -1;
		b = d[i++];
		if ((b & PCX_RLE_TAG) != PCX_RLE_TAG)
		{
			out[o++] = b;
			continue;
		}
		run = b & 0x3Fu;
		if (run == 0)
			return -1; /* a zero-length run is malformed */
		if (i >= n)
			return -1;
		if (run > out_len - o)
			return -1; /* run would overrun the raster */
		b = d[i++];
		while (run--)
			out[o++] = b;
	}
	return 0;
}

/* Read the 256-colour palette: the trailing 0x0C + 768 bytes when present,
 * otherwise the fixed VGA default. */
static void pcx_read_palette(const unsigned char *d, unsigned n, unsigned char *pal)
{
	if (n >= 769u && d[n - 769] == 0x0Cu)
		memcpy(pal, d + n - 768, 768);
	else
		pcx_vga_palette(pal);
}

int mmb_pcx_decode_rgba(const unsigned char *file, unsigned n,
			uint32_t **out, int *ow, int *oh)
{
	pcx_hdr hd;
	unsigned planar_len;
	unsigned char *planar = 0;
	uint32_t *pix = 0;
	unsigned char pal[768];
	int x, y, rc = -1;

	if (!file || !out || !ow || !oh)
		return -1;
	*out = 0;
	*ow = 0;
	*oh = 0;
	if (pcx_parse_header(file, n, &hd) != 0)
		return -1;

	planar_len = (unsigned)hd.h * (unsigned)hd.planes * hd.bpl;
	planar = G.plat->alloc(planar_len);
	pix = G.plat->alloc((unsigned)hd.w * (unsigned)hd.h * sizeof(uint32_t));
	if (!planar || !pix)
		goto done;

	if (pcx_decode_data(file, n, PCX_HEADER, hd.encoding, planar,
			    planar_len) != 0)
		goto done;

	if (hd.planes == 1)
	{
		pcx_read_palette(file, n, pal);
		for (y = 0; y < hd.h; y++)
			for (x = 0; x < hd.w; x++)
			{
				unsigned idx = planar[(unsigned)y * hd.bpl + (unsigned)x];
				unsigned r = pal[idx * 3];
				unsigned g = pal[idx * 3 + 1];
				unsigned b = pal[idx * 3 + 2];
				pix[(unsigned)y * (unsigned)hd.w + (unsigned)x] =
					0xFF000000u | mmb_rgb_pack((int)r, (int)g, (int)b);
			}
	}
	else
	{
		unsigned plane_size = (unsigned)hd.h * hd.bpl;
		for (y = 0; y < hd.h; y++)
			for (x = 0; x < hd.w; x++)
			{
				unsigned off = (unsigned)y * hd.bpl + (unsigned)x;
				unsigned r = planar[off];
				unsigned g = planar[plane_size + off];
				unsigned b = planar[2u * plane_size + off];
				pix[(unsigned)y * (unsigned)hd.w + (unsigned)x] =
					0xFF000000u | mmb_rgb_pack((int)r, (int)g, (int)b);
			}
	}

	*out = pix;
	*ow = hd.w;
	*oh = hd.h;
	pix = 0;
	rc = 0;

done:
	G.plat->free(planar);
	G.plat->free(pix);
	return rc;
}

int mmb_pcx_decode_indexed(const unsigned char *file, unsigned n,
			   unsigned char **out, int *ow, int *oh,
			   unsigned char *pal, int *ncol)
{
	pcx_hdr hd;
	unsigned planar_len;
	unsigned char *planar = 0;
	unsigned char *idx = 0;
	int y, rc = -1;

	if (!file || !out || !ow || !oh || !pal)
		return -1;
	*out = 0;
	*ow = 0;
	*oh = 0;
	if (ncol)
		*ncol = 0;
	if (pcx_parse_header(file, n, &hd) != 0)
		return -1;
	if (hd.planes != 1) /* indexed data is single-plane only */
		return -1;

	planar_len = (unsigned)hd.h * hd.bpl;
	planar = G.plat->alloc(planar_len);
	idx = G.plat->alloc((unsigned)hd.w * (unsigned)hd.h);
	if (!planar || !idx)
		goto done;

	if (pcx_decode_data(file, n, PCX_HEADER, hd.encoding, planar,
			    planar_len) != 0)
		goto done;

	for (y = 0; y < hd.h; y++)
		memcpy(idx + (unsigned)y * (unsigned)hd.w,
		       planar + (unsigned)y * hd.bpl, (unsigned)hd.w);

	pcx_read_palette(file, n, pal);
	if (ncol)
		*ncol = 256;

	*out = idx;
	*ow = hd.w;
	*oh = hd.h;
	idx = 0;
	rc = 0;

done:
	G.plat->free(planar);
	G.plat->free(idx);
	return rc;
}

int mmb_pcx_encode_indexed(const unsigned char *idx, int w, int h,
			   const unsigned char *pal,
			   unsigned char **out, unsigned *out_len)
{
	unsigned bpl, o, y;
	unsigned long total;
	unsigned char *buf;
	unsigned char vga[768];

	if (out)
		*out = 0;
	if (out_len)
		*out_len = 0;
	if (!idx || !out || !out_len || w <= 0 || h <= 0)
		return -1;
	if (w > MMB_PCX_MAX_DIM || h > MMB_PCX_MAX_DIM)
		return -1;
	if ((uint64_t)(unsigned)w * (unsigned)h > MMB_PCX_MAX_PIXELS)
		return -1;

	bpl = ((unsigned)w + 1u) & ~1u; /* scanlines are padded to even */
	/* Worst case every input byte emits a two-byte RLE pair. */
	total = (unsigned long)PCX_HEADER + (unsigned long)(unsigned)h * bpl * 2u +
		1u + 768u;
	buf = G.plat->alloc((unsigned)total);
	if (!buf)
		return -1;
	memset(buf, 0, PCX_HEADER);

	buf[0] = 0x0Au;
	buf[1] = 5;   /* version 5 */
	buf[2] = 1;   /* RLE */
	buf[3] = 8;   /* 8 bits per plane per pixel */
	buf[8] = (unsigned char)(((unsigned)(w - 1)) & 0xFFu);   /* xmax lo */
	buf[9] = (unsigned char)(((unsigned)(w - 1) >> 8) & 0xFFu);
	buf[10] = (unsigned char)(((unsigned)(h - 1)) & 0xFFu);  /* ymax lo */
	buf[11] = (unsigned char)(((unsigned)(h - 1) >> 8) & 0xFFu);
	buf[12] = 72; /* hres */
	buf[14] = 72; /* vres */
	buf[65] = 1;  /* one plane */
	buf[66] = (unsigned char)(bpl & 0xFFu);
	buf[67] = (unsigned char)((bpl >> 8) & 0xFFu);
	buf[68] = 1;  /* palette info: colour */

	o = PCX_HEADER;
	for (y = 0; y < (unsigned)h; y++)
	{
		const unsigned char *row = idx + (unsigned long)y * (unsigned)w;
		unsigned x = 0;

		while (x < bpl)
		{
			unsigned char v = x < (unsigned)w ? row[x] : 0;
			unsigned run = 1;

			while (x + run < bpl && run < 63u)
			{
				unsigned char nv = (x + run < (unsigned)w)
							   ? row[x + run]
							   : 0;
				if (nv != v)
					break;
				run++;
			}
			if (run > 1 || (v & PCX_RLE_TAG))
			{
				buf[o++] = (unsigned char)(PCX_RLE_TAG | run);
				buf[o++] = v;
			}
			else
				buf[o++] = v;
			x += run;
		}
	}

	buf[o++] = 0x0Cu; /* 256-colour palette marker */
	if (pal)
		memcpy(buf + o, pal, 768);
	else
	{
		pcx_vga_palette(vga);
		memcpy(buf + o, vga, 768);
	}
	o += 768;

	*out = buf;
	*out_len = o;
	return 0;
}
