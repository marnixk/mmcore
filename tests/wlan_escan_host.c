/*
 * Host-side checks for console/wlan_escan.c. Builds a packed Broadcom
 * escan blob the same way the CYW4343x firmware does, then proves we
 * list beacon SSIDs and ignore printable junk in IEs / rates.
 */
#include "wlan_escan.h"

#include <stdio.h>
#include <string.h>

static int fails;

static void expect_eq(const char *name, int got, int want)
{
	if (got != want)
	{
		fprintf(stderr, "FAIL %s: got %d want %d\n", name, got, want);
		fails++;
	}
}

static void expect_str(const char *name, const char *got, const char *want)
{
	if (strcmp(got, want) != 0)
	{
		fprintf(stderr, "FAIL %s: got \"%s\" want \"%s\"\n", name, got, want);
		fails++;
	}
}

static void put_le32(unsigned char *p, unsigned v)
{
	p[0] = (unsigned char)(v & 0xffu);
	p[1] = (unsigned char)((v >> 8) & 0xffu);
	p[2] = (unsigned char)((v >> 16) & 0xffu);
	p[3] = (unsigned char)((v >> 24) & 0xffu);
}

static void put_le16(unsigned char *p, unsigned v)
{
	p[0] = (unsigned char)(v & 0xffu);
	p[1] = (unsigned char)((v >> 8) & 0xffu);
}

/* hdr (12) + truncated BSS head (51) + optional IE tail */
static unsigned build_bss(unsigned char *buf, unsigned cap, unsigned *off,
			  unsigned version, const char *ssid, unsigned ie_fill)
{
	unsigned slen = ssid ? (unsigned)strlen(ssid) : 0;
	unsigned length = 51 + ie_fill;
	unsigned i;
	unsigned char *bss;

	if (*off == 0)
	{
		memset(buf, 0, cap);
		put_le32(buf + 0, 12 + length);
		put_le32(buf + 4, 0);
		put_le16(buf + 8, 1);
		put_le16(buf + 10, 1);
		*off = 12;
	}
	if (*off + length > cap)
		return 0;
	bss = buf + *off;
	put_le32(bss + 0, version);
	put_le32(bss + 4, length);
	memset(bss + 8, 0xaa, 6);
	put_le16(bss + 14, 100);
	put_le16(bss + 16, 0x0411);
	bss[18] = (unsigned char)slen;
	memset(bss + 19, 0, 32);
	if (ssid && slen)
		memcpy(bss + 19, ssid, slen);
	for (i = 0; i < ie_fill; i++)
		bss[51 + i] = (unsigned char)("LookLikeAnSSIDNameXXXX" [i % 22]);
	*off += length;
	put_le32(buf + 0, *off);
	return *off;
}

static void set_bss_count(unsigned char *buf, unsigned short n)
{
	put_le16(buf + 10, n);
}

int main(void)
{
	unsigned char buf[512];
	char ssids[8][MMB_ESCAN_SSID_CAP];
	int beacons, skipped, n;
	unsigned off;

	/* Real beacon SSID, plus printable junk in the IE tail. */
	off = 0;
	build_bss(buf, sizeof buf, &off, 109, "HomeNet", 22);
	beacons = skipped = 0;
	memset(ssids, 0, sizeof ssids);
	n = mmb_escan_ssids(buf, off, ssids, 8, &beacons, &skipped);
	expect_eq("home n", n, 1);
	expect_eq("home beacons", beacons, 1);
	expect_eq("home skipped", skipped, 0);
	expect_str("home ssid", ssids[0], "HomeNet");
	if (memcmp(buf + 12 + 51, "LookLikeAnSSIDNameXXXX", 22) != 0)
	{
		fprintf(stderr, "FAIL fixture missing IE junk\n");
		fails++;
	}

	/* Binary SSID_len=32 must not be listed (old harvester would). */
	off = 0;
	build_bss(buf, sizeof buf, &off, 109, "", 0);
	buf[12 + 18] = 32;
	{
		int i;
		for (i = 0; i < 32; i++)
			buf[12 + 19 + i] = (unsigned char)(i + 1);
	}
	beacons = skipped = 0;
	memset(ssids, 0, sizeof ssids);
	n = mmb_escan_ssids(buf, off, ssids, 8, &beacons, &skipped);
	expect_eq("bin n", n, 0);
	expect_eq("bin skipped", skipped, 1);

	/* Hidden network (zero-length SSID). */
	off = 0;
	build_bss(buf, sizeof buf, &off, 109, "", 0);
	beacons = skipped = 0;
	n = mmb_escan_ssids(buf, off, ssids, 8, &beacons, &skipped);
	expect_eq("hidden n", n, 0);
	expect_eq("hidden skipped", skipped, 1);

	/* Two packed BSS records. */
	off = 0;
	build_bss(buf, sizeof buf, &off, 109, "One", 0);
	build_bss(buf, sizeof buf, &off, 109, "Two", 0);
	set_bss_count(buf, 2);
	beacons = skipped = 0;
	memset(ssids, 0, sizeof ssids);
	n = mmb_escan_ssids(buf, off, ssids, 8, &beacons, &skipped);
	expect_eq("two n", n, 2);
	expect_eq("two beacons", beacons, 2);
	expect_str("two[0]", ssids[0], "One");
	expect_str("two[1]", ssids[1], "Two");

	/* Whole-buffer printable harvest was the old bug; short/random
	 * bytes with no BSS header must yield nothing. */
	memset(buf, 'A', 80);
	beacons = skipped = 0;
	n = mmb_escan_ssids(buf, 80, ssids, 8, &beacons, &skipped);
	expect_eq("aaaa n", n, 0);

	/* Wrong BSS version is skipped even if SSID bytes look fine. */
	off = 0;
	build_bss(buf, sizeof buf, &off, 1, "Nope", 0);
	beacons = skipped = 0;
	n = mmb_escan_ssids(buf, off, ssids, 8, &beacons, &skipped);
	expect_eq("ver n", n, 0);
	expect_eq("ver skipped", skipped, 1);

	if (fails)
	{
		fprintf(stderr, "%d check(s) failed\n", fails);
		return 1;
	}
	puts("wlan_escan_host: all checks passed");
	return 0;
}
