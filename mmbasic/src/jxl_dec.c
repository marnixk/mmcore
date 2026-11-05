#include "jxl_dec.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define MMB_JXL_MAX_DIM 16384
#define MMB_JXL_HEAP_CAP (64u * 1024u * 1024u)

static size_t s_jxl_used;

typedef struct {
	size_t n;
} jxl_hdr;

static void *jxl_alloc(size_t n)
{
	jxl_hdr *h;

	if (n == 0)
		n = 1;
	if (s_jxl_used >= MMB_JXL_HEAP_CAP ||
	    n > MMB_JXL_HEAP_CAP - s_jxl_used - sizeof(jxl_hdr))
		return NULL;
	h = (jxl_hdr *)malloc(sizeof(jxl_hdr) + n);
	if (!h)
		return NULL;
	h->n = n;
	s_jxl_used += sizeof(jxl_hdr) + n;
	return h + 1;
}

static void *jxl_calloc(size_t a, size_t b)
{
	void *p;

	if (a && b > (size_t)-1 / a)
		return NULL;
	p = jxl_alloc(a * b);
	if (p)
		memset(p, 0, a * b);
	return p;
}

static void jxl_free(void *p)
{
	jxl_hdr *h;

	if (!p)
		return;
	h = (jxl_hdr *)p - 1;
	if (s_jxl_used >= sizeof(jxl_hdr) + h->n)
		s_jxl_used -= sizeof(jxl_hdr) + h->n;
	free(h);
}

static void *jxl_realloc(void *p, size_t n)
{
	jxl_hdr *h;
	void *q;

	if (!p)
		return jxl_alloc(n);
	h = (jxl_hdr *)p - 1;
	if (n <= h->n)
		return p;
	q = jxl_alloc(n);
	if (!q)
		return NULL;
	memcpy(q, p, h->n);
	jxl_free(p);
	return q;
}

static void jxl_qsort(void *base, size_t n, size_t size,
		      int (*cmp)(const void *, const void *))
{
	unsigned char *a = (unsigned char *)base;
	size_t gap, i, j, k;

	for (gap = n / 2; gap > 0; gap /= 2)
	{
		for (i = gap; i < n; i++)
		{
			for (j = i; j >= gap; j -= gap)
			{
				unsigned char *x = a + (j - gap) * size;
				unsigned char *y = a + j * size;

				if (cmp(x, y) <= 0)
					break;
				for (k = 0; k < size; k++)
				{
					unsigned char t = x[k];

					x[k] = y[k];
					y[k] = t;
				}
			}
		}
	}
}

#define J40_IMPLEMENTATION
#define J40_CONFIRM_THAT_THIS_IS_EXPERIMENTAL_AND_POTENTIALLY_UNSAFE
#define qsort jxl_qsort
#define J40_MALLOC jxl_alloc
#define J40_CALLOC jxl_calloc
#define J40_REALLOC jxl_realloc
#define J40_FREE jxl_free
#undef _POSIX_C_SOURCE
#undef _ISOC11_SOURCE
#include "j40.h"

typedef struct {
	const unsigned char *p;
	size_t bits;
	size_t nbits;
	int err;
} jxl_bits;

static unsigned jxl_u(jxl_bits *b, int n)
{
	unsigned v = 0;
	int i;

	for (i = 0; i < n; i++)
	{
		if (b->bits >= b->nbits)
		{
			b->err = 1;
			return 0;
		}
		v |= (unsigned)((b->p[b->bits >> 3] >> (b->bits & 7)) & 1u) << i;
		b->bits++;
	}
	return v;
}

static unsigned jxl_u32(jxl_bits *b, int o0, int n0, int o1, int n1,
			int o2, int n2, int o3, int n3)
{
	int sel = (int)jxl_u(b, 2);
	int n[4] = { n0, n1, n2, n3 };
	int ofs[4] = { o0, o1, o2, o3 };

	return jxl_u(b, n[sel]) + (unsigned)ofs[sel];
}

