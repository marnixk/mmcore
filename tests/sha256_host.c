/* Host test for the self-contained SHA-256 (#1525). Standard FIPS 180-4
 * vectors plus incremental updates and the hex formatter. */
#include <stdio.h>
#include <string.h>

#include "sha256.h"

static int fails;

static void check_hex(const void *data, size_t n, const char *want,
		      const char *what)
{
	unsigned char digest[32];
	char hex[65];

	mmb_sha256(data, n, digest);
	mmb_sha256_hex(digest, hex);
	if (strcmp(hex, want) != 0)
	{
		printf("FAIL: %s: got %s want %s\n", what, hex, want);
		fails++;
	}
}

int main(void)
{
	static const char abc[] = "abc";
	static const char two[] =
		"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
	unsigned char digest[32], digest2[32];
	char hex[65];
	mmb_sha256_ctx ctx;
	int i;

	check_hex("", 0,
		  "e3b0c44298fc1c149afbf4c8996fb924"
		  "27ae41e4649b934ca495991b7852b855",
		  "empty");
	check_hex(abc, 3,
		  "ba7816bf8f01cfea414140de5dae2223"
		  "b00361a396177a9cb410ff61f20015ad",
		  "abc");
	check_hex(two, strlen(two),
		  "248d6a61d20638b8e5c026930c3e6039"
		  "a33ce45964ff2167f6ecedd419db06c1",
		  "two-block");

	/* Incremental updates must match the one-shot hash at every split. */
	mmb_sha256_init(&ctx);
	for (i = 0; abc[i]; i++)
		mmb_sha256_update(&ctx, &abc[i], 1);
	mmb_sha256_final(&ctx, digest);
	mmb_sha256(abc, 3, digest2);
	if (memcmp(digest, digest2, 32) != 0)
	{
		printf("FAIL: incremental abc\n");
		fails++;
	}

	mmb_sha256_init(&ctx);
	mmb_sha256_update(&ctx, two, 17);
	mmb_sha256_update(&ctx, two + 17, 10);
	mmb_sha256_update(&ctx, two + 27, strlen(two) - 27);
	mmb_sha256_final(&ctx, digest);
	mmb_sha256(two, strlen(two), digest2);
	if (memcmp(digest, digest2, 32) != 0)
	{
		printf("FAIL: incremental split\n");
		fails++;
	}

	/* One million 'a's. */
	{
		char block[1000];
		memset(block, 'a', sizeof(block));
		mmb_sha256_init(&ctx);
		for (i = 0; i < 1000; i++)
			mmb_sha256_update(&ctx, block, sizeof(block));
		mmb_sha256_final(&ctx, digest);
		mmb_sha256_hex(digest, hex);
		if (strcmp(hex,
			   "cdc76e5c9914fb9281a1c7e284d73e67"
			   "f1809a48a497200e046d39ccc7112cd0") != 0)
		{
			printf("FAIL: million a: %s\n", hex);
			fails++;
		}
	}

	if (fails)
	{
		printf("%d check(s) failed\n", fails);
		return 1;
	}
	printf("all checks passed\n");
	return 0;
}
