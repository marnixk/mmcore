/*
 * TLS transport for WEB on the bare-metal Circle build (#1479).
 *
 * mbedTLS 2.28 (vendored under mmbasic/third_party/mbedtls) runs a TLS 1.2+
 * client over a Circle CSocket. There is no operating-system trust store,
 * entropy source, clock or socket API here, so this file supplies all four:
 *
 *  - sockets: mmb_net_tcp_open() resolves DNS and connects on a Circle task
 *    (so a slow SYN never freezes the interpreter); the connected CSocket is
 *    then handed to mbedTLS through custom blocking-free BIO callbacks.
 *  - entropy: the BCM2835/BCM2711 hardware RNG via mbedtls_hardware_poll().
 *  - clock: the existing Circle libc time() shim (RTC/NTP wall clock), used by
 *    the X.509 NotBefore/NotAfter checks.
 *  - trust: the Mozilla root bundle embedded at build time plus any PEM added
 *    with WEB TLS CA file$.
 *
 * Verification (chain + hostname) is on by default. WEB TLS NOVERIFY selects
 * the explicit insecure mode. Handshake and request I/O are bounded by the
 * caller's timeout and yield through mmb_net_yield() so the cooperative task
 * model keeps running.
 */
#include "mmbasic.h"

#if defined(MMB_CIRCLE_NET) && defined(MMB_CIRCLE_TLS)

#include <circle/net/socket.h>
#include <circle/net/error.h>
#include <circle/net/in.h>
#include <circle/netdevice.h>
#include <circle/sched/scheduler.h>
#include <circle/timer.h>
#include <circle/bcmrandom.h>
#include <circle/types.h>

#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/net_sockets.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

/* C helpers in net.cpp (not exposed through mmbasic.h). */
extern "C" int mmb_net_tcp_open(const char *host, int port);
extern "C" const char *mmb_net_tcp_errmsg(void);
extern "C" void mmb_net_tcp_close(void);
extern "C" void mmb_net_yield(void);
extern "C" unsigned mmb_now_ms(void);
extern "C" CSocket *mmb_net_tcp_take_socket(void);

/* Generated from cacert.pem by scripts/gen_ca_bundle.py. */
extern "C" const char mmb_tls_ca_bundle_pem[];
extern "C" const unsigned mmb_tls_ca_bundle_len;

/* The Circle newlib shims provide time() (RTC/NTP wall clock) but not
 * gmtime(), which mbedTLS needs to compare certificate NotBefore/NotAfter
 * against the clock. Convert Unix seconds in UTC here. Only the calendar
 * fields mbedTLS reads are filled; tm_yday/tm_zone are left zero. */
extern "C" struct tm *gmtime(const time_t *tp)
{
	static struct tm out;
	long long t, days, rem, z, era, yy;
	unsigned doe, yoe, doy, mp, dd, mm;

	if (!tp || *tp < 0)
		return 0;
	t = (long long)*tp;
	days = t / 86400;
	rem = t % 86400;

	memset(&out, 0, sizeof out);
	z = days + 719468;
	era = (z >= 0 ? z : z - 146096) / 146097;
	doe = (unsigned)(z - era * 146097);
	yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	yy = (long long)yoe + era * 400;
	doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	mp = (5 * doy + 2) / 153;
	dd = doy - (153 * mp + 2) / 5 + 1;
	mm = mp < 10 ? mp + 3 : mp - 9;
	yy += (mm <= 2);

	out.tm_year = (int)yy - 1900;
	out.tm_mon = (int)mm - 1;
	out.tm_mday = (int)dd;
	out.tm_hour = (int)(rem / 3600);
	out.tm_min = (int)((rem % 3600) / 60);
	out.tm_sec = (int)(rem % 60);
	{
		long long wd = (days + 4) % 7;

		if (wd < 0)
			wd += 7;
		out.tm_wday = (int)wd;
	}
	out.tm_isdst = 0;
	return &out;
}

/* mbedTLS may ask the BIO for as little as a TLS record header. Circle's
 * CSocket::Receive copies min(requested, segment) and frees the rest of the
 * segment, so the buffer handed to it must cover a full Ethernet frame or
 * data is silently dropped. Keep a frame-sized receive buffer and drip bytes
 * out to mbedTLS. */
static u8 s_rx[FRAME_BUFFER_SIZE];
static unsigned s_rxn;
static unsigned s_rxoff;

static CSocket *s_sock;
static int s_peer_closed;
static int s_verify = 1;
static char s_errmsg[192];