static uint32_t jxl_be32(const unsigned char *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint64_t jxl_be64(const unsigned char *p)
{
	return ((uint64_t)jxl_be32(p) << 32) | jxl_be32(p + 4);
}

static int jxl_codestream(const unsigned char *src, size_t len,
			  size_t *off, size_t *clen)
{
	size_t pos = 0;

	if (len >= 2 && src[0] == 0xff && src[1] == 0x0a)
	{
		*off = 0;
		*clen = len;
		return 0;
	}
	if (len < 12 || jxl_be32(src) != 12 ||
	    memcmp(src + 4, "JXL ", 4) != 0 ||
	    memcmp(src + 8, "\r\n\x87\n", 4) != 0)
		return -1;
	while (pos + 8 <= len)
	{
		uint64_t size;
		size_t hdr = 8;
		uint32_t type = jxl_be32(src + pos + 4);

		size = jxl_be32(src + pos);
		if (size == 1)
		{
			if (pos + 16 > len)
				return -1;
			size = jxl_be64(src + pos + 8);
			hdr = 16;
			if (size < 16)
				return -1;
		}
		else if (size == 0)
			size = len - pos;
		if (size < hdr || size > len - pos)
			return -1;
		if (type == 0x6a786c63)
		{
			*off = pos + hdr;
			*clen = (size_t)size - hdr;
			return 0;
		}
		if (type == 0x6a786c70)
		{
			if (size < hdr + 4)
				return -1;
			*off = pos + hdr + 4;
			*clen = (size_t)size - hdr - 4;
			return 0;
		}
		pos += (size_t)size;
	}
	return -1;
}

int mmb_jxl_probe(const unsigned char *src, size_t len, int *w, int *h)
{
	size_t off, clen;
	jxl_bits b;
	int div8, height, width, ratio;

	if (!src || !w || !h)
		return -1;
	if (jxl_codestream(src, len, &off, &clen) != 0 || clen < 2)
		return -1;
	memset(&b, 0, sizeof(b));
	b.p = src + off + 2;
	b.nbits = (clen - 2) * 8;
	div8 = (int)jxl_u(&b, 1);
	height = div8 ? (int)(jxl_u(&b, 5) + 1) * 8
		      : (int)jxl_u32(&b, 1, 9, 1, 13, 1, 18, 1, 30);
	ratio = (int)jxl_u(&b, 3);
	switch (ratio)
	{
	case 0:
		width = div8 ? (int)(jxl_u(&b, 5) + 1) * 8
			     : (int)jxl_u32(&b, 1, 9, 1, 13, 1, 18, 1, 30);
		break;
	case 1:
		width = height;
		break;
	case 2:
		width = (int)((uint64_t)height * 6 / 5);
		break;
	case 3:
		width = (int)((uint64_t)height * 4 / 3);
		break;
	case 4:
		width = (int)((uint64_t)height * 3 / 2);
		break;
	case 5:
		width = (int)((uint64_t)height * 16 / 9);
		break;
	case 6:
		width = (int)((uint64_t)height * 5 / 4);
		break;
	default:
		if (height >= 0x40000000)
			return -1;
		width = height * 2;
		break;
	}
	if (b.err || width < 1 || height < 1 ||
	    width > MMB_JXL_MAX_DIM || height > MMB_JXL_MAX_DIM)
		return -1;
	*w = width;
	*h = height;
	return 0;
}

int mmb_jxl_decode(const unsigned char *src, size_t len,
		   unsigned max_pixels, mmb_img_rgb *out)
{
	j40_image image;
	j40_frame frame;
	j40_pixels_u8x4 pixels;
	unsigned char *rgb;
	int w, h, y, x;

	if (!out)
		return -1;
	out->rgb = NULL;
	out->w = out->h = 0;
	if (!src || mmb_jxl_probe(src, len, &w, &h) != 0)
		return -1;
	if ((uint64_t)(unsigned)w * (unsigned)h > max_pixels)
		return -1;
	s_jxl_used = 0;
	j40_from_memory(&image, (void *)src, len, NULL);
	if (j40_error(&image))
	{
		j40_free(&image);
		return -1;
	}
	if (j40_output_format(&image, J40_RGBA, J40_U8X4) ||
	    j40_error(&image) || !j40_next_frame(&image) || j40_error(&image))
	{
		j40_free(&image);
		return -1;
	}
	frame = j40_current_frame(&image);
	pixels = j40_frame_pixels_u8x4(&frame, J40_RGBA);
	if (!pixels.data || pixels.width < 1 || pixels.height < 1 ||
	    (uint64_t)(unsigned)pixels.width * (unsigned)pixels.height >
		    max_pixels)
	{
		j40_free(&image);
		return -1;
	}
	w = pixels.width;
	h = pixels.height;
	rgb = (unsigned char *)malloc((size_t)w * (size_t)h * 3u);
	if (!rgb)
	{
		j40_free(&image);
		return -1;
	}
	for (y = 0; y < h; y++)
	{
		const unsigned char *row = (const unsigned char *)
			j40_row_u8x4(pixels, y);
		unsigned char *dst = rgb + (size_t)y * (size_t)w * 3u;

		for (x = 0; x < w; x++)
		{
			dst[x * 3] = row[x * 4];
			dst[x * 3 + 1] = row[x * 4 + 1];
			dst[x * 3 + 2] = row[x * 4 + 2];
		}
	}
	j40_free(&image);
	out->rgb = rgb;
	out->w = w;
	out->h = h;
	return 0;
}

typedef struct {
	const unsigned char *p;
	size_t len;
	size_t pos;
} ppm_reader;

static int ppm_skip(ppm_reader *r)
{
	while (r->pos < r->len)
	{
		unsigned char c = r->p[r->pos];

		if (c == '#')
		{
			while (r->pos < r->len && r->p[r->pos] != '\n')
				r->pos++;
			continue;
		}
		if (c == ' ' || c == '\t' || c == '\r' || c == '\n' ||
		    c == '\v' || c == '\f')
		{
			r->pos++;
			continue;
		}
		break;
	}
	return r->pos < r->len ? 0 : -1;
}

static int ppm_uint(ppm_reader *r, unsigned *out)
{
	unsigned v = 0;
	int any = 0;

	if (ppm_skip(r) != 0)
		return -1;
	while (r->pos < r->len)
	{
		unsigned char c = r->p[r->pos];

		if (c < '0' || c > '9')
			break;
		if (v > 0x7fffffffu)
			return -1;
		v = v * 10u + (unsigned)(c - '0');
		r->pos++;
		any = 1;
	}
	if (!any)
		return -1;
	*out = v;
	return 0;
}

static unsigned ppm_scale(unsigned v, unsigned maxval)
{
	if (maxval == 0)
		return 0;
	if (v > maxval)
		v = maxval;
	if (maxval == 255)
		return v;
	return (v * 255u + maxval / 2u) / maxval;
}

static int ppm_binary_start(ppm_reader *r)
{
	if (r->pos >= r->len)
		return -1;
	if (r->p[r->pos] == '\r' && r->pos + 1 < r->len &&
	    r->p[r->pos + 1] == '\n')
		r->pos += 2;
	else
		r->pos++;
	return r->pos <= r->len ? 0 : -1;
}

int mmb_ppm_decode(const unsigned char *src, size_t len,
		   unsigned max_pixels, mmb_img_rgb *out)
{
	ppm_reader r;
	unsigned magic, w, h, maxval = 255, v;
	unsigned char *rgb;
	size_t i, n;

	if (!out)
		return -1;
	out->rgb = NULL;
	out->w = out->h = 0;
	if (!src || len < 2 || src[0] != 'P' || src[1] < '1' || src[1] > '6')
		return -1;
	magic = (unsigned)(src[1] - '0');
	memset(&r, 0, sizeof(r));
	r.p = src;
	r.len = len;
	r.pos = 2;
	if (ppm_uint(&r, &w) != 0 || ppm_uint(&r, &h) != 0)
		return -1;
	if (magic != 1 && magic != 4 && ppm_uint(&r, &maxval) != 0)
		return -1;
	if (w < 1 || h < 1 || w > MMB_JXL_MAX_DIM || h > MMB_JXL_MAX_DIM ||
	    (uint64_t)w * h > max_pixels || maxval < 1 || maxval > 255)
		return -1;
	n = (size_t)w * (size_t)h;
	rgb = (unsigned char *)malloc(n * 3u);
	if (!rgb)
		return -1;

	if (magic == 6 || magic == 5)
	{
		if (ppm_binary_start(&r) != 0)
			goto fail;
		if (magic == 6)
		{
			if (r.len - r.pos < n * 3u)
				goto fail;
			for (i = 0; i < n; i++)
			{
				rgb[i * 3] = (unsigned char)ppm_scale(
					r.p[r.pos + i * 3], maxval);
				rgb[i * 3 + 1] = (unsigned char)ppm_scale(
					r.p[r.pos + i * 3 + 1], maxval);
				rgb[i * 3 + 2] = (unsigned char)ppm_scale(
					r.p[r.pos + i * 3 + 2], maxval);
			}
		}
		else
		{
			if (r.len - r.pos < n)
				goto fail;
			for (i = 0; i < n; i++)
			{
				unsigned char g = (unsigned char)ppm_scale(
					r.p[r.pos + i], maxval);

				rgb[i * 3] = rgb[i * 3 + 1] = rgb[i * 3 + 2] = g;
			}
		}
	}
	else if (magic == 4 || magic == 1)
	{
		size_t rowbytes = (w + 7u) / 8u;

		if (magic == 4)
		{
			if (ppm_binary_start(&r) != 0 ||
			    r.len - r.pos < rowbytes * h)
				goto fail;
			for (size_t yy = 0; yy < h; yy++)
				for (size_t xx = 0; xx < w; xx++)
				{
					unsigned bit = (r.p[r.pos + yy * rowbytes +
						(xx >> 3)] >> (7 - (xx & 7))) & 1u;
					unsigned char g = bit ? 0 : 255;

					i = yy * w + xx;
					rgb[i * 3] = rgb[i * 3 + 1] =
						rgb[i * 3 + 2] = g;
				}
		}
		else
		{
			for (i = 0; i < n; i++)
			{
				if (ppm_uint(&r, &v) != 0 || v > 1)
					goto fail;
				rgb[i * 3] = rgb[i * 3 + 1] =
					rgb[i * 3 + 2] = v ? 0 : 255;
			}
		}
	}
	else
	{
		unsigned comps = (magic == 3) ? 3u : 1u;

		for (i = 0; i < n; i++)
		{
			unsigned c[3];

			for (unsigned k = 0; k < comps; k++)
				if (ppm_uint(&r, &c[k]) != 0)
					goto fail;
			if (comps == 1)
				c[1] = c[2] = c[0];
			rgb[i * 3] = (unsigned char)ppm_scale(c[0], maxval);
			rgb[i * 3 + 1] = (unsigned char)ppm_scale(c[1], maxval);
			rgb[i * 3 + 2] = (unsigned char)ppm_scale(c[2], maxval);
		}
	}
	out->rgb = rgb;
	out->w = (int)w;
	out->h = (int)h;
	return 0;

fail:
	free(rgb);
	return -1;
}

void mmb_img_free(mmb_img_rgb *img)
{
	if (!img)
		return;
	free(img->rgb);
	img->rgb = NULL;
	img->w = img->h = 0;
}
