/* Minimal stand-in for mmbasic/include/mmb_priv.h so tests/ntp_host.c can
 * compile the real mmbasic/src/ntp.c on the host (#1181). Only the symbols
 * ntp.c uses are declared here; the implementations live in tests/ntp_host.c.
 */
#ifndef NTP_TEST_MMB_PRIV_H
#define NTP_TEST_MMB_PRIV_H

#include <stddef.h>
#include <stdint.h>

typedef struct mmb_options {
	char ntp_server[64]; /* OPTION NTP SERVER "host[:port]" (#524) */
	int ntp_enabled;     /* OPTION NTP: MMB_NTP_AUTO/0 OFF/1 ON (#581) */
} mmb_options;

typedef struct mmb {
	const char *p;
	mmb_options opt;
	int running;
	char date_s[16];
	char time_s[16];
} mmb;

extern mmb *g_cur;
#define G (*g_cur)

#define MMB_NTP_DEFAULT_SERVER "pool.ntp.org"
#define MMB_NTP_DEFAULT_PORT   123
#define MMB_NTP_AUTO           (-1)

void mmb_syntax(void);
void mmb_skip_sp(void);
void mmb_out(const char *s);

int mmb_net_available(void);
int mmb_net_udp_roundtrip(const char *host, int port,
			  const void *tx, unsigned txlen,
			  void *rx, unsigned rxcap, int timeout_ms);

int mmb_clock_set_epoch(int64_t utc_epoch);
int64_t mmb_epoch_now(void);
unsigned mmb_now_ms(void);

void mmb_ntp_poll(void);

#endif
