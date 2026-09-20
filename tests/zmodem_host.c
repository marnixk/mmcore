/*
 * Host test for the ZMODEM receive engine (mmbasic/src/zmodem.c).
 *
 * A miniature sender builds frames with the same CRC/escape rules as rz/sz and
 * the receiver writes into an in-memory file through mmb_zm_ops.
 */
#include "mmb_zmodem.h"

#include <stdio.h>
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
	ZRQINIT = 0, ZRINIT = 1, ZACK = 3, ZFILE = 4, ZSKIP = 5, ZNAK = 6,
	ZFIN = 8, ZRPOS = 9, ZDATA = 10, ZEOF = 11
};

static int failures;

#define CHECK(cond) do { \
	if (!(cond)) { \
		printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		failures++; \
	} \
} while (0)

static unsigned rc16(unsigned crc, unsigned b)
{
	int i;
	crc ^= (b & 0xFFu) << 8;
	for (i = 0; i < 8; i++)
		crc = (crc & 0x8000u) ? ((crc << 1) ^ 0x1021u) : (crc << 1);
	return crc & 0xFFFFu;
}

static unsigned rc32(unsigned crc, unsigned b)
{
	int i;
	crc ^= (b & 0xFFu);
	for (i = 0; i < 8; i++)
		crc = (crc & 1u) ? ((crc >> 1) ^ 0xEDB88320u) : (crc >> 1);
	return crc;
}

static unsigned crc_bytes(const unsigned char *p, unsigned n, int use32)
{
	unsigned crc = use32 ? 0xFFFFFFFFu : 0;
	unsigned i;
	for (i = 0; i < n; i++)
		crc = use32 ? rc32(crc, p[i]) : rc16(crc, p[i]);
	return use32 ? (crc ^ 0xFFFFFFFFu) : crc;
}

static unsigned crc_frame(const unsigned char *p, unsigned n, int end, int use32)
{
	unsigned crc = use32 ? 0xFFFFFFFFu : 0;
	unsigned i;
	for (i = 0; i < n; i++)
		crc = use32 ? rc32(crc, p[i]) : rc16(crc, p[i]);
	crc = use32 ? rc32(crc, (unsigned)end) : rc16(crc, (unsigned)end);
	return use32 ? (crc ^ 0xFFFFFFFFu) : crc;
}

typedef struct {
	unsigned char out[8192];
	unsigned out_n;
	unsigned char file[8192];
	unsigned file_n;
	char fname[128];
	unsigned fsize;
	int open_calls;
	int open_rc;
	int close_ok;
	int close_calls;
} sink;

static int sink_open(void *ctx, const char *name, unsigned size)
{
	sink *s = (sink *)ctx;
	s->open_calls++;
	s->file_n = 0;
	s->fsize = size;
	strncpy(s->fname, name, sizeof(s->fname) - 1);
	s->fname[sizeof(s->fname) - 1] = 0;
	return s->open_rc;
}

static int sink_write(void *ctx, const unsigned char *data, unsigned n)
{
	sink *s = (sink *)ctx;
	if (s->file_n + n > sizeof(s->file))
		return -1;
	memcpy(s->file + s->file_n, data, n);
	s->file_n += n;
	return 0;
}

static void sink_close(void *ctx, int ok)
{
	sink *s = (sink *)ctx;
	s->close_calls++;
	s->close_ok = ok;
}

static int sink_send(void *ctx, const unsigned char *data, unsigned n)
{
	sink *s = (sink *)ctx;
	if (s->out_n + n > sizeof(s->out))
		return -1;
	memcpy(s->out + s->out_n, data, n);
	s->out_n += n;
	return 0;
}

/* --- sender helpers ---------------------------------------------------- */

static int esc(unsigned char *out, int n, unsigned char c)
{
	if (c == ZDLE || c == 0x11 || c == 0x13 || c == 0x91 || c == 0x93 ||
	    c == 0x7F || c == 0xFF || (c & 0x60) == 0)
	{
		out[n++] = ZDLE;
		if (c == 0x7F)
			out[n++] = ZRUB0;
		else if (c == 0xFF)
			out[n++] = ZRUB1;
		else
			out[n++] = (unsigned char)(c ^ 0x40);
		return n;
	}
	out[n++] = c;
	return n;
}

