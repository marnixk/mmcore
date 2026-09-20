/*
 * ZMODEM receive-only engine (see mmb_zmodem.h).
 *
 * Wire format follows Chuck Forsberg's "The ZMODEM Inter Application File
 * Transfer Protocol": ZPAD/ZDLE framed 16-bit or 32-bit FCS headers, ZDLE
 * escaped binary data subpackets, ZFILE/ZRPOS/ZDATA/ZACK/ZEOF/ZFIN session
 * flow.  The receiver sends hex headers only; the FCS of a data subpacket is
 * taken from the header that introduced it (hex/ZBIN => CRC-16, ZBIN32 =>
 * CRC-32), which is exactly how the sender switches (sz's Crc32t).
 */
#include "mmb_zmodem.h"
#include <string.h>

#define ZPAD  0x2A
#define ZDLE  0x18
#define ZBIN  'A'
#define ZHEX  'B'
#define ZBIN32 'C'
#define ZCRCE 'h'
#define ZCRCG 'i'
#define ZCRCQ 'j'
#define ZCRCW 'k'
#define ZRUB0 'l'
#define ZRUB1 'm'

enum {
	ZRQINIT = 0, ZRINIT = 1, ZSINIT = 2, ZACK = 3, ZFILE = 4,
	ZSKIP = 5, ZNAK = 6, ZABORT = 7, ZFIN = 8, ZRPOS = 9,
	ZDATA = 10, ZEOF = 11, ZFERR = 12, ZCRC = 13, ZCHALLENGE = 14,
	ZCOMPL = 15, ZCAN = 16, ZFREECNT = 17, ZCOMMAND = 18
};

/* Parser states. */
enum {
	PS_HDR = 0,		/* scanning for ZPAD */
	PS_PAD,			/* saw one ZPAD */
	PS_FMT,			/* saw ZPAD ZDLE, want format byte */
	PS_HEX,			/* collecting hex header body */
	PS_HEXT,		/* hex header CR/LF/XON tail */
	PS_HEXT2,		/* CR seen, want LF */
	PS_BIN,			/* collecting binary header body */
	PS_SUB,			/* data subpacket body */
	PS_SUBESC,		/* data subpacket after ZDLE */
	PS_CRC			/* data subpacket FCS bytes */
};

/* What a completed subpacket belongs to. */
enum { ROLE_NONE = 0, ROLE_SINIT, ROLE_ZFILE, ROLE_DATA };

/* Receiver capabilities: 32-bit FCS, 1024 byte buffer (segmented ZCRCW). */
#define ZM_CAPS   0x20u
#define ZM_BUF    0x0400u

static unsigned crc16_update(unsigned crc, unsigned b)
{
	int i;
	crc ^= (b & 0xFFu) << 8;
	for (i = 0; i < 8; i++)
		crc = (crc & 0x8000u) ? ((crc << 1) ^ 0x1021u) : (crc << 1);
	return crc & 0xFFFFu;
}

static unsigned crc16_bytes(const unsigned char *p, unsigned n)
{
	unsigned crc = 0;
	while (n--)
		crc = crc16_update(crc, *p++);
	return crc;
}

static unsigned crc32_update(unsigned crc, unsigned b)
{
	int i;
	crc ^= (b & 0xFFu);
	for (i = 0; i < 8; i++)
		crc = (crc & 1u) ? ((crc >> 1) ^ 0xEDB88320u) : (crc >> 1);
	return crc;
}

static unsigned crc32_bytes(const unsigned char *p, unsigned n)
{
	unsigned crc = 0xFFFFFFFFu;
	while (n--)
		crc = crc32_update(crc, *p++);
	return crc ^ 0xFFFFFFFFu;
}

