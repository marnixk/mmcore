#include "mmbasic.h"

/*
 * TCP platform layer used by CONNECT and TERM.
 *
 * Hardware images define MMB_CIRCLE_WLAN and already construct CNetSubSystem
 * in wlan.cpp. QEMU raspi3b has no NIC, so the stubs keep CONNECT from hanging.
 *
 * Circle's CSocket::Connect waits until SYN completes or TCP retransmits
 * give up (~90s). That must not run on the interpreter task: TERM/CONNECT
 * would freeze the UI. Open runs on a CTask; the caller polls with a short
 * timeout. Circle patch circle-wifi-149.patch wakes Connect() on abort.
 *
 * CSocket::Receive copies min(buflen, segment) then frees the rest of that
 * TCP buffer, so dest must be at least FRAME_BUFFER_SIZE (1600). This layer
 * only holds one frame leftover. TERM owns the 512KB interpret ring.
 */

#ifdef MMB_CIRCLE_WLAN
#include <circle/net/netsubsystem.h>
#include <circle/net/socket.h>
#include <circle/net/in.h>
#include <circle/net/dnsclient.h>
#include <circle/net/ipaddress.h>
#include <circle/net/error.h>
#include <circle/net/networklayer.h>
#include <circle/net/checksumcalculator.h>
#include <circle/net/icmphandler.h>
#include <circle/net/transportlayer.h>
#include <circle/netdevice.h>
#include <circle/sched/scheduler.h>
#include <circle/sched/task.h>
#include <circle/string.h>
#include <circle/timer.h>
#include <circle/util.h>
#include <circle/macros.h>
#include <circle/new.h>
#endif

#ifdef MMB_CIRCLE_WLAN

#define MMB_NET_CONNECT_MS  25000
#define MMB_NET_GW_PROBE_MS 2000

static CSocket *s_sock;
static u8 s_rx[FRAME_BUFFER_SIZE];
static unsigned s_rxn;
static unsigned s_rxoff;

static char s_open_host[80];
static int s_open_port;
static volatile unsigned s_gen;
static volatile int s_open_done;
static volatile int s_open_result;
static volatile int s_abandon;
static volatile int s_task_busy;
static volatile int s_open_error;
static char s_pending_host[80];
static int s_pending_port;
static volatile int s_pending;
static int s_peer_closed;
static int s_rx_error;

static unsigned s_gw_at;
static int s_gw_cached;

static void set_error(int err)
{
	s_open_error = err;
}

static const char *err_text(int err)
{
	switch (err)
	{
	case 1:
		return "Network not available";
	case 2:
		return "DNS failed";
	case 3:
		return "TCP timeout";
	case 4:
		return "TCP refused";
	default:
		return "Connect failed";
	}
}

static const char *close_text(int err)
{
	switch (err)
	{
	case -NET_ERROR_NOT_CONNECTED:
		return "closed by remote host";
	case -NET_ERROR_CONNECTION_RESET:
		return "reset by peer";
	case -NET_ERROR_CONNECTION_TIMED_OUT:
		return "timed out (no ACK from peer)";
	case -NET_ERROR_CONNECTION_REFUSED:
		return "refused";
	case -NET_ERROR_DESTINATION_UNREACHABLE:
		return "host unreachable";
	case -NET_ERROR_PROTOCOL_ERROR:
		return "protocol error";
	case 0:
		return "";
	default:
		return "network error";
	}
}

static void note_rx_error(int n)
{
	s_peer_closed = 1;
	if (!s_rx_error)
		s_rx_error = n;
}

static void abort_inflight(void)
{
	CNetSubSystem *net = CNetSubSystem::Get();

	if (net && net->GetTransportLayer())
		net->GetTransportLayer()->AbortConnecting();
}

static int wait_net(unsigned ms)
{
	CNetSubSystem *net = CNetSubSystem::Get();
	unsigned start, limit;

	if (!net)
		return 0;
	if (net->IsRunning())
		return 1;
	start = CTimer::GetClockTicks();
	limit = ms * 1000u;
	while (CTimer::GetClockTicks() - start < limit)
	{
		if (net->IsRunning())
			return 1;
		if (CScheduler::IsActive())
			CScheduler::Get()->MsSleep(50);
		else
			CTimer::SimpleMsDelay(50);
	}
	return net->IsRunning() ? 1 : 0;
}