static mbedtls_ssl_context s_ssl;
static mbedtls_ssl_config s_conf;
static mbedtls_entropy_context s_entropy;
static mbedtls_ctr_drbg_context s_drbg;
static int s_ssl_live;

static mbedtls_x509_crt s_ca;
static int s_ca_live;
static int s_ca_loaded;
static char *s_custom_pem;
static int s_custom_len;

static void set_err(const char *m)
{
	if (!m || !m[0])
		m = "TLS error";
	strncpy(s_errmsg, m, sizeof(s_errmsg) - 1);
	s_errmsg[sizeof(s_errmsg) - 1] = 0;
}

static void set_err_verify(void)
{
	uint32_t flags = mbedtls_ssl_get_verify_result(&s_ssl);

	if (flags != 0 && flags != (uint32_t)-1)
	{
		if (mbedtls_x509_crt_verify_info(s_errmsg, sizeof(s_errmsg),
						 "", flags) < 0)
			set_err("certificate verify failed");
		else
		{
			/* The info string ends in a newline; trim it. */
			char *p = s_errmsg + strlen(s_errmsg);
			while (p > s_errmsg && (p[-1] == '\n' || p[-1] == '\r'))
				*--p = 0;
		}
	}
	else
		set_err("certificate verify failed");
}

static void set_err_mbed(int rc)
{
	switch (rc)
	{
	case MBEDTLS_ERR_X509_CERT_VERIFY_FAILED:
		set_err_verify();
		break;
	case MBEDTLS_ERR_SSL_FATAL_ALERT_MESSAGE:
		set_err("TLS alert from server");
		break;
	case MBEDTLS_ERR_NET_CONN_RESET:
		set_err("connection reset by peer");
		break;
	case MBEDTLS_ERR_NET_SEND_FAILED:
		set_err("TLS send failed");
		break;
	case MBEDTLS_ERR_NET_RECV_FAILED:
		set_err("TLS receive failed");
		break;
	default:
		set_err("TLS handshake failed");
		break;
	}
}

static void ca_free(void)
{
	if (s_ca_live)
	{
		mbedtls_x509_crt_free(&s_ca);
		s_ca_live = 0;
	}
	s_ca_loaded = 0;
}

/* Build the trust chain: the embedded Mozilla bundle plus any custom PEM
 * installed with WEB TLS CA. Parsed lazily on the first verified open so a
 * NOVERIFY-only session pays nothing. */
static int ca_build(void)
{
	int rc;

	if (s_ca_loaded)
		return 0;
	ca_free();
	mbedtls_x509_crt_init(&s_ca);
	s_ca_live = 1;
	rc = mbedtls_x509_crt_parse(&s_ca,
				    (const unsigned char *)mmb_tls_ca_bundle_pem,
				    (size_t)mmb_tls_ca_bundle_len + 1);
	if (rc < 0)
	{
		ca_free();
		return -1;
	}
	if (s_custom_pem && s_custom_len > 0)
	{
		rc = mbedtls_x509_crt_parse(&s_ca,
					    (const unsigned char *)s_custom_pem,
					    (size_t)s_custom_len);
		if (rc < 0)
		{
			ca_free();
			return -1;
		}
	}
	s_ca_loaded = 1;
	return 0;
}

static void tls_reset(void)
{
	if (s_ssl_live)
	{
		mbedtls_ssl_close_notify(&s_ssl);
		mbedtls_ssl_free(&s_ssl);
		mbedtls_ssl_config_free(&s_conf);
		mbedtls_ctr_drbg_free(&s_drbg);
		mbedtls_entropy_free(&s_entropy);
		s_ssl_live = 0;
	}
	if (s_sock)
	{
		delete s_sock;
		s_sock = 0;
	}
	s_rxn = 0;
	s_rxoff = 0;
	s_peer_closed = 0;
}

static void tls_pause(void)
{
	if (CScheduler::IsActive())
		CScheduler::Get()->MsSleep(1);
	else
		CTimer::SimpleMsDelay(1);
}

/* ---- mbedTLS BIO over Circle CSocket --------------------------------- */