/* Header: binary if use32; hex is always CRC-16. */
static int snd_header(unsigned char *out, int type, unsigned pos, int use32)
{
	unsigned char body[9];
	int blen = use32 ? 9 : 7;
	int n = 0, i;
	unsigned crc;

	body[0] = (unsigned char)type;
	body[1] = (unsigned char)pos;
	body[2] = (unsigned char)(pos >> 8);
	body[3] = (unsigned char)(pos >> 16);
	body[4] = (unsigned char)(pos >> 24);
	crc = crc_bytes(body, 5, use32);
	if (use32)
	{
		/* lrzsz zsbh32 sends the 32-bit FCS least significant byte first. */
		body[5] = (unsigned char)crc;
		body[6] = (unsigned char)(crc >> 8);
		body[7] = (unsigned char)(crc >> 16);
		body[8] = (unsigned char)(crc >> 24);
	}
	else
	{
		body[5] = (unsigned char)(crc >> 8);
		body[6] = (unsigned char)crc;
	}

	if (use32)
	{
		out[n++] = ZPAD;
		out[n++] = ZDLE;
		out[n++] = ZBIN32;
		for (i = 0; i < blen; i++)
			n = esc(out, n, body[i]);
		return n;
	}
	out[n++] = ZPAD;
	out[n++] = ZPAD;
	out[n++] = ZDLE;
	out[n++] = ZHEX;
	{
		static const char hx[] = "0123456789abcdef";
		for (i = 0; i < 7; i++)
		{
			out[n++] = (unsigned char)hx[body[i] >> 4];
			out[n++] = (unsigned char)hx[body[i] & 15];
		}
	}
	out[n++] = '\r';
	out[n++] = '\n';
	out[n++] = 0x11;
	return n;
}

static int snd_data(unsigned char *out, const unsigned char *data, int len,
		    int end, int use32)
{
	unsigned crc;
	int n = 0, i;

	for (i = 0; i < len; i++)
		n = esc(out, n, data[i]);
	crc = crc_frame(data, (unsigned)len, end, use32);
	out[n++] = ZDLE;
	out[n++] = (unsigned char)end;
	if (use32)
	{
		/* lrzsz zsda32 sends the 32-bit FCS least significant byte first. */
		n = esc(out, n, (unsigned char)crc);
		n = esc(out, n, (unsigned char)(crc >> 8));
		n = esc(out, n, (unsigned char)(crc >> 16));
		n = esc(out, n, (unsigned char)(crc >> 24));
	}
	else
	{
		n = esc(out, n, (unsigned char)(crc >> 8));
		n = esc(out, n, (unsigned char)crc);
	}
	return n;
}

static void feed(mmb_zm_rx *z, const unsigned char *p, int n)
{
	int off;
	for (off = 0; off < n; off += 7)
		mmb_zm_feed(z, p + off, (unsigned)(n - off > 7 ? 7 : n - off),
			    1000u + (unsigned)off);
}

static int name_subpacket(unsigned char *hdr, const char *name, int len)
{
	int hn = 0, ln;
	while (name[hn])
		hn++;
	hn++;
	memcpy(hdr, name, (size_t)hn);
	ln = sprintf((char *)hdr + hn, "%d", len);
	return hn + ln;
}

/* `mid_end` is the frame-end type for every chunk but the last (which is
 * ZCRCE): ZCRCG/ZCRCQ keep one frame open, ZCRCW ends the frame so the sender
 * must emit a fresh ZDATA header for the next one (what Synchronet/lrzsz do
 * for a full block when the receiver advertises a buffer size). */
static void send_session2(sink *s, mmb_zm_rx *z, const char *name,
			  const unsigned char *data, int len, int use32,
			  int corrupt_first, int mid_end)
{
	unsigned char buf[8192];
	unsigned char hdr[512];
	int n = 0, off, chunk, first = 1;

	(void)s;
	mmb_zm_begin(z);
	n += snd_header(buf + n, ZRQINIT, 0, use32);
	n += snd_header(buf + n, ZFILE, 0, use32);
	{
		int hn = name_subpacket(hdr, name, len);
		n += snd_data(buf + n, hdr, hn, ZCRCW, use32);
	}
	feed(z, buf, n);

	n = 0;
	off = 0;
	while (off < len)
	{
		int end;
		chunk = len - off;
		if (chunk > 256)
			chunk = 256;
		end = (off + chunk >= len) ? ZCRCE : mid_end;
		if (corrupt_first && first)
		{
			unsigned char bad[2048];
			int bn;

			first = 0;
			n += snd_header(buf + n, ZDATA, (unsigned)off, use32);
			{
				int i;
				for (i = 0; i < len; i++)
					bad[i] = data[i];
			}
			bn = snd_data(buf + n, bad, chunk, end, use32);
			buf[n + 1] ^= 0x01;	/* corrupt first data byte */
			n += bn;
			/* sender retries the frame after the NAK */
			n += snd_header(buf + n, ZDATA, (unsigned)off, use32);
			n += snd_data(buf + n, data + off, chunk, end, use32);
			feed(z, buf, n);
			n = 0;
		}
		else
		{
			if (off == 0 || mid_end == ZCRCW)
				n += snd_header(buf + n, ZDATA, (unsigned)off,
						use32);
			n += snd_data(buf + n, data + off, chunk, end, use32);
		}
		off += chunk;
	}
	n += snd_header(buf + n, ZEOF, (unsigned)len, use32);
	n += snd_header(buf + n, ZFIN, 0, use32);
	feed(z, buf, n);
}