static int gateway_probe(int force)
{
	CNetSubSystem *net = CNetSubSystem::Get();
	CNetConfig *cfg;
	CNetworkLayer *nl;
	const CIPAddress *gw;
	const CIPAddress *own;
	unsigned start, now;
	u8 pkt[12];
	u8 rx[FRAME_BUFFER_SIZE];
	unsigned rxn;
	CIPAddress sender, recv;
	static u16 s_seq;

	now = CTimer::GetClockTicks();
	if (!force && s_gw_at && now - s_gw_at < 2000u * 1000u)
		return s_gw_cached;

	s_gw_cached = 0;
	s_gw_at = now;
	if (!net)
		return 0;
	cfg = net->GetConfig();
	nl = net->GetNetworkLayer();
	if (!cfg || !nl)
		return 0;
	gw = cfg->GetDefaultGateway();
	own = cfg->GetIPAddress();
	if (!gw || !gw->IsSet() || gw->IsNull() || !own || !own->IsSet() || own->IsNull())
		return 0;

	memset(pkt, 0, sizeof pkt);
	pkt[0] = ICMP_TYPE_ECHO;
	pkt[1] = ICMP_CODE_ECHO;
	s_seq++;
	pkt[4] = (u8)(0x4D);
	pkt[5] = (u8)(0x4D);
	pkt[6] = (u8)(s_seq >> 8);
	pkt[7] = (u8)(s_seq & 0xFF);
	pkt[8] = 'm';
	pkt[9] = 'm';
	pkt[10] = 'b';
	pkt[11] = 0;
	{
		u16 csum = CChecksumCalculator::SimpleCalculate(pkt, sizeof pkt);
		memcpy(pkt + 2, &csum, sizeof csum);
	}

	nl->EnableReceiveICMP(TRUE);
	if (!nl->Send(*gw, pkt, sizeof pkt, IPPROTO_ICMP))
	{
		nl->EnableReceiveICMP(FALSE);
		return 0;
	}

	start = CTimer::GetClockTicks();
	while (CTimer::GetClockTicks() - start < MMB_NET_GW_PROBE_MS * 1000u)
	{
		rxn = 0;
		if (nl->ReceiveICMP(rx, &rxn, &sender, &recv) && rxn >= 8)
		{
			if (rx[0] == ICMP_TYPE_ECHO_REPLY &&
			    rx[4] == pkt[4] && rx[5] == pkt[5])
			{
				s_gw_cached = 1;
				s_gw_at = CTimer::GetClockTicks();
				nl->EnableReceiveICMP(FALSE);
				return 1;
			}
		}
		if (CScheduler::IsActive())
			CScheduler::Get()->MsSleep(50);
		else
			CTimer::SimpleMsDelay(50);
	}
	nl->EnableReceiveICMP(FALSE);
	return 0;
}

static int live_net(void)
{
	CNetSubSystem *net = CNetSubSystem::Get();

	if (!net || !net->IsRunning())
		return 0;
	return gateway_probe(0);
}

static int map_connect_rc(int rc)
{
	if (rc == 0)
		return 0;
	if (s_abandon)
	{
		set_error(3);
		return -1;
	}
	if (rc == -NET_ERROR_CONNECTION_REFUSED)
	{
		set_error(4);
		return -1;
	}
	if (rc == -NET_ERROR_CONNECTION_TIMED_OUT)
	{
		set_error(3);
		return -1;
	}
	set_error(5);
	return -1;
}

