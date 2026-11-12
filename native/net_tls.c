/*
 * TLS transport for WEB on the native build.
 *
 * The high-level WEB(...) HTTPS forms and WEB OPEN TLS CLIENT need a TLS
 * stack.  This host build links OpenSSL when the toolchain provides it
 * (native/Makefile sets -DMMB_HAVE_TLS); every other backend (Circle, and a
 * Windows/MinGW host without OpenSSL) compiles the stubs below so HTTPS
 * reports a clear platform limitation instead of failing to build.
 *
 * Server certificate and hostname verification are ON by default.  The only
 * way to disable them is the explicit WEB TLS NOVERIFY command.  A custom CA
 * (self-signed test server, private PKI) is supplied with WEB TLS CA file$,
 * which parses the PEM in the interpreter and installs it here.
 */
#include "mmb_priv.h"

#if defined(MMB_HAVE_TLS) && !defined(_WIN32)

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509v3.h>
#include <openssl/pem.h>
#include <openssl/bio.h>

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

static int s_fd = -1;
static SSL *s_ssl;
static SSL_CTX *s_ctx;
static int s_peer_closed;
static char s_errmsg[160];
static char *s_ca_pem;
static int s_ca_len;
static int s_verify = 1;

static void set_err(const char *m)
{
	if (!m || !m[0])
		m = "TLS error";
	strncpy(s_errmsg, m, sizeof(s_errmsg) - 1);
	s_errmsg[sizeof(s_errmsg) - 1] = 0;
}

static void tls_reset(void)
{
	if (s_ssl)
	{
		SSL_shutdown(s_ssl);
		SSL_free(s_ssl);
		s_ssl = 0;
	}
	if (s_ctx)
	{
		SSL_CTX_free(s_ctx);
		s_ctx = 0;
	}
	if (s_fd >= 0)
	{
		close(s_fd);
		s_fd = -1;
	}
	s_peer_closed = 0;
}

int mmb_net_tls_available(void) { return 1; }

void mmb_net_tls_close(void)
{
	tls_reset();
}

int mmb_net_tls_set_ca_pem(const char *pem, int len)
{
	BIO *bio;
	X509 *x;
	int n = 0;

	if (!pem || len <= 0)
		return -1;
	if (s_ca_pem)
	{
		G.plat->free(s_ca_pem);
		s_ca_pem = 0;
		s_ca_len = 0;
	}
	s_ca_pem = G.plat->alloc((unsigned)len);
	if (!s_ca_pem)
		return -1;
	memcpy(s_ca_pem, pem, (unsigned)len);
	s_ca_len = len;

	/* Validate it parses as at least one PEM certificate. */
	bio = BIO_new_mem_buf(pem, len);
	if (!bio)
		return -1;
	while ((x = PEM_read_bio_X509(bio, 0, 0, 0)) != 0)
	{
		X509_free(x);
		n++;
	}
	BIO_free(bio);
	ERR_clear_error();
	return n > 0 ? 0 : -1;
}

void mmb_net_tls_set_verify(int verify)
{
	s_verify = verify != 0;
}

static int install_ca(SSL_CTX *ctx)
{
	BIO *bio;
	X509 *x;
	int n = 0;

	if (!s_ca_pem || s_ca_len <= 0)
		return 0;
	bio = BIO_new_mem_buf(s_ca_pem, s_ca_len);
	if (!bio)
		return -1;
	while ((x = PEM_read_bio_X509(bio, 0, 0, 0)) != 0)
	{
		if (X509_STORE_add_cert(SSL_CTX_get_cert_store(ctx), x) == 1)
			n++;
		X509_free(x);
	}
	BIO_free(bio);
	ERR_clear_error();
	return n > 0 ? 0 : -1;
}

