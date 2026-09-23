#ifdef HOST_PNG_TEST
#include "host_mmb_priv.h"
#else
#include "mmb_priv.h"
#include "upng.h"
#endif

/* Stack-safe PNG decoder: zlib stored blocks only (zlib.compress level 0). */

#define PNG_SIG_LEN 8

static unsigned read_be32(const unsigned char *p)
{
	return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) |
	       ((unsigned)p[2] << 8) | (unsigned)p[3];
}

static int png_inflate_stored(const unsigned char *in, unsigned in_len,
			      unsigned char **out, unsigned *out_len)
{
	unsigned pos = 0;
	unsigned char *buf = 0;
	unsigned cap = 0, len = 0;
	unsigned char cmf, flg;

	if (in_len < 6)
		return -1;
	cmf = in[pos++];
	flg = in[pos++];
	if ((cmf & 0x0F) != 8)
		return -1;
	if (((unsigned)cmf << 8 | flg) % 31 != 0)
		return -1;

	while (pos < in_len)
	{
		unsigned bfinal, btype, blen, nlen, i;

		if (pos >= in_len)
			break;
		bfinal = in[pos] & 1;
		btype = (in[pos] >> 1) & 3;
		pos++;
		if (btype != 0)
			return -1;
		if (pos + 4 > in_len)
			return -1;
		blen = (unsigned)in[pos] | ((unsigned)in[pos + 1] << 8);
		nlen = (unsigned)in[pos + 2] | ((unsigned)in[pos + 3] << 8);
		pos += 4;
		if ((blen ^ 0xFFFF) != nlen)
			return -1;
		if (pos + blen > in_len)
			return -1;
		if (len + blen > cap)
		{
			unsigned ncap = cap ? cap * 2 : 256;
			unsigned char *nb;
			while (ncap < len + blen)
				ncap *= 2;
			nb = G.plat->alloc(ncap);
			if (!nb)
			{
				G.plat->free(buf);
				return -1;
			}
			if (buf)
			{
				memcpy(nb, buf, len);
				G.plat->free(buf);
			}
			buf = nb;
			cap = ncap;
		}
		for (i = 0; i < blen; i++)
			buf[len++] = in[pos++];
		if (bfinal)
			break;
	}
	/* skip trailing adler32 if present */
	*out = buf;
	*out_len = len;
	return 0;
}

static int png_unfilter(unsigned char *raw, unsigned w, unsigned h, unsigned bpp)
{
	unsigned y, x;
	unsigned rowbytes = 1 + w * bpp;
	unsigned char *prev = 0;

	prev = G.plat->alloc(w * bpp);
	if (!prev)
		return -1;
	memset(prev, 0, w * bpp);

	for (y = 0; y < h; y++)
	{
		unsigned char *row = raw + y * rowbytes;
		unsigned char filt = row[0];
		unsigned char *cur = row + 1;

		if (filt == 0)
		{
			/* None */
		}
		else if (filt == 1)
		{
			for (x = 0; x < w * bpp; x++)
			{
				unsigned char left = x >= bpp ? cur[x - bpp] : 0;
				cur[x] = (unsigned char)(cur[x] + left);
			}
		}
		else
		{
			G.plat->free(prev);
			return -1;
		}
		memcpy(prev, cur, w * bpp);
	}
	G.plat->free(prev);
	return 0;
}

static int png_decode_stored(const unsigned char *file, unsigned n, int x, int y,
			    int has_trans, unsigned trans_rgb);

static int png_skip_plot(unsigned rgb, unsigned alpha, int has_trans, unsigned trans_rgb)
{
	if (alpha == 0)
		return 1;
	if (has_trans && rgb == trans_rgb)
		return 1;
	return 0;
}

int mmb_png_decode_rgba(const unsigned char *file, unsigned n,
			uint32_t **out, int *ow, int *oh)
{
#ifndef HOST_PNG_TEST
	upng_t *u;
	unsigned w, h, i, j;
	upng_format fmt;
	const unsigned char *buf;
	uint32_t *pix;
	unsigned bpp;

	*out = 0;
	if (!file || !n || !out || !ow || !oh)
		return -1;
	u = upng_new_from_bytes(file, n);
	if (!u)
		return -1;
	if (upng_header(u) != UPNG_EOK || upng_decode(u) != UPNG_EOK)
	{
		upng_free(u);
		return -1;
	}
	w = upng_get_width(u);
	h = upng_get_height(u);
	fmt = upng_get_format(u);
	buf = upng_get_buffer(u);
	if (!buf || !w || !h)
	{
		upng_free(u);
		return -1;
	}
	if (fmt == UPNG_RGB8)
		bpp = 3;
	else if (fmt == UPNG_RGBA8)
		bpp = 4;
	else
	{
		upng_free(u);
		return -1;
	}
	pix = G.plat->alloc(w * h * sizeof(uint32_t));
	if (!pix)
	{
		upng_free(u);
		return -1;
	}
	for (j = 0; j < h; j++)
		for (i = 0; i < w; i++)
		{
			const unsigned char *p = buf + (j * w + i) * bpp;
			unsigned a = (bpp == 4) ? p[3] : 255;
			unsigned rgb = mmb_rgb_pack(p[0], p[1], p[2]);
			pix[j * w + i] = (a == 0) ? 0 : (rgb | 0xFF000000u);
		}
	upng_free(u);
	*out = pix;
	*ow = (int)w;
	*oh = (int)h;
	return 0;
#else
	(void)file;
	(void)n;
	(void)out;
	(void)ow;
	(void)oh;
	return -1;
#endif
}

