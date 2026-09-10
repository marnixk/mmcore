#include "net_rxbuf.h"

#include <string.h>

void mmb_net_rxbuf_init(mmb_net_rxbuf *b, unsigned char *data, unsigned cap)
{
	b->data = data;
	b->cap = cap;
	b->head = 0;
	b->tail = 0;
	b->used = 0;
}

void mmb_net_rxbuf_reset(mmb_net_rxbuf *b)
{
	b->head = 0;
	b->tail = 0;
	b->used = 0;
}

unsigned mmb_net_rxbuf_used(const mmb_net_rxbuf *b)
{
	return b->used;
}

unsigned mmb_net_rxbuf_free(const mmb_net_rxbuf *b)
{
	return b->cap - b->used;
}

unsigned mmb_net_rxbuf_push(mmb_net_rxbuf *b, const unsigned char *src, unsigned n)
{
	unsigned first;

	if (!b->data || !src || !n)
		return 0;
	if (n > b->cap - b->used)
		n = b->cap - b->used;
	if (!n)
		return 0;
	first = b->cap - b->tail;
	if (first > n)
		first = n;
	memcpy(b->data + b->tail, src, first);
	if (n > first)
		memcpy(b->data, src + first, n - first);
	b->tail += n;
	if (b->tail >= b->cap)
		b->tail -= b->cap;
	b->used += n;
	return n;
}

unsigned mmb_net_rxbuf_pop(mmb_net_rxbuf *b, unsigned char *dst, unsigned maxn)
{
	unsigned n, first;

	if (!b->data || !dst || !maxn || !b->used)
		return 0;
	n = maxn;
	if (n > b->used)
		n = b->used;
	first = b->cap - b->head;
	if (first > n)
		first = n;
	memcpy(dst, b->data + b->head, first);
	if (n > first)
		memcpy(dst + first, b->data, n - first);
	b->head += n;
	if (b->head >= b->cap)
		b->head -= b->cap;
	b->used -= n;
	return n;
}
