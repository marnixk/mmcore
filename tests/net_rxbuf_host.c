#include "net_rxbuf.h"

#include <stdio.h>
#include <string.h>

static int fails;

static void expect_eq(const char *name, unsigned got, unsigned want)
{
	if (got != want)
	{
		fprintf(stderr, "FAIL %s: got %u want %u\n", name, got, want);
		fails++;
	}
}

static void expect_mem(const char *name, const unsigned char *got, const unsigned char *want,
		       unsigned n)
{
	if (memcmp(got, want, n) != 0)
	{
		fprintf(stderr, "FAIL %s\n", name);
		fails++;
	}
}

int main(void)
{
	unsigned char store[64];
	unsigned char out[64];
	mmb_net_rxbuf b;
	const unsigned char csi[] = { 0x1b, '[', '2', 'J', 'H', 'i' };
	unsigned char wrap[128];
	unsigned i;

	mmb_net_rxbuf_init(&b, store, sizeof store);
	expect_eq("empty used", mmb_net_rxbuf_used(&b), 0);
	expect_eq("empty free", mmb_net_rxbuf_free(&b), sizeof store);

	expect_eq("push csi", mmb_net_rxbuf_push(&b, csi, sizeof csi), sizeof csi);
	expect_eq("used after csi", mmb_net_rxbuf_used(&b), sizeof csi);

	memset(out, 0, sizeof out);
	expect_eq("pop esc", mmb_net_rxbuf_pop(&b, out, 1), 1);
	expect_eq("esc byte", out[0], 0x1b);
	expect_eq("used mid-csi", mmb_net_rxbuf_used(&b), sizeof csi - 1);
	expect_eq("pop rest", mmb_net_rxbuf_pop(&b, out, sizeof out), sizeof csi - 1);
	expect_mem("csi tail", out, csi + 1, sizeof csi - 1);
	expect_eq("drained", mmb_net_rxbuf_used(&b), 0);

	for (i = 0; i < sizeof wrap; i++)
		wrap[i] = (unsigned char)i;
	expect_eq("push wrap", mmb_net_rxbuf_push(&b, wrap, 50), 50);
	expect_eq("pop 40", mmb_net_rxbuf_pop(&b, out, 40), 40);
	expect_mem("wrap head", out, wrap, 40);
	expect_eq("push 54", mmb_net_rxbuf_push(&b, wrap + 50, 54), 54);
	expect_eq("full", mmb_net_rxbuf_used(&b), 64);
	expect_eq("no room", mmb_net_rxbuf_push(&b, wrap, 1), 0);
	expect_eq("pop 24", mmb_net_rxbuf_pop(&b, out, 24), 24);
	expect_mem("across wrap", out, wrap + 40, 24);
	expect_eq("pop 40", mmb_net_rxbuf_pop(&b, out, 40), 40);
	expect_mem("after wrap", out, wrap + 64, 40);
	expect_eq("empty again", mmb_net_rxbuf_used(&b), 0);

	mmb_net_rxbuf_reset(&b);
	expect_eq("cap matches header", MMB_NET_RX_CAP, 512u * 1024u);

	if (fails)
	{
		fprintf(stderr, "%d checks failed\n", fails);
		return 1;
	}
	puts("net_rxbuf_host: all checks passed");
	return 0;
}
