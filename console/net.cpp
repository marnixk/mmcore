#include "mmbasic.h"

/*
 * TCP platform layer used by CONNECT.
 *
 * Circle's network stack (libnet + scheduler + a NIC) is not linked in the
 * QEMU console image: raspi3b has no emulated Ethernet, and Wi-Fi is a
 * separate CYW4343x path. Hardware can later define MMB_CIRCLE_NET and
 * provide a CSocket here. Until then every open fails cleanly so CONNECT
 * cannot hang the serial prompt.
 */

extern "C" {

int mmb_net_available(void)
{
	return 0;
}

int mmb_net_tcp_open(const char *host, int port)
{
	(void)host;
	(void)port;
	return -1;
}

int mmb_net_tcp_send(const void *data, unsigned n)
{
	(void)data;
	(void)n;
	return -1;
}

int mmb_net_tcp_recv(void *data, unsigned maxn)
{
	(void)data;
	(void)maxn;
	return 0;
}

void mmb_net_tcp_close(void)
{
}

}
