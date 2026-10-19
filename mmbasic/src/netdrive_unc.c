/*
 * Pure helpers for OPTION NETWORK DRIVE (#1128): UNC normalisation and
 * password hashing. Kept out of netfs_smb2.c so a host test can compile and
 * exercise them without libsmb2's socket layer or an interpreter context.
 */
#include "mmb_priv.h"

#ifdef MMB_HAVE_SMB2

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "md4.h"

static int netfs_is_sep(char c)
{
	return c == '\\' || c == '/';
}

int mmb_netdrive_normalize(const char *in, char *out, int outsz)
{
	char buf[256];
	char host[160], share[128], sub[160];
	int i = 0, o = 0, n;
	const char *p;

	if (!in || !out || outsz < 8)
		return 0;
	p = in;
	while (*p == ' ' || *p == '\t')
		p++;
	if ((p[0] == 's' || p[0] == 'S') && (p[1] == 'm' || p[1] == 'M') &&
	    (p[2] == 'b' || p[2] == 'B') && p[3] == ':')
		p += 4;
	strncpy(buf, p, sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = 0;

	while (buf[i] && netfs_is_sep(buf[i]))
		i++;
	o = 0;
	while (buf[i] && !netfs_is_sep(buf[i]) && o < (int)sizeof(host) - 1)
		host[o++] = buf[i++];
	host[o] = 0;
	if (!o)
		return 0;
	while (buf[i] && netfs_is_sep(buf[i]))
		i++;
	o = 0;
	while (buf[i] && !netfs_is_sep(buf[i]) && o < (int)sizeof(share) - 1)
		share[o++] = buf[i++];
	share[o] = 0;
	if (!o)
		return 0;
	o = 0;
	while (buf[i] && o < (int)sizeof(sub) - 1)
	{
		if (netfs_is_sep(buf[i]))
		{
			if (o && sub[o - 1] != '/')
				sub[o++] = '/';
			i++;
			continue;
		}
		sub[o++] = buf[i++];
	}
	while (o > 0 && sub[o - 1] == '/')
		o--;
	sub[o] = 0;

	n = snprintf(out, (size_t)outsz, "\\\\%s\\%s", host, share);
	if (n < 0 || n >= outsz)
		return 0;
	if (sub[0])
	{
		n = (int)strlen(out);
		if (n + 1 + (int)strlen(sub) >= outsz)
			return 0;
		out[n++] = '\\';
		for (i = 0; sub[i]; i++)
			out[n++] = sub[i] == '/' ? '\\' : sub[i];
		out[n] = 0;
	}
	return 1;
}

/* UTF-8 -> UTF-16LE; returns byte length, or -1 if out is too small. */
static int netfs_utf16le(const char *s, unsigned char *out, int outcap)
{
	int n = 0;
	const unsigned char *p = (const unsigned char *)s;

	while (*p)
	{
		unsigned cp;
		if (p[0] < 0x80)
			cp = *p++;
		else if ((p[0] & 0xe0) == 0xc0)
		{
			cp = (unsigned)(p[0] & 0x1f) << 6 | (p[1] & 0x3f);
			p += 2;
		}
		else if ((p[0] & 0xf0) == 0xe0)
		{
			cp = (unsigned)(p[0] & 0x0f) << 12 |
			     (unsigned)(p[1] & 0x3f) << 6 | (p[2] & 0x3f);
			p += 3;
		}
		else
		{
			cp = (unsigned)(p[0] & 0x07) << 18 |
			     (unsigned)(p[1] & 0x3f) << 12 |
			     (unsigned)(p[2] & 0x3f) << 6 | (p[3] & 0x3f);
			p += 4;
		}
		if (cp <= 0xffff)
		{
			if (n + 2 > outcap)
				return -1;
			out[n++] = (unsigned char)(cp & 0xff);
			out[n++] = (unsigned char)(cp >> 8);
		}
		else
		{
			unsigned v = cp - 0x10000;
			unsigned hi = 0xd800 + (v >> 10);
			unsigned lo = 0xdc00 + (v & 0x3ff);
			if (n + 4 > outcap)
				return -1;
			out[n++] = (unsigned char)(hi & 0xff);
			out[n++] = (unsigned char)(hi >> 8);
			out[n++] = (unsigned char)(lo & 0xff);
			out[n++] = (unsigned char)(lo >> 8);
		}
	}
	return n;
}

void mmb_netdrive_hash_password(const char *pass, char *out, int outsz)
{
	static const char hex[] = "0123456789abcdef";
	unsigned char w[512], hash[16];
	MD4_CTX ctx;
	int wlen, i;

	if (out && outsz > 0)
		out[0] = 0;
	if (!pass || !pass[0] || !out || outsz < 6)
		return;
	if (strncmp(pass, "ntlm:", 5) == 0)
	{
		/* Already hashed: keep it verbatim (bounded). */
		strncpy(out, pass, (size_t)outsz - 1);
		out[outsz - 1] = 0;
		return;
	}
	wlen = netfs_utf16le(pass, w, (int)sizeof(w));
	if (wlen < 0)
		return;
	MD4Init(&ctx);
	MD4Update(&ctx, w, (unsigned)wlen);
	MD4Final(hash, &ctx);
	if (outsz < 38)
		return;
	memcpy(out, "ntlm:", 5);
	for (i = 0; i < 16; i++)
	{
		out[5 + i * 2] = hex[hash[i] >> 4];
		out[6 + i * 2] = hex[hash[i] & 0xf];
	}
	out[37] = 0;
}

#endif /* MMB_HAVE_SMB2 */
