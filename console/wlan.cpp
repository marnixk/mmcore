#include "mmbasic.h"
#include "wlan_escan.h"
#include <circle/util.h>

/*
 * Thin WLAN platform layer.
 *
 * Circle's CYW4343x driver lives in addon/wlan (CBcm4343Device). Hardware
 * images define MMB_CIRCLE_WLAN and link libwlan + hostap. QEMU raspi3b has
 * no radio (Circle --qemu sets NO_SDHOST), so the stubs below always report
 * unavailable.
 *
 * Firmware files must sit on the SD volume (C:/firmware/, Circle SD:/firmware/).
 * Init is lazy so a missing radio cannot break the 115200 serial console.
 *
 * On Pi 3 the WLAN chip and the SD card share SDIO unless Circle USE_SDHOST
 * is on. Hardware builds (no --qemu) leave SDHOST enabled; QEMU sets
 * NO_SDHOST and never compiles this path.
 */

#ifdef MMB_CIRCLE_WLAN
#include <wlan/bcm4343.h>
#include <wlan/hostap/wpa_supplicant/wpasupplicant.h>
#include <circle/net/netsubsystem.h>
#include <circle/net/ipaddress.h>
#include <circle/macaddress.h>
#include <circle/netdevice.h>
#include <circle/sched/scheduler.h>
#include <circle/string.h>
#include <circle/new.h>
#include <circle/timer.h>
#endif

extern "C" {
int mmb_vfs_exists(const char *path);
int mmb_vfs_write(const char *path, const void *data, unsigned n, int append);
int mmb_keyword_eq(const char *a, const char *b);
}

