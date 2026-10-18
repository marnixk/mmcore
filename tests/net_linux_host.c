/*
 * Host test for native/net_linux.c (#831): Wi-Fi scan, status and address
 * parsing driven by fake `iw` / `wpa_cli` / `ip` commands.
 *
 * The pytest writes those fakes into MMB_NET_CMD_DIR and pins the interface
 * names with MMB_NET_WLAN_IFACE / MMB_NET_ETH_IFACE. argv[1] == "wpa" expects
 * the wpa_cli fallback (the fake `iw` fails there). argv[1] == "boot" models
 * the startup path of #1057: the lease only appears once MMB_NET_WLAN_READY
 * exists, so the first mmb_wlan_start() must report pending and a retry must
 * finish.
 */
#include "mmbasic.h"
#include "mmb_priv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static int fails;

/* net_linux.c asks mmb_net_kind() what OPTION configured; production sets this
 * up in mmb_init(). The discovery tests only need the zeroed option fields. */
static mmb s_test_mmb;
mmb *g_cur = &s_test_mmb;

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
	int connect_mode = (argc > 1 && strcmp(argv[1], "connect") == 0);
	int stuck = (argc > 1 && strcmp(argv[1], "stuck") == 0);
	int boot = (argc > 1 && strcmp(argv[1], "boot") == 0);
	int boot_timeout = (argc > 1 && strcmp(argv[1], "boot_timeout") == 0);
	int avail = (argc > 1 && strcmp(argv[1], "avail") == 0);
	int late_eth = (argc > 1 && strcmp(argv[1], "late_eth") == 0);
	int kind_wifi = (argc > 1 && strcmp(argv[1], "kind_wifi") == 0);
	int kind_eth = (argc > 1 && strcmp(argv[1], "kind_eth") == 0);
	int kind_eth_option = (argc > 1 && strcmp(argv[1], "kind_eth_option") == 0);
	int virtual_eth = (argc > 1 && strcmp(argv[1], "virtual_eth") == 0);
	int lease_eth = (argc > 1 && strcmp(argv[1], "lease_eth") == 0);
	int lease_late = (argc > 1 && strcmp(argv[1], "lease_late") == 0);
	int n;
	char buf[256];

	/* The discovery scenarios drive /sys/class/net through MMB_NET_SYSFS_NET;
	 * the name pins would short-circuit that, so clear them. */
	if (late_eth || kind_wifi || kind_eth || kind_eth_option ||
	    virtual_eth || lease_eth || lease_late)
	{
		unsetenv("MMB_NET_WLAN_IFACE");
		unsetenv("MMB_NET_ETH_IFACE");
	}

	if (lease_late)
	{
		const char *eth0 = getenv("MMB_NET_ETH0_READY");
		const char *eth1 = getenv("MMB_NET_ETH1_READY");
		FILE *f;

		CHECK(eth0 != 0 && eth1 != 0, "the lease markers are set");
		CHECK(mmb_eth_ip(buf, sizeof buf) == 0 &&
		      strcmp(buf, "192.0.2.10") == 0,
		      "the wired NIC with the first lease is selected");
		if (eth0)
			unlink(eth0);
		f = eth1 ? fopen(eth1, "w") : 0;
		CHECK(f != 0, "the second NIC lease marker was created");
		if (f)
			fclose(f);
		CHECK(mmb_eth_status() == 1,
		      "a lease landing on another wired NIC is seen");
		CHECK(mmb_eth_ip(buf, sizeof buf) == 0 &&
		      strcmp(buf, "203.0.113.9") == 0,
		      "the newly leased wired NIC replaces the cached one");
		if (fails)
			return 1;
		printf("all checks passed\n");
		return 0;
	}

	if (virtual_eth)
	{
		const char *docker_ip = "172.17.0.1";

		CHECK(mmb_eth_available() == 1, "a physical wired NIC is discovered");
		CHECK(mmb_eth_status() == 0, "the physical NIC has no lease");
		CHECK(mmb_eth_ip(buf, sizeof buf) == 0, "eth ip query succeeds");
		CHECK(strcmp(buf, docker_ip) != 0,
		      "a virtual interface with a lease is not latched");
		if (fails)
			return 1;
		printf("all checks passed\n");
		return 0;
	}

	if (lease_eth)
	{
		CHECK(mmb_eth_available() == 1, "a wired NIC is discovered");
		CHECK(mmb_eth_status() == 1, "the leased wired NIC reports up");
		CHECK(mmb_eth_ip(buf, sizeof buf) == 0 &&
		      strcmp(buf, "203.0.113.9") == 0,
		      "discovery prefers the wired NIC that has a lease");
		if (fails)
			return 1;
		printf("all checks passed\n");
		return 0;
	}

	if (kind_eth_option)
	{
		CHECK(mmb_wlan_available() == 1, "a wireless interface is present");
		CHECK(mmb_eth_available() == 1, "a wired interface is present");
		CHECK(mmb_net_kind() == MMB_NET_WIFI,
		      "unconfigured discovery still reports the present Wi-Fi");
		G.opt.ethernet_enabled = 1;
		G.opt.wifi_enabled = 0;
		CHECK(mmb_net_kind() == MMB_NET_ETH,
		      "OPTION ETHERNET ON wins on a dual-interface host");
		G.opt.ethernet_enabled = 0;
		CHECK(mmb_net_kind() == MMB_NET_WIFI,
		      "switching back to Wi-Fi is reflected again");
		if (fails)
			return 1;
		printf("all checks passed\n");
		return 0;
	}

	if (late_eth)
	{
		const char *root = getenv("MMB_NET_SYSFS_NET");
		char ethdir[256];
		char devdir[256];

		CHECK(mmb_eth_available() == 0, "an absent wired NIC is a miss");
		CHECK(mmb_eth_status() == 0, "the miss is not cached");
		CHECK(root != 0, "the sysfs root is set");
		snprintf(ethdir, sizeof ethdir, "%s/eth0", root ? root : "");
		snprintf(devdir, sizeof devdir, "%s/device", ethdir);
		CHECK(mkdir(ethdir, 0755) == 0, "the late NIC directory exists");
		CHECK(mkdir(devdir, 0755) == 0, "the late NIC looks physical");
		CHECK(mmb_eth_available() == 1,
		      "a wired NIC that appears later is discovered");
		if (fails)
			return 1;
		printf("all checks passed\n");
		return 0;
	}

	if (kind_wifi)
	{
		CHECK(mmb_wlan_available() == 1, "a wireless interface is discovered");
		CHECK(mmb_wlan_status() == 0, "the interface has no lease yet");
		CHECK(mmb_net_kind() == MMB_NET_WIFI,
		      "an unassociated Wi-Fi device reports WIFI");
		if (fails)
			return 1;
		printf("all checks passed\n");
		return 0;
	}

	if (kind_eth)
	{
		CHECK(mmb_wlan_available() == 0, "no wireless interface exists");
		CHECK(mmb_eth_available() == 1, "a wired interface exists");
		CHECK(mmb_net_kind() == MMB_NET_ETH,
		      "with no wireless interface the kind falls back to ETH");
		if (fails)
			return 1;
		printf("all checks passed\n");
		return 0;
	}

	if (avail)
	{
		const char *marker = getenv("MMB_NET_AVAIL_READY");
		const char *ttl_s = getenv("MMB_NET_AVAIL_TTL_MS");
		int ttl = ttl_s ? atoi(ttl_s) : 0;
		unsigned wait_ms;
		FILE *f;

		/* No lease on either interface: the link is down (#1187). */
		CHECK(mmb_net_available() == 0, "no lease means unavailable");
		f = fopen(marker, "w");
		CHECK(f != 0, "the lease marker was created");
		if (f)
			fclose(f);
		if (ttl == 0)
		{
			CHECK(mmb_net_available() == 1,
			      "a lease appears immediately with the cache off");
		}
		else
		{
			CHECK(mmb_net_available() == 0,
			      "the TTL cache hides the new lease");
			wait_ms = (unsigned)ttl + 300u;
			usleep((useconds_t)wait_ms * 1000);
			CHECK(mmb_net_available() == 1,
			      "the lease is seen once the cache expires");
		}
		unlink(marker);
		if (ttl == 0)
		{
			CHECK(mmb_net_available() == 0,
			      "a lease drop is seen immediately");
		}
		else
		{
			wait_ms = (unsigned)ttl + 300u;
			usleep((useconds_t)wait_ms * 1000);
			CHECK(mmb_net_available() == 0,
			      "the lease drop is seen after expiry");
		}
		if (fails)
			return 1;
		printf("all checks passed\n");
		return 0;
	}

	if (boot)
	{
		FILE *f;

		CHECK(mmb_wlan_start("SavedNet", "secret") != 0,
		      "a cold boot start is pending");
		CHECK(mmb_wlan_radio_pending() == 1,
		      "the pending window is advertised for retry");
		f = fopen(getenv("MMB_NET_WLAN_READY"), "w");
		CHECK(f != 0, "the ready marker was created");
		if (f)
			fclose(f);
		usleep(150000);
		CHECK(mmb_wlan_start("SavedNet", "secret") == 0,
		      "start completes once the lease appears");
		CHECK(mmb_wlan_radio_pending() == 0,
		      "pending clears on success");
		if (fails)
			return 1;
		printf("all checks passed\n");
		return 0;
	}

	if (boot_timeout)
	{
		CHECK(mmb_wlan_start("SavedNet", "secret") != 0,
		      "a start without a lease stays pending");
		CHECK(mmb_wlan_radio_pending() == 1, "pending is advertised");
		usleep(2500000);
		CHECK(mmb_wlan_radio_pending() == 0,
		      "the pending window gives up after its budget");
		CHECK(mmb_wlan_start("SavedNet", "secret") != 0,
		      "start does not silently succeed after the budget");
		if (fails)
			return 1;
		printf("all checks passed\n");
		return 0;
	}

	if (stuck)
	{
		FILE *f;

		CHECK(mmb_wlan_start("StuckNet", "secret") != 0,
		      "a helper that never exits is abandoned");
		f = fopen(getenv("MMB_WPA_CONF"), "r");
		CHECK(f != 0, "the wpa config file was written");
		if (f)
		{
			char all[1024] = "";
			char line[256];

			while (fgets(line, sizeof line, f))
			{
				size_t used = strlen(all);

				if (used < sizeof all - 1)
					strncat(all, line,
						sizeof all - used - 1);
			}
			fclose(f);
			CHECK(strstr(all, "ssid=\"StuckNet\"") != 0,
			      "ssid is saved before the helpers run");
		}
		if (fails)
			return 1;
		printf("all checks passed\n");
		return 0;
	}

	if (connect_mode)
	{
		FILE *f;

		CHECK(mmb_wlan_start("NetOne", "se\"cret") == 0,
		      "wlan_start writes the config");
		f = fopen(getenv("MMB_WPA_CONF"), "r");
		CHECK(f != 0, "the wpa config file was written");
		if (f)
		{
			char all[1024] = "";
			char line[256];

			while (fgets(line, sizeof line, f))
			{
				size_t used = strlen(all);

				if (used < sizeof all - 1)
					strncat(all, line,
						sizeof all - used - 1);
			}
			fclose(f);
			CHECK(strstr(all, "ssid=\"NetOne\"") != 0,
			      "ssid is written");
			CHECK(strstr(all, "psk=\"se\\\"cret\"") != 0,
			      "psk is quoted and escaped");
		}
		CHECK(mmb_wlan_connect("NetOne", "secret") == 0,
		      "wlan_connect succeeds with the address up");
		CHECK(mmb_eth_start() == 0, "eth_start succeeds");
		CHECK(mmb_eth_wait_dhcp(1000) == 1,
		      "eth dhcp wait sees the address");

		if (fails)
			return 1;
		printf("all checks passed\n");
		return 0;
	}

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
