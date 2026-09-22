/*
 * Winsock network backend for the Windows native build.
 *
 * Mirrors native/net_posix.c: the same non-blocking contracts (send/recv return
 * 0 for "would block / no data", connect is poll-driven, server sends report
 * backpressure) so the portable interpreter is unchanged. Wi-Fi radio features
 * are out of scope and report unavailable; IPCONFIG/status use the host's
 * resolved address.
 */
#include "win_compat.h"

#include "mmbasic.h"
#include "mmb_priv.h"

#include <stdio.h>
#include <string.h>

#define RXCAP 1600
#define SRV_MAX_LISTEN 2
#define SRV_MAX_CONN 4

static int set_nonblock(SOCKET fd)
{
	u_long one = 1;

	return ioctlsocket(fd, FIONBIO, &one) == 0 ? 0 : -1;
}

static void sock_close(SOCKET fd)
{
	if (fd != INVALID_SOCKET)
		closesocket(fd);
}

/* ---- client -------------------------------------------------------- */

enum { C_IDLE = 0, C_CONNECTING = 1, C_CONNECTED = 2, C_FAILED = 3, C_CLOSED = 4 };

static SOCKET s_fd = INVALID_SOCKET;
static int s_state = C_IDLE;
static unsigned char s_rx[RXCAP];
static unsigned s_rx_len, s_rx_pos;
static int s_peer_closed;
static int s_err;
static int s_close_reason;

static void rx_reset(void)
{
	s_rx_len = s_rx_pos = 0;
}

static void client_reset(void)
{
	sock_close(s_fd);
	s_fd = INVALID_SOCKET;
	s_state = C_IDLE;
	s_peer_closed = 0;
	s_err = 0;
	s_close_reason = 0;
	rx_reset();
}

int mmb_net_kind(void) { return MMB_NET_ETH; }
int mmb_net_open(int kind) { (void)kind; return mmb_win_wsa_start(); }
int mmb_net_available(void) { return 1; }
int mmb_net_gateway_ok(int force) { (void)force; return 1; }
void mmb_net_yield(void) { Sleep(0); }

int mmb_net_tcp_begin(const char *host, int port)
{
	struct addrinfo hints, *res = 0, *ai;
	char ps[16];
	SOCKET fd = INVALID_SOCKET;
	int last = 0;

	if (mmb_win_wsa_start() != 0)
		return -1;
	client_reset();
	if (!host || port <= 0)
	{
		s_err = 5;
		s_state = C_FAILED;
		return -1;
	}
	snprintf(ps, sizeof ps, "%d", port);
	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	if (getaddrinfo(host, ps, &hints, &res) != 0 || !res)
	{
		s_err = 2;
		s_state = C_FAILED;
		return -1;
	}
	for (ai = res; ai; ai = ai->ai_next)
	{
		int r, e;

		fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (fd == INVALID_SOCKET)
		{
			last = WSAGetLastError();
			continue;
		}
		set_nonblock(fd);
		r = connect(fd, ai->ai_addr, (int)ai->ai_addrlen);
		if (r == 0)
		{
			s_fd = fd;
			s_state = C_CONNECTED;
			break;
		}
		e = WSAGetLastError();
		if (e == WSAEWOULDBLOCK || e == WSAEINPROGRESS)
		{
			s_fd = fd;
			s_state = C_CONNECTING;
			break;
		}
		last = e;
		sock_close(fd);
		fd = INVALID_SOCKET;
	}
	freeaddrinfo(res);
	if (s_fd == INVALID_SOCKET)
	{
		s_err = (last == WSAECONNREFUSED) ? 1 : 3;
		s_state = C_FAILED;
		return -1;
	}
	return 0;
}

