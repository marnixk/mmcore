#include "mmb_priv.h"

static unsigned u16le(const unsigned char *p)
{
	return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

static unsigned u32le(const unsigned char *p)
{
	return (unsigned)p[0] | ((unsigned)p[1] << 8) |
	       ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

static void put16le(unsigned char *p, unsigned v)
{
	p[0] = (unsigned char)v;
	p[1] = (unsigned char)(v >> 8);
}

static void put32le(unsigned char *p, unsigned v)
{
	p[0] = (unsigned char)v;
	p[1] = (unsigned char)(v >> 8);
	p[2] = (unsigned char)(v >> 16);
	p[3] = (unsigned char)(v >> 24);
}

unsigned mmb_crc32(const void *data, unsigned n)
{
	const unsigned char *p = (const unsigned char *)data;
	unsigned crc = 0xFFFFFFFFu;
	unsigned i, b;
	for (i = 0; i < n; i++)
	{
		crc ^= p[i];
		for (b = 0; b < 8; b++)
			crc = (crc & 1u) ? (crc >> 1) ^ 0xEDB88320u : (crc >> 1);
	}
	return ~crc;
}

int mmb_zip_path_ok(const char *name)
{
	char buf[128];
	char *p;
	int n = 0;
	if (!name || !name[0])
		return 0;
	while (name[n] && n < (int)sizeof(buf) - 1)
	{
		buf[n] = name[n] == '\\' ? '/' : name[n];
		n++;
	}
	buf[n] = 0;
	if (buf[0] == '/' || strchr(buf, ':'))
		return 0;
	p = buf;
	while (*p)
	{
		char *tok = p;
		while (*p && *p != '/')
			p++;
		if (*p == '/')
			*p++ = 0;
		if (!tok[0] || mmb_keyword_eq(tok, "."))
			continue;
		if (mmb_keyword_eq(tok, ".."))
			return 0;
	}
	return 1;
}

static int buf_grow(unsigned char **buf, unsigned *cap, unsigned used, unsigned need)
{
	unsigned ncap;
	unsigned char *p;
	if (need <= *cap)
		return 0;
	if (need > MMB_ZIP_MAX_BYTES)
		return -1;
	ncap = *cap ? *cap * 2 : 4096;
	while (ncap < need)
		ncap *= 2;
	if (ncap > MMB_ZIP_MAX_BYTES)
		ncap = MMB_ZIP_MAX_BYTES;
	if (need > ncap)
		return -1;
	p = G.plat->alloc(ncap);
	if (!p)
		return -1;
	if (*buf)
	{
		if (used)
			memcpy(p, *buf, used);
		G.plat->free(*buf);
	}
	*buf = p;
	*cap = ncap;
	return 0;
}

/* Grow the entry table so it can hold `need` entries. The old writer stored a
 * fixed MMB_ZIP_MAX_FILES array in the struct; the format's 16-bit EOCD count
 * is the only real ceiling, so keep the table on the heap and double it (#712).
 * Returns 0, or -1 on allocation failure / beyond the format limit. */
static int ent_grow(mmb_zip_w *z, unsigned need)
{
	unsigned ncap;
	mmb_zip_ent *p;
	if (need <= (unsigned)z->ent_cap)
		return 0;
	if (need > MMB_ZIP_MAX_FILES)
		return -1;
	ncap = z->ent_cap ? (unsigned)z->ent_cap * 2 : 16;
	while (ncap < need)
		ncap *= 2;
	if (ncap > MMB_ZIP_MAX_FILES)
		ncap = MMB_ZIP_MAX_FILES;
	if (need > ncap)
		return -1;
	p = G.plat->alloc(ncap * sizeof(*p));
	if (!p)
		return -1;
	if (z->ent)
	{
		if (z->nent)
			memcpy(p, z->ent, (unsigned)z->nent * sizeof(*p));
		G.plat->free(z->ent);
	}
	z->ent = p;
	z->ent_cap = (int)ncap;
	return 0;
}

int mmb_zip_begin(mmb_zip_w *z)
{
	memset(z, 0, sizeof(*z));
	return 0;
}

void mmb_zip_abort(mmb_zip_w *z)
{
	if (z->buf)
		G.plat->free(z->buf);
	if (z->ent)
		G.plat->free(z->ent);
	memset(z, 0, sizeof(*z));
}

int mmb_zip_add(mmb_zip_w *z, const char *name, const void *data, unsigned n)
{
	unsigned nlen, hdr, need, crc;
	unsigned char *p;
	if (!z || !name || !mmb_zip_path_ok(name))
		return -1;
	if (ent_grow(z, (unsigned)z->nent + 1) != 0)
		return -1;
	nlen = (unsigned)strlen(name);
	if (!nlen || nlen > 120)
		return -1;
	hdr = 30 + nlen;
	need = z->len + hdr + n;
	if (buf_grow(&z->buf, &z->cap, z->len, need) != 0)
		return -1;
	crc = mmb_crc32(data, n);
	p = z->buf + z->len;
	put32le(p, 0x04034b50u);
	put16le(p + 4, 20);
	put16le(p + 6, 0);
	put16le(p + 8, 0);
	put16le(p + 10, 0);
	put16le(p + 12, 0);
	put32le(p + 14, crc);
	put32le(p + 18, n);
	put32le(p + 22, n);
	put16le(p + 26, nlen);
	put16le(p + 28, 0);
	memcpy(p + 30, name, nlen);
	if (n && data)
		memcpy(p + 30 + nlen, data, n);
	z->ent[z->nent].local_off = z->len;
	z->ent[z->nent].size = n;
	z->ent[z->nent].crc = crc;
	z->ent[z->nent].nlen = nlen;
	strncpy(z->ent[z->nent].name, name, sizeof(z->ent[z->nent].name) - 1);
	z->ent[z->nent].name[sizeof(z->ent[z->nent].name) - 1] = 0;
	z->nent++;
	z->len = need;
	return 0;
}

int mmb_zip_finish(mmb_zip_w *z, unsigned char **out, unsigned *n)
{
	unsigned i, cd_off, cd_len = 0, eocd, need;
	if (!z || !out || !n)
		return -1;
	cd_off = z->len;
	for (i = 0; i < z->nent; i++)
		cd_len += 46 + z->ent[i].nlen;
	eocd = 22;
	need = z->len + cd_len + eocd;
	if (buf_grow(&z->buf, &z->cap, z->len, need) != 0)
		return -1;
	for (i = 0; i < z->nent; i++)
	{
		unsigned char *p = z->buf + z->len;
		unsigned nlen = z->ent[i].nlen;
		put32le(p, 0x02014b50u);
		put16le(p + 4, 20);
		put16le(p + 6, 20);
		put16le(p + 8, 0);
		put16le(p + 10, 0);
		put16le(p + 12, 0);
		put16le(p + 14, 0);
		put32le(p + 16, z->ent[i].crc);
		put32le(p + 20, z->ent[i].size);
		put32le(p + 24, z->ent[i].size);
		put16le(p + 28, nlen);
		put16le(p + 30, 0);
		put16le(p + 32, 0);
		put16le(p + 34, 0);
		put16le(p + 36, 0);
		put32le(p + 38, 0);
		put32le(p + 42, z->ent[i].local_off);
		memcpy(p + 46, z->ent[i].name, nlen);
		z->len += 46 + nlen;
	}
	{
		unsigned char *p = z->buf + z->len;
		put32le(p, 0x06054b50u);
		put16le(p + 4, 0);
		put16le(p + 6, 0);
		put16le(p + 8, z->nent);
		put16le(p + 10, z->nent);
		put32le(p + 12, cd_len);
		put32le(p + 16, cd_off);
		put16le(p + 20, 0);
		z->len += 22;
	}
	*out = z->buf;
	*n = z->len;
	if (z->ent)
		G.plat->free(z->ent);
	z->ent = 0;
	z->nent = 0;
	z->ent_cap = 0;
	z->buf = 0;
	z->cap = 0;
	z->len = 0;
	return 0;
}

static int find_eocd(const unsigned char *zip, unsigned n, unsigned *off)
{
	unsigned i, min;
	if (n < 22)
		return -1;
	min = n > 22 + 65535u ? n - (22 + 65535u) : 0;
	i = n - 22;
	for (;;)
	{
		if (zip[i] == 0x50 && zip[i + 1] == 0x4b &&
		    zip[i + 2] == 0x05 && zip[i + 3] == 0x06)
		{
			unsigned comment = u16le(zip + i + 20);
			if (i + 22 + comment == n)
			{
				*off = i;
				return 0;
			}
		}
		if (i == min)
			break;
		i--;
	}
	return -1;
}

int mmb_zip_foreach(const unsigned char *zip, unsigned n, mmb_zip_file_fn fn, void *ctx)
{
	unsigned eocd, cd_off, nent, i, pos, total = 0;
	if (!zip || n < 22 || !fn)
		return -1;
	if (n > MMB_ZIP_MAX_BYTES)
		return -1;
	if (find_eocd(zip, n, &eocd) != 0)
		return -1;
	nent = u16le(zip + eocd + 10);
	cd_off = u32le(zip + eocd + 16);
	if (cd_off >= n)
		return -1;
	pos = cd_off;
	for (i = 0; i < nent; i++)
	{
		unsigned method, nlen, elen, clen, csz, usz, crc, local, data_off;
		char name[128];
		if (pos + 46 > n)
			return -1;
		if (u32le(zip + pos) != 0x02014b50u)
			return -1;
		method = u16le(zip + pos + 10);
		crc = u32le(zip + pos + 16);
		csz = u32le(zip + pos + 20);
		usz = u32le(zip + pos + 24);
		nlen = u16le(zip + pos + 28);
		elen = u16le(zip + pos + 30);
		clen = u16le(zip + pos + 32);
		local = u32le(zip + pos + 42);
		if (pos + 46 + nlen + elen + clen > n || nlen >= sizeof(name))
			return -1;
		memcpy(name, zip + pos + 46, nlen);
		name[nlen] = 0;
		pos += 46 + nlen + elen + clen;
		if (nlen && name[nlen - 1] == '/')
		{
			name[nlen - 1] = 0;
			if (!name[0] || !mmb_zip_path_ok(name))
				return -1;
			if (fn(name, 0, 0, ctx) != 0)
				return -1;
			continue;
		}
		if (!mmb_zip_path_ok(name))
			return -1;
		if (local + 30 > n)
			return -1;
		if (u32le(zip + local) != 0x04034b50u)
			return -1;
		{
			unsigned ln = u16le(zip + local + 26);
			unsigned lx = u16le(zip + local + 28);
			data_off = local + 30 + ln + lx;
		}
		if (data_off + csz > n)
			return -1;
		if (total + usz > MMB_ZIP_MAX_BYTES)
			return -1;
		if (method == 0)
		{
			if (csz != usz || mmb_crc32(zip + data_off, usz) != crc)
				return -1;
			total += usz;
			if (fn(name, zip + data_off, usz, ctx) != 0)
				return -1;
		}
		else if (method == 8)
		{
			unsigned char *out;
			unsigned outn = 0;
			int r;
			if (usz == 0)
			{
				total += 0;
				if (fn(name, zip + data_off, 0, ctx) != 0)
					return -1;
				continue;
			}
			out = G.plat->alloc(usz);
			if (!out)
				return -1;
			if (mmb_inflate(zip + data_off, csz, out, usz, &outn) != 0 ||
			    outn != usz || mmb_crc32(out, outn) != crc)
			{
				G.plat->free(out);
				return -1;
			}
			total += outn;
			r = fn(name, out, outn, ctx);
			G.plat->free(out);
			if (r != 0)
				return -1;
		}
		else
		{
			return -1;
		}
	}
	return 0;
}

/* Read one entry by walking the central directory through a positional
 * reader. Only the ZIP tail, the central directory headers and the chosen
 * entry's local header + data are fetched, so inspecting metadata for a large
 * package costs a few small reads rather than the whole archive. */
int mmb_zip_read_entry(mmb_zip_range_fn rd, void *ctx, unsigned zip_size,
		       const char *name, void *out, unsigned out_cap,
		       unsigned *out_len)
{
	unsigned char tail[1024];
	unsigned char *big = 0;
	const unsigned char *buf = tail;
	unsigned tail_n, eocd, nent, cd_off, pos, i;
	unsigned method = 0, crc = 0, csz = 0, usz = 0, local = 0;
	unsigned name_len, got;
	int found = 0, rc = -1;

	if (out_len)
		*out_len = 0;
	if (!rd || !name || !name[0] || !out || !out_cap || zip_size < 22)
		return -1;
	name_len = (unsigned)strlen(name);
	tail_n = zip_size < sizeof(tail) ? zip_size : (unsigned)sizeof(tail);
	if (rd(ctx, zip_size - tail_n, tail, tail_n, &got) != 0 ||
	    got != tail_n)
		return -1;
	if (find_eocd(tail, tail_n, &eocd) != 0)
	{
		/* An archive comment can push the EOCD beyond the first tail
		 * read; retry once with the ZIP-format maximum tail. */
		unsigned want = zip_size < 22 + 65535u ? zip_size : 22 + 65535u;
		if (want <= tail_n)
			return -1;
		big = G.plat->alloc(want);
		if (!big)
			return -1;
		if (rd(ctx, zip_size - want, big, want, &got) != 0 ||
		    got != want || find_eocd(big, want, &eocd) != 0)
			goto done;
		buf = big;
	}
	nent = u16le(buf + eocd + 10);
	cd_off = u32le(buf + eocd + 16);
	if (cd_off >= zip_size || nent == 0)
		goto done;
	pos = cd_off;
	for (i = 0; i < nent; i++)
	{
		unsigned char h[46];
		unsigned nlen, elen, clen;
		if (pos + 46 > zip_size)
			break;
		if (rd(ctx, pos, h, 46, &got) != 0 || got != 46)
			break;
		if (u32le(h) != 0x02014b50u)
			break;
		method = u16le(h + 10);
		crc = u32le(h + 16);
		csz = u32le(h + 20);
		usz = u32le(h + 24);
		nlen = u16le(h + 28);
		elen = u16le(h + 30);
		clen = u16le(h + 32);
		local = u32le(h + 42);
		if (pos + 46 + nlen + elen + clen > zip_size)
			break;
		if (nlen == name_len && nlen < 128)
		{
			char ename[128];
			if (rd(ctx, pos + 46, ename, nlen, &got) == 0 &&
			    got == nlen)
			{
				ename[nlen] = 0;
				if (mmb_keyword_eq(ename, name))
				{
					found = 1;
					break;
				}
			}
		}
		pos += 46 + nlen + elen + clen;
	}
	if (!found || usz > out_cap)
		goto done;
	{
		unsigned char lh[30];
		unsigned ln, lx, data_off;

		if (local + 30 > zip_size)
			goto done;
		if (rd(ctx, local, lh, 30, &got) != 0 || got != 30 ||
		    u32le(lh) != 0x04034b50u)
			goto done;
		ln = u16le(lh + 26);
		lx = u16le(lh + 28);
		data_off = local + 30 + ln + lx;
		if (data_off + csz > zip_size)
			goto done;
		if (method == 0)
		{
			if (csz != usz ||
			    rd(ctx, data_off, out, usz, &got) != 0 ||
			    got != usz || mmb_crc32(out, usz) != crc)
				goto done;
			if (out_len)
				*out_len = usz;
			rc = 0;
		}
		else if (method == 8)
		{
			unsigned char *comp;
			unsigned outn = 0;

			if (usz == 0)
			{
				if (out_len)
					*out_len = 0;
				rc = 0;
				goto done;
			}
			comp = G.plat->alloc(csz ? csz : 1);
			if (!comp)
				goto done;
			if (rd(ctx, data_off, comp, csz, &got) != 0 ||
			    got != csz ||
			    mmb_inflate(comp, csz, out, out_cap, &outn) != 0 ||
			    outn != usz || mmb_crc32(out, outn) != crc)
			{
				G.plat->free(comp);
				goto done;
			}
			G.plat->free(comp);
			if (out_len)
				*out_len = outn;
			rc = 0;
		}
	}
done:
	if (big)
		G.plat->free(big);
	return rc;
}
