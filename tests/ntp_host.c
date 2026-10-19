/*
 * Host test for the boot-time NTP poll in mmbasic/src/ntp.c (#1181).
 *
 * Compiles the real ntp.c against the shims in tests/ntp_shim/mmb_priv.h. A
 * fake clock, a switchable "link ready" flag and a fake NTP exchange let the
 * gate/retry policy be exercised without touching the real network.
 *
 * The binary is compiled twice:
 *   - without -DMMB_NTP_AUTOSYNC (the POSIX host/desktop build): scenarios
 *     "host_off" and "host_forced_on" prove auto-sync stays off even when the
 *     link reports ready and OPTION NTP is explicitly ON.
 *   - with -DMMB_NTP_AUTOSYNC (the framebuffer/ISO build): "offline",
 *     "linkup", "retry_window" and "late_linkup" exercise the enabled policy.
 *
 * argv[1] selects one scenario; there is no scenario argument default.
 */
#include "mmb_priv.h"

#include <stdio.h>
#include <string.h>

static mmb g_store;
mmb *g_cur = &g_store;

static int s_link;        /* value of mmb_net_available() */
static int s_exchange_ok; /* the fake server replies when non-zero */
static unsigned s_now;    /* fake mmb_now_ms() */
static int s_attempts;    /* mmb_net_udp_roundtrip() calls */
static int s_clock_sets;  /* mmb_clock_set_epoch() calls */

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

/* ---- shims ------------------------------------------------------------ */

unsigned mmb_now_ms(void) { return s_now; }

int64_t mmb_epoch_now(void) { return 1700000000; }

int mmb_net_available(void) { return s_link; }

int mmb_clock_set_epoch(int64_t utc_epoch)
{
	(void)utc_epoch;
	s_clock_sets++;
	return 0;
}

void mmb_syntax(void) {}
void mmb_skip_sp(void) {}
void mmb_out(const char *s) { (void)s; }

/* A fake SNTP server: echoes the request nonce and returns a valid transmit
 * timestamp so mmb_ntp_sync() succeeds, or fails when s_exchange_ok is 0. */
int mmb_net_udp_roundtrip(const char *host, int port, const void *tx,
			  unsigned txlen, void *rx, unsigned rxcap, int timeout_ms)
{
	unsigned char *r = (unsigned char *)rx;
	const unsigned char *t = (const unsigned char *)tx;
	unsigned long ntp = 3917000000UL; /* well past the Unix-epoch delta */

	(void)host;
	(void)port;
	(void)timeout_ms;
	s_attempts++;
	if (!s_exchange_ok)
		return -1;
	if (!tx || !rx || txlen < 48 || rxcap < 48)
		return -4;
	memset(r, 0, 48);
	r[0] = (unsigned char)((4 << 3) | 4); /* LI 0, VN 4, mode 4 (server) */
	memcpy(r + 24, t + 40, 8);            /* originate echoes our nonce */
	r[40] = (unsigned char)(ntp >> 24);
	r[41] = (unsigned char)(ntp >> 16);
	r[42] = (unsigned char)(ntp >> 8);
	r[43] = (unsigned char)ntp;
	return 48;
}

/* ---- scenarios -------------------------------------------------------- */

static void reset(void)
{
	memset(&g_store, 0, sizeof g_store);
	s_link = 0;
	s_exchange_ok = 1;
	s_now = 0;
	s_attempts = 0;
	s_clock_sets = 0;
	strcpy(G.opt.ntp_server, "pool.ntp.org");
	G.opt.ntp_enabled = MMB_NTP_AUTO;
	G.running = 0;
}

static int scenario_host_off(void)
{
	int i;

	reset();
	/* net_posix.c reports the network as always available, which is exactly
	 * why the host build must not auto-sync. */
	s_link = 1;
	for (i = 0; i < 20; i++)
	{
		mmb_ntp_poll();
		s_now += 5000;
	}
	CHECK(s_attempts == 0, "host build never auto-syncs");
	CHECK(s_clock_sets == 0, "host build leaves the clock alone");
	return fails ? 1 : 0;
}

static int scenario_host_forced_on(void)
{
	int i;

	reset();
	s_link = 1;
	G.opt.ntp_enabled = 1; /* OPTION NTP ON */
	for (i = 0; i < 20; i++)
	{
		mmb_ntp_poll();
		s_now += 5000;
	}
	CHECK(s_attempts == 0, "explicit ON does not bypass the host gate");
	CHECK(s_clock_sets == 0, "explicit ON still leaves the clock alone");
	return fails ? 1 : 0;
}

static int scenario_offline(void)
{
	int i;

	reset();
	s_link = 0;
	for (i = 0; i < 50; i++)
	{
		mmb_ntp_poll();
		s_now += 5000;
	}
	CHECK(s_attempts == 0, "no link means no attempt");
	CHECK(s_clock_sets == 0, "offline stays quiet");
	return fails ? 1 : 0;
}

static int scenario_linkup(void)
{
	reset();
	mmb_ntp_poll();
	CHECK(s_attempts == 0, "no attempt before the link appears");
	s_link = 1;
	mmb_ntp_poll();
	CHECK(s_attempts == 1, "link-up triggers an immediate exchange");
	CHECK(s_clock_sets == 1, "a successful sync sets the clock");
	/* done: further polls must not repeat the exchange. */
	s_now += 60000;
	mmb_ntp_poll();
	CHECK(s_attempts == 1, "the sync runs once");
	return fails ? 1 : 0;
}

static int scenario_retry_window(void)
{
	int i;
	int after;

	reset();
	s_link = 1;
	s_exchange_ok = 0;
	/* 400 simulated seconds of one poll per second. */
	for (i = 0; i < 400; i++)
	{
		mmb_ntp_poll();
		s_now += 1000;
	}
	CHECK(s_attempts >= 2, "retries while the window is open");
	CHECK(s_attempts <= 12, "retries are bounded");
	/* Once the window has elapsed, further polls are no-ops. */
	after = s_attempts;
	for (i = 0; i < 200; i++)
	{
		mmb_ntp_poll();
		s_now += 1000;
	}
	CHECK(s_attempts == after, "gives up once the window closes");
	return fails ? 1 : 0;
}

static int scenario_late_linkup(void)
{
	int i;

	reset();
	s_link = 0;
	/* Stay offline for well past the window: no window opens while offline. */
	for (i = 0; i < 400; i++)
	{
		mmb_ntp_poll();
		s_now += 1000;
	}
	CHECK(s_attempts == 0, "offline never attempts");
	s_link = 1;
	mmb_ntp_poll();
	CHECK(s_attempts == 1, "a link that appears late still syncs");
	CHECK(s_clock_sets == 1, "late link-up sets the clock");
	return fails ? 1 : 0;
}

int main(int argc, char **argv)
{
	const char *what = argc > 1 ? argv[1] : "";
	int rc;

	if (strcmp(what, "host_off") == 0)
		rc = scenario_host_off();
	else if (strcmp(what, "host_forced_on") == 0)
		rc = scenario_host_forced_on();
	else if (strcmp(what, "offline") == 0)
		rc = scenario_offline();
	else if (strcmp(what, "linkup") == 0)
		rc = scenario_linkup();
	else if (strcmp(what, "retry_window") == 0)
		rc = scenario_retry_window();
	else if (strcmp(what, "late_linkup") == 0)
		rc = scenario_late_linkup();
	else
	{
		fprintf(stderr, "unknown scenario: %s\n", what);
		return 2;
	}
	if (rc != 0)
		return rc;
	printf("all checks passed\n");
	return 0;
}