static int hexval(int c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

static void send_hdr(mmb_zm_rx *z, int type, unsigned char d0, unsigned char d1,
		     unsigned char d2, unsigned char d3)
{
	static const char hx[] = "0123456789abcdef";
	unsigned char body[7];
	unsigned char out[32];
	unsigned crc = 0;
	int i, n = 0;

	body[0] = (unsigned char)type;
	body[1] = d0;
	body[2] = d1;
	body[3] = d2;
	body[4] = d3;
	for (i = 0; i < 5; i++)
		crc = crc16_update(crc, body[i]);
	body[5] = (unsigned char)(crc >> 8);
	body[6] = (unsigned char)crc;

	out[n++] = ZPAD;
	out[n++] = ZPAD;
	out[n++] = ZDLE;
	out[n++] = ZHEX;
	for (i = 0; i < 7; i++)
	{
		out[n++] = (unsigned char)hx[body[i] >> 4];
		out[n++] = (unsigned char)hx[body[i] & 15];
	}
	out[n++] = '\r';
	out[n++] = '\n';
	if (type != ZACK && type != ZFIN)
		out[n++] = 0x11;	/* XON uncorks a spurious XOFF */
	z->ops->send(z->ops->ctx, out, (unsigned)n);
	z->rep[0] = d0;
	z->rep[1] = d1;
	z->rep[2] = d2;
	z->rep[3] = d3;
	z->rep_type = type;
}

static void reply_pos(mmb_zm_rx *z, int type, unsigned pos)
{
	send_hdr(z, type, (unsigned char)pos, (unsigned char)(pos >> 8),
		 (unsigned char)(pos >> 16), (unsigned char)(pos >> 24));
}

static void reply_rinit(mmb_zm_rx *z)
{
	send_hdr(z, ZRINIT, (unsigned char)ZM_BUF, (unsigned char)(ZM_BUF >> 8),
		 0, (unsigned char)ZM_CAPS);
}

static void set_status(mmb_zm_rx *z, const char *s)
{
	unsigned i = 0;
	if (s)
		for (; s[i] && i + 1 < MMB_ZM_STATUS; i++)
			z->status[i] = s[i];
	z->status[i] = 0;
}

static void finish(mmb_zm_rx *z, int st)
{
	if (z->file_open)
	{
		z->ops->close(z->ops->ctx, st == MMB_ZM_DONE);
		z->file_open = 0;
	}
	z->state = st;
	set_status(z, st == MMB_ZM_DONE ? "done" :
		   st == MMB_ZM_CANCELLED ? "cancelled" : "failed");
}

void mmb_zm_init(mmb_zm_rx *z, const mmb_zm_ops *ops)
{
	memset(z, 0, sizeof(*z));
	z->ops = ops;
	z->state = MMB_ZM_IDLE;
	z->rep_type = -1;
}

void mmb_zm_begin(mmb_zm_rx *z)
{
	z->state = MMB_ZM_ACTIVE;
	z->ps = PS_HDR;
	z->role = ROLE_NONE;
	z->hdr32 = 0;
	z->sub32 = 0;
	z->last_ms = 0;
	z->bytes = 0;
	z->size = 0;
	z->got = 0;
	z->files = 0;
	z->file_open = 0;
	z->synced = 0;
	z->name[0] = 0;
	z->hex_n = 0;
	z->hex_byte = -1;
	z->bin_n = 0;
	z->bin_esc = 0;
	z->sub_n = 0;
	z->sub_esc = 0;
	z->crc_n = 0;
	z->rep_type = -1;
	z->rep_count = 0;
	set_status(z, "starting");
}

int mmb_zm_active(const mmb_zm_rx *z)
{
	return z->state == MMB_ZM_ACTIVE;
}

const char *mmb_zm_status(const mmb_zm_rx *z)
{
	return z->status;
}

const char *mmb_zm_name(const mmb_zm_rx *z)
{
	return z->name;
}

unsigned mmb_zm_progress(const mmb_zm_rx *z, unsigned *total)
{
	if (total)
		*total = z->size;
	return z->got;
}

int mmb_zm_files(const mmb_zm_rx *z)
{
	return z->files;
}

void mmb_zm_forget(mmb_zm_rx *z)
{
	z->state = MMB_ZM_IDLE;
	z->name[0] = 0;
	set_status(z, "");
}

void mmb_zm_cancel(mmb_zm_rx *z)
{
	if (z->state != MMB_ZM_ACTIVE)
		return;
	send_hdr(z, ZABORT, 0, 0, 0, 0);
	/* Session abort sequence: enough CANs to stop a streaming sender. */
	{
		const unsigned char can[8] = {
			ZDLE, ZDLE, ZDLE, ZDLE, ZDLE, ZDLE, ZDLE, ZDLE
		};
		z->ops->send(z->ops->ctx, can, sizeof(can));
	}
	finish(z, MMB_ZM_CANCELLED);
}

static void accept_header(mmb_zm_rx *z)
{
	unsigned char d0 = z->hbody[1], d1 = z->hbody[2];
	unsigned char d2 = z->hbody[3], d3 = z->hbody[4];
	unsigned pos = (unsigned)d0 | ((unsigned)d1 << 8) |
		       ((unsigned)d2 << 16) | ((unsigned)d3 << 24);
	int type = z->hbody[0];

	z->ps = PS_HDR;
	z->synced = 1;
	switch (type)
	{
	case ZRQINIT:
		reply_rinit(z);
		set_status(z, "connected");
		break;
	case ZRINIT:
		break;
	case ZSINIT:
		z->role = ROLE_SINIT;
		z->sub_n = 0;
		z->sub32 = z->hdr32;
		z->ps = PS_SUB;
		break;
	case ZFILE:
		z->role = ROLE_ZFILE;
		z->sub_n = 0;
		z->sub32 = z->hdr32;
		z->ps = PS_SUB;
		break;
	case ZDATA:
		if (!z->file_open)
			break;
		if (z->got != pos)
		{
			reply_pos(z, ZRPOS, z->got);
			break;
		}
		z->role = ROLE_DATA;
		z->sub_n = 0;
		z->sub32 = z->hdr32;
		z->ps = PS_SUB;
		break;
	case ZEOF:
		if (z->file_open && z->got == pos)
		{
			z->ops->close(z->ops->ctx, 1);
			z->file_open = 0;
			z->files++;
			set_status(z, "saved");
			reply_rinit(z);
		}
		else if (z->file_open)
			reply_pos(z, ZRPOS, z->got);
		else
			reply_rinit(z);
		z->role = ROLE_NONE;
		break;
	case ZFIN:
		finish(z, MMB_ZM_DONE);
		send_hdr(z, ZFIN, 0, 0, 0, 0);
		{
			const unsigned char oo[2] = { 'O', 'O' };
			z->ops->send(z->ops->ctx, oo, 2);
		}
		break;
	case ZABORT:
		finish(z, MMB_ZM_FAILED);
		break;
	case ZFERR:
		finish(z, MMB_ZM_FAILED);
		break;
	case ZCAN:
		finish(z, MMB_ZM_CANCELLED);
		break;
	default:
		break;
	}
}

static void header_bad(mmb_zm_rx *z)
{
	if (!z->synced)
	{
		/* Auto-detect misfire: leave quietly without sending anything. */
		finish(z, MMB_ZM_FAILED);
		return;
	}
	send_hdr(z, ZNAK, 0, 0, 0, 0);
	z->ps = PS_HDR;
	z->role = ROLE_NONE;
}

static void zfile_ready(mmb_zm_rx *z)
{
	char name[MMB_ZM_MAX_NAME];
	unsigned size = 0, i, nlen;
	int rc;

	nlen = 0;
	while (nlen < (unsigned)z->sub_n && z->sub[nlen])
		nlen++;
	if (nlen == 0 || nlen >= sizeof(name))
	{
		send_hdr(z, ZSKIP, 0, 0, 0, 0);
		z->role = ROLE_NONE;
		z->ps = PS_HDR;
		return;
	}
	memcpy(name, z->sub, nlen);
	name[nlen] = 0;
	i = nlen + 1;
	while (i < (unsigned)z->sub_n && z->sub[i] == ' ')
		i++;
	while (i < (unsigned)z->sub_n && z->sub[i] >= '0' && z->sub[i] <= '9')
		size = size * 10 + (unsigned)(z->sub[i++] - '0');

	if (z->file_open)
	{
		z->ops->close(z->ops->ctx, 0);
		z->file_open = 0;
	}
	rc = z->ops->open(z->ops->ctx, name, size);
	if (rc != 0)
	{
		send_hdr(z, ZSKIP, 0, 0, 0, 0);
		z->role = ROLE_NONE;
		z->ps = PS_HDR;
		return;
	}
	z->file_open = 1;
	z->got = 0;
	z->size = size;
	for (i = 0; name[i] && i + 1 < MMB_ZM_MAX_NAME; i++)
		z->name[i] = name[i];
	z->name[i] = 0;
	set_status(z, "receiving");
	reply_pos(z, ZRPOS, 0);
	z->role = ROLE_NONE;
	z->ps = PS_HDR;
}

static int sub_crc_ok(mmb_zm_rx *z)
{
	unsigned exp;
	if (z->sub32)
	{
		unsigned char tmp[MMB_ZM_MAX_SUB + 1];
		if (z->sub_n > MMB_ZM_MAX_SUB)
			return 0;
		memcpy(tmp, z->sub, (unsigned)z->sub_n);
		tmp[z->sub_n] = (unsigned char)z->sub_end;
		exp = crc32_bytes(tmp, (unsigned)z->sub_n + 1);
		/* The 32-bit FCS goes out least significant byte first (lrzsz
		 * zsda32); a big-endian compare rejects every real sender. */
		return z->crcbuf[0] == (unsigned char)exp &&
		       z->crcbuf[1] == (unsigned char)(exp >> 8) &&
		       z->crcbuf[2] == (unsigned char)(exp >> 16) &&
		       z->crcbuf[3] == (unsigned char)(exp >> 24);
	}
	exp = crc16_update(crc16_bytes(z->sub, (unsigned)z->sub_n), z->sub_end);
	return z->crcbuf[0] == (unsigned char)(exp >> 8) &&
	       z->crcbuf[1] == (unsigned char)exp;
}

static void sub_bad(mmb_zm_rx *z)
{
	send_hdr(z, ZNAK, 0, 0, 0, 0);
	if (z->file_open)
		reply_pos(z, ZRPOS, z->got);
	z->role = ROLE_NONE;
	z->ps = PS_HDR;
	z->sub_n = 0;
}

static void accept_sub(mmb_zm_rx *z)
{
	int end = z->sub_end;

	if (!sub_crc_ok(z))
	{
		sub_bad(z);
		return;
	}
	if (z->role == ROLE_SINIT)
	{
		reply_pos(z, ZACK, 0);
		z->role = ROLE_NONE;
		z->ps = PS_HDR;
		return;
	}
	if (z->role == ROLE_ZFILE)
	{
		zfile_ready(z);
		return;
	}
	if (z->role == ROLE_DATA && z->file_open)
	{
		if (z->sub_n > 0)
		{
			if (z->ops->write(z->ops->ctx, z->sub,
					  (unsigned)z->sub_n) != 0)
			{
				finish(z, MMB_ZM_FAILED);
				return;
			}
			z->got += (unsigned)z->sub_n;
			z->bytes += (unsigned)z->sub_n;
		}
		if (end == ZCRCG)
			z->ps = PS_SUB;
		else if (end == ZCRCQ)
		{
			/* Frame continues; ZACK the offset and stay in the frame. */
			reply_pos(z, ZACK, z->got);
			z->ps = PS_SUB;
		}
		else if (end == ZCRCW)
		{
			/* Frame ends; ZACK it and return to header parsing. */
			reply_pos(z, ZACK, z->got);
			z->ps = PS_HDR;
		}
		else /* ZCRCE ends the frame; ZEOF follows */
			z->ps = PS_HDR;
		z->sub_n = 0;
		return;
	}
	z->role = ROLE_NONE;
	z->ps = PS_HDR;
	z->sub_n = 0;
}

static void parse_byte(mmb_zm_rx *z, int b)
{
	int v;

	for (;;)
	{
		switch (z->ps)
		{
		case PS_HDR:
			if (b == ZPAD)
				z->ps = PS_PAD;
			return;
		case PS_PAD:
			if (b == ZPAD)
				return;
			if (b == ZDLE)
			{
				z->ps = PS_FMT;
				return;
			}
			z->ps = PS_HDR;
			continue;
		case PS_FMT:
			if (b == ZHEX)
			{
				z->hdr32 = 0;
				z->hex_n = 0;
				z->hex_byte = -1;
				z->ps = PS_HEX;
			}
			else if (b == ZBIN)
			{
				z->hdr32 = 0;
				z->bin_n = 0;
				z->bin_need = 7;
				z->bin_esc = 0;
				z->ps = PS_BIN;
			}
			else if (b == ZBIN32)
			{
				z->hdr32 = 1;
				z->bin_n = 0;
				z->bin_need = 9;
				z->bin_esc = 0;
				z->ps = PS_BIN;
			}
			else
				z->ps = PS_HDR;
			return;
		case PS_HEX:
			v = hexval(b);
			if (v < 0)
			{
				z->ps = PS_HDR;
				continue;
			}
			if (z->hex_byte < 0)
			{
				z->hex_byte = v;
				return;
			}
			z->hbody[z->hex_n++] =
				(unsigned char)((z->hex_byte << 4) | v);
			z->hex_byte = -1;
			if (z->hex_n < 7)
				return;
			if (crc16_bytes(z->hbody, 5) !=
			    ((unsigned)z->hbody[5] << 8 | z->hbody[6]))
			{
				header_bad(z);
				return;
			}
			z->ps = PS_HEXT;
			z->hex_n = 0;
			return;
		case PS_HEXT:
			if (b == '\r')
			{
				z->ps = PS_HEXT2;
				return;
			}
			if (b == '\n')
			{
				accept_header(z);
				return;
			}
			accept_header(z);
			continue;
		case PS_HEXT2:
			if (b == '\n')
			{
				accept_header(z);
				return;
			}
			accept_header(z);
			continue;
		case PS_BIN:
			if (z->bin_esc)
			{
				z->bin_esc = 0;
				if (b == 0x11 || b == 0x13 || b == 0x91 ||
				    b == 0x93)
				{
					z->bin_esc = 1;
					return;
				}
				if (b == ZRUB0)
					b = 0x7F;
				else if (b == ZRUB1)
					b = 0xFF;
				else
					b = (b ^ 0x40) & 0xFF;
			}
			else if (b == ZDLE)
			{
				z->bin_esc = 1;
				return;
			}
			else if (b == 0x11 || b == 0x13 || b == 0x91 ||
				 b == 0x93)
				return;
			z->hbody[z->bin_n++] = (unsigned char)b;
			if (z->bin_n < z->bin_need)
				return;
			{
				int ok;
				if (z->hdr32)
					/* 32-bit FCS is transmitted low byte
					 * first (lrzsz zsbh32). */
					ok = crc32_bytes(z->hbody, 5) ==
					     ((unsigned)z->hbody[5] |
					      (unsigned)z->hbody[6] << 8 |
					      (unsigned)z->hbody[7] << 16 |
					      (unsigned)z->hbody[8] << 24);
				else
					ok = crc16_bytes(z->hbody, 5) ==
					     ((unsigned)z->hbody[5] << 8 |
					      z->hbody[6]);
				if (!ok)
				{
					header_bad(z);
					return;
				}
			}
			accept_header(z);
			return;
		case PS_SUB:
			if (b == ZDLE)
			{
				z->ps = PS_SUBESC;
				return;
			}
			if (b == 0x11 || b == 0x13 || b == 0x91 || b == 0x93)
				return;
			if (z->sub_n >= MMB_ZM_MAX_SUB)
			{
				sub_bad(z);
				return;
			}
			z->sub[z->sub_n++] = (unsigned char)b;
			return;
		case PS_SUBESC:
			if (b == ZCRCE || b == ZCRCG || b == ZCRCQ || b == ZCRCW)
			{
				z->sub_end = b;
				z->crc_n = 0;
				z->crc_need = z->sub32 ? 4 : 2;
				z->ps = PS_CRC;
				z->sub_esc = 0;
				return;
			}
			if (b == 0x11 || b == 0x13 || b == 0x91 || b == 0x93)
				return;
			if (b == ZRUB0)
				v = 0x7F;
			else if (b == ZRUB1)
				v = 0xFF;
			else
				v = (b ^ 0x40) & 0xFF;
			if (z->sub_n >= MMB_ZM_MAX_SUB)
			{
				sub_bad(z);
				return;
			}
			z->sub[z->sub_n++] = (unsigned char)v;
			z->ps = PS_SUB;
			return;
		case PS_CRC:
			if (z->sub_esc)
			{
				z->sub_esc = 0;
				if (b == 0x11 || b == 0x13 || b == 0x91 ||
				    b == 0x93)
				{
					z->sub_esc = 1;
					return;
				}
				b = (b ^ 0x40) & 0xFF;
			}
			else if (b == ZDLE)
			{
				z->sub_esc = 1;
				return;
			}
			z->crcbuf[z->crc_n++] = (unsigned char)b;
			if (z->crc_n < z->crc_need)
				return;
			accept_sub(z);
			return;
		default:
			z->ps = PS_HDR;
			return;
		}
	}
}

void mmb_zm_feed(mmb_zm_rx *z, const unsigned char *data, unsigned n,
		 unsigned now_ms)
{
	unsigned i;
	int can = 0;

	if (z->state != MMB_ZM_ACTIVE || !data)
		return;
	for (i = 0; i < n; i++)
	{
		int b = data[i];
		z->last_ms = now_ms;
		if (b == ZDLE)
		{
			if (++can >= 5)
			{
				finish(z, MMB_ZM_CANCELLED);
				return;
			}
		}
		else
			can = 0;
		parse_byte(z, b);
		if (z->state != MMB_ZM_ACTIVE)
			return;
	}
}

void mmb_zm_tick(mmb_zm_rx *z, unsigned now_ms)
{
	if (z->state != MMB_ZM_ACTIVE)
		return;
	if (z->last_ms == 0)
	{
		z->last_ms = now_ms;
		return;
	}
	if (now_ms - z->last_ms < 5000)
		return;
	if (z->rep_type < 0 || z->rep_count >= 8)
	{
		finish(z, MMB_ZM_FAILED);
		return;
	}
	send_hdr(z, z->rep_type, z->rep[0], z->rep[1], z->rep[2], z->rep[3]);
	z->rep_count++;
	z->last_ms = now_ms;
}
