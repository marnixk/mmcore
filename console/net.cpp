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
 * timeout.
 */

#ifdef MMB_CIRCLE_WLAN
#include <circle/net/netsubsystem.h>
#include <circle/net/socket.h>
#include <circle/net/in.h>
#include <circle/netdevice.h>
#include <circle/sched/scheduler.h>
#include <circle/sched/task.h>
#include <circle/string.h>
#include <circle/timer.h>
#include <circle/util.h>
#include <circle/new.h>
#endif

#ifdef MMB_CIRCLE_WLAN

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

static int tcp_connect_once(CSocket **out_sock)
{
	CNetSubSystem *net = CNetSubSystem::Get();
	CString portstr;
	CSocket *sock;

	*out_sock = 0;
	if (!net)
		return -1;
	if (!wait_net(3000))
		return -1;
	sock = new CSocket(net, IPPROTO_TCP);
	if (!sock)
		return -1;
	portstr.Format("%u", (unsigned)s_open_port);
	if (static_cast<CNetSocket *>(sock)->Connect(s_open_host,
						      (const char *)portstr) < 0)
	{
		delete sock;
		return -1;
	}
	*out_sock = sock;
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
		int rc = tcp_connect_once(&sock);

		if (m_gen != s_gen || s_abandon)
		{
			if (sock)
				delete sock;
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
	}

private:
	unsigned m_gen;
};

extern "C" {

void mmb_net_tcp_close(void)
{
	s_abandon = 1;
	s_gen++;
	s_rxn = 0;
	s_rxoff = 0;
	if (s_sock)
	{
		delete s_sock;
		s_sock = 0;
	}
}

int mmb_net_available(void)
{
	CNetSubSystem *net = CNetSubSystem::Get();
	return (net && net->IsRunning()) ? 1 : 0;
}

int mmb_net_tcp_begin(const char *host, int port)
{
	unsigned n;

	if (!host || !host[0] || port < 1 || port > 65535)
		return -1;
	if (!CNetSubSystem::Get())
		return -1;
	if (!CScheduler::IsActive())
		return -1;

	s_abandon = 1;
	s_gen++;
	if (s_sock)
	{
		delete s_sock;
		s_sock = 0;
	}
	s_rxn = 0;
	s_rxoff = 0;

	n = 0;
	while (host[n] && n + 1 < sizeof s_open_host)
	{
		s_open_host[n] = host[n];
		n++;
	}
	s_open_host[n] = 0;
	s_open_port = port;
	s_open_done = 0;
	s_open_result = -1;
	s_abandon = 0;
	s_gen++;
	new CTcpOpenTask(s_gen);
	return 0;
}

int mmb_net_tcp_status(void)
{
	if (s_sock)
		return 1;
	if (s_abandon)
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
		if (CTimer::GetClockTicks() - start > 8000u * 1000u)
		{
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
	/* Circle CSocket::Receive copies min(buflen, segment) then frees
	 * the rest of the TCP buffer, so a small dest would drop bytes.
	 * Always pull a full FRAME_BUFFER_SIZE and hold leftovers. */
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
				return out ? (int)out : n;
			if (n == 0)
				return (int)out;
			s_rxn = (unsigned)n;
			s_rxoff = 0;
		}
	}
}

}

#else /* !MMB_CIRCLE_WLAN */

extern "C" {

int mmb_net_available(void)
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

void mmb_net_tcp_close(void)
{
}

}

#endif