static int bio_send(void *ctx, const unsigned char *buf, size_t len)
{
	CSocket *sock = (CSocket *)ctx;
	int rc;

	if (!sock)
		return MBEDTLS_ERR_NET_SEND_FAILED;
	rc = sock->Send(buf, (unsigned)len, MSG_DONTWAIT);
	if (rc > 0)
		return rc;
	if (rc == -NET_ERROR_WOULD_BLOCK)
		return MBEDTLS_ERR_SSL_WANT_WRITE;
	return MBEDTLS_ERR_NET_SEND_FAILED;
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len)
{
	CSocket *sock = (CSocket *)ctx;
	unsigned avail;

	if (!sock)
		return MBEDTLS_ERR_NET_RECV_FAILED;
	if (s_rxoff < s_rxn)
	{
		avail = s_rxn - s_rxoff;
		if (avail > len)
			avail = (unsigned)len;
		memcpy(buf, s_rx + s_rxoff, avail);
		s_rxoff += avail;
		return (int)avail;
	}
	{
		int rc = sock->Receive(s_rx, FRAME_BUFFER_SIZE, MSG_DONTWAIT);

		if (rc > 0)
		{
			s_rxn = (unsigned)rc;
			s_rxoff = 0;
			avail = (unsigned)rc;
			if (avail > len)
				avail = (unsigned)len;
			memcpy(buf, s_rx, avail);
			s_rxoff = avail;
			return (int)avail;
		}
		if (rc == 0 || rc == -NET_ERROR_WOULD_BLOCK)
			return MBEDTLS_ERR_SSL_WANT_READ;
		s_peer_closed = 1;
		return MBEDTLS_ERR_NET_CONN_RESET;
	}
}

/* ---- platform entropy ------------------------------------------------ */

extern "C" int mbedtls_hardware_poll(void *data, unsigned char *output,
				     size_t len, size_t *olen)
{
	static CBcmRandomNumberGenerator *rng;
	size_t i = 0;

	(void)data;
	if (!rng)
		rng = new CBcmRandomNumberGenerator();
	if (!rng)
	{
		*olen = 0;
		return -1;
	}
	while (i < len)
	{
		u32 v = rng->GetNumber();
		int j;

		for (j = 0; j < 4 && i < len; j++, i++)
			output[i] = (unsigned char)(v >> (8 * j));
	}
	*olen = len;
	return 0;
}

/* ---- seam ------------------------------------------------------------- */

extern "C" int mmb_net_tls_available(void) { return 1; }

extern "C" void mmb_net_tls_close(void)
{
	tls_reset();
}

extern "C" int mmb_net_tls_set_ca_pem(const char *pem, int len)
{
	mbedtls_x509_crt probe;
	char *copy;
	int rc;

	if (!pem || len <= 0)
		return -1;

	/* mbedTLS only treats a buffer as PEM when its last byte is NUL, and the
	 * parser hands it a length that counts that byte. The VFS read does not
	 * NUL-terminate, so copy once with the terminator. */
	copy = (char *)malloc((size_t)len + 1);
	if (!copy)
		return -1;
	memcpy(copy, pem, (size_t)len);
	copy[len] = 0;

	mbedtls_x509_crt_init(&probe);
	rc = mbedtls_x509_crt_parse(&probe, (const unsigned char *)copy,
				    (size_t)len + 1);
	mbedtls_x509_crt_free(&probe);
	if (rc < 0)
	{
		free(copy);
		return -1;
	}

	if (s_custom_pem)
	{
		free(s_custom_pem);
		s_custom_pem = 0;
		s_custom_len = 0;
	}
	s_custom_pem = copy;
	s_custom_len = len + 1;
	/* Force a rebuild with the new anchor on the next open. */
	ca_free();
	return 0;
}

extern "C" void mmb_net_tls_set_verify(int verify)
{
	s_verify = verify != 0;
}

