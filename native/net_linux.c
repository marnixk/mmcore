/*
 * Linux network backend (native build): Wi-Fi scan/status and wired/wireless
 * interface queries.
 *
 * Interface discovery reads /sys/class/net (a wireless interface has a
 * `wireless` directory). External tools are run through one runner so tests
 * can inject fakes with MMB_NET_CMD_DIR; the interface names can be pinned
 * with MMB_NET_WLAN_IFACE / MMB_NET_ETH_IFACE. Wi-Fi scan uses `iw` and falls
 * back to `wpa_cli`; addresses come from `ip -4 addr show`. Joining a network
 * (wpa_supplicant config + service restart) lives in a follow-up.
 */
#include "mmbasic.h"
#include "mmb_priv.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define IFNAME_MAX 32
#define CMD_OUT 8192

static int path_exists(const char *path)
{
	struct stat st;

	return stat(path, &st) == 0;
}

static int is_wireless(const char *name)
{
	char path[128];

	snprintf(path, sizeof path, "/sys/class/net/%s/wireless", name);
	return path_exists(path);
}

/* First interface matching `want_wireless` (1 wireless, 0 wired), or NULL. */
static const char *discover_iface(int want_wireless)
{
	static char name[IFNAME_MAX];
	DIR *d = opendir("/sys/class/net");
	struct dirent *e;

	name[0] = 0;
	if (!d)
		return 0;
	while ((e = readdir(d)))
	{
		if (e->d_name[0] == '.')
			continue;
		if (strcmp(e->d_name, "lo") == 0)
			continue;
		if (is_wireless(e->d_name) != want_wireless)
			continue;
		snprintf(name, sizeof name, "%s", e->d_name);
		break;
	}
	closedir(d);
	return name[0] ? name : 0;
}

static const char *wlan_iface(void)
{
	static char cached[IFNAME_MAX];
	static int done;
	const char *env;

	if (done)
		return cached[0] ? cached : 0;
	done = 1;
	env = getenv("MMB_NET_WLAN_IFACE");
	if (env && env[0])
	{
		snprintf(cached, sizeof cached, "%s", env);
		return cached;
	}
	{
		const char *name = discover_iface(1);

		if (name)
			snprintf(cached, sizeof cached, "%s", name);
	}
	return cached[0] ? cached : 0;
}

static const char *eth_iface(void)
{
	static char cached[IFNAME_MAX];
	static int done;
	const char *env;

	if (done)
		return cached[0] ? cached : 0;
	done = 1;
	env = getenv("MMB_NET_ETH_IFACE");
	if (env && env[0])
	{
		snprintf(cached, sizeof cached, "%s", env);
		return cached;
	}
	{
		const char *name = discover_iface(0);

		if (name)
			snprintf(cached, sizeof cached, "%s", name);
	}
	return cached[0] ? cached : 0;
}

/* Run `cmd` (parsed by the shell) capturing stdout. MMB_NET_CMD_DIR prefixes
 * argv[0] so tests supply fakes. Returns the exit status, or -1. */
static int run_capture(const char *cmd, char *out, size_t outcap)
{
	char full[512];
	const char *dir = getenv("MMB_NET_CMD_DIR");
	FILE *p;

	if (outcap)
		out[0] = 0;
	if (dir && dir[0])
		snprintf(full, sizeof full, "%s/%s", dir, cmd);
	else
		snprintf(full, sizeof full, "%s", cmd);
	p = popen(full, "r");
	if (!p)
		return -1;
	if (out && outcap)
	{
		size_t n = fread(out, 1, outcap - 1, p);

		out[n] = 0;
	}
	return pclose(p);
}

/* IPv4 address of `iface` from `ip -4 addr show dev <iface>`, or 0. */
static int iface_ipv4(const char *iface, char *buf, size_t cap)
{
	char cmd[160], out[CMD_OUT];
	const char *p;
	size_t i = 0;

	if (!iface || !buf || cap == 0)
		return 0;
	buf[0] = 0;
	snprintf(cmd, sizeof cmd, "ip -4 addr show dev %s", iface);
	if (run_capture(cmd, out, sizeof out) != 0)
		return 0;
	p = strstr(out, "inet ");
	if (!p)
		return 0;
	for (p += 5; *p && *p != ' ' && *p != '/' && i + 1 < cap; p++)
		buf[i++] = *p;
	buf[i] = 0;
	return buf[0] != 0;
}