static void send_session(sink *s, mmb_zm_rx *z, const char *name,
			 const unsigned char *data, int len, int use32,
			 int corrupt_first)
{
	send_session2(s, z, name, data, len, use32, corrupt_first, ZCRCG);
}

static void init_sink(sink *s, mmb_zm_ops *ops, mmb_zm_rx *z)
{
	memset(s, 0, sizeof(*s));
	s->open_rc = 0;
	ops->ctx = s;
	ops->open = sink_open;
	ops->write = sink_write;
	ops->close = sink_close;
	ops->send = sink_send;
	mmb_zm_init(z, ops);
}

static int count_type(const sink *s, int type)
{
	unsigned i;
	int hits = 0;
	for (i = 0; i + 5 < s->out_n; i++)
	{
		int hi, lo;
		if (s->out[i] != ZPAD || s->out[i + 1] != ZPAD ||
		    s->out[i + 2] != ZDLE || s->out[i + 3] != ZHEX)
			continue;
		hi = s->out[i + 4];
		lo = s->out[i + 5];
		hi = (hi >= 'a') ? hi - 'a' + 10 : hi - '0';
		lo = (lo >= 'a') ? lo - 'a' + 10 : lo - '0';
		if (((hi << 4) | lo) == type)
			hits++;
	}
	return hits;
}

