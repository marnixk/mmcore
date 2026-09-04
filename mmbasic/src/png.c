#ifdef HOST_PNG_TEST
#include "host_mmb_priv.h"
#else
#include "mmb_priv.h"
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

int mmb_png_decode(const unsigned char *file, unsigned n, int x, int y)
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
			mmb_gfx_plot(x + (int)i, y + (int)j, mmb_rgb_pack((int)r, (int)g, (int)b));
		}
	}
	rc = 0;

done:
	G.plat->free(idat);
	G.plat->free(inflated);
	return rc;
}
