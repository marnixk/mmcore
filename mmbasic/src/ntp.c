/*
 * NTP clock sync (#524).
 *
 * A minimal one-shot SNTP client: build a 48-byte NTP v4 request, send it over
 * UDP with the platform transport, and set the clock from the server transmit
 * timestamp. Timezones are applied for display only (DATE$/TIME$/DATETIME$);
 * EPOCH() stays UTC.
 */
#include "mmb_priv.h"
#include <string.h>

/* Seconds between the NTP epoch (1900-01-01) and the Unix epoch. */
#define NTP_UNIX_DELTA 2208988800ULL

#define MMB_NTP_QUERY_TIMEOUT_MS 2000
#define MMB_NTP_AUTOSYNC_TRIES   3

int mmb_ntp_parse_server(const char *spec, char *host, int hostcap, int *port)
{
	const char *p;
	const char *colon = 0;
	int n = 0, pnum = 0;
	unsigned digits = 0;

	if (!spec || !host || hostcap < 1 || !port)
		return 0;
	while (*spec == ' ' || *spec == '\t')
		spec++;
	p = spec;
	while (*p)
	{
		if (*p == ':')
			colon = p;
		p++;
	}
	*port = MMB_NTP_DEFAULT_PORT;
	if (colon && colon != spec && colon[1])
	{
		const char *q = colon + 1;
		digits = 0;
		while (*q >= '0' && *q <= '9')
		{
			pnum = pnum * 10 + (*q - '0');
			digits++;
			q++;
		}
		if (*q == 0 && digits > 0 && digits <= 5 && pnum >= 1 && pnum <= 65535)
			*port = pnum;
		else
			colon = 0; /* not a port: treat the whole spec as the host */
	}
	p = spec;
	n = 0;
	while (*p && *p != ' ' && !(colon && p == colon))
	{
		if (n + 1 >= hostcap)
			return 0;
		host[n++] = *p++;
	}
	host[n] = 0;
	return n > 0;
}

static void put_u32_be(unsigned char *p, unsigned long v)
{
	p[0] = (unsigned char)(v >> 24);
	p[1] = (unsigned char)(v >> 16);
	p[2] = (unsigned char)(v >> 8);
	p[3] = (unsigned char)v;
}

static unsigned long get_u32_be(const unsigned char *p)
{
	return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) |
	       ((unsigned long)p[2] << 8) | (unsigned long)p[3];
}

/* Build and send the request, then wait for a server reply. */
static int ntp_exchange(const char *server, unsigned char *resp, int *out_len)
{
	unsigned char req[48];
	char host[80];
	int port, rc;
	unsigned long secs, frac;

	if (!mmb_ntp_parse_server(server, host, (int)sizeof(host), &port))
		return -2;
	memset(req, 0, sizeof(req));
	req[0] = (unsigned char)((4 << 3) | 3); /* LI 0, VN 4, mode 3 (client) */
	/* A single monotonically-varying transmit nonce detects stale replies. */
	secs = (unsigned long)(mmb_epoch_now() & 0xffffffffu);
	frac = mmb_now_ms();
	put_u32_be(req + 40, secs);
	put_u32_be(req + 44, frac);
	rc = mmb_net_udp_roundtrip(host, port, req, (unsigned)sizeof(req),
				   resp, 48, MMB_NTP_QUERY_TIMEOUT_MS);
	if (rc < 0)
		return rc == -1 ? -1 : -2;
	if (rc == 0)
		return -3;
	if (rc < 48)
		return -4;
	/* Reject replies whose originate field does not echo our request. */
	if (get_u32_be(resp + 24) != secs || get_u32_be(resp + 28) != frac)
		return -4;
	if ((resp[0] & 0x07) != 4 && (resp[0] & 0x07) != 5)
		return -4;
	*out_len = rc;
	return 0;
}

int mmb_ntp_sync(const char *server, int64_t *out_epoch)
{
	unsigned char resp[48];
	int len = 0, rc;
	unsigned long ntp_secs;

	if (!server || !server[0])
		return -2;
	if (!mmb_net_available())
		return -1;
	rc = ntp_exchange(server, resp, &len);
	if (rc != 0)
		return rc;
	ntp_secs = get_u32_be(resp + 40);
	if ((unsigned long long)ntp_secs <= NTP_UNIX_DELTA)
		return -4;
	if (out_epoch)
		*out_epoch = (int64_t)((unsigned long long)ntp_secs - NTP_UNIX_DELTA);
	return 0;
}

const char *mmb_ntp_errmsg(int rc)
{
	switch (rc)
	{
	case -1: return "Network not available";
	case -2: return "Server not reachable";
	case -3: return "No reply from server";
	case -4: return "Invalid reply";
	default: return "Sync failed";
	}
}

void mmb_cmd_ntp(void)
{
	int64_t epoch = 0;
	int rc;
	char msg[128];
	int n = 0;
	const char *a;

	mmb_skip_sp();
	if (*G.p && *G.p != ':' && *G.p != '\'' && *G.p != ',')
		mmb_syntax();
	if (!G.opt.ntp_server[0])
	{
		mmb_out("NTP server not set");
		return;
	}
	rc = mmb_ntp_sync(G.opt.ntp_server, &epoch);
	if (rc == 0)
	{
		mmb_clock_set_epoch(epoch);
		a = "Time synced: ";
	}
	else
	{
		a = "NTP failed: ";
	}
	while (*a && n < (int)sizeof(msg) - 1)
		msg[n++] = *a++;
	if (rc == 0)
	{
		a = G.date_s;
		while (*a && n < (int)sizeof(msg) - 1)
			msg[n++] = *a++;
		if (n < (int)sizeof(msg) - 1)
			msg[n++] = ' ';
		a = G.time_s;
		while (*a && n < (int)sizeof(msg) - 1)
			msg[n++] = *a++;
	}
	else
	{
		a = mmb_ntp_errmsg(rc);
		while (*a && n < (int)sizeof(msg) - 1)
			msg[n++] = *a++;
	}
	msg[n] = 0;
	mmb_out(msg);
}

/* Boot-time best-effort sync. Runs at most a few times and only once the
 * network is up; a failure leaves the clock as-is. */
void mmb_ntp_poll(void)
{
	static int attempts;
	int64_t epoch = 0;

	if (!G.opt.ntp_enabled || attempts >= MMB_NTP_AUTOSYNC_TRIES)
		return;
	if (!mmb_net_available())
		return;
	attempts++;
	if (mmb_ntp_sync(G.opt.ntp_server, &epoch) == 0)
	{
		mmb_clock_set_epoch(epoch);
		attempts = MMB_NTP_AUTOSYNC_TRIES;
	}
}
