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

int mmb_zip_begin(mmb_zip_w *z)
{
	memset(z, 0, sizeof(*z));
	return 0;
}

void mmb_zip_abort(mmb_zip_w *z)
{
	if (z->buf)
		G.plat->free(z->buf);
	memset(z, 0, sizeof(*z));
}

int mmb_zip_add(mmb_zip_w *z, const char *name, const void *data, unsigned n)
{
	unsigned nlen, hdr, need, crc;
	unsigned char *p;
	if (!z || !name || !mmb_zip_path_ok(name))
		return -1;
	if (z->nent >= MMB_ZIP_MAX_FILES)
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
	if (nent > MMB_ZIP_MAX_FILES || cd_off >= n)
		return -1;
	pos = cd_off;
	for (i = 0; i < nent; i++)
	{
		unsigned method, flags, nlen, elen, clen, csz, usz, local, data_off;
		char name[128];
		const unsigned char *data;
		if (pos + 46 > n)
			return -1;
		if (u32le(zip + pos) != 0x02014b50u)
			return -1;
		flags = u16le(zip + pos + 8);
		method = u16le(zip + pos + 10);
		csz = u32le(zip + pos + 20);
		usz = u32le(zip + pos + 24);
		nlen = u16le(zip + pos + 28);
		elen = u16le(zip + pos + 30);
		clen = u16le(zip + pos + 32);
		local = u32le(zip + pos + 42);
		if (flags & 8)
			return -1;
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
		if (method != 0)
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
		if (data_off + csz > n || csz != usz)
			return -1;
		if (total + usz > MMB_ZIP_MAX_BYTES)
			return -1;
		total += usz;
		data = zip + data_off;
		if (fn(name, data, usz, ctx) != 0)
			return -1;
	}
	return 0;
}