static void copy_bounded(char *dst, size_t cap, const char *src, size_t len)
{
	if (cap == 0)
		return;
	if (len >= cap)
		len = cap - 1;
	memcpy(dst, src, len);
	dst[len] = 0;
}

/* The SSID on an `iw`/`wpa_cli` line, or NULL. */
static const char *line_ssid(const char *line)
{
	const char *p = strstr(line, "SSID: ");

	if (p)
		return p + 6;
	if (strchr(line, '\t') && strncmp(line, "bssid", 5) != 0 &&
	    strncmp(line, "Selected", 8) != 0)
	{
		const char *last = strrchr(line, '\t');

		if (last)
			return last + 1;
	}
	return 0;
}

int mmb_wlan_available(void)
{
	return wlan_iface() != 0;
}

int mmb_wlan_radio_pending(void) { return 0; }

int mmb_wlan_scan(char ssids[][64], int maxn)
{
	const char *iface = wlan_iface();
	char cmd[160], out[CMD_OUT];
	const char *line;
	int rc, n = 0;

	if (!iface || maxn <= 0)
		return 0;
	snprintf(cmd, sizeof cmd, "iw dev %s scan", iface);
	rc = run_capture(cmd, out, sizeof out);
	if (rc != 0)
	{
		snprintf(cmd, sizeof cmd, "wpa_cli -i %s scan_results", iface);
		rc = run_capture(cmd, out, sizeof out);
	}
	if (rc != 0)
		return 0;

	line = out;
	while (*line)
	{
		const char *end = strchr(line, '\n');
		size_t len = end ? (size_t)(end - line) : strlen(line);
		char buf[256];

		copy_bounded(buf, sizeof buf, line, len);
		if (buf[0] && buf[strlen(buf) - 1] == '\r')
			buf[strlen(buf) - 1] = 0;
		{
			const char *ssid = line_ssid(buf);

			if (ssid && ssid[0] && n < maxn)
			{
				int i, dup = 0;

				for (i = 0; i < n; i++)
					if (strcmp(ssids[i], ssid) == 0)
					{
						dup = 1;
						break;
					}
				if (!dup)
				{
					snprintf(ssids[n], 64, "%s", ssid);
					n++;
				}
			}
		}
		if (!end)
			break;
		line = end + 1;
	}
	return n;
}

/* Join a network: write the wpa_supplicant config and (re)start the OpenRC
 * services that own WPA and DHCP. */
static const char *wpa_conf_path(void)
{
	const char *p = getenv("MMB_WPA_CONF");

	return (p && p[0]) ? p : "/etc/wpa_supplicant/wpa_supplicant.conf";
}

static void fput_quoted(FILE *f, const char *s)
{
	fputc('"', f);
	for (; s && *s; s++)
	{
		if (*s == '"' || *s == '\\')
			fputc('\\', f);
		fputc(*s, f);
	}
	fputc('"', f);
}

static int write_wpa_conf(const char *ssid, const char *psk)
{
	FILE *f = fopen(wpa_conf_path(), "w");

	if (!f)
		return -1;
	fprintf(f, "ctrl_interface=/var/run/wpa_supplicant\n");
	fprintf(f, "update_config=1\n\n");
	fprintf(f, "network={\n\tssid=");
	fput_quoted(f, ssid);
	fprintf(f, "\n");
	if (psk && psk[0])
	{
		fprintf(f, "\tpsk=");
		fput_quoted(f, psk);
		fprintf(f, "\n");
	}
	else
		fprintf(f, "\tkey_mgmt=NONE\n");
	fprintf(f, "}\n");
	fclose(f);
	return 0;
}

static void service_restart(const char *service)
{
	char cmd[160];

	snprintf(cmd, sizeof cmd, "rc-service %s restart", service);
	run_capture(cmd, 0, 0);
}

