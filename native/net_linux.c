/*
 * Linux network backend (native build): Wi-Fi scan/status and wired/wireless
 * interface queries.
 *
 * Interface discovery reads /sys/class/net (a wireless interface has a
 * `wireless` directory). External tools are run through one runner so tests
 * can inject fakes with MMB_NET_CMD_DIR; the interface names can be pinned
 * with MMB_NET_WLAN_IFACE / MMB_NET_ETH_IFACE. Wi-Fi scan uses `iw` and falls
 * back to `wpa_cli`; addresses come from `ip -4 addr show`. Joining writes
 * the wpa_supplicant config and reloads it in place. Every helper has a
 * deadline (MMB_NET_CMD_TIMEOUT_MS in tests) so a stuck supplicant cannot
 * freeze the console.
 */
#include "mmbasic.h"
#include "mmb_priv.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
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
	const char *env;

	/* Keep a found name. A miss is not cached: the radio module may
	 * appear after the first OPTION WIFI. */
	if (cached[0])
		return cached;
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

static long long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* MMB_NET_CMD_TIMEOUT_MS shortens every helper deadline (tests). */
static unsigned cmd_budget_ms(unsigned fallback)
{
	const char *e = getenv("MMB_NET_CMD_TIMEOUT_MS");
	unsigned v;

	if (!e || !e[0])
		return fallback;
	v = (unsigned)atoi(e);
	return v ? v : fallback;
}

/* Run `cmd` (parsed by the shell) capturing stdout. MMB_NET_CMD_DIR prefixes
 * argv[0] so tests supply fakes. Stdin is /dev/null and the child is its own
 * process group: a helper that waits on the keyboard, or on OpenRC, is killed
 * when `budget_ms` elapses instead of freezing the console. Returns the exit
 * status, or -1. */
