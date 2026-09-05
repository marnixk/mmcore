#include "mmbasic.h"
#include <circle/util.h>

extern "C" {
int mmb_vfs_exists(const char *path);
int mmb_vfs_write(const char *path, const void *data, unsigned n, int append);
int mmb_keyword_eq(const char *a, const char *b);
}

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
#include <circle/sched/scheduler.h>
#include <circle/new.h>
#include <circle/timer.h>
#endif

extern "C" {

#ifdef MMB_CIRCLE_WLAN

static CScheduler *s_sched;
static CBcm4343Device *s_wlan;
static CNetSubSystem *s_net;
static CWPASupplicant *s_wpa;
static int s_tried;
static int s_ready;

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
	while (CTimer::GetClockTicks() - start < limit)
	{
		if (s_wlan && s_wlan->IsLinkUp())
			return 1;
		if (CScheduler::IsActive())
			CScheduler::Get()->MsSleep(100);
		else
			CTimer::SimpleMsDelay(100);
	}
	return s_wlan && s_wlan->IsLinkUp() ? 1 : 0;
}

static int wlan_ensure(void)
{
	if (s_tried)
		return s_ready;
	s_tried = 1;
	s_ready = 0;
	if (!firmware_present())
		return 0;
	sched_ensure();
	s_wlan = new CBcm4343Device("SD:/firmware/");
	if (!s_wlan)
		return 0;
	if (!s_wlan->Initialize())
	{
		delete s_wlan;
		s_wlan = 0;
		return 0;
	}
	s_net = new CNetSubSystem(0, 0, 0, 0, "mmbasic", NetDeviceTypeWLAN);
	if (s_net && !s_net->Initialize(FALSE))
	{
		delete s_net;
		s_net = 0;
	}
	s_ready = 1;
	return 1;
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
	const char *head = "country=00\nnetwork={\n\tssid=";
	const char *mid = "\n\tpsk=";
	const char *wpa = "\n\tproto=RSN\n\tkey_mgmt=WPA-PSK\n}\n";
	const char *open_tail = "\n\tkey_mgmt=NONE\n}\n";
	while (*head && n + 1 < sizeof buf)
		buf[n++] = *head++;
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
	return mmb_vfs_write("C:/wpa_supplicant.conf", buf, n, 0) == 0 ? 1 : 0;
}

static int start_wpa(const char *ssid, const char *psk)
{
	if (!write_wpa_conf(ssid, psk))
		return 0;
	if (s_wpa)
	{
		delete s_wpa;
		s_wpa = 0;
	}
	s_wpa = new CWPASupplicant("SD:/wpa_supplicant.conf");
	if (!s_wpa)
		return 0;
	if (!s_wpa->Initialize())
	{
		delete s_wpa;
		s_wpa = 0;
		return 0;
	}
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

int mmb_wlan_scan(char ssids[][64], int maxn)
{
	unsigned char buf[1600];
	unsigned nlen;
	unsigned start;
	int count = 0, i;
	if (!wlan_ensure() || !s_wlan || maxn <= 0)
		return 0;
	s_wlan->Control("escan 2");
	start = CTimer::GetClockTicks();
	while (CTimer::GetClockTicks() - start < 3 * 1000000)
	{
		nlen = 0;
		if (!s_wlan->ReceiveScanResult(buf, &nlen) || nlen < 2)
		{
			yield_some();
			continue;
		}
		/* Extract printable SSID-like tokens from the scan blob. */
		i = 0;
		while (i < (int)nlen && count < maxn)
		{
			while (i < (int)nlen && (buf[i] < 32 || buf[i] > 126))
				i++;
			if (i >= (int)nlen)
				break;
			{
				int n = 0;
				char tmp[64];
				while (i < (int)nlen && buf[i] >= 32 && buf[i] <= 126 && n < 63)
					tmp[n++] = (char)buf[i++];
				tmp[n] = 0;
				if (n >= 1 && n <= 32)
				{
					int dup = 0, k;
					for (k = 0; k < count; k++)
						if (mmb_keyword_eq(ssids[k], tmp))
							dup = 1;
					if (!dup)
					{
						strncpy(ssids[count], tmp, 63);
						ssids[count][63] = 0;
						count++;
					}
				}
			}
		}
		yield_some();
	}
	return count;
}

int mmb_wlan_start(const char *ssid, const char *psk)
{
	if (!ssid || !ssid[0])
		return -1;
	if (!wlan_ensure() || !s_wlan)
		return -1;
	if (!psk || !psk[0])
		return s_wlan->JoinOpenNet(ssid) ? 0 : -1;
	return start_wpa(ssid, psk) ? 0 : -1;
}

int mmb_wlan_connect(const char *ssid, const char *psk)
{
	if (mmb_wlan_start(ssid, psk) != 0)
		return -1;
	return wait_link(20000) ? 0 : -1;
}

int mmb_wlan_status(void)
{
	if (!s_ready || !s_wlan)
		return 0;
	return s_wlan->IsLinkUp() ? 1 : 0;
}

#else /* !MMB_CIRCLE_WLAN */

int mmb_wlan_available(void)
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

#endif

}
