/*
 * Broadcom escan SSID harvest. Independent of Circle so host tests can
 * prove we read packed brcmf_bss_info_le.SSID, not random printable bytes.
 */
#ifndef MMB_WLAN_ESCAN_H
#define MMB_WLAN_ESCAN_H

#ifdef __cplusplus
extern "C" {
#endif

#define MMB_ESCAN_SSID_CAP 33

int mmb_ssid_printable(const unsigned char *s, unsigned n);

/* Pull printable SSIDs out of one escan result. out[i] is NUL-terminated.
 * beacons/skipped may be NULL; when set they are incremented. */
int mmb_escan_ssids(const unsigned char *buf, unsigned nlen,
		    char out[][MMB_ESCAN_SSID_CAP], int maxn,
		    int *beacons, int *skipped);

#ifdef __cplusplus
}
#endif

#endif