int mmb_net_tcp_status(void)
{
	if (s_state == C_CONNECTED)
		return 1;
	if (s_state == C_CONNECTING)
	{
		fd_set wfds, efds;
		struct timeval tv;
		int r, so = 0, sl = sizeof so;

		FD_ZERO(&wfds);
		FD_ZERO(&efds);
		FD_SET(s_fd, &wfds);
		FD_SET(s_fd, &efds);
		tv.tv_sec = 0;
		tv.tv_usec = 0;
		r = select(0, 0, &wfds, &efds, &tv);
		if (r <= 0)
			return 0;
		if (getsockopt(s_fd, SOL_SOCKET, SO_ERROR, (char *)&so, &sl) != 0 ||
		    so != 0)
		{
			s_err = (so == WSAECONNREFUSED) ? 1 : 3;
			sock_close(s_fd);
			s_fd = INVALID_SOCKET;
			s_state = C_FAILED;
			return -1;
		}
		s_state = C_CONNECTED;
		return 1;
	}
	return -1;
}

int mmb_net_tcp_open(const char *host, int port)
{
	unsigned start;
	int st;

	if (mmb_net_tcp_begin(host, port) != 0)
		return -1;
	start = mmb_now_ms();
	for (;;)
	{
		st = mmb_net_tcp_status();
		if (st != 0)
			break;
		if (mmb_now_ms() - start > 25000u)
		{
			mmb_net_tcp_close();
			return -1;
		}
		Sleep(0);
	}
	return st == 1 ? 0 : -1;
}

int mmb_net_tcp_send(const void *data, unsigned n)
{
	int w;

	if (s_state != C_CONNECTED || s_fd == INVALID_SOCKET)
		return -1;
	w = send(s_fd, data, (int)n, 0);
	if (w == SOCKET_ERROR)
	{
		int e = WSAGetLastError();

		if (e == WSAEWOULDBLOCK)
			return 0;
		s_state = C_CLOSED;
		return -1;
	}
	return w;
}

/* Returns >0 bytes buffered, 0 none, <0 closed/error. */
static int rx_fill(void)
{
	int r;

	if (s_rx_len > 0)
		return (int)(s_rx_len - s_rx_pos);
	if (s_state != C_CONNECTED || s_fd == INVALID_SOCKET)
		return (s_peer_closed || s_state == C_CLOSED) ? -1 : 0;
	r = recv(s_fd, (char *)s_rx, RXCAP, 0);
	if (r > 0)
	{
		s_rx_len = (unsigned)r;
		s_rx_pos = 0;
		return r;
	}
	if (r == 0)
	{
		s_peer_closed = 1;
		s_close_reason = 1;
		s_state = C_CLOSED;
		return -1;
	}
	if (WSAGetLastError() == WSAEWOULDBLOCK)
		return 0;
	s_peer_closed = 1;
	s_close_reason = 2;
	s_state = C_CLOSED;
	return -1;
}

int mmb_net_tcp_recv(void *data, unsigned maxn)
{
	int avail = rx_fill();
	unsigned n;

	if (avail <= 0)
		return avail;
	n = (unsigned)avail;
	if (n > maxn)
		n = maxn;
	memcpy(data, s_rx + s_rx_pos, n);
	s_rx_pos += n;
	if (s_rx_pos >= s_rx_len)
		s_rx_len = s_rx_pos = 0;
	return (int)n;
}

int mmb_net_tcp_rx_avail(void)
{
	int a = rx_fill();

	return a > 0 ? a : 0;
}

int mmb_net_tcp_peer_closed(void) { return s_peer_closed; }

const char *mmb_net_tcp_errmsg(void)
{
	static const char *m[] = {
		"OK", "Connection refused", "DNS failed", "Connect failed",
		"No route", "Invalid"
	};

	return m[s_err < 0 ? 0 : (s_err > 5 ? 5 : s_err)];
}

const char *mmb_net_tcp_close_reason(void)
{
	if (s_close_reason == 1)
		return "Peer closed";
	if (s_close_reason == 2)
		return "Connection reset";
	return "Closed";
}

int mmb_net_tcp_cancelling(void) { return s_state == C_CONNECTING; }
void mmb_net_tcp_close(void) { client_reset(); }
void mmb_net_tcp_debug_poll(void) {}

/* ---- server (FTP PASV) -------------------------------------------- */

typedef struct {
	int used;
	SOCKET fd;
	int port;
} listener_t;

