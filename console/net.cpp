#include "mmbasic.h"

/*
 * TCP platform layer used by CONNECT.
 *
 * Hardware images define MMB_CIRCLE_WLAN and already construct CNetSubSystem
 * in wlan.cpp. QEMU raspi3b has no NIC, so the stubs keep CONNECT from hanging.
 */

#ifdef MMB_CIRCLE_WLAN
#include <circle/net/netsubsystem.h>
#include <circle/net/socket.h>
#include <circle/net/in.h>
#include <circle/netdevice.h>
#include <circle/sched/scheduler.h>
#include <circle/string.h>
#include <circle/timer.h>
#include <circle/util.h>
#include <circle/new.h>
#endif

extern "C" {

#ifdef MMB_CIRCLE_WLAN

static CSocket *s_sock;
static u8 s_rx[FRAME_BUFFER_SIZE];
static unsigned s_rxn;
static unsigned s_rxoff;

void mmb_net_tcp_close(void)
{
	s_rxn = 0;
	s_rxoff = 0;
	if (s_sock)
	{
		delete s_sock;
		s_sock = 0;
	}
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

int mmb_net_available(void)
{
	CNetSubSystem *net = CNetSubSystem::Get();
	return (net && net->IsRunning()) ? 1 : 0;
}

int mmb_net_tcp_open(const char *host, int port)
{
	CNetSubSystem *net = CNetSubSystem::Get();
	CString portstr;

	if (!host || !host[0] || port < 1 || port > 65535)
		return -1;
	if (!net)
		return -1;
	if (!wait_net(15000))
		return -1;
	mmb_net_tcp_close();
	s_sock = new CSocket(net, IPPROTO_TCP);
	if (!s_sock)
		return -1;
	portstr.Format("%u", (unsigned)port);
	if (static_cast<CNetSocket *>(s_sock)->Connect(host, (const char *)portstr) < 0)
	{
		delete s_sock;
		s_sock = 0;
		return -1;
	}
	return 0;
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

#else /* !MMB_CIRCLE_WLAN */

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

#endif

}