int mmb_png_decode(const unsigned char *file, unsigned n, int x, int y,
		  int has_trans, unsigned trans_rgb)
{
	uint32_t *pix = 0;
	int w = 0, h = 0, i, j, rc;

	rc = png_decode_stored(file, n, x, y, has_trans, trans_rgb);
	if (rc == 0)
		return 0;
	if (mmb_png_decode_rgba(file, n, &pix, &w, &h) == 0)
	{
		for (j = 0; j < h; j++)
			for (i = 0; i < w; i++)
			{
				unsigned c = pix[j * w + i];
				unsigned rgb = c & 0xFFFFFFu;
				unsigned a = (c >> 24) & 255u;
				if (png_skip_plot(rgb, a, has_trans, trans_rgb))
					continue;
				mmb_gfx_plot(x + i, y + j, rgb);
			}
		G.plat->free(pix);
		return 0;
	}
	return -1;
}

static int png_decode_stored(const unsigned char *file, unsigned n, int x, int y,
			    int has_trans, unsigned trans_rgb)
{
	unsigned pos = 0;
	unsigned w = 0, h = 0, bpp = 0;
	unsigned char bit_depth = 0, color_type = 0;
	unsigned char *idat = 0;
	unsigned idat_len = 0, idat_cap = 0;
	unsigned char *inflated = 0;
	unsigned inflated_len = 0;
	unsigned i, j;
	int rc = -1;

	if (n < PNG_SIG_LEN)
		return -1;
	if (memcmp(file, "\x89PNG\r\n\x1a\n", PNG_SIG_LEN) != 0)
		return -1;
	pos = PNG_SIG_LEN;

	while (pos + 12 <= n)
	{
		unsigned clen = read_be32(file + pos);
		const unsigned char *tag = file + pos + 4;
		const unsigned char *data = file + pos + 8;
		unsigned chunk_total = 12 + clen;

		if (pos + chunk_total > n)
			break;

		if (memcmp(tag, "IHDR", 4) == 0 && clen >= 13)
		{
			w = read_be32(data);
			h = read_be32(data + 4);
			bit_depth = data[8];
			color_type = data[9];
			if (bit_depth != 8)
				goto done;
			if (color_type == 2)
				bpp = 3;
			else if (color_type == 6)
				bpp = 4;
			else
				goto done;
		}
		else if (memcmp(tag, "IDAT", 4) == 0)
		{
			if (idat_len + clen > idat_cap)
			{
				unsigned ncap = idat_cap ? idat_cap * 2 : 256;
				unsigned char *nb;
				while (ncap < idat_len + clen)
					ncap *= 2;
				nb = G.plat->alloc(ncap);
				if (!nb)
					goto done;
				if (idat)
				{
					memcpy(nb, idat, idat_len);
					G.plat->free(idat);
				}
				idat = nb;
				idat_cap = ncap;
			}
			memcpy(idat + idat_len, data, clen);
			idat_len += clen;
		}
		else if (memcmp(tag, "IEND", 4) == 0)
			break;

		pos += chunk_total;
	}

	if (!w || !h || !idat)
		goto done;
	if (png_inflate_stored(idat, idat_len, &inflated, &inflated_len) != 0)
		goto done;
	if (inflated_len < h * (1 + w * bpp))
		goto done;
	if (png_unfilter(inflated, w, h, bpp) != 0)
		goto done;

	for (j = 0; j < h; j++)
	{
		const unsigned char *row = inflated + j * (1 + w * bpp) + 1;
		for (i = 0; i < w; i++)
		{
			const unsigned char *p = row + i * bpp;
			unsigned r = p[0], g = p[1], b = p[2];
			unsigned a = bpp == 4 ? p[3] : 255u;
			unsigned rgb = mmb_rgb_pack((int)r, (int)g, (int)b);
			if (png_skip_plot(rgb, a, has_trans, trans_rgb))
				continue;
			mmb_gfx_plot(x + (int)i, y + (int)j, rgb);
		}
	}
	rc = 0;

done:
	G.plat->free(idat);
	G.plat->free(inflated);
	return rc;
}

/* ---- PNG encoder (zlib stored blocks, RGB8) ------------------------ */

static unsigned png_crc_table[256];
static int png_crc_ready;