typedef struct {
	int used;
	SOCKET fd;
	int closed;
	unsigned char rx[RXCAP];
	unsigned rx_len, rx_pos;
} conn_t;

static listener_t s_lsn[SRV_MAX_LISTEN];
static conn_t s_conn[SRV_MAX_CONN];

int mmb_net_srv_listen(int port)
{
	struct sockaddr_in a, b;
	int bl, slot, one = 1;
	SOCKET fd;

	if (mmb_win_wsa_start() != 0)
		return -1;
	for (slot = 0; slot < SRV_MAX_LISTEN; slot++)
		if (s_lsn[slot].used && s_lsn[slot].port == port)
			return slot;
	for (slot = 0; slot < SRV_MAX_LISTEN; slot++)
		if (!s_lsn[slot].used)
			break;
	if (slot >= SRV_MAX_LISTEN)
		return -1;
	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd == INVALID_SOCKET)
		return -1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof one);
	memset(&a, 0, sizeof a);
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_ANY);
	a.sin_port = htons((unsigned short)port);
	if (bind(fd, (struct sockaddr *)&a, sizeof a) != 0 ||
	    listen(fd, 4) != 0)
	{
		sock_close(fd);
		return -1;
	}
	set_nonblock(fd);
	memset(&b, 0, sizeof b);
	bl = sizeof b;
	getsockname(fd, (struct sockaddr *)&b, &bl);
	s_lsn[slot].used = 1;
	s_lsn[slot].fd = fd;
	s_lsn[slot].port = ntohs(b.sin_port);
	return slot;
}

int mmb_net_srv_port(int lsn)
{
	if (lsn < 0 || lsn >= SRV_MAX_LISTEN || !s_lsn[lsn].used)
		return 0;
	return s_lsn[lsn].port;
}

void mmb_net_srv_listen_close(int lsn)
{
	if (lsn < 0 || lsn >= SRV_MAX_LISTEN || !s_lsn[lsn].used)
		return;
	sock_close(s_lsn[lsn].fd);
	s_lsn[lsn].used = 0;
}

int mmb_net_srv_accept(int lsn)
{
	SOCKET cfd;
	int slot;

	if (lsn < 0 || lsn >= SRV_MAX_LISTEN || !s_lsn[lsn].used)
		return -1;
	cfd = accept(s_lsn[lsn].fd, 0, 0);
	if (cfd == INVALID_SOCKET)
		return -1;
	set_nonblock(cfd);
	for (slot = 0; slot < SRV_MAX_CONN; slot++)
		if (!s_conn[slot].used)
			break;
	if (slot >= SRV_MAX_CONN)
	{
		sock_close(cfd);
		return -1;
	}
	memset(&s_conn[slot], 0, sizeof s_conn[slot]);
	s_conn[slot].used = 1;
	s_conn[slot].fd = cfd;
	return slot;
}

int mmb_net_srv_recv(int conn, void *data, unsigned maxn)
{
	conn_t *c;
	int avail;
	unsigned n;

	if (conn < 0 || conn >= SRV_MAX_CONN || !s_conn[conn].used)
		return -1;
	c = &s_conn[conn];
	if (c->rx_len > 0)
		avail = (int)(c->rx_len - c->rx_pos);
	else
	{
		int r = recv(c->fd, (char *)c->rx, RXCAP, 0);

		if (r > 0)
		{
			c->rx_len = (unsigned)r;
			c->rx_pos = 0;
			avail = r;
		}
		else if (r == 0)
		{
			c->closed = 1;
			return -1; /* orderly FIN */
		}
		else if (WSAGetLastError() == WSAEWOULDBLOCK)
			return 0;
		else
		{
			c->closed = 1;
			return -2; /* reset / hard error */
		}
	}
	n = (unsigned)avail;
	if (n > maxn)
		n = maxn;
	memcpy(data, c->rx + c->rx_pos, n);
	c->rx_pos += n;
	if (c->rx_pos >= c->rx_len)
		c->rx_len = c->rx_pos = 0;
	return (int)n;
}

