#include "wlan_escan.h"

#include <string.h>

#ifdef __GNUC__
#define MMB_PACKED __attribute__((packed))
#else
#define MMB_PACKED
#endif

/*
 * On-wire Broadcom escan result (little-endian). Matches Circle's
 * brcmf_escan_result_le / brcmf_bss_info_le in driver_circle.cpp.
 * Only the BSS header is needed to read SSID; IEs after that are ignored.
 */
struct mmb_escan_hdr {
	unsigned int buflen;
	unsigned int version;
	unsigned short sync_id;
	unsigned short bss_count;
} MMB_PACKED;

struct mmb_bss_head {
	unsigned int version;
	unsigned int length;
	unsigned char bssid[6];
	unsigned short beacon_period;
	unsigned short capability;
	unsigned char ssid_len;
	unsigned char ssid[32];
} MMB_PACKED;

#define MMB_BSS_INFO_VERSION 109u

typedef char mmb_escan_hdr_size_ok[(sizeof(struct mmb_escan_hdr) == 12) ? 1 : -1];
typedef char mmb_bss_head_size_ok[(sizeof(struct mmb_bss_head) == 51) ? 1 : -1];

int mmb_ssid_printable(const unsigned char *s, unsigned n)
{
	unsigned i;
	if (!s || n < 1 || n > 32)
		return 0;
	for (i = 0; i < n; i++)
		if (s[i] < 32 || s[i] > 126)
			return 0;
	return 1;
}

static int version_ok(unsigned int ver)
{
	unsigned int swapped;

	if (ver == MMB_BSS_INFO_VERSION)
		return 1;
	swapped = ((ver & 0xffu) << 24) | ((ver & 0xff00u) << 8) |
		  ((ver & 0xff0000u) >> 8) | ((ver >> 24) & 0xffu);
	return swapped == MMB_BSS_INFO_VERSION;
}

int mmb_escan_ssids(const unsigned char *buf, unsigned nlen,
		    char out[][MMB_ESCAN_SSID_CAP], int maxn,
		    int *beacons, int *skipped)
{
	const struct mmb_escan_hdr *hdr;
	const struct mmb_bss_head *bss;
	unsigned short n_bss;
	unsigned short i;
	int got = 0;

	if (!buf || maxn <= 0 || !out)
		return 0;
	if (nlen < sizeof(*hdr) + sizeof(*bss))
		return 0;

	hdr = (const struct mmb_escan_hdr *)buf;
	n_bss = hdr->bss_count;
	if (n_bss < 1)
		n_bss = 1;
	if (n_bss > 32)
		n_bss = 32;

	bss = (const struct mmb_bss_head *)(buf + sizeof(*hdr));
	for (i = 0; i < n_bss; i++)
	{
		unsigned off = (unsigned)((const unsigned char *)bss - buf);

		if (off + sizeof(*bss) > nlen)
			break;
		if (beacons)
			(*beacons)++;
		if (!version_ok(bss->version) ||
		    !mmb_ssid_printable(bss->ssid, bss->ssid_len))
		{
			if (skipped)
				(*skipped)++;
		}
		else if (got < maxn)
		{
			memcpy(out[got], bss->ssid, bss->ssid_len);
			out[got][bss->ssid_len] = 0;
			got++;
		}
		if (bss->length < sizeof(*bss) || off + bss->length > nlen)
			break;
		bss = (const struct mmb_bss_head *)((const unsigned char *)bss +
						    bss->length);
	}
	return got;
}