static void png_crc_build(void)
{
	unsigned c, i, k;
	if (png_crc_ready)
		return;
	for (i = 0; i < 256; i++)
	{
		c = i;
		for (k = 0; k < 8; k++)
			c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
		png_crc_table[i] = c;
	}
	png_crc_ready = 1;
}

static unsigned png_crc(const unsigned char *p, unsigned n)
{
	unsigned c = 0xFFFFFFFFu, i;
	png_crc_build();
	for (i = 0; i < n; i++)
		c = png_crc_table[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
	return c ^ 0xFFFFFFFFu;
}

static void png_put32(unsigned char *p, unsigned v)
{
	p[0] = (unsigned char)(v >> 24);
	p[1] = (unsigned char)(v >> 16);
	p[2] = (unsigned char)(v >> 8);
	p[3] = (unsigned char)v;
}

/* Encode packed RGB888 scanlines (w*3 bytes per row, no padding) as an 8-bit
 * truecolour PNG. Uses uncompressed zlib blocks so the decoder's stored-block
 * fast path (see png_decode_stored) handles it without an inflater. On success
 * *out points at a G.plat->alloc'd buffer holding the whole file; the caller
 * frees it with G.plat->free. */
int mmb_png_encode_rgb(const unsigned char *rgb, int w, int h,
		       unsigned char **out, unsigned *out_len)
{
	unsigned rowbytes, raw_len, nblocks, idat_len, total, o, pos;
	unsigned char *raw, *png;
	unsigned adler = 1, a = 1, b = 0;
	unsigned type_ofs, i, y;

	if (out)
		*out = 0;
	if (out_len)
		*out_len = 0;
	if (!rgb || !out || !out_len || w <= 0 || h <= 0)
		return -1;
	rowbytes = 1u + (unsigned)w * 3u;
	raw_len = rowbytes * (unsigned)h;
	nblocks = raw_len ? (raw_len + 65534u) / 65535u : 1u;
	idat_len = 2u + raw_len + 5u * nblocks + 4u;
	total = 8u + 25u + 12u + idat_len + 12u;

	raw = G.plat->alloc(raw_len ? raw_len : 1u);
	png = G.plat->alloc(total);
	if (!raw || !png)
	{
		G.plat->free(raw);
		G.plat->free(png);
		return -1;
	}
	for (y = 0; y < (unsigned)h; y++)
	{
		unsigned char *dst = raw + y * rowbytes;
		dst[0] = 0; /* filter: None */
		memcpy(dst + 1, rgb + (unsigned long)y * (unsigned)w * 3u,
		       (unsigned)w * 3u);
	}
	for (i = 0; i < raw_len; i++)
	{
		a = (a + raw[i]) % 65521u;
		b = (b + a) % 65521u;
	}
	adler = (b << 16) | a;

	o = 0;
	memcpy(png + o, "\x89PNG\r\n\x1a\n", PNG_SIG_LEN);
	o += PNG_SIG_LEN;

	png_put32(png + o, 13);
	o += 4;
	type_ofs = o;
	memcpy(png + o, "IHDR", 4);
	o += 4;
	png_put32(png + o, (unsigned)w);
	o += 4;
	png_put32(png + o, (unsigned)h);
	o += 4;
	png[o++] = 8; /* bit depth */
	png[o++] = 2; /* colour type: truecolour */
	png[o++] = 0;
	png[o++] = 0;
	png[o++] = 0;
	png_put32(png + o, png_crc(png + type_ofs, 4 + 13));
	o += 4;

	png_put32(png + o, idat_len);
	o += 4;
	type_ofs = o;
	memcpy(png + o, "IDAT", 4);
	o += 4;
	png[o++] = 0x78;
	png[o++] = 0x01;
	pos = 0;
	do
	{
		unsigned chunk = raw_len - pos;
		int last;
		if (chunk > 65535u)
			chunk = 65535u;
		last = (pos + chunk) >= raw_len;
		png[o++] = (unsigned char)(last ? 1 : 0);
		png[o++] = (unsigned char)(chunk & 0xFFu);
		png[o++] = (unsigned char)((chunk >> 8) & 0xFFu);
		png[o++] = (unsigned char)((~chunk) & 0xFFu);
		png[o++] = (unsigned char)(((~chunk) >> 8) & 0xFFu);
		if (chunk)
		{
			memcpy(png + o, raw + pos, chunk);
			o += chunk;
		}
		pos += chunk;
	} while (pos < raw_len);
	png_put32(png + o, adler);
	o += 4;
	png_put32(png + o, png_crc(png + type_ofs, 4 + idat_len));
	o += 4;

	png_put32(png + o, 0);
	o += 4;
	type_ofs = o;
	memcpy(png + o, "IEND", 4);
	o += 4;
	png_put32(png + o, png_crc(png + type_ofs, 4));
	o += 4;

	G.plat->free(raw);
	*out = png;
	*out_len = o;
	return 0;
}