static int run_capture(const char *cmd, char *out, size_t outcap, unsigned budget_ms)
{
	char full[512];
	const char *dir = getenv("MMB_NET_CMD_DIR");
	int pipefd[2];
	pid_t pid;
	long long deadline;
	size_t used = 0;
	int status = 0;
	int timed_out = 0;

	if (out && outcap)
		out[0] = 0;
	budget_ms = cmd_budget_ms(budget_ms);
	if (dir && dir[0])
		snprintf(full, sizeof full, "%s/%s", dir, cmd);
	else
		snprintf(full, sizeof full, "%s", cmd);
	if (pipe(pipefd) != 0)
		return -1;
	pid = fork();
	if (pid < 0)
	{
		close(pipefd[0]);
		close(pipefd[1]);
		return -1;
	}
	if (pid == 0)
	{
		int devnull, fd;

		setpgid(0, 0);
		devnull = open("/dev/null", O_RDWR);
		if (devnull >= 0)
		{
			dup2(devnull, 0);
			dup2(devnull, 2);
			if (devnull > 2)
				close(devnull);
		}
		dup2(pipefd[1], 1);
		close(pipefd[0]);
		close(pipefd[1]);
		for (fd = 3; fd < 256; fd++)
			close(fd);
		execl("/bin/sh", "sh", "-c", full, (char *)0);
		_exit(127);
	}
	setpgid(pid, pid);
	close(pipefd[1]);
	deadline = now_ms() + (long long)budget_ms;
	for (;;)
	{
		struct pollfd pfd;
		long long left = deadline - now_ms();
		int pr;
		char chunk[512];
		ssize_t n;

		if (left < 0)
			left = 0;
		pfd.fd = pipefd[0];
		pfd.events = POLLIN;
		pr = poll(&pfd, 1, (int)left);
		if (pr < 0)
		{
			if (errno == EINTR)
				continue;
			break;
		}
		if (pr == 0)
		{
			timed_out = 1;
			break;
		}
		n = read(pipefd[0], chunk, sizeof chunk);
		if (n > 0)
		{
			if (out && outcap > 1 && used + 1 < outcap)
			{
				size_t take = (size_t)n;

				if (used + take >= outcap)
					take = outcap - 1 - used;
				memcpy(out + used, chunk, take);
				used += take;
				out[used] = 0;
			}
			continue;
		}
		break;
	}
	close(pipefd[0]);
	if (!timed_out)
	{
		for (;;)
		{
			pid_t w = waitpid(pid, &status, WNOHANG);

			if (w == pid)
				break;
			if (w < 0 && errno != EINTR)
				return -1;
			if (now_ms() > deadline)
			{
				timed_out = 1;
				break;
			}
			usleep(10000);
		}
	}
	if (timed_out)
	{
		kill(-pid, SIGKILL);
		kill(pid, SIGKILL);
		waitpid(pid, &status, 0);
		return -1;
	}
	if (!WIFEXITED(status))
		return -1;
	return WEXITSTATUS(status);
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
	if (run_capture(cmd, out, sizeof out, 3000) != 0)
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

/* #1057: boot-time association. The ini can already hold an SSID when
 * mmb_poll() first runs, but the radio module, the interface, wpa_supplicant,
 * association and DHCP may all still be pending. mmb_wlan_start() is therefore
 * a non-blocking, idempotent bring-up: it returns 0 only once the station has
 * an IPv4 address, and -1 while the attempt is still in flight.
 * mmb_wlan_radio_pending() reports that window so core's boot bring-up retries
 * instead of latching `net_boot` and giving up. The window is bounded so a
 * missing radio cannot spin forever. */
#define WLAN_BOOT_BUDGET_MS 60000u
#define WLAN_ATTEMPT_MS 700u

static int s_wlan_window;       /* a start attempt window is open */
static int s_wlan_conf;         /* wpa_supplicant.conf written this window */
static int s_wlan_modprobe;     /* tried to load the radio module */
static int s_wlan_kicked;       /* link up + association command issued */
static long long s_wlan_deadline;
static long long s_wlan_next_try;
static char s_wlan_ssid[128];
static char s_wlan_psk[128];

/* Shorten the retry window (tests). */
static unsigned wlan_env_ms(const char *name, unsigned fallback)
{
	const char *e = getenv(name);
	unsigned v;

	if (!e || !e[0])
		return fallback;
	v = (unsigned)atoi(e);
	return v ? v : fallback;
}

int mmb_wlan_available(void)
{
	return wlan_iface() != 0;
}

int mmb_wlan_radio_pending(void)
{
	if (!s_wlan_window)
		return 0;
	return now_ms() < s_wlan_deadline;
}

int mmb_wlan_scan(char ssids[][64], int maxn)
{
	const char *iface = wlan_iface();
	char cmd[160], out[CMD_OUT];
	const char *line;
	int rc, n = 0;

	if (!iface || maxn <= 0)
		return 0;
	snprintf(cmd, sizeof cmd, "iw dev %s scan", iface);
	rc = run_capture(cmd, out, sizeof out, 20000);
	if (rc != 0)
	{
		snprintf(cmd, sizeof cmd, "wpa_cli -i %s scan_results", iface);
		rc = run_capture(cmd, out, sizeof out, 8000);
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

/* Join a network: write the wpa_supplicant config and reload it. */
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
	run_capture(cmd, 0, 0, 8000);
}

/* Request a DHCP lease on `iface` (busybox udhcpc, as shipped on the ISO).
 * No -b: a backgrounded client would outlive this call and keep the console
 * waiting on its parent. */
static void dhcp_get(const char *iface)
{
	char cmd[200];

	if (!iface)
		return;
	snprintf(cmd, sizeof cmd, "udhcpc -i %s -n -q -t 4 -T 2", iface);
	run_capture(cmd, 0, 0, 12000);
}

/* Open a fresh association window for `ssid`/`psk`. */
static void wlan_begin_window(const char *ssid, const char *psk)
{
	snprintf(s_wlan_ssid, sizeof s_wlan_ssid, "%s", ssid);
	snprintf(s_wlan_psk, sizeof s_wlan_psk, "%s", psk ? psk : "");
	s_wlan_conf = 0;
	s_wlan_modprobe = 0;
	s_wlan_kicked = 0;
	s_wlan_next_try = 0;
	s_wlan_window = 1;
	s_wlan_deadline = now_ms() +
			  (long long)wlan_env_ms("MMB_NET_WLAN_BUDGET_MS",
						 WLAN_BOOT_BUDGET_MS);
}

/* One bring-up attempt: make sure the interface is up, association has been
 * requested and DHCP has run. Returns 0 when an IPv4 address is present. */
static int wlan_attempt(const char *ssid, const char *psk)
{
	const char *iface;
	char cmd[320];
	char ip[40];

	if (!s_wlan_conf)
	{
		if (write_wpa_conf(ssid, psk) != 0)
			return -1;
		s_wlan_conf = 1;
	}
	if (!wlan_iface() && !s_wlan_modprobe)
	{
		run_capture("modprobe iwlwifi", 0, 0, 8000);
		s_wlan_modprobe = 1;
	}
	iface = wlan_iface();
	if (!iface)
		return -1;
	if (!s_wlan_kicked)
	{
		snprintf(cmd, sizeof cmd, "ip link set %s up", iface);
		run_capture(cmd, 0, 0, 3000);
		run_capture("rfkill unblock wifi", 0, 0, 3000);
		/* Reload the config the running supplicant already has open.
		 * Restarting the OpenRC service waits on supervise-daemon, which
		 * does not return when the driver or the stop is stuck. */
		snprintf(cmd, sizeof cmd, "wpa_cli -i %s reconfigure", iface);
		if (run_capture(cmd, 0, 0, 5000) != 0)
		{
			snprintf(cmd, sizeof cmd,
				 "rc-service wpa_supplicant restart");
			if (run_capture(cmd, 0, 0, 8000) != 0)
			{
				snprintf(cmd, sizeof cmd,
					 "wpa_supplicant -B -i %s -c %s",
					 iface, wpa_conf_path());
				if (run_capture(cmd, 0, 0, 8000) != 0)
					return -1;
			}
		}
		s_wlan_kicked = 1;
	}
	/* Association is asynchronous; the lease only lands once it does. Returning
	 * early here would latch the boot bring-up before DHCP ever ran. */
	if (iface_ipv4(iface, ip, sizeof ip))
		return 0;
	dhcp_get(iface);
	return iface_ipv4(iface, ip, sizeof ip) ? 0 : -1;
}

int mmb_wlan_start(const char *ssid, const char *psk)
{
	long long now;
	unsigned interval;
	int rc;

	if (!ssid || !ssid[0])
	{
		s_wlan_window = 0;
		return -1;
	}
	if (!s_wlan_window || strcmp(ssid, s_wlan_ssid) != 0 ||
	    strcmp(psk ? psk : "", s_wlan_psk) != 0)
		wlan_begin_window(ssid, psk);
	now = now_ms();
	if (now >= s_wlan_deadline)
		return -1;
	interval = wlan_env_ms("MMB_NET_WLAN_INTERVAL_MS", WLAN_ATTEMPT_MS);
	if (now < s_wlan_next_try)
		return -1;
	rc = wlan_attempt(ssid, psk);
	/* Pace on completion, not on entry: a helper that blocks for seconds must
	 * not make the next poll immediately re-run it. */
	s_wlan_next_try = now_ms() + (long long)interval;
	if (rc == 0)
		s_wlan_window = 0;
	return rc;
}

int mmb_wlan_connect(const char *ssid, const char *psk)
{
	unsigned waited = 0;
	unsigned budget;

	if (!ssid || !ssid[0])
		return -1;
	/* A manual join gets a fresh window even if a boot attempt already
	 * exhausted its budget and mmb_poll latched. */
	wlan_begin_window(ssid, psk);
	budget = wlan_env_ms("MMB_NET_WLAN_BUDGET_MS", WLAN_BOOT_BUDGET_MS);
	if (budget > 20000u)
		budget = 20000u;
	while (waited < budget)
	{
		if (mmb_wlan_start(ssid, psk) == 0)
			return 0;
		if (!mmb_wlan_radio_pending())
			return -1;
		usleep(100000);
		waited += 100;
	}
	return mmb_wlan_start(ssid, psk) == 0 ? 0 : -1;
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
