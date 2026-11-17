#ifndef MMB_SHA256_H
#define MMB_SHA256_H

#include <stddef.h>
#include <stdint.h>

/* Self-contained SHA-256 (FIPS 180-4). Deliberately independent of any TLS
 * library so the HUB checksum check works on every target, including the
 * bare-metal Circle build where no OpenSSL/mbedTLS SHA API is guaranteed. */
typedef struct {
	uint32_t state[8];
	uint64_t bytes;
	unsigned char block[64];
	unsigned block_len;
} mmb_sha256_ctx;

void mmb_sha256_init(mmb_sha256_ctx *ctx);
void mmb_sha256_update(mmb_sha256_ctx *ctx, const void *data, size_t n);
void mmb_sha256_final(mmb_sha256_ctx *ctx, unsigned char out[32]);

/* One-shot convenience: hash `n` bytes into `out` (32 bytes). */
void mmb_sha256(const void *data, size_t n, unsigned char out[32]);

/* Lowercase hex of a 32-byte digest into out[65]. */
void mmb_sha256_hex(const unsigned char digest[32], char out[65]);

#endif