extern "C" int mmb_net_tls_open(const char *host, int port, int timeout_ms)
{
	unsigned start;
	int rc;

	tls_reset();
	set_err("");

	if (!host || !host[0] || port < 1 || port > 65535)
	{
		set_err("invalid host or port");
		return -1;
	}
	if (timeout_ms < 1)
		timeout_ms = 1;

	/* DNS + TCP connect runs on Circle's open task, bounded by its own
	 * 25s cap; the caller's timeout then bounds the TLS handshake. */
	if (mmb_net_tcp_open(host, port) != 0)
	{
		set_err(mmb_net_tcp_errmsg());
		return -1;
	}
	s_sock = mmb_net_tcp_take_socket();
	if (!s_sock)
	{
		set_err("connect failed");
		return -1;
	}

	mbedtls_ssl_init(&s_ssl);
	mbedtls_ssl_config_init(&s_conf);
	mbedtls_entropy_init(&s_entropy);
	mbedtls_ctr_drbg_init(&s_drbg);
	s_ssl_live = 1;

	{
		static const char pers[] = "mmbcircle";

		rc = mbedtls_ctr_drbg_seed(&s_drbg, mbedtls_entropy_func,
					   &s_entropy,
					   (const unsigned char *)pers,
					   sizeof(pers) - 1);
		if (rc != 0)
		{
			set_err("TLS RNG seed failed");
			tls_reset();
			return -1;
		}
	}

	rc = mbedtls_ssl_config_defaults(&s_conf, MBEDTLS_SSL_IS_CLIENT,
					 MBEDTLS_SSL_TRANSPORT_STREAM,
					 MBEDTLS_SSL_PRESET_DEFAULT);
	if (rc != 0)
	{
		set_err("TLS init failed");
		tls_reset();
		return -1;
	}
	mbedtls_ssl_conf_min_version(&s_conf, MBEDTLS_SSL_MAJOR_VERSION_3,
				     MBEDTLS_SSL_MINOR_VERSION_3);
	mbedtls_ssl_conf_rng(&s_conf, mbedtls_ctr_drbg_random, &s_drbg);
	mbedtls_ssl_conf_authmode(&s_conf,
				  s_verify ? MBEDTLS_SSL_VERIFY_REQUIRED
					   : MBEDTLS_SSL_VERIFY_NONE);
	if (s_verify)
	{
		if (ca_build() != 0)
		{
			set_err("bad CA certificate");
			tls_reset();
			return -1;
		}
		mbedtls_ssl_conf_ca_chain(&s_conf, &s_ca, 0);
	}

	rc = mbedtls_ssl_setup(&s_ssl, &s_conf);
	if (rc != 0)
	{
		set_err("TLS init failed");
		tls_reset();
		return -1;
	}
	/* Sets SNI and the name checked against the certificate. */
	rc = mbedtls_ssl_set_hostname(&s_ssl, host);
	if (rc != 0)
	{
		set_err("TLS host setup failed");
		tls_reset();
		return -1;
	}
	mbedtls_ssl_set_bio(&s_ssl, s_sock, bio_send, bio_recv, 0);

	start = mmb_now_ms();
	for (;;)
	{
		rc = mbedtls_ssl_handshake(&s_ssl);
		if (rc == 0)
			break;
		if (rc == MBEDTLS_ERR_SSL_WANT_READ ||
		    rc == MBEDTLS_ERR_SSL_WANT_WRITE)
		{
			if (mmb_now_ms() - start >= (unsigned)timeout_ms)
			{
				set_err("TLS handshake timeout");
				tls_reset();
				return -1;
			}
			tls_pause();
			continue;
		}
		set_err_mbed(rc);
		tls_reset();
		return -1;
	}
	return 0;
}

extern "C" int mmb_net_tls_send(const void *data, unsigned n)
{
	int rc;

	if (!s_ssl_live || !s_sock)
		return -1;
	if (!data || !n)
		return 0;
	rc = mbedtls_ssl_write(&s_ssl, (const unsigned char *)data, (size_t)n);
	if (rc >= 0)
		return rc;
	if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE)
		return 0;
	set_err_mbed(rc);
	return -1;
}

extern "C" int mmb_net_tls_recv(void *data, unsigned maxn)
{
	int rc;

	if (!s_ssl_live || !s_sock)
		return -1;
	if (!data || !maxn)
		return 0;
	rc = mbedtls_ssl_read(&s_ssl, (unsigned char *)data, (size_t)maxn);
	if (rc > 0)
		return rc;
	if (rc == 0 || rc == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY ||
	    rc == MBEDTLS_ERR_SSL_CONN_EOF ||
	    rc == MBEDTLS_ERR_NET_CONN_RESET)
	{
		s_peer_closed = 1;
		return -1;
	}
	if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE)
		return 0;
	set_err_mbed(rc);
	s_peer_closed = 1;
	return -1;
}

extern "C" int mmb_net_tls_rx_avail(void)
{
	if (!s_ssl_live || !s_sock || s_peer_closed)
		return 0;
	return (s_rxoff < s_rxn) ? (int)(s_rxn - s_rxoff) : 0;
}

extern "C" int mmb_net_tls_peer_closed(void) { return s_peer_closed; }

extern "C" const char *mmb_net_tls_errmsg(void)
{
	return s_errmsg[0] ? s_errmsg : "TLS error";
}

#endif /* MMB_CIRCLE_NET && MMB_CIRCLE_TLS */