/* Request a DHCP lease on `iface` (busybox udhcpc, as shipped on the ISO). */
static void dhcp_get(const char *iface)
{
	char cmd[200];

	if (!iface)
		return;
	snprintf(cmd, sizeof cmd, "udhcpc -i %s -b -q -n", iface);
	run_capture(cmd, 0, 0);
}

int mmb_wlan_start(const char *ssid, const char *psk)
{
	if (!ssid || !ssid[0])
		return -1;
	if (write_wpa_conf(ssid, psk) != 0)
		return -1;
	service_restart("wpa_supplicant");
	service_restart("networking");
	return 0;
}

int mmb_wlan_connect(const char *ssid, const char *psk)
{
	unsigned waited = 0;
	char ip[40];

	if (mmb_wlan_start(ssid, psk) != 0)
		return -1;
	dhcp_get(wlan_iface());
	/* Association + DHCP take a moment; poll for the lease. */
	while (waited < 15000)
	{
		if (iface_ipv4(wlan_iface(), ip, sizeof ip))
			return 0;
		usleep(100000);
		waited += 100;
	}
	return iface_ipv4(wlan_iface(), ip, sizeof ip) ? 0 : -1;
}

int mmb_wlan_status(void)
{
	char ip[40];

	return iface_ipv4(wlan_iface(), ip, sizeof ip);
}

int mmb_wlan_ip(char *buf, int bufsize)
{
	if (!buf || bufsize < 8)
		return -1;
	if (!iface_ipv4(wlan_iface(), buf, (size_t)bufsize))
		buf[0] = 0;
	return 0;
}

int mmb_wlan_ipconfig(char *buf, int bufsize)
{
	char ip[40];

	if (!buf || bufsize < 32)
		return -1;
	if (!mmb_wlan_available())
		snprintf(buf, (size_t)bufsize, "Interface: Wi-Fi\nWi-Fi not available\n");
	else if (iface_ipv4(wlan_iface(), ip, sizeof ip))
		snprintf(buf, (size_t)bufsize,
			 "Interface: Wi-Fi\nIP Address: %s\nDHCP: yes\n", ip);
	else
		snprintf(buf, (size_t)bufsize, "Interface: Wi-Fi\nNot connected\n");
	return 0;
}

void mmb_wlan_poll(void) {}
void mmb_wlan_apply_country(void) {}

int mmb_eth_available(void)
{
	return eth_iface() != 0;
}

int mmb_eth_start(void)
{
	if (!eth_iface())
		return -1;
	service_restart("networking");
	dhcp_get(eth_iface());
	return 0;
}

int mmb_eth_status(void)
{
	char ip[40];

	return iface_ipv4(eth_iface(), ip, sizeof ip);
}

int mmb_eth_wait_dhcp(unsigned ms)
{
	char ip[40];
	unsigned waited = 0;

	if (ms == 0)
		ms = 1;
	while (waited < ms)
	{
		if (iface_ipv4(eth_iface(), ip, sizeof ip))
			return 1;
		usleep(100000);
		waited += 100;
	}
	return iface_ipv4(eth_iface(), ip, sizeof ip) ? 1 : 0;
}

int mmb_eth_ip(char *buf, int bufsize)
{
	if (!buf || bufsize < 8)
		return -1;
	if (!iface_ipv4(eth_iface(), buf, (size_t)bufsize))
		buf[0] = 0;
	return 0;
}

int mmb_eth_ipconfig(char *buf, int bufsize)
{
	char ip[40];

	if (!buf || bufsize < 32)
		return -1;
	if (iface_ipv4(eth_iface(), ip, sizeof ip))
		snprintf(buf, (size_t)bufsize,
			 "Interface: Ethernet\nIP Address: %s\nDHCP: yes\n", ip);
	else
		snprintf(buf, (size_t)bufsize, "Interface: Ethernet\nNot connected\n");
	return 0;
}

int mmb_net_kind(void)
{
	return mmb_wlan_status() ? MMB_NET_WIFI : MMB_NET_ETH;
}
