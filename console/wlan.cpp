#include "mmbasic.h"
#include <circle/util.h>

extern "C" {
int mmb_vfs_exists(const char *path);
int mmb_keyword_eq(const char *a, const char *b);
}

/*
 * Thin WLAN platform layer.
 *
 * Circle's CYW4343x driver lives in addon/wlan (CBcm4343Device). Pi 3/4 have
 * onboard Wi-Fi; QEMU raspi3b does not emulate the radio. Firmware files must
 * sit on the SD volume (C:/firmware/). Init is lazy so a missing radio cannot
 * break the 115200 serial console.
 *
 * Circle WLAN and the SD card share SDIO on Pi 3 unless USE_SDHOST is set
 * (this project uses NO_SDHOST). We therefore only construct the device when
 * firmware is present; QEMU and cards without firmware report "not available".
 */

#ifdef MMB_CIRCLE_WLAN
#include <wlan/bcm4343.h>
#include <circle/new.h>
#include <circle/timer.h>
#endif

extern "C" {

#ifdef MMB_CIRCLE_WLAN
static CBcm4343Device *s_wlan;
static int s_tried;
static int s_ready;

static int firmware_present(void)
{
	return mmb_vfs_exists("C:/firmware/brcmfmac43430-sdio.bin")
	    || mmb_vfs_exists("C:/firmware/brcmfmac43455-sdio.bin")
	    || mmb_vfs_exists("C:/firmware/brcmfmac43436-sdio.bin")
	    || mmb_vfs_exists("/firmware/brcmfmac43430-sdio.bin");
}

static int wlan_ensure(void)
{
	if (s_tried)
		return s_ready;
	s_tried = 1;
	s_ready = 0;
	if (!firmware_present())
		return 0;
	s_wlan = new CBcm4343Device("SD:/firmware/");
	if (!s_wlan)
		return 0;
	if (!s_wlan->Initialize())
	{
		delete s_wlan;
		s_wlan = 0;
		return 0;
	}
	s_ready = 1;
	return 1;
}

int mmb_wlan_available(void)
{
	return wlan_ensure();
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
			continue;
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
	}
	return count;
}

int mmb_wlan_connect(const char *ssid, const char *psk)
{
	if (!ssid || !ssid[0])
		return -1;
	if (!wlan_ensure() || !s_wlan)
		return -1;
	if (!psk || !psk[0])
	{
		if (s_wlan->JoinOpenNet(ssid))
			return s_wlan->IsLinkUp() ? 0 : -1;
		return -1;
	}
	/* WPA needs hostap/wpa_supplicant (not vendored). Try a join with key. */
	if (!s_wlan->Control("join %s %s 0 %s", ssid, "FFFFFFFFFFFF", psk))
		return -1;
	return s_wlan->IsLinkUp() ? 0 : -1;
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

int mmb_wlan_scan(char ssids[][64], int maxn)
{
	(void)ssids;
	(void)maxn;
	return 0;
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
