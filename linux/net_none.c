/*
 * LN-01 link stubs: the interpreter links the whole network surface even when
 * offline. The real POSIX sockets backend lands in LN-18; Wi-Fi radio features
 * stay unavailable in the Linux port. Everything reports "not available".
 */
#include "mmbasic.h"
#include "mmb_priv.h"

/* ---- Wi-Fi radio (out of scope for the Linux port) ---- */
int mmb_wlan_available(void) { return 0; }
int mmb_wlan_radio_pending(void) { return 0; }
int mmb_wlan_scan(char ssids[][64], int maxn)
{
	(void)ssids;
	(void)maxn;
	return 0;
}
int mmb_wlan_start(const char *ssid, const char *psk)
{
	(void)ssid;
	(void)psk;
	return -1;
}
int mmb_wlan_connect(const char *ssid, const char *psk)
{
	(void)ssid;
	(void)psk;
	return -1;
}
int mmb_wlan_status(void) { return 0; }
int mmb_wlan_ip(char *buf, int bufsize)
{
	(void)buf;
	(void)bufsize;
	return -1;
}
int mmb_wlan_ipconfig(char *buf, int bufsize)
{
	(void)buf;
	(void)bufsize;
	return -1;
}
void mmb_wlan_poll(void) {}
void mmb_wlan_apply_country(void) {}

/* ---- Ethernet / status ---- */
int mmb_eth_available(void) { return 0; }
int mmb_eth_start(void) { return -1; }
int mmb_eth_status(void) { return 0; }
int mmb_eth_wait_dhcp(unsigned ms)
{
	(void)ms;
	return -1;
}
int mmb_eth_ip(char *buf, int bufsize)
{
	(void)buf;
	(void)bufsize;
	return -1;
}
int mmb_eth_ipconfig(char *buf, int bufsize)
{
	(void)buf;
	(void)bufsize;
	return -1;
}

/* ---- TCP client ---- */
int mmb_net_available(void) { return 0; }
int mmb_net_kind(void) { return MMB_NET_NONE; }
int mmb_net_open(int kind)
{
	(void)kind;
	return -1;
}
int mmb_net_gateway_ok(int force)
{
	(void)force;
	return 0;
}
int mmb_net_tcp_open(const char *host, int port)
{
	(void)host;
	(void)port;
	return -1;
}
int mmb_net_tcp_begin(const char *host, int port)
{
	(void)host;
	(void)port;
	return -1;
}
int mmb_net_tcp_status(void) { return -1; }
int mmb_net_tcp_cancelling(void) { return 0; }
const char *mmb_net_tcp_errmsg(void) { return "No network"; }
const char *mmb_net_tcp_close_reason(void) { return "No network"; }
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
	return -1;
}
int mmb_net_tcp_rx_avail(void) { return 0; }
int mmb_net_tcp_peer_closed(void) { return 0; }
void mmb_net_tcp_close(void) {}
void mmb_net_tcp_debug_poll(void) {}
void mmb_net_yield(void) {}

/* ---- TCP server (FTP) ---- */
int mmb_net_srv_listen(int port)
{
	(void)port;
	return -1;
}
int mmb_net_srv_port(int lsn)
{
	(void)lsn;
	return 0;
}
void mmb_net_srv_listen_close(int lsn) { (void)lsn; }
int mmb_net_srv_accept(int lsn)
{
	(void)lsn;
	return -1;
}
int mmb_net_srv_recv(int conn, void *data, unsigned maxn)
{
	(void)conn;
	(void)data;
	(void)maxn;
	return -1;
}
int mmb_net_srv_send(int conn, const void *data, unsigned n)
{
	(void)conn;
	(void)data;
	(void)n;
	return -1;
}
int mmb_net_srv_closed(int conn)
{
	(void)conn;
	return 1;
}
void mmb_net_srv_close(int conn) { (void)conn; }
int mmb_net_srv_ip(char *buf, int bufsize)
{
	(void)buf;
	(void)bufsize;
	return -1;
}