extern "C" {

#ifdef MMB_CIRCLE_WLAN

static CScheduler *s_sched;
static CBcm4343Device *s_wlan;
static CNetSubSystem *s_net;
static CWPASupplicant *s_wpa;
static int s_tried;
static int s_ready;
static char s_last_ssid[64];
static char s_last_psk[64];

static void wlan_emit(const char *s)
{
	mmb_console_write(s);
}

static void wlan_log(const char *fmt, ...)
{
	CString line;
	va_list ap;

	if (!mmb_opt_wifi_debug())
		return;
	va_start(ap, fmt);
	line.FormatV(fmt, ap);
	va_end(ap);
	wlan_emit("\n[wifi] ");
	wlan_emit((const char *)line);
	wlan_emit("\r\n");
}

static int firmware_present(void)
{
	return mmb_vfs_exists("C:/firmware/brcmfmac43430-sdio.bin")
	    || mmb_vfs_exists("C:/firmware/brcmfmac43455-sdio.bin")
	    || mmb_vfs_exists("C:/firmware/brcmfmac43436-sdio.bin")
	    || mmb_vfs_exists("C:/firmware/brcmfmac43436s-sdio.bin")
	    || mmb_vfs_exists("C:/firmware/brcmfmac43456-sdio.bin");
}

static void sched_ensure(void)
{
	if (!CScheduler::IsActive())
		s_sched = new CScheduler();
}

static void yield_some(void)
{
	if (CScheduler::IsActive())
		CScheduler::Get()->Yield();
}

static int wait_link(unsigned ms)
{
	unsigned start = CTimer::GetClockTicks();
	unsigned limit = ms * 1000u;
	unsigned last_note = 0;
	while (CTimer::GetClockTicks() - start < limit)
	{
		int up = 0;
		if (s_wlan && s_wlan->IsLinkUp())
			up = 1;
		if (s_wpa && CWPASupplicant::IsConnected())
			up = 1;
		if (up)
			return 1;
		{
			unsigned elapsed = (CTimer::GetClockTicks() - start) / 1000000u;
			if (elapsed >= last_note + 2)
			{
				last_note = elapsed;
				wlan_log("waiting for link %us/%us", elapsed, ms / 1000u);
			}
		}
		if (CScheduler::IsActive())
			CScheduler::Get()->MsSleep(100);
		else
			CTimer::SimpleMsDelay(100);
	}
	return (s_wlan && s_wlan->IsLinkUp()) ||
	       (s_wpa && CWPASupplicant::IsConnected()) ? 1 : 0;
}

static int wait_dhcp(unsigned ms)
{
	unsigned start, limit, last_note;

	if (!s_net)
		return 0;
	start = CTimer::GetClockTicks();
	limit = ms * 1000u;
	last_note = 0;
	while (CTimer::GetClockTicks() - start < limit)
	{
		if (s_net->IsRunning())
			return 1;
		{
			unsigned elapsed = (CTimer::GetClockTicks() - start) / 1000000u;
			if (elapsed >= last_note + 2)
			{
				last_note = elapsed;
				wlan_log("waiting for DHCP %us/%us", elapsed, ms / 1000u);
			}
		}
		if (CScheduler::IsActive())
			CScheduler::Get()->MsSleep(100);
		else
			CTimer::SimpleMsDelay(100);
	}
	return s_net->IsRunning() ? 1 : 0;
}

static int apply_radio_country(void)
{
	const char *cc = mmb_opt_wifi_country();

	if (!s_wlan)
		return 0;
	if (!s_wlan->Control("country %s", cc))
	{
		wlan_log("radio country %s rejected", cc);
		return 0;
	}
	wlan_log("radio country %s", cc);
	return 1;
}

static int wlan_ensure(void)
{
	if (s_ready)
		return 1;
	if (!firmware_present())
	{
		static int noted;
		if (!noted)
		{
			wlan_log("no brcmfmac firmware on C:/firmware/");
			noted = 1;
		}
		return 0;
	}
	if (s_tried)
		return 0;
	s_tried = 1;
	s_ready = 0;
	wlan_log("firmware present, initialising radio");
	sched_ensure();
	s_wlan = new CBcm4343Device("SD:/firmware/");
	if (!s_wlan)
	{
		wlan_log("radio alloc failed");
		return 0;
	}
	if (!s_wlan->Initialize())
	{
		wlan_log("radio Initialize() failed");
		delete s_wlan;
		s_wlan = 0;
		return 0;
	}
	wlan_log("radio up");
	if (CScheduler::IsActive())
		CScheduler::Get()->MsSleep(300);
	else
		CTimer::SimpleMsDelay(300);
	if (!apply_radio_country())
		wlan_log("country apply failed; join needs a Circle-accepted code");
	s_net = new CNetSubSystem(0, 0, 0, 0, "mmbasic", NetDeviceTypeWLAN);
	if (!s_net)
		wlan_log("net stack alloc failed");
	else if (!s_net->Initialize(FALSE))
	{
		wlan_log("net stack Initialize() failed");
		delete s_net;
		s_net = 0;
	}
	else
		wlan_log("net stack ready (DHCP after link)");
	s_ready = 1;
	return 1;
}

int mmb_wlan_radio_pending(void)
{
	if (s_ready || s_tried)
		return 0;
	return firmware_present() ? 0 : 1;
}

static void append_quoted(char *out, unsigned *n, unsigned max, const char *s)
{
	if (*n + 1 < max)
		out[(*n)++] = '"';
	while (s && *s && *n + 2 < max)
	{
		if (*s == '"' || *s == '\\')
			out[(*n)++] = '\\';
		out[(*n)++] = *s++;
	}
	if (*n + 1 < max)
		out[(*n)++] = '"';
}

static int write_wpa_conf(const char *ssid, const char *psk)
{
	char buf[512];
	unsigned n = 0;
	const char *cc = mmb_opt_wifi_country();
	const char *mid = "\n\tpsk=";
	const char *wpa = "\n\tproto=WPA2\n\tkey_mgmt=WPA-PSK\n}\n";
	const char *open_tail = "\n\tkey_mgmt=NONE\n}\n";
	const char *p;

	strncpy(s_last_ssid, ssid ? ssid : "", sizeof(s_last_ssid) - 1);
	s_last_ssid[sizeof(s_last_ssid) - 1] = 0;
	strncpy(s_last_psk, psk ? psk : "", sizeof(s_last_psk) - 1);
	s_last_psk[sizeof(s_last_psk) - 1] = 0;

	p = "country=";
	while (*p && n + 1 < sizeof buf)
		buf[n++] = *p++;
	while (*cc && n + 1 < sizeof buf)
		buf[n++] = *cc++;
	p = "\nnetwork={\n\tssid=";
	while (*p && n + 1 < sizeof buf)
		buf[n++] = *p++;
	append_quoted(buf, &n, sizeof buf, ssid);
	if (psk && psk[0])
	{
		while (*mid && n + 1 < sizeof buf)
			buf[n++] = *mid++;
		append_quoted(buf, &n, sizeof buf, psk);
		while (*wpa && n + 1 < sizeof buf)
			buf[n++] = *wpa++;
	}
	else
	{
		while (*open_tail && n + 1 < sizeof buf)
			buf[n++] = *open_tail++;
	}
	buf[n] = 0;
	if (mmb_vfs_write("C:/wpa_supplicant.conf", buf, n, 0) != 0)
	{
		wlan_log("cannot write C:/wpa_supplicant.conf (need SD C:)");
		return 0;
	}
	wlan_log("wrote C:/wpa_supplicant.conf ssid=%s psk=%s country=%s proto=WPA2",
		 ssid, (psk && psk[0]) ? "yes" : "no", mmb_opt_wifi_country());
	return 1;
}

static int start_wpa(const char *ssid, const char *psk)
{
	if (!write_wpa_conf(ssid, psk))
		return 0;
	if (s_wpa)
	{
		wlan_log("restarting wpa_supplicant");
		delete s_wpa;
		s_wpa = 0;
		if (CScheduler::IsActive())
			CScheduler::Get()->MsSleep(200);
		else
			CTimer::SimpleMsDelay(200);
	}
	s_wpa = new CWPASupplicant("SD:/wpa_supplicant.conf");
	if (!s_wpa)
	{
		wlan_log("wpa_supplicant alloc failed");
		return 0;
	}
	if (!s_wpa->Initialize())
	{
		wlan_log("wpa_supplicant Initialize() failed");
		delete s_wpa;
		s_wpa = 0;
		return 0;
	}
	wlan_log("wpa_supplicant started");
	return 1;
}

static int add_ssid(char ssids[][64], int *count, int maxn, const char *tmp)
{
	int k;
	for (k = 0; k < *count; k++)
		if (mmb_keyword_eq(ssids[k], tmp))
			return 0;
	if (*count >= maxn)
		return 0;
	strncpy(ssids[*count], tmp, 63);
	ssids[*count][63] = 0;
	(*count)++;
	return 1;
}

int mmb_wlan_available(void)
{
	return wlan_ensure();
}

void mmb_wlan_poll(void)
{
	yield_some();
}

static void ingest_escan(const u8 *buf, unsigned nlen,
			 char ssids[][64], int *count, int maxn,
			 int *beacons, int *skipped)
{
	char found[8][MMB_ESCAN_SSID_CAP];
	int n, i;

	n = mmb_escan_ssids(buf, nlen, found, 8, beacons, skipped);
	for (i = 0; i < n; i++)
		if (add_ssid(ssids, count, maxn, found[i]))
			wlan_log("ssid \"%s\"", found[i]);
}

int mmb_wlan_scan(char ssids[][64], int maxn)
{
	u8 buf[FRAME_BUFFER_SIZE];
	unsigned nlen;
	unsigned start;
	int count = 0;
	int beacons = 0, skipped = 0;

	if (!wlan_ensure() || !s_wlan || maxn <= 0)
		return 0;
	if (!apply_radio_country())
		return 0;
	wlan_log("scan start (5s escan)");
	s_wlan->Control("escan 5");
	start = CTimer::GetClockTicks();
	while (CTimer::GetClockTicks() - start < 4 * 1000000)
	{
		nlen = 0;
		if (s_wlan->ReceiveScanResult(buf, &nlen) && nlen)
			ingest_escan(buf, nlen, ssids, &count, maxn, &beacons, &skipped);
		yield_some();
	}
	/* Circle's WPA driver stops escan then drains the queue. */
	s_wlan->Control("escan 0");
	start = CTimer::GetClockTicks();
	while (CTimer::GetClockTicks() - start < 300000)
	{
		nlen = 0;
		if (s_wlan->ReceiveScanResult(buf, &nlen) && nlen)
			ingest_escan(buf, nlen, ssids, &count, maxn, &beacons, &skipped);
		else
			yield_some();
	}
	wlan_log("scan done: %d network(s), %d beacon(s), %d skipped",
		 count, beacons, skipped);
	return count;
}

int mmb_wlan_start(const char *ssid, const char *psk)
{
	if (!ssid || !ssid[0])
		return -1;
	if (!wlan_ensure() || !s_wlan)
		return -1;
	if (!apply_radio_country())
		return -1;
	strncpy(s_last_ssid, ssid, sizeof(s_last_ssid) - 1);
	s_last_ssid[sizeof(s_last_ssid) - 1] = 0;
	strncpy(s_last_psk, psk ? psk : "", sizeof(s_last_psk) - 1);
	s_last_psk[sizeof(s_last_psk) - 1] = 0;
	if (!psk || !psk[0])
	{
		wlan_log("join open ssid=\"%s\"", ssid);
		if (!s_wlan->JoinOpenNet(ssid))
		{
			wlan_log("JoinOpenNet failed");
			return -1;
		}
		return 0;
	}
	wlan_log("join WPA2 ssid=\"%s\"", ssid);
	return start_wpa(ssid, psk) ? 0 : -1;
}

int mmb_wlan_connect(const char *ssid, const char *psk)
{
	if (mmb_wlan_start(ssid, psk) != 0)
		return -1;
	if (wait_link(20000))
	{
		wlan_log("link up");
		if (wait_dhcp(15000))
			wlan_log("DHCP bound");
		else
			wlan_log("DHCP timeout");
		return 0;
	}
	wlan_log("link timeout (radio=%d wpa=%d)",
		 s_wlan && s_wlan->IsLinkUp() ? 1 : 0,
		 s_wpa && CWPASupplicant::IsConnected() ? 1 : 0);
	return -1;
}

int mmb_wlan_status(void)
{
	if (!s_ready || !s_wlan)
		return 0;
	if (s_wpa && CWPASupplicant::IsConnected())
		return 1;
	return s_wlan->IsLinkUp() ? 1 : 0;
}

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

static int current_ipv4(CString *ipstr)
{
	CNetConfig *cfg;
	const CIPAddress *ip;

	if (!ipstr || !s_net)
		return -1;
	cfg = s_net->GetConfig();
	ip = cfg ? cfg->GetIPAddress() : 0;
	if (!s_net->IsRunning() || !ip || !ip->IsSet() || ip->IsNull())
		return -1;
	ip->Format(ipstr);
	return 0;
}

int mmb_wlan_ip(char *buf, int bufsize)
{
	CString ipstr;

	if (!buf || bufsize < 1)
		return -1;
	buf[0] = 0;
	if (current_ipv4(&ipstr) != 0)
		return -1;
	copy_out(buf, bufsize, (const char *)ipstr);
	return 0;
}

int mmb_wlan_ipconfig(char *buf, int bufsize)
{
	CString out;
	CNetConfig *cfg;
	CString ipstr;

	if (!buf || bufsize < 1)
		return -1;
	buf[0] = 0;
	if (!wlan_ensure() || !s_wlan)
	{
		copy_out(buf, bufsize, "Wi-Fi not available");
		return -1;
	}
	if (!s_net)
	{
		out = "Not connected";
		if (s_last_ssid[0])
		{
			out.Append("\n  SSID: ");
			out.Append(s_last_ssid);
		}
		copy_out(buf, bufsize, (const char *)out);
		return -1;
	}
	cfg = s_net->GetConfig();
	if (current_ipv4(&ipstr) != 0)
	{
		out = "Not connected";
		if (s_last_ssid[0])
		{
			out.Append("\n  SSID: ");
			out.Append(s_last_ssid);
		}
		if (mmb_wlan_status())
			out.Append("\n  Link is up; waiting for DHCP");
		copy_out(buf, bufsize, (const char *)out);
		return -1;
	}
	out.Format("Connected as %s\n", (const char *)ipstr);
	if (s_last_ssid[0])
	{
		out.Append("  SSID: ");
		out.Append(s_last_ssid);
		out.Append("\n");
	}
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
	{
		const char *cc = mmb_opt_wifi_country();
		if (cc && cc[0])
		{
			out.Append("  Country: ");
			out.Append(cc);
			out.Append("\n");
		}
	}
	if (s_net->GetNetDeviceLayer())
	{
		const CMACAddress *mac = s_net->GetNetDeviceLayer()->GetMACAddress();
		if (mac)
		{
			CString macstr;
			mac->Format(&macstr);
			out.Append("  MAC: ");
			out.Append((const char *)macstr);
			out.Append("\n");
		}
	}
	copy_out(buf, bufsize, (const char *)out);
	return 0;
}

void mmb_wlan_apply_country(void)
{
	if (s_wlan && !apply_radio_country())
		return;
	if (s_last_ssid[0] && s_wpa)
		start_wpa(s_last_ssid, s_last_psk);
	else if (s_last_ssid[0])
		write_wpa_conf(s_last_ssid, s_last_psk);
}

#else /* !MMB_CIRCLE_WLAN */

int mmb_wlan_available(void)
{
	return 0;
}

int mmb_wlan_radio_pending(void)
{
	return 0;
}

void mmb_wlan_poll(void)
{
}

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

int mmb_wlan_status(void)
{
	return 0;
}

int mmb_wlan_ip(char *buf, int bufsize)
{
	if (buf && bufsize > 0)
		buf[0] = 0;
	return -1;
}

int mmb_wlan_ipconfig(char *buf, int bufsize)
{
	const char *msg = "Wi-Fi not available";
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

void mmb_wlan_apply_country(void)
{
}

#endif

}