int main(void)
{
	mmb_zm_ops ops;
	mmb_zm_rx z;
	sink s;
	const char *text = "hello zmodem world\n";
	unsigned char binary[600];
	int i;

	for (i = 0; i < (int)sizeof(binary); i++)
		binary[i] = (unsigned char)(i * 7 + 3);

	/* 1. CRC-16 hex session */
	init_sink(&s, &ops, &z);
	send_session(&s, &z, "HELLO.TXT", (const unsigned char *)text,
		     (int)strlen(text), 0, 0);
	CHECK(z.state == MMB_ZM_DONE);
	CHECK(s.open_calls == 1);
	CHECK(strcmp(s.fname, "HELLO.TXT") == 0);
	CHECK(s.fsize == strlen(text));
	CHECK(s.file_n == strlen(text));
	CHECK(memcmp(s.file, text, strlen(text)) == 0);
	CHECK(s.close_calls == 1 && s.close_ok == 1);
	CHECK(z.files == 1);
	CHECK(count_type(&s, ZRINIT) >= 1);
	CHECK(count_type(&s, ZRPOS) >= 1);
	CHECK(count_type(&s, ZFIN) >= 1);

	/* 2. CRC-32 binary session exercising every escape class */
	init_sink(&s, &ops, &z);
	send_session(&s, &z, "BIN.DAT", binary, (int)sizeof(binary), 1, 0);
	CHECK(z.state == MMB_ZM_DONE);
	CHECK(s.file_n == sizeof(binary));
	CHECK(memcmp(s.file, binary, sizeof(binary)) == 0);
	CHECK(z.files == 1);

	/* 3. corrupt a data subpacket: receiver NAKs, then the retry lands */
	init_sink(&s, &ops, &z);
	send_session(&s, &z, "FIX.BIN", binary, 300, 1, 1);
	CHECK(z.state == MMB_ZM_DONE);
	CHECK(s.file_n == 300);
	CHECK(memcmp(s.file, binary, 300) == 0);
	CHECK(count_type(&s, ZNAK) >= 1);

	/* 4. auto-detect misfire: a bogus header fails without a session */
	init_sink(&s, &ops, &z);
	mmb_zm_begin(&z);
	{
		unsigned char junk[64];
		int jn = snd_header(junk, ZRQINIT, 0, 0);
		junk[6] = (junk[6] == '0') ? '1' : '0';	/* wrong CRC, valid hex */
		feed(&z, junk, jn);
	}
	CHECK(z.state == MMB_ZM_FAILED);

	/* 5. open refuses: the file is skipped, not written */
	init_sink(&s, &ops, &z);
	s.open_rc = -1;
	send_session(&s, &z, "NOPE.TXT", (const unsigned char *)text,
		     (int)strlen(text), 0, 0);
	CHECK(z.state == MMB_ZM_DONE);
	CHECK(s.close_calls == 0);
	CHECK(count_type(&s, ZSKIP) >= 1);

	/* 6. multi-frame session where each full block is a ZCRCW frame, the
	 * way Synchronet/lrzsz send once the receiver advertises a buffer size.
	 * A ZCRCW ends the frame, so the sender must reopen with a ZDATA header;
	 * the receiver must ZACK and return to header parsing. */
	init_sink(&s, &ops, &z);
	send_session2(&s, &z, "FRAMES.BIN", binary, (int)sizeof(binary), 1, 0,
		      ZCRCW);
	CHECK(z.state == MMB_ZM_DONE);
	CHECK(s.file_n == sizeof(binary));
	CHECK(memcmp(s.file, binary, sizeof(binary)) == 0);
	CHECK(z.files == 1);
	CHECK(count_type(&s, ZACK) >= 2);
	CHECK(count_type(&s, ZNAK) == 0);

	/* 7. same shape with ZCRCQ, which does continue the frame (ZACK, no
	 * new ZDATA header). */
	init_sink(&s, &ops, &z);
	send_session2(&s, &z, "QCRCQ.BIN", binary, (int)sizeof(binary), 1, 0,
		      ZCRCQ);
	CHECK(z.state == MMB_ZM_DONE);
	CHECK(s.file_n == sizeof(binary));
	CHECK(memcmp(s.file, binary, sizeof(binary)) == 0);
	CHECK(z.files == 1);
	CHECK(count_type(&s, ZACK) >= 1);
	CHECK(count_type(&s, ZNAK) == 0);

	/* 8. byte-for-byte lrzsz capture: ZBIN32 ZFILE whose 32-bit FCS is
	 * sent least significant byte first, with 0x93 escaped to 18 D3. The
	 * header data bytes are raw (lrzsz does not escape control bytes
	 * unless the receiver requests ESCCTL). Regression for the hang where
	 * the receiver NAKed every ZFILE because it compared the FCS high
	 * byte first. */
	init_sink(&s, &ops, &z);
	{
		static const unsigned char lrzsz_zfile[] = {
			0x2A, 0x2A, 0x18, 0x43, 0x04, 0x00, 0x00, 0x02,
			0x01, 0xC9, 0x03, 0x18, 0xD3, 0x76, 0x61, 0x72,
			0x61, 0x6B, 0x2D, 0x61, 0x72, 0x74, 0x2E, 0x7A,
			0x69, 0x70, 0x00, 0x31, 0x38, 0x38, 0x33, 0x37,
			0x20, 0x31, 0x33, 0x35, 0x37, 0x34, 0x37, 0x33,
			0x34, 0x37, 0x31, 0x37, 0x20, 0x30, 0x20, 0x30,
			0x20, 0x31, 0x20, 0x31, 0x38, 0x38, 0x33, 0x37,
			0x20, 0x30, 0x00, 0x18, 0x6B, 0x2F, 0xC2, 0xE5,
			0x17, 0x11
		};
		mmb_zm_begin(&z);
		{
			static const unsigned char zrqinit[] = {
				0x2A, 0x2A, 0x18, 0x42,
				'0', '0', '0', '0', '0', '0', '0',
				'0', '0', '0', '0', '0', '0', '0',
				'\r', '\n'
			};
			feed(&z, zrqinit, (int)sizeof(zrqinit));
		}
		feed(&z, lrzsz_zfile, (int)sizeof(lrzsz_zfile));
	}
	CHECK(z.state == MMB_ZM_ACTIVE);
	CHECK(s.open_calls == 1);
	CHECK(strcmp(s.fname, "arak-art.zip") == 0);
	CHECK(s.fsize == 18837);
	CHECK(count_type(&s, ZRPOS) >= 1);
	CHECK(count_type(&s, ZNAK) == 0);

	if (failures == 0)
		printf("all checks passed\n");
	else
		printf("%d check(s) failed\n", failures);
	return failures ? 1 : 0;
}