static int tcp_connect_once(CSocket **out_sock)
{
	CNetSubSystem *net = CNetSubSystem::Get();
	CSocket *sock;
	CIPAddress ip;
	int rc;

	*out_sock = 0;
	if (!net)
	{
		set_error(1);
		return -1;
	}
	if (!wait_net(3000) || !live_net())
	{
		set_error(1);
		return -1;
	}
	if (s_abandon)
		return -1;
	{
		CDNSClient dns(net);
		if (!dns.Resolve(s_open_host, &ip))
		{
			set_error(2);
			return -1;
		}
	}
	if (s_abandon)
		return -1;
	sock = new CSocket(net, IPPROTO_TCP);
	if (!sock)
	{
		set_error(5);
		return -1;
	}
	rc = sock->Connect(ip, (u16)s_open_port);
	if (s_abandon || rc < 0)
	{
		delete sock;
		return map_connect_rc(rc);
	}
	*out_sock = sock;
	set_error(0);
	return 0;
}

class CTcpOpenTask : public CTask
{
public:
	CTcpOpenTask(unsigned gen)
	: CTask(TASK_STACK_SIZE),
	  m_gen(gen)
	{
	}

	void Run(void)
	{
		CSocket *sock = 0;
		int rc;

		s_task_busy = 1;
		rc = tcp_connect_once(&sock);

		if (m_gen != s_gen || s_abandon)
		{
			if (sock)
				delete sock;
			s_task_busy = 0;
			return;
		}
		if (rc == 0)
		{
			s_sock = sock;
			s_open_result = 0;
		}
		else
		{
			s_open_result = -1;
		}
		s_open_done = 1;
		s_task_busy = 0;
	}

private:
	unsigned m_gen;
};

static void start_open_task(void)
{
	s_open_done = 0;
	s_open_result = -1;
	s_abandon = 0;
	s_gen++;
	new CTcpOpenTask(s_gen);
}