int mmb_net_tls_open(const char *host, int port, int timeout_ms)
{
	struct addrinfo hints, *res = 0, *ai;
	char ps[16];
	int fd = -1, rc = -1, err = 0;
	struct timeval tv;

	tls_reset();
	set_err("");
	if (!host || !host[0] || port < 1 || port > 65535)
	{
		set_err("Invalid host or port");
		return -1;
	}
	if (timeout_ms < 1)
		timeout_ms = 1;

	snprintf(ps, sizeof ps, "%d", port);
	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	if (getaddrinfo(host, ps, &hints, &res) != 0 || !res)
	{
		set_err("DNS failed");
		return -1;
	}
	for (ai = res; ai; ai = ai->ai_next)
	{
		int fl;

		fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (fd < 0)
		{
			err = errno;
			continue;
		}
		fl = fcntl(fd, F_GETFL, 0);
		if (fl >= 0)
			fcntl(fd, F_SETFL, fl | O_NONBLOCK);
		if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
			break;
		if (errno == EINPROGRESS)
		{
			struct pollfd p;
			int so = 0;
			socklen_t sl = sizeof so;

			p.fd = fd;
			p.events = POLLOUT;
			p.revents = 0;
			if (poll(&p, 1, timeout_ms) > 0 &&
			    getsockopt(fd, SOL_SOCKET, SO_ERROR, &so, &sl) == 0 &&
			    so == 0)
				break;
		}
		err = errno;
		close(fd);
		fd = -1;
	}
	freeaddrinfo(res);
	if (fd < 0)
	{
		set_err(err == ECONNREFUSED ? "Connection refused" :
			"Connect failed");
		return -1;
	}

	/* Back to blocking with socket-level timeouts for the TLS handshake and
	 * the request/response exchange. */
	{
		int fl = fcntl(fd, F_GETFL, 0);

		if (fl >= 0)
			fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
	}
	tv.tv_sec = timeout_ms / 1000;
	tv.tv_usec = (timeout_ms % 1000) * 1000;
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);

	s_ctx = SSL_CTX_new(TLS_client_method());
	if (!s_ctx)
	{
		close(fd);
		set_err("TLS init failed");
		return -1;
	}
	SSL_CTX_set_min_proto_version(s_ctx, TLS1_2_VERSION);
	SSL_CTX_set_default_verify_paths(s_ctx);
	if (install_ca(s_ctx) != 0)
	{
		close(fd);
		tls_reset();
		set_err("Bad CA certificate");
		return -1;
	}
	SSL_CTX_set_verify(s_ctx, s_verify ? SSL_VERIFY_PEER : SSL_VERIFY_NONE,
			   0);

	s_ssl = SSL_new(s_ctx);
	if (!s_ssl)
	{
		close(fd);
		tls_reset();
		set_err("TLS init failed");
		return -1;
	}
	SSL_set_fd(s_ssl, fd);
	SSL_set_tlsext_host_name(s_ssl, host);
	if (s_verify)
	{
		X509_VERIFY_PARAM *param = SSL_get0_param(s_ssl);

		X509_VERIFY_PARAM_set_hostflags(param,
			X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
		X509_VERIFY_PARAM_set1_host(param, host, 0);
	}
	s_fd = fd;
	rc = SSL_connect(s_ssl);
	if (rc != 1)
	{
		unsigned long e = ERR_get_error();
		const char *m = e ? ERR_reason_error_string(e) : 0;

		set_err(m ? m : "TLS handshake failed");
		tls_reset();
		return -1;
	}
	if (s_verify && SSL_get_verify_result(s_ssl) != X509_V_OK)
	{
		set_err("Server certificate verification failed");
		tls_reset();
		return -1;
	}
	return 0;
}

int mmb_net_tls_send(const void *data, unsigned n)
{
	int r;

	if (!s_ssl)
		return -1;
	r = SSL_write(s_ssl, data, (int)n);
	if (r > 0)
		return r;
	switch (SSL_get_error(s_ssl, r))
	{
	case SSL_ERROR_WANT_READ:
	case SSL_ERROR_WANT_WRITE:
		return 0;
	default:
		set_err("TLS send failed");
		return -1;
	}
}

int mmb_net_tls_recv(void *data, unsigned maxn)
{
	int r;

	if (!s_ssl)
		return -1;
	r = SSL_read(s_ssl, data, (int)maxn);
	if (r > 0)
		return r;
	{
		int e = SSL_get_error(s_ssl, r);

		if (e == SSL_ERROR_ZERO_RETURN)
		{
			s_peer_closed = 1;
			return -1;
		}
		if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE)
			return 0;
		if (e == SSL_ERROR_SYSCALL && (errno == EAGAIN ||
					       errno == EWOULDBLOCK))
			return 0;
		if (e == SSL_ERROR_SYSCALL && errno == 0)
		{
			s_peer_closed = 1;
			return -1;
		}
		s_peer_closed = 1;
		set_err("TLS receive failed");
		return -1;
	}
}

int mmb_net_tls_rx_avail(void) { return 0; }
int mmb_net_tls_peer_closed(void) { return s_peer_closed; }

const char *mmb_net_tls_errmsg(void)
{
	return s_errmsg[0] ? s_errmsg : "TLS error";
}

#else /* no TLS stack on this backend */

int mmb_net_tls_available(void) { return 0; }
void mmb_net_tls_close(void) {}
int mmb_net_tls_set_ca_pem(const char *pem, int len)
{
	(void)pem;
	(void)len;
	return -1;
}
void mmb_net_tls_set_verify(int verify) { (void)verify; }
int mmb_net_tls_open(const char *host, int port, int timeout_ms)
{
	(void)host;
	(void)port;
	(void)timeout_ms;
	return -1;
}
int mmb_net_tls_send(const void *data, unsigned n)
{
	(void)data;
	(void)n;
	return -1;
}
int mmb_net_tls_recv(void *data, unsigned maxn)
{
	(void)data;
	(void)maxn;
	return -1;
}
int mmb_net_tls_rx_avail(void) { return 0; }
int mmb_net_tls_peer_closed(void) { return 1; }
const char *mmb_net_tls_errmsg(void) { return "TLS unavailable"; }

#endif
