/*
 * Self-contained SHA-256 (FIPS 180-4), public-domain style implementation.
 *
 * The HUB needs a checksum on every target (native SDL, Linux framebuffer and
 * ISO, macOS, Windows, bare-metal Circle). The TLS stack is optional on some
 * of those builds (#1479/#1480), so a small in-tree implementation is more
 * portable than reaching into OpenSSL/mbedTLS for SHA-256.
 */
#include "sha256.h"

static const uint32_t K[64] = {
	0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
	0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
	0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
	0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
	0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
	0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
	0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
	0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
	0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
	0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
	0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
	0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
	0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
	0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
	0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
	0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

static uint32_t ror(uint32_t x, unsigned n)
{
	return (x >> n) | (x << (32 - n));
}

static void sha256_block(mmb_sha256_ctx *ctx, const unsigned char *p)
{
	uint32_t w[64];
	uint32_t a, b, c, d, e, f, g, h;
	int i;

	for (i = 0; i < 16; i++)
		w[i] = ((uint32_t)p[i * 4] << 24) |
		       ((uint32_t)p[i * 4 + 1] << 16) |
		       ((uint32_t)p[i * 4 + 2] << 8) |
		       (uint32_t)p[i * 4 + 3];
	for (i = 16; i < 64; i++)
	{
		uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^
			      (w[i - 15] >> 3);
		uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^
			      (w[i - 2] >> 10);
		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}

	a = ctx->state[0];
	b = ctx->state[1];
	c = ctx->state[2];
	d = ctx->state[3];
	e = ctx->state[4];
	f = ctx->state[5];
	g = ctx->state[6];
	h = ctx->state[7];

	for (i = 0; i < 64; i++)
	{
		uint32_t S1 = ror(e, 6) ^ ror(e, 11) ^ ror(e, 25);
		uint32_t ch = (e & f) ^ (~e & g);
		uint32_t t1 = h + S1 + ch + K[i] + w[i];
		uint32_t S0 = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22);
		uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
		uint32_t t2 = S0 + maj;

		h = g;
		g = f;
		f = e;
		e = d + t1;
		d = c;
		c = b;
		b = a;
		a = t1 + t2;
	}

	ctx->state[0] += a;
	ctx->state[1] += b;
	ctx->state[2] += c;
	ctx->state[3] += d;
	ctx->state[4] += e;
	ctx->state[5] += f;
	ctx->state[6] += g;
	ctx->state[7] += h;
}

void mmb_sha256_init(mmb_sha256_ctx *ctx)
{
	static const uint32_t iv[8] = {
		0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
		0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
	};

	for (int i = 0; i < 8; i++)
		ctx->state[i] = iv[i];
	ctx->bytes = 0;
	ctx->block_len = 0;
}

void mmb_sha256_update(mmb_sha256_ctx *ctx, const void *data, size_t n)
{
	const unsigned char *p = (const unsigned char *)data;

	ctx->bytes += (uint64_t)n;
	while (n)
	{
		size_t take = 64 - ctx->block_len;

		if (take > n)
			take = n;
		for (size_t i = 0; i < take; i++)
			ctx->block[ctx->block_len + i] = p[i];
		ctx->block_len += (unsigned)take;
		p += take;
		n -= take;
		if (ctx->block_len == 64)
		{
			sha256_block(ctx, ctx->block);
			ctx->block_len = 0;
		}
	}
}

void mmb_sha256_final(mmb_sha256_ctx *ctx, unsigned char out[32])
{
	uint64_t bits = ctx->bytes * 8;
	unsigned char pad[72];
	unsigned padlen;
	int i;

	pad[0] = 0x80;
	padlen = (ctx->block_len < 56) ? (56 - ctx->block_len)
				       : (120 - ctx->block_len);
	for (i = 1; i < (int)padlen; i++)
		pad[i] = 0;
	/* padlen bytes of padding (0x80 plus zeros) then the 64-bit length. Use a
	 * direct block update rather than mmb_sha256_update so `bytes` does not
	 * grow before the length is encoded. */
	{
		const unsigned char *p = pad;
		size_t n = padlen;

		while (n)
		{
			size_t take = 64 - ctx->block_len;

			if (take > n)
				take = n;
			for (size_t k = 0; k < take; k++)
				ctx->block[ctx->block_len + k] = p[k];
			ctx->block_len += (unsigned)take;
			p += take;
			n -= take;
			if (ctx->block_len == 64)
			{
				sha256_block(ctx, ctx->block);
				ctx->block_len = 0;
			}
		}
	}
	/* The final 8 bytes always land in a fresh block after the 0x80 padding
	 * above (padlen chose 56 or 120), so block_len is 56 here. */
	for (i = 0; i < 8; i++)
		ctx->block[ctx->block_len + i] =
			(unsigned char)(bits >> (56 - i * 8));
	ctx->block_len += 8;
	if (ctx->block_len == 64)
	{
		sha256_block(ctx, ctx->block);
		ctx->block_len = 0;
	}

	for (i = 0; i < 8; i++)
	{
		out[i * 4] = (unsigned char)(ctx->state[i] >> 24);
		out[i * 4 + 1] = (unsigned char)(ctx->state[i] >> 16);
		out[i * 4 + 2] = (unsigned char)(ctx->state[i] >> 8);
		out[i * 4 + 3] = (unsigned char)(ctx->state[i]);
	}
}

void mmb_sha256(const void *data, size_t n, unsigned char out[32])
{
	mmb_sha256_ctx ctx;

	mmb_sha256_init(&ctx);
	mmb_sha256_update(&ctx, data, n);
	mmb_sha256_final(&ctx, out);
}

void mmb_sha256_hex(const unsigned char digest[32], char out[65])
{
	static const char hex[] = "0123456789abcdef";
	int i;

	for (i = 0; i < 32; i++)
	{
		out[i * 2] = hex[digest[i] >> 4];
		out[i * 2 + 1] = hex[digest[i] & 15];
	}
	out[64] = 0;
}