extern "C" {

int mmb_net_gateway_ok(int force)
{
	CNetSubSystem *net = CNetSubSystem::Get();

	if (!net || !net->IsRunning())
		return 0;
	return gateway_probe(force ? 1 : 0);
}

void mmb_net_tcp_close(void)
{
	s_abandon = 1;
	s_pending = 0;
	s_peer_closed = 0;
	s_gen++;
	s_rxn = 0;
	s_rxoff = 0;
	abort_inflight();
	if (s_sock)
	{
		delete s_sock;
		s_sock = 0;
	}
}

int mmb_net_available(void)
{
	return live_net() ? 1 : 0;
}

const char *mmb_net_tcp_errmsg(void)
{
	return err_text(s_open_error);
}

const char *mmb_net_tcp_close_reason(void)
{
	return close_text(s_rx_error);
}

int mmb_net_tcp_cancelling(void)
{
	return (s_pending && s_task_busy) ? 1 : 0;
}

int mmb_net_tcp_begin(const char *host, int port)
{
	unsigned n;

	if (!host || !host[0] || port < 1 || port > 65535)
	{
		set_error(5);
		return -1;
	}
	if (!CNetSubSystem::Get())
	{
		set_error(1);
		return -1;
	}
	if (!CScheduler::IsActive())
	{
		set_error(1);
		return -1;
	}

	s_abandon = 1;
	s_gen++;
	abort_inflight();
	if (s_sock)
	{
		delete s_sock;
		s_sock = 0;
	}
	s_rxn = 0;
	s_rxoff = 0;
	s_peer_closed = 0;
	s_rx_error = 0;

	n = 0;
	while (host[n] && n + 1 < sizeof s_open_host)
	{
		s_open_host[n] = host[n];
		n++;
	}
	s_open_host[n] = 0;
	s_open_port = port;
	set_error(0);

	if (s_task_busy)
	{
		n = 0;
		while (host[n] && n + 1 < sizeof s_pending_host)
		{
			s_pending_host[n] = host[n];
			n++;
		}
		s_pending_host[n] = 0;
		s_pending_port = port;
		s_pending = 1;
		return 0;
	}

	s_pending = 0;
	start_open_task();
	return 0;
}

int mmb_net_tcp_status(void)
{
	if (s_sock)
		return 1;
	if (s_pending && !s_task_busy)
	{
		unsigned n = 0;

		while (s_pending_host[n] && n + 1 < sizeof s_open_host)
		{
			s_open_host[n] = s_pending_host[n];
			n++;
		}
		s_open_host[n] = 0;
		s_open_port = s_pending_port;
		s_pending = 0;
		start_open_task();
		return 0;
	}
	if (s_pending)
		return 0;
	if (s_abandon && !s_task_busy && !s_open_done)
		return -1;
	if (s_open_done)
		return s_open_result == 0 ? 1 : -1;
	return 0;
}

int mmb_net_tcp_open(const char *host, int port)
{
	unsigned start;

	if (mmb_net_tcp_begin(host, port) != 0)
		return -1;
	start = CTimer::GetClockTicks();
	for (;;)
	{
		int st = mmb_net_tcp_status();
		if (st == 1)
			return 0;
		if (st < 0)
			return -1;
		if (CTimer::GetClockTicks() - start > MMB_NET_CONNECT_MS * 1000u)
		{
			set_error(3);
			mmb_net_tcp_close();
			return -1;
		}
		if (CScheduler::IsActive())
			CScheduler::Get()->MsSleep(50);
		else
			CTimer::SimpleMsDelay(50);
	}
}

int mmb_net_tcp_send(const void *data, unsigned n)
{
	int rc;

	if (!s_sock || !data || !n)
		return -1;
	rc = s_sock->Send(data, n, MSG_DONTWAIT);
	if (CScheduler::IsActive())
		CScheduler::Get()->Yield();
	return rc;
}

int mmb_net_tcp_recv(void *data, unsigned maxn)
{
	unsigned char *dst;
	unsigned out = 0;

	if (!s_sock)
		return -1;
	if (!data || !maxn)
		return 0;
	dst = (unsigned char *)data;
	for (;;)
	{
		if (s_rxoff < s_rxn)
		{
			unsigned n = s_rxn - s_rxoff;
			if (n > maxn - out)
				n = maxn - out;
			memcpy(dst + out, s_rx + s_rxoff, n);
			s_rxoff += n;
			out += n;
			if (out == maxn)
				return (int)out;
			continue;
		}
		s_rxn = 0;
		s_rxoff = 0;
		{
			int n = s_sock->Receive(s_rx, FRAME_BUFFER_SIZE, MSG_DONTWAIT);
			if (CScheduler::IsActive())
				CScheduler::Get()->Yield();
			if (n < 0)
			{
				note_rx_error(n);
				return out ? (int)out : n;
			}
			if (n == 0)
				return (int)out;
			s_rxn = (unsigned)n;
			s_rxoff = 0;
		}
	}
}

int mmb_net_tcp_rx_avail(void)
{
	if (!s_sock || s_peer_closed)
		return 0;
	if (s_rxoff < s_rxn)
		return (int)(s_rxn - s_rxoff);
	{
		int n = s_sock->Receive(s_rx, FRAME_BUFFER_SIZE, MSG_DONTWAIT);
		if (CScheduler::IsActive())
			CScheduler::Get()->Yield();
		if (n < 0)
		{
			note_rx_error(n);
			return 0;
		}
		if (n <= 0)
			return 0;
		s_rxn = (unsigned)n;
		s_rxoff = 0;
		return n;
	}
}

int mmb_net_tcp_peer_closed(void)
{
	return s_peer_closed;
}

void mmb_net_yield(void)
{
	if (CScheduler::IsActive())
		CScheduler::Get()->Yield();
}

}

#else /* !MMB_CIRCLE_WLAN */

extern "C" {

int mmb_net_available(void)
{
	return 0;
}

int mmb_net_gateway_ok(int force)
{
	(void)force;
	return 0;
}

const char *mmb_net_tcp_errmsg(void)
{
	return "Network not available";
}

const char *mmb_net_tcp_close_reason(void)
{
	return "";
}

int mmb_net_tcp_cancelling(void)
{
	return 0;
}

int mmb_net_tcp_begin(const char *host, int port)
{
	(void)host;
	(void)port;
	return -1;
}

int mmb_net_tcp_status(void)
{
	return -1;
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

int mmb_net_tcp_rx_avail(void)
{
	return 0;
}

int mmb_net_tcp_peer_closed(void)
{
	return 1;
}

void mmb_net_tcp_close(void)
{
}

void mmb_net_yield(void)
{
}

}

#endif
