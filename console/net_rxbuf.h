#ifndef MMB_NET_RXBUF_H
#define MMB_NET_RXBUF_H

#ifdef __cplusplus
extern "C" {
#endif

#define MMB_NET_RX_CAP (512u * 1024u)

typedef struct {
	unsigned char *data;
	unsigned cap;
	unsigned head;
	unsigned tail;
	unsigned used;
} mmb_net_rxbuf;

void mmb_net_rxbuf_init(mmb_net_rxbuf *b, unsigned char *data, unsigned cap);
void mmb_net_rxbuf_reset(mmb_net_rxbuf *b);
unsigned mmb_net_rxbuf_used(const mmb_net_rxbuf *b);
unsigned mmb_net_rxbuf_free(const mmb_net_rxbuf *b);
unsigned mmb_net_rxbuf_push(mmb_net_rxbuf *b, const unsigned char *src, unsigned n);
unsigned mmb_net_rxbuf_pop(mmb_net_rxbuf *b, unsigned char *dst, unsigned maxn);

#ifdef __cplusplus
}
#endif

#endif
