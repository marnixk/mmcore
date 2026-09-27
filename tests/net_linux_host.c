/*
 * Host test for native/net_linux.c (#831): Wi-Fi scan, status and address
 * parsing driven by fake `iw` / `wpa_cli` / `ip` commands.
 *
 * The pytest writes those fakes into MMB_NET_CMD_DIR and pins the interface
 * names with MMB_NET_WLAN_IFACE / MMB_NET_ETH_IFACE. argv[1] == "wpa" expects
 * the wpa_cli fallback (the fake `iw` fails there).
 */
#include "mmbasic.h"
#include "mmb_priv.h"

#include <stdio.h>
#include <string.h>

static int fails;

#define CHECK(cond, msg)                                                       \
	do                                                                     \
	{                                                                      \
		if (!(cond))                                                   \
		{                                                              \
			fprintf(stderr, "FAIL %s\n", msg);                     \
			fails++;                                               \
		}                                                              \
	} while (0)

int main(int argc, char **argv)
{
	char ssids[16][64];
	int wpa = (argc > 1 && strcmp(argv[1], "wpa") == 0);
	int n;
	char buf[256];

	CHECK(mmb_wlan_available() == 1, "a wireless interface is discovered");
	CHECK(mmb_eth_available() == 1, "a wired interface is discovered");

	n = mmb_wlan_scan(ssids, 16);
	CHECK(n == 2, "scan returns two unique SSIDs");
	if (n == 2)
	{
		CHECK(strcmp(ssids[0], wpa ? "WpaOne" : "NetOne") == 0,
		      "first SSID parsed");
		CHECK(strcmp(ssids[1], wpa ? "Wpa Two" : "Net Two") == 0,
		      "second SSID parsed (dedup + spaces)");
	}

	CHECK(mmb_wlan_status() == 1, "wlan reports connected");
	CHECK(mmb_wlan_ip(buf, sizeof buf) == 0 &&
	      strcmp(buf, "192.0.2.55") == 0, "wlan IPv4 parsed");

	CHECK(mmb_wlan_ipconfig(buf, sizeof buf) == 0, "wlan ipconfig succeeds");
	CHECK(strstr(buf, "IP Address: 192.0.2.55") != 0,
	      "wlan ipconfig reports the address");

	CHECK(mmb_eth_ipconfig(buf, sizeof buf) == 0, "eth ipconfig succeeds");
	CHECK(strstr(buf, "IP Address: 198.51.100.7") != 0,
	      "eth ipconfig reports the address");

	CHECK(mmb_net_kind() == MMB_NET_WIFI, "net kind follows the live Wi-Fi");

	n = mmb_wlan_scan(ssids, 1);
	CHECK(n == 1, "scan honours the maxn cap");

	if (fails)
		return 1;
	printf("all checks passed\n");
	return 0;
}
