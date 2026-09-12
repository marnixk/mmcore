#include "mmbasic.h"
#include <circle/util.h>

/*
 * Wired Ethernet (DHCP). Shares Circle's singleton CNetSubSystem with Wi-Fi:
 * only one interface is active. MMB_CIRCLE_NET opens NetDeviceTypeEthernet:
 * GENET on Pi 4, MACB on Pi 5, USB LAN9514/LAN7800 on Pi 3, USB CDC
 * (QEMU -device usb-net) on raspi3b. No gadget/NIC means not available.
 */

#ifdef MMB_CIRCLE_NET
#include <circle/net/netsubsystem.h>
#include <circle/net/ipaddress.h>
#include <circle/macaddress.h>
#include <circle/netdevice.h>
#include <circle/sched/scheduler.h>
#include <circle/string.h>
#include <circle/timer.h>
#include <circle/stdarg.h>
#endif

#ifdef MMB_CIRCLE_NET
CNetSubSystem *mmb_circle_net(void);
#endif

extern "C" {
int mmb_net_gateway_ok(int force);

#ifdef MMB_CIRCLE_NET

static void copy_out(char *buf, int bufsize, const char *s)
{
	int i = 0;

	if (!buf || bufsize < 1)
		return;
	if (!s)
		s = "";
	while (s[i] && i < bufsize - 1)
	{
		buf[i] = s[i];
		i++;
	}
	buf[i] = 0;
}

static void eth_log(const char *fmt, ...)
{
	CString line;
	va_list ap;

	if (!mmb_opt_wifi_debug())
		return;
	va_start(ap, fmt);
	line.FormatV(fmt, ap);
	va_end(ap);
	mmb_console_write("\n[eth] ");
	mmb_console_write((const char *)line);
	mmb_console_write("\r\n");
}

static void append_ip(CString *out, const char *label, const CIPAddress *ip)
{
	CString s;

	if (!out || !label || !ip || !ip->IsSet())
		return;
	ip->Format(&s);
	out->Append(label);
	out->Append((const char *)s);
	out->Append("\n");
}

int mmb_eth_available(void)
{
	return CNetDevice::GetNetDevice(NetDeviceTypeEthernet) ? 1 : 0;
}

int mmb_eth_start(void)
{
	int kind = mmb_net_kind();

	if (kind == MMB_NET_ETH)
		return 0;
	if (kind == MMB_NET_WIFI)
	{
		eth_log("Wi-Fi owns the net stack");
		return -2;
	}
	if (!CNetDevice::GetNetDevice(NetDeviceTypeEthernet))
	{
		eth_log("no Ethernet device");
		return -1;
	}
	eth_log("initialising Ethernet (DHCP)");
	if (mmb_net_open(MMB_NET_ETH) != 0)
	{
		eth_log("net stack Initialize() failed");
		return -1;
	}
	eth_log("net stack ready (DHCP after link)");
	return 0;
}

int mmb_eth_status(void)
{
	CNetSubSystem *net;
	CNetDevice *dev;

	if (mmb_net_kind() != MMB_NET_ETH)
		return 0;
	net = mmb_circle_net();
	if (!net)
		return 0;
	dev = CNetDevice::GetNetDevice(NetDeviceTypeEthernet);
	if (dev && dev->IsLinkUp())
		return 1;
	return net->IsRunning() ? 1 : 0;
}

int mmb_eth_wait_dhcp(unsigned ms)
{
	CNetSubSystem *net = mmb_circle_net();
	unsigned start, limit;

	if (!net)
		return 0;
	start = CTimer::GetClockTicks();
	limit = ms * 1000u;
	while (CTimer::GetClockTicks() - start < limit)
	{
		if (net->IsRunning())
			return 1;
		if (CScheduler::IsActive())
			CScheduler::Get()->MsSleep(100);
		else
			CTimer::SimpleMsDelay(100);
	}
	return net->IsRunning() ? 1 : 0;
}

int mmb_eth_ip(char *buf, int bufsize)
{
	CNetSubSystem *net;
	CNetConfig *cfg;
	const CIPAddress *ip;
	CString ipstr;

	if (!buf || bufsize < 1)
		return -1;
	buf[0] = 0;
	net = mmb_circle_net();
	if (!net || mmb_net_kind() != MMB_NET_ETH)
		return -1;
	cfg = net->GetConfig();
	ip = cfg ? cfg->GetIPAddress() : 0;
	if (!net->IsRunning() || !ip || !ip->IsSet() || ip->IsNull())
		return -1;
	ip->Format(&ipstr);
	copy_out(buf, bufsize, (const char *)ipstr);
	return 0;
}

int mmb_eth_ipconfig(char *buf, int bufsize)
{
	CString out;
	CNetSubSystem *net;
	CNetConfig *cfg;
	CString ipstr;
	const CIPAddress *ip;
	CNetDevice *dev;

	if (!buf || bufsize < 1)
		return -1;
	buf[0] = 0;
	if (mmb_net_kind() == MMB_NET_WIFI)
	{
		copy_out(buf, bufsize, "Interface: Ethernet\nWi-Fi is active; reboot to use Ethernet");
		return -1;
	}
	if (mmb_eth_start() != 0 && mmb_net_kind() != MMB_NET_ETH)
	{
		copy_out(buf, bufsize, "Interface: Ethernet\nEthernet not available");
		return -1;
	}
	net = mmb_circle_net();
	if (!net)
	{
		copy_out(buf, bufsize, "Interface: Ethernet\nEthernet not available");
		return -1;
	}
	cfg = net->GetConfig();
	ip = cfg ? cfg->GetIPAddress() : 0;
	dev = CNetDevice::GetNetDevice(NetDeviceTypeEthernet);
	if (!net->IsRunning() || !ip || !ip->IsSet() || ip->IsNull())
	{
		out = "Interface: Ethernet\nNot connected";
		if (dev && dev->IsLinkUp())
			out.Append("\n  Link is up; waiting for DHCP");
		else
			out.Append("\n  Link is down");
		copy_out(buf, bufsize, (const char *)out);
		return -1;
	}
	if (mmb_net_gateway_ok(1) != 1)
	{
		out = "Interface: Ethernet\nNot connected";
		if (dev && dev->IsLinkUp())
			out.Append("\n  Link is up; gateway unreachable");
		copy_out(buf, bufsize, (const char *)out);
		return -1;
	}
	ip->Format(&ipstr);
	out.Format("Interface: Ethernet\nConnected as %s\n", (const char *)ipstr);
	if (cfg)
	{
		const u8 *mask = cfg->GetNetMask();
		append_ip(&out, "  Gateway: ", cfg->GetDefaultGateway());
		if (mask)
		{
			CIPAddress netmask(mask);
			append_ip(&out, "  Netmask: ", &netmask);
		}
		append_ip(&out, "  DNS: ", cfg->GetDNSServer());
		out.Append(cfg->IsDHCPUsed() ? "  DHCP: yes\n" : "  DHCP: no\n");
	}
	if (net->GetNetDeviceLayer())
	{
		const CMACAddress *mac = net->GetNetDeviceLayer()->GetMACAddress();
		if (mac)
		{
			CString macstr;
			mac->Format(&macstr);
			out.Append("  MAC: ");
			out.Append((const char *)macstr);
			out.Append("\n");
		}
	}
	if (dev)
	{
		TNetDeviceSpeed speed = dev->GetLinkSpeed();
		if (speed != NetDeviceSpeedUnknown)
		{
			out.Append("  Speed: ");
			out.Append(CNetDevice::GetSpeedString(speed));
			out.Append("\n");
		}
	}
	copy_out(buf, bufsize, (const char *)out);
	return 0;
}

#else /* !MMB_CIRCLE_NET */

int mmb_eth_available(void)
{
	return 0;
}

int mmb_eth_start(void)
{
	return -1;
}

int mmb_eth_status(void)
{
	return 0;
}

int mmb_eth_wait_dhcp(unsigned ms)
{
	(void)ms;
	return 0;
}

int mmb_eth_ip(char *buf, int bufsize)
{
	if (buf && bufsize > 0)
		buf[0] = 0;
	return -1;
}

int mmb_eth_ipconfig(char *buf, int bufsize)
{
	const char *msg = "Interface: Ethernet\nEthernet not available";
	int i = 0;

	if (!buf || bufsize < 1)
		return -1;
	while (msg[i] && i < bufsize - 1)
	{
		buf[i] = msg[i];
		i++;
	}
	buf[i] = 0;
	return -1;
}

#endif

}