/* Orderly FIN (mmb_net_srv_recv -1) vs reset/hard error (-2). */
int mmb_net_srv_eof(int err)
{
	return err == -1;
}

const char *mmb_net_srv_reason(int err)
{
	if (err == -1)
		return "Connection closed";
	if (err == -2)
		return "Connection reset";
	return "Connection lost";
}

int mmb_net_srv_send(int conn, const void *data, unsigned n)
{
	int w;

	if (conn < 0 || conn >= SRV_MAX_CONN || !s_conn[conn].used)
		return -1;
	w = send(s_conn[conn].fd, data, (int)n, 0);
	if (w == SOCKET_ERROR)
	{
		if (WSAGetLastError() == WSAEWOULDBLOCK)
			return 0;
		s_conn[conn].closed = 1;
		return -1;
	}
	return w;
}

int mmb_net_srv_closed(int conn)
{
	if (conn < 0 || conn >= SRV_MAX_CONN || !s_conn[conn].used)
		return 1;
	return s_conn[conn].closed;
}

void mmb_net_srv_close(int conn)
{
	if (conn < 0 || conn >= SRV_MAX_CONN || !s_conn[conn].used)
		return;
	sock_close(s_conn[conn].fd);
	s_conn[conn].used = 0;
}

/* ---- local interface status (LN-19) ------------------------------- */

static int is_loopback(const struct sockaddr_in *s)
{
	unsigned long a = ntohl(s->sin_addr.s_addr);

	return (a >> 24) == 127;
}

static int local_ipv4(char *buf, int bufsize)
{
	struct addrinfo hints, *res = 0, *ai;
	char host[256];

	if (bufsize > 0)
		buf[0] = 0;
	mmb_win_wsa_start();
	if (gethostname(host, sizeof host) == 0)
	{
		memset(&hints, 0, sizeof hints);
		hints.ai_family = AF_INET;
		hints.ai_socktype = SOCK_STREAM;
		if (getaddrinfo(host, 0, &hints, &res) == 0)
		{
			for (ai = res; ai; ai = ai->ai_next)
			{
				struct sockaddr_in *s = (struct sockaddr_in *)ai->ai_addr;

				if (is_loopback(s))
					continue;
				if (inet_ntop(AF_INET, &s->sin_addr, buf,
					      (size_t)bufsize))
				{
					freeaddrinfo(res);
					return 0;
				}
			}
			freeaddrinfo(res);
		}
	}
	snprintf(buf, (size_t)bufsize, "127.0.0.1");
	return 0;
}

int mmb_net_srv_ip(char *buf, int bufsize)
{
	if (!buf || bufsize < 8)
		return -1;
	return local_ipv4(buf, bufsize);
}

int mmb_eth_available(void)
{
	char b[32];

	local_ipv4(b, sizeof b);
	return strcmp(b, "127.0.0.1") != 0;
}

int mmb_eth_start(void) { return 0; }
int mmb_eth_status(void) { return mmb_eth_available(); }
int mmb_eth_wait_dhcp(unsigned ms) { (void)ms; return 1; }

int mmb_eth_ip(char *buf, int bufsize)
{
	if (!buf || bufsize < 8)
		return -1;
	return local_ipv4(buf, bufsize);
}

int mmb_eth_ipconfig(char *buf, int bufsize)
{
	char ip[64];

	if (!buf || bufsize < 32)
		return -1;
	local_ipv4(ip, sizeof ip);
	snprintf(buf, (size_t)bufsize,
		 "Interface: Ethernet\nIP Address: %s\nDHCP: yes\n", ip);
	return 0;
}

/* ---- Wi-Fi radio: out of scope ------------------------------------ */
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
	if (!buf || bufsize < 8)
		return -1;
	return local_ipv4(buf, bufsize);
}
int mmb_wlan_ipconfig(char *buf, int bufsize)
{
	if (!buf || bufsize < 32)
		return -1;
	snprintf(buf, (size_t)bufsize, "Interface: Wi-Fi\n");
	return 0;
}
void mmb_wlan_poll(void) {}
void mmb_wlan_apply_country(void) {}
