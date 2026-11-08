/*
 * WEB: PicoMite-compatible TCP/TLS primitives plus a small HTTP client.
 *
 * High-level forms (approved notation):
 *
 *   r$       = WEB$(HTTP GET url$)
 *   r$       = WEB$(HTTP POST url$, payload HEADERS h$())
 *   person   = WEB(HTTP GET url$ AS PersonType)
 *
 * Low-level PicoMite parity:
 *
 *   WEB OPEN TCP CLIENT address$, port [,timeout]
 *   WEB OPEN TLS CLIENT address$, port [,timeout]
 *   WEB TCP CLIENT REQUEST request$, buff%() [,timeout]
 *   WEB TCP CLIENT WRITE data%() [,timeout]
 *   WEB TCP CLIENT READ buff%() [,timeout]
 *   WEB CLOSE TCP CLIENT
 *   WEB TLS CA file$ / WEB TLS NOVERIFY / WEB TLS VERIFY
 *   WEB TIMEOUT ms
 *   WEB TCP PORT port / WEB TCP INTERRUPT / WEB TCP READ buff%()
 *   WEB TCP SEND data%() / WEB TCP CLOSE
 *   WEB TRANSMIT PAGE cb%, filename$ [,buffersize]
 *   WEB TRANSMIT FILE cb%, filename$, content-type$
 *   WEB TRANSMIT CODE cb%, nnn%
 *
 * Plain TCP reuses the single machine-wide mmb_net_tcp_* connection (the one
 * OPEN "TCP:" and CONNECT use) and claims it for the calling console, so WEB
 * and an open TCP: stream cannot run at the same time. TLS uses its own
 * platform seam (native OpenSSL when present) and does not touch that socket.
 *
 * Buffers passed as buff%()/data%() are INTEGER arrays used PicoMite-style:
 * the first linear element holds the length and the following elements hold
 * one byte each (0..255). The linear layout means "first element" is index 0
 * under OPTION BASE 0 and index 1 under OPTION BASE 1.
 *
 * Returned HTTP text/JSON is data only; it is never executed. PAGE template
 * substitution is the one explicit evaluation feature and is opt-in.
 */
#include "mmb_priv.h"
#include <stdio.h>
#include <string.h>

#define WEB_DEF_TIMEOUT_MS 30000u
#define WEB_MAX_TIMEOUT_MS 600000u
#define WEB_HDR_MAX        65536u
#define WEB_BODY_MAX       (16u * 1024u * 1024u)
#define WEB_URL_HOST_MAX   256
#define WEB_URL_PATH_MAX   4096
#define WEB_REDIR_MAX      5
#define WEB_RX_TMP         1600

static unsigned s_timeout_ms = WEB_DEF_TIMEOUT_MS;
static int s_status;
static char *s_resp_headers;
static unsigned s_resp_headers_len;

enum { TR_NONE = 0, TR_TCP = 1, TR_TLS = 2 };
static int s_tr;
static int s_tcp_claimed;

/* server side */
static int s_lsn = -1;
static int s_sconn = -1;

typedef struct {
	char *p;
	unsigned n;
	unsigned cap;
} wb_t;

static void wb_init(wb_t *b)
{
	b->p = 0;
	b->n = 0;
	b->cap = 0;
}

static void wb_free(wb_t *b)
{
	if (b->p)
		mmb_str_free(b->p);
	b->p = 0;
	b->n = 0;
	b->cap = 0;
}

static void wb_reserve(wb_t *b, unsigned extra)
{
	unsigned need = b->n + extra + 1;
	unsigned cap;
	char *np;

	if (need <= b->cap)
		return;
	cap = b->cap ? b->cap : 256;
	while (cap < need)
	{
		if (cap > (1u << 30))
			mmb_error("?OUT OF MEMORY");
		cap *= 2;
	}
	np = mmb_str_alloc((int)cap);
	if (b->p)
	{
		if (b->n)
			memcpy(np, b->p, b->n);
		mmb_str_free(b->p);
	}
	b->p = np;
	b->cap = cap;
}

static void wb_put(wb_t *b, const void *d, unsigned n)
{
	if (!n)
		return;
	wb_reserve(b, n);
	memcpy(b->p + b->n, d, n);
	b->n += n;
	b->p[b->n] = 0;
}

static void wb_putc(wb_t *b, char c)
{
	wb_put(b, &c, 1);
}

static void wb_puts(wb_t *b, const char *s)
{
	if (s)
		wb_put(b, s, (unsigned)strlen(s));
}

static void wb_putu(wb_t *b, unsigned v)
{
	char tmp[16];

	sprintf(tmp, "%u", v);
	wb_puts(b, tmp);
}

static void wb_puti(wb_t *b, int v)
{
	char tmp[16];

	sprintf(tmp, "%d", v);
	wb_puts(b, tmp);
}

/* ---- transport ------------------------------------------------------- */

static int tr_send(const void *d, unsigned n)
{
	return s_tr == TR_TLS ? mmb_net_tls_send(d, n) : mmb_net_tcp_send(d, n);
}

static int tr_recv(void *d, unsigned n)
{
	return s_tr == TR_TLS ? mmb_net_tls_recv(d, n) : mmb_net_tcp_recv(d, n);
}

static void tr_close(void)
{
	if (s_tr == TR_TLS)
		mmb_net_tls_close();
	else if (s_tr == TR_TCP)
		mmb_net_tcp_close();
	if (s_tcp_claimed)
	{
		mmb_tcp_release();
		s_tcp_claimed = 0;
	}
	s_tr = TR_NONE;
}

static void web_fail(const char *msg)
{
	tr_close();
	mmb_error(msg);
}

static void web_failf(const char *prefix, const char *detail)
{
	char msg[192];

	if (!detail || !detail[0])
		detail = "failed";
	sprintf(msg, "%s%s", prefix, detail);
	web_fail(msg);
}

static void web_open(const char *host, int port, int tls, unsigned tmo)
{
	unsigned start, last;

	tr_close();
	if (!mmb_net_available())
		web_fail("?WEB: network not available");
	if (tls)
	{
		if (!mmb_net_tls_available())
			web_fail("?WEB: TLS unavailable on this platform");
		s_tr = TR_TLS;
		if (mmb_net_tls_open(host, port, (int)tmo) != 0)
			web_failf("?WEB: ", mmb_net_tls_errmsg());
		return;
	}
	if (mmb_tcp_claim() != 0)
		mmb_tcp_in_use();
	s_tcp_claimed = 1;
	s_tr = TR_TCP;
	if (mmb_net_tcp_begin(host, port) != 0)
		web_failf("?WEB: ", mmb_net_tcp_errmsg());
	start = mmb_now_ms();
	last = start;
	for (;;)
	{
		int st = mmb_net_tcp_status();

		if (st == 1)
			return;
		if (st < 0)
			web_failf("?WEB: ", mmb_net_tcp_errmsg());
		if (mmb_now_ms() - start > tmo)
			web_fail("?WEB: connect timeout");
		if (mmb_now_ms() - last >= 10)
		{
			last = mmb_now_ms();
			mmb_check_break();
			mmb_net_yield();
		}
	}
}

static void web_send_all(const void *d, unsigned n, unsigned tmo)
{
	const char *p = (const char *)d;
	unsigned left = n;
	unsigned start = mmb_now_ms();
	unsigned last = start;

	while (left)
	{
		int w = tr_send(p, left);

		if (w < 0)
			web_fail("?WEB: send failed");
		if (w > 0)
		{
			p += w;
			left -= (unsigned)w;
			last = mmb_now_ms();
			continue;
		}
		if (mmb_now_ms() - last > tmo)
			web_fail("?WEB: send timeout");
		mmb_check_break();
		mmb_net_yield();
	}
}

static int web_recv_wait(void *buf, unsigned cap, unsigned tmo, int *closed)
{
	unsigned start = mmb_now_ms();
	unsigned last = start;

	for (;;)
	{
		int n = tr_recv(buf, cap);

		if (n != 0)
		{
			*closed = n < 0;
			return n;
		}
		if (mmb_now_ms() - last > tmo)
			return 0;
		if (mmb_now_ms() - start > tmo + 1000u)
			return 0;
		mmb_check_break();
		mmb_net_yield();
	}
}

/* ---- small text helpers --------------------------------------------- */

static int ci_eq(const char *a, const char *b)
{
	return mmb_keyword_eq(a, b);
}

static int ci_eq_n(const char *a, const char *b, int n)
{
	int i;

	for (i = 0; i < n; i++)
	{
		char ca = a[i], cb = b[i];

		if (ca >= 'a' && ca <= 'z') ca = (char)(ca - 32);
		if (cb >= 'a' && cb <= 'z') cb = (char)(cb - 32);
		if (ca != cb)
			return 0;
	}
	return 1;
}

static void trim_span(const char **a, const char **b)
{
	while (*a < *b && (**a == ' ' || **a == '\t'))
		(*a)++;
	while (*b > *a && ((*b)[-1] == ' ' || (*b)[-1] == '\t' ||
			   (*b)[-1] == '\r'))
		(*b)--;
}

/* Look up a response header in the stored raw header block. Returns 1 and a
 * NUL-terminated copy in out on a hit. */
static int header_lookup(const char *hdr, const char *name, char *out, int outsz)
{
	const char *p = hdr;
	int nl = (int)strlen(name);

	out[0] = 0;
	if (!hdr)
		return 0;
	while (*p)
	{
		const char *e = p;
		const char *colon;
		const char *a, *b, *v, *ve;

		while (*e && *e != '\n')
			e++;
		colon = p;
		while (colon < e && *colon != ':')
			colon++;
		if (colon < e)
		{
			a = p;
			b = colon;
			trim_span(&a, &b);
			if ((int)(b - a) == nl && ci_eq_n(a, name, nl))
			{
				int len;

				v = colon + 1;
				ve = e;
				trim_span(&v, &ve);
				len = (int)(ve - v);
				if (len > outsz - 1)
					len = outsz - 1;
				if (len > 0)
					memcpy(out, v, (unsigned)len);
				out[len] = 0;
				return 1;
			}
		}
		p = (*e == '\n') ? e + 1 : e;
	}
	return 0;
}

static void web_store_headers(const char *p, unsigned n)
{
	if (s_resp_headers)
	{
		mmb_str_free(s_resp_headers);
		s_resp_headers = 0;
		s_resp_headers_len = 0;
	}
	s_resp_headers = mmb_str_alloc((int)n);
	if (!s_resp_headers)
		return;
	memcpy(s_resp_headers, p, n);
	s_resp_headers[n] = 0;
	s_resp_headers_len = n;
}

static int parse_status(const char *hdr)
{
	const char *s = hdr;
	int code = 0;

	if (!s)
		return 0;
	while (*s && *s != ' ')
		s++;
	while (*s == ' ')
		s++;
	while (*s >= '0' && *s <= '9')
	{
		code = code * 10 + (*s - '0');
		s++;
	}
	return code;
}

/* ---- URL ------------------------------------------------------------- */

typedef struct {
	int tls;
	char host[WEB_URL_HOST_MAX];
	int port;
	char path[WEB_URL_PATH_MAX];
} web_url;

static int parse_port(const char *a, const char *b, int defport)
{
	int p = 0;

	if (a == b)
		return defport;
	while (a < b)
	{
		if (*a < '0' || *a > '9')
			return -1;
		p = p * 10 + (*a - '0');
		a++;
	}
	if (p < 1 || p > 65535)
		return -1;
	return p;
}

static int url_parse(const char *u, web_url *o)
{
	const char *p = u;
	const char *auth, *ae, *pa;
	int defport;

	memset(o, 0, sizeof(*o));
	if (!u)
		return -1;
	if (!strncmp(u, "http://", 7))
	{
		o->tls = 0;
		p += 7;
	}
	else if (!strncmp(u, "https://", 8))
	{
		o->tls = 1;
		p += 8;
	}
	else
		return -1;
	defport = o->tls ? 443 : 80;

	auth = p;
	ae = p;
	while (*ae && *ae != '/' && *ae != '?' && *ae != '#')
		ae++;
	if (ae == auth)
		return -1;
	{
		const char *q;

		for (q = auth; q < ae; q++)
			if (*q == '@')
				return -1; /* userinfo is not supported */
	}

	pa = auth;
	if (*pa == '[')
	{
		const char *rb = pa;

		while (rb < ae && *rb != ']')
			rb++;
		if (rb >= ae)
			return -1;
		if ((int)(rb - pa + 1) >= WEB_URL_HOST_MAX)
			return -1;
		memcpy(o->host, pa + 1, (size_t)(rb - pa - 1));
		o->host[rb - pa - 1] = 0;
		pa = rb + 1;
		if (pa < ae)
		{
			if (*pa != ':')
				return -1;
			o->port = parse_port(pa + 1, ae, -1);
			if (o->port < 0)
				return -1;
		}
		else
			o->port = defport;
	}
	else
	{
		const char *colon = 0;
		const char *q;

		for (q = pa; q < ae; q++)
			if (*q == ':')
				colon = q;
		if (colon)
		{
			if ((int)(colon - pa) >= WEB_URL_HOST_MAX)
				return -1;
			memcpy(o->host, pa, (size_t)(colon - pa));
			o->host[colon - pa] = 0;
			o->port = parse_port(colon + 1, ae, -1);
			if (o->port < 0)
				return -1;
		}
		else
		{
			if ((int)(ae - pa) >= WEB_URL_HOST_MAX)
				return -1;
			memcpy(o->host, pa, (size_t)(ae - pa));
			o->host[ae - pa] = 0;
			o->port = defport;
		}
	}
	if (!o->host[0])
		return -1;

	pa = ae;
	/* Copy the request target, dropping any fragment. A bare query starts
	 * with '/', so http://host?q becomes /?q rather than ?q. */
	{
		unsigned k = 0;
		const char *q;

		if (*pa == 0 || *pa == '#')
		{
			strcpy(o->path, "/");
			return 0;
		}
		if (*pa != '/')
			o->path[k++] = '/';
		for (q = pa; *q && *q != '#'; q++)
		{
			if (k + 1 >= WEB_URL_PATH_MAX)
				return -1;
			o->path[k++] = *q;
		}
		o->path[k] = 0;
	}
	return 0;
}

static int url_resolve(const web_url *base, const char *loc, web_url *out)
{
	char joined[WEB_URL_PATH_MAX];

	if (!loc || !loc[0])
		return -1;
	if (!strncmp(loc, "http://", 7) || !strncmp(loc, "https://", 8))
		return url_parse(loc, out);
	if (loc[0] == '/')
	{
		*out = *base;
		if ((int)strlen(loc) >= WEB_URL_PATH_MAX)
			return -1;
		strcpy(out->path, loc);
		return 0;
	}
	*out = *base;
	{
		const char *slash = 0;
		const char *q;
		int dirlen, ll;

		for (q = base->path; *q; q++)
			if (*q == '/')
				slash = q;
		dirlen = slash ? (int)(slash - base->path) + 1 : 1;
		ll = (int)strlen(loc);

		if (dirlen + ll >= WEB_URL_PATH_MAX)
			return -1;
		memcpy(joined, base->path, (size_t)dirlen);
		memcpy(joined + dirlen, loc, (size_t)ll);
		joined[dirlen + ll] = 0;
		strcpy(out->path, joined);
	}
	return 0;
}

/* ---- request build/parse -------------------------------------------- */

static void append_host(wb_t *req, const web_url *u)
{
	int defport = u->tls ? 443 : 80;

	wb_puts(req, "Host: ");
	wb_puts(req, u->host);
	if (u->port != defport)
	{
		wb_putc(req, ':');
		wb_puti(req, u->port);
	}
	wb_puts(req, "\r\n");
}

static int header_element(const char *el, const char **name, int *nlen,
			  const char **val, int *vlen)
{
	const char *colon = strchr(el, ':');
	const char *a, *b, *v, *ve;
	int i;

	if (!colon || colon == el)
		return -1;
	a = el;
	b = colon;
	trim_span(&a, &b);
	if (a == b)
		return -1;
	for (i = 0; a + i < b; i++)
	{
		unsigned char c = (unsigned char)a[i];
		if (c <= 32 || c >= 127 || c == ':')
			return -1;
	}
	v = colon + 1;
	ve = el + strlen(el);
	trim_span(&v, &ve);
	*name = a;
	*nlen = (int)(b - a);
	*val = v;
	*vlen = (int)(ve - v);
	return 0;
}

static void build_request(wb_t *req, const web_url *u, const char *method,
			  const char *body, unsigned blen, int has_body,
			  int body_json, mmb_arrview *hdr, int have_hdr,
			  int drop_auth)
{
	int i;
	int c_host = 0, c_ua = 0, c_accept = 0, c_conn = 0, c_ct = 0;

	if (have_hdr)
	{
		for (i = 0; i < hdr->count; i++)
		{
			const char *el = hdr->v->data.s[i];
			const char *nm, *vl;
			int nl, vlen;

			if (!el || !*el)
				continue;
			if (strchr(el, '\r') || strchr(el, '\n'))
				mmb_error("?WEB: CR/LF in header");
			if (header_element(el, &nm, &nl, &vl, &vlen) != 0)
				mmb_error("?WEB: bad header");
			if (drop_auth && nl == 13 && ci_eq_n(nm, "Authorization", 13))
				continue;
			if (nl == 4 && ci_eq_n(nm, "Host", 4))
				c_host = 1;
			else if (nl == 10 && ci_eq_n(nm, "User-Agent", 10))
				c_ua = 1;
			else if (nl == 6 && ci_eq_n(nm, "Accept", 6))
				c_accept = 1;
			else if (nl == 10 && ci_eq_n(nm, "Connection", 10))
				c_conn = 1;
			else if (nl == 12 && ci_eq_n(nm, "Content-Type", 12))
				c_ct = 1;
			else if (nl == 14 && ci_eq_n(nm, "Content-Length", 14))
				mmb_error("?WEB: custom Content-Length not allowed");
		}
	}

	wb_puts(req, method);
	wb_putc(req, ' ');
	wb_puts(req, u->path);
	wb_puts(req, " HTTP/1.1\r\n");
	if (!c_host)
		append_host(req, u);
	if (!c_ua)
		wb_puts(req, "User-Agent: mmcore\r\n");
	if (!c_accept)
		wb_puts(req, "Accept: */*\r\n");
	if (!c_conn)
		wb_puts(req, "Connection: close\r\n");
	if (has_body && body_json && !c_ct)
		wb_puts(req, "Content-Type: application/json\r\n");
	if (has_body)
	{
		wb_puts(req, "Content-Length: ");
		wb_putu(req, blen);
		wb_puts(req, "\r\n");
	}
	if (have_hdr)
	{
		for (i = 0; i < hdr->count; i++)
		{
			const char *el = hdr->v->data.s[i];
			const char *nm, *vl;
			int nl, vlen;

			if (!el || !*el)
				continue;
			header_element(el, &nm, &nl, &vl, &vlen);
			if (drop_auth && nl == 13 && ci_eq_n(nm, "Authorization", 13))
				continue;
			wb_put(req, nm, (unsigned)nl);
			wb_puts(req, ": ");
			wb_put(req, vl, (unsigned)vlen);
			wb_puts(req, "\r\n");
		}
	}
	wb_puts(req, "\r\n");
	if (has_body && blen)
		wb_put(req, body, blen);
}

static int find_headers_end(const char *p, unsigned n, unsigned *end)
{
	unsigned i;

	for (i = 0; i + 3 < n; i++)
		if (p[i] == '\r' && p[i + 1] == '\n' &&
		    p[i + 2] == '\r' && p[i + 3] == '\n')
		{
			*end = i + 4;
			return 1;
		}
	for (i = 0; i + 1 < n; i++)
		if (p[i] == '\n' && p[i + 1] == '\n')
		{
			*end = i + 2;
			return 1;
		}
	return 0;
}

static int hexval(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/* Decode a chunked body from p[0..n). Rebuilds out from scratch each call.
 * Returns 0 complete, 1 needs more data, -1 malformed. */
static int dechunk(const char *p, unsigned n, wb_t *out, int *done)
{
	unsigned i = 0;

	out->n = 0;
	*done = 0;
	for (;;)
	{
		unsigned j = i, sz = 0;
		int any = 0, h;

		while (j < n && p[j] != '\r' && p[j] != '\n')
		{
			h = hexval(p[j]);
			if (h < 0)
				return -1;
			if (sz > (WEB_BODY_MAX >> 4))
				return -1;
			sz = sz * 16 + (unsigned)h;
			any = 1;
			j++;
		}
		if (j >= n)
			return 1;
		if (!any)
			return -1;
		if (p[j] == '\r')
		{
			j++;
			if (j >= n)
				return 1;
		}
		if (p[j] != '\n')
			return -1;
		j++;
		if (sz == 0)
		{
			/* trailers: accept a blank line or ignore them */
			if (j + 1 < n && p[j] == '\r' && p[j + 1] == '\n')
			{
				*done = 1;
				return 0;
			}
			if (j < n && p[j] == '\n')
			{
				*done = 1;
				return 0;
			}
			return 1;
		}
		if (j + sz > n)
			return 1;
		wb_put(out, p + j, sz);
		if (out->n > WEB_BODY_MAX)
			return -1;
		j += sz;
		if (j >= n)
			return 1;
		if (p[j] == '\r')
		{
			j++;
			if (j >= n)
				return 1;
		}
		if (p[j] != '\n')
			return -1;
		i = j + 1;
	}
}

static void read_response(wb_t *body)
{
	char tmp[WEB_RX_TMP];
	wb_t raw;
	unsigned header_end = 0;
	int have_headers = 0;
	unsigned tmo = s_timeout_ms;
	char value[64];
	int chunked, have_len;
	long clen = 0;

	wb_init(&raw);
	wb_init(body);

	/* headers */
	while (!have_headers)
	{
		int closed = 0;
		int n;

		n = web_recv_wait(tmp, sizeof(tmp), tmo, &closed);
		if (n > 0)
		{
			wb_put(&raw, tmp, (unsigned)n);
			if (raw.n > WEB_HDR_MAX)
			{
				wb_free(&raw);
				wb_free(body);
				web_fail("?WEB: response headers too large");
			}
			if (find_headers_end(raw.p, raw.n, &header_end))
				have_headers = 1;
		}
		else if (closed)
		{
			wb_free(&raw);
			wb_free(body);
			web_fail("?WEB: connection closed before response");
		}
		else
		{
			wb_free(&raw);
			wb_free(body);
			web_fail("?WEB: response timeout");
		}
	}

	s_status = parse_status(raw.p);
	web_store_headers(raw.p, header_end);
	chunked = header_lookup(s_resp_headers, "Transfer-Encoding", value,
				sizeof(value)) &&
		  strstr(value, "chunked") != 0;
	have_len = header_lookup(s_resp_headers, "Content-Length", value,
				 sizeof(value));
	if (have_len)
	{
		const char *q = value;
		int ok = 0;

		clen = 0;
		while (*q >= '0' && *q <= '9')
		{
			clen = clen * 10 + (*q - '0');
			q++;
			ok = 1;
			if (clen > (long)WEB_BODY_MAX)
			{
				wb_free(&raw);
				wb_free(body);
				web_fail("?WEB: response body too large");
			}
		}
		have_len = ok;
	}

	if (chunked)
	{
		for (;;)
		{
			int done = 0;
			int r = dechunk(raw.p + header_end, raw.n - header_end,
					body, &done);
			int closed = 0;
			int n;

			if (r < 0)
			{
				wb_free(&raw);
				wb_free(body);
				web_fail("?WEB: malformed chunked body");
			}
			if (r == 0 && done)
				break;
			n = web_recv_wait(tmp, sizeof(tmp), tmo, &closed);
			if (n > 0)
				wb_put(&raw, tmp, (unsigned)n);
			else if (closed)
			{
				/* incomplete chunked stream */
				wb_free(&raw);
				wb_free(body);
				web_fail("?WEB: incomplete chunked body");
			}
			else
			{
				wb_free(&raw);
				wb_free(body);
				web_fail("?WEB: response timeout");
			}
			if (raw.n > WEB_BODY_MAX + WEB_HDR_MAX)
			{
				wb_free(&raw);
				wb_free(body);
				web_fail("?WEB: response body too large");
			}
		}
	}
	else if (have_len)
	{
		unsigned have = raw.n - header_end;

		while (have < (unsigned)clen)
		{
			int closed = 0;
			int n = web_recv_wait(tmp, sizeof(tmp), tmo, &closed);

			if (n > 0)
			{
				wb_put(&raw, tmp, (unsigned)n);
				have = raw.n - header_end;
				if (raw.n > WEB_BODY_MAX + WEB_HDR_MAX)
				{
					wb_free(&raw);
					wb_free(body);
					web_fail("?WEB: response body too large");
				}
			}
			else if (closed)
				break;
			else
			{
				wb_free(&raw);
				wb_free(body);
				web_fail("?WEB: response timeout");
			}
		}
		have = raw.n - header_end;
		if (have > (unsigned)clen)
			have = (unsigned)clen;
		wb_put(body, raw.p + header_end, have);
	}
	else
	{
		int closed = 0;
		int n;

		wb_put(body, raw.p + header_end, raw.n - header_end);
		if (body->n > WEB_BODY_MAX)
		{
			wb_free(&raw);
			wb_free(body);
			web_fail("?WEB: response body too large");
		}
		while (!closed)
		{
			n = web_recv_wait(tmp, sizeof(tmp), tmo, &closed);
			if (n > 0)
			{
				wb_put(body, tmp, (unsigned)n);
				if (body->n > WEB_BODY_MAX)
				{
					wb_free(&raw);
					wb_free(body);
					web_fail("?WEB: response body too large");
				}
			}
			else if (n == 0)
				break; /* idle timeout ends a close-delimited body */
		}
	}
	wb_free(&raw);
}

/* Perform one logical request (following redirects). Returns the final body
 * and leaves the final status/headers in s_status/s_resp_headers. */
static void http_request(wb_t *body, const char *method0, const web_url *u0,
			 const char *payload, unsigned plen, int has_payload,
			 int payload_json, mmb_arrview *hdr, int have_hdr)
{
	char method[8];
	web_url u = *u0;
	const char *p = payload;
	unsigned pn = plen;
	int has = has_payload;
	int isjson = payload_json;
	int redirects = 0;
	int cross = 0;

	strncpy(method, method0, sizeof(method) - 1);
	method[sizeof(method) - 1] = 0;

	for (;;)
	{
		wb_t req;
		char loc[WEB_URL_PATH_MAX];
		int status;

		wb_init(&req);
		build_request(&req, &u, method, p, pn, has, isjson, hdr,
			      have_hdr, cross);
		web_open(u.host, u.port, u.tls, s_timeout_ms);
		web_send_all(req.p, req.n, s_timeout_ms);
		wb_free(&req);
		read_response(body);
		status = s_status;
		tr_close();

		if ((status == 301 || status == 302 || status == 303 ||
		     status == 307 || status == 308) &&
		    redirects < WEB_REDIR_MAX &&
		    header_lookup(s_resp_headers, "Location", loc, sizeof(loc)))
		{
			web_url nu;

			if (url_resolve(&u, loc, &nu) != 0)
				return; /* leave the 3xx response visible */
			if (status == 303 ||
			    ((status == 301 || status == 302) &&
			     strcmp(method, "POST") == 0))
			{
				strcpy(method, "GET");
				p = 0;
				pn = 0;
				has = 0;
				isjson = 0;
			}
			cross = (nu.tls != u.tls) ||
				strcmp(nu.host, u.host) != 0 ||
				nu.port != u.port;
			u = nu;
			redirects++;
			wb_free(body);
			wb_init(body);
			continue;
		}
		return;
	}
}

/* ---- high-level expression ------------------------------------------ */

static mmb_val web_function(int text_form)
{
	char method[8];
	mmb_val url;
	mmb_val payload;
	int has_payload = 0, payload_json = 0;
	mmb_arrview hdr;
	int have_hdr = 0;
	int sid = -1;
	const char *bodyp = 0;
	unsigned blen = 0;
	web_url u;
	wb_t body;

	mmb_expect('(');
	mmb_skip_sp();
	if (!mmb_match("HTTP"))
		mmb_syntax();
	mmb_skip_sp();
	if (mmb_match("GET"))
		strcpy(method, "GET");
	else if (mmb_match("DELETE"))
		strcpy(method, "DELETE");
	else if (mmb_match("POST"))
		strcpy(method, "POST");
	else if (mmb_match("PUT"))
		strcpy(method, "PUT");
	else if (mmb_match("PATCH"))
		strcpy(method, "PATCH");
	else
		mmb_syntax();
	mmb_skip_sp();
	url = mmb_expr();
	if (url.type != T_STR)
		mmb_error("?TYPE MISMATCH");
	mmb_skip_sp();
	if (*G.p == ',')
	{
		G.p++;
		mmb_skip_sp();
		payload = mmb_expr();
		if (payload.type == T_STR)
		{
			has_payload = 1;
			bodyp = payload.s ? payload.s : "";
			blen = (unsigned)strlen(bodyp);
		}
		else if (payload.type == T_STRUCT)
		{
			mmb_val js = mmb_json_stringify(payload);

			has_payload = 1;
			payload_json = 1;
			bodyp = js.s ? js.s : "";
			blen = (unsigned)strlen(bodyp);
		}
		else
			mmb_error("?TYPE MISMATCH");
		mmb_skip_sp();
	}
	if (mmb_match("HEADERS"))
	{
		if (!mmb_try_parse_arrview(&hdr))
			mmb_syntax();
		if (hdr.moff >= 0 || hdr.v->type != T_STR)
			mmb_error("?TYPE MISMATCH");
		have_hdr = 1;
		mmb_skip_sp();
	}
	if (mmb_match("AS"))
	{
		char tn[MMB_MAX_NAME];

		if (text_form)
			mmb_error("?WEB: WEB$ returns text, use WEB(... AS Type)");
		mmb_ident(tn, sizeof(tn));
		mmb_type_suffix(tn);
		sid = mmb_struct_lookup(tn);
		if (sid < 0)
			mmb_error("?UNKNOWN TYPE");
		mmb_skip_sp();
	}
	mmb_expect(')');
	if (!text_form && sid < 0)
		mmb_error("?WEB: WEB() needs AS Type");

	if (url_parse(url.s, &u) != 0)
		mmb_error("?WEB: URL must start with http:// or https://");

	wb_init(&body);
	http_request(&body, method, &u, bodyp, blen, has_payload, payload_json,
		     have_hdr ? &hdr : 0, have_hdr);

	if (text_form)
	{
		char *out;

		out = mmb_tmp_alloc((int)body.n + 1);
		if (body.n)
			memcpy(out, body.p, body.n);
		out[body.n] = 0;
		wb_free(&body);
		return mmb_arena_val(out);
	}
	else
	{
		int sz = G.sdef[sid].total;
		unsigned char *blob;
		char *js;
		int rc;

		js = mmb_tmp_alloc((int)body.n + 1);
		if (body.n)
			memcpy(js, body.p, body.n);
		js[body.n] = 0;
		wb_free(&body);

		blob = (unsigned char *)mmb_tmp_alloc(sz);
		memset(blob, 0, (size_t)sz);
		rc = mmb_json_parse_struct(sid, js, blob);
		if (rc == -1)
			mmb_error("?JSON");
		if (rc == -2)
			mmb_error("?JSON: expected object");
		{
			mmb_val out;
			memset(&out, 0, sizeof(out));
			out.type = T_STRUCT;
			out.struct_idx = sid;
			out.blob = blob;
			return out;
		}
	}
}

mmb_val mmb_web_fn(int text_form)
{
	return web_function(text_form);
}

int mmb_web_status_get(void)
{
	return s_status;
}

mmb_val mmb_web_header_val(const char *name)
{
	char value[512];

	if (!name || !s_resp_headers)
		return mmb_str_val("");
	if (!header_lookup(s_resp_headers, name, value, sizeof(value)))
		return mmb_str_val("");
	return mmb_str_val(value);
}

/* ---- integer array buffers ------------------------------------------ */

static int parse_int_buffer(mmb_arrview *a)
{
	if (!mmb_try_parse_arrview(a))
		mmb_syntax();
	if (a->moff >= 0 || a->v->type != T_INT)
		mmb_error("?TYPE MISMATCH");
	if (a->count < 1)
		mmb_error("?WEB: buffer too small");
	return 0;
}

static void buf_store(mmb_arrview a, const char *src, unsigned n)
{
	unsigned cap = (unsigned)(a.count - 1);
	unsigned k;

	if (n > cap)
		n = cap;
	a.v->data.i[0] = (int64_t)n;
	for (k = 0; k < n; k++)
		a.v->data.i[1 + k] = (int64_t)(unsigned char)src[k];
}

static unsigned buf_load(mmb_arrview a, char *dst, unsigned cap)
{
	int64_t len = a.v->data.i[0];
	unsigned k;

	if (len < 0)
		len = 0;
	if (len > (int64_t)(a.count - 1))
		len = (int64_t)(a.count - 1);
	if (len > (int64_t)cap)
		len = (int64_t)cap;
	for (k = 0; k < (unsigned)len; k++)
		dst[k] = (char)(unsigned char)a.v->data.i[1 + k];
	return (unsigned)len;
}

/* ---- low-level client commands -------------------------------------- */

static void cmd_client_open(int tls)
{
	mmb_val addr;
	int port;
	unsigned tmo = s_timeout_ms;

	addr = mmb_expr();
	if (addr.type != T_STR)
		mmb_error("?TYPE MISMATCH");
	mmb_skip_sp();
	mmb_expect(',');
	port = (int)mmb_as_int(mmb_expr());
	mmb_skip_sp();
	if (*G.p == ',')
	{
		unsigned long t;

		G.p++;
		t = (unsigned long)mmb_as_int(mmb_expr());
		if (t < 1)
			t = 1;
		if (t > WEB_MAX_TIMEOUT_MS)
			t = WEB_MAX_TIMEOUT_MS;
		tmo = (unsigned)t;
	}
	if (port < 1 || port > 65535)
		mmb_error("?WEB: bad port");
	web_open(addr.s ? addr.s : "", port, tls, tmo);
}

static void cmd_client_request(void)
{
	mmb_val request;
	mmb_arrview buf;
	unsigned tmo = s_timeout_ms;
	wb_t got;
	char tmp[WEB_RX_TMP];

	request = mmb_expr();
	if (request.type != T_STR)
		mmb_error("?TYPE MISMATCH");
	mmb_skip_sp();
	mmb_expect(',');
	parse_int_buffer(&buf);
	mmb_skip_sp();
	if (*G.p == ',')
	{
		unsigned long t;

		G.p++;
		t = (unsigned long)mmb_as_int(mmb_expr());
		if (t < 1)
			t = 1;
		if (t > WEB_MAX_TIMEOUT_MS)
			t = WEB_MAX_TIMEOUT_MS;
		tmo = (unsigned)t;
	}
	if (s_tr == TR_NONE)
		mmb_error("?WEB: no connection");
	web_send_all(request.s ? request.s : "",
		     request.s ? (unsigned)strlen(request.s) : 0, tmo);

	wb_init(&got);
	for (;;)
	{
		int closed = 0;
		int n = web_recv_wait(tmp, sizeof(tmp), tmo, &closed);

		if (n < 0)
			break;
		if (n == 0)
			break;
		wb_put(&got, tmp, (unsigned)n);
		if (got.n >= WEB_BODY_MAX)
			break;
	}
	buf_store(buf, got.p ? got.p : "", got.n);
	wb_free(&got);
}

static void cmd_client_write(void)
{
	mmb_arrview buf;
	char tmp[WEB_RX_TMP];
	unsigned n, tmo = s_timeout_ms;

	parse_int_buffer(&buf);
	if (*G.p == ',')
	{
		unsigned long t;

		G.p++;
		t = (unsigned long)mmb_as_int(mmb_expr());
		if (t < 1)
			t = 1;
		if (t > WEB_MAX_TIMEOUT_MS)
			t = WEB_MAX_TIMEOUT_MS;
		tmo = (unsigned)t;
	}
	if (s_tr == TR_NONE)
		mmb_error("?WEB: no connection");
	n = buf_load(buf, tmp, sizeof(tmp));
	web_send_all(tmp, n, tmo);
}

static void cmd_client_read(void)
{
	mmb_arrview buf;
	char tmp[WEB_RX_TMP];
	unsigned tmo = s_timeout_ms;
	int closed = 0;
	int n;

	parse_int_buffer(&buf);
	if (*G.p == ',')
	{
		unsigned long t;

		G.p++;
		t = (unsigned long)mmb_as_int(mmb_expr());
		if (t < 1)
			t = 1;
		if (t > WEB_MAX_TIMEOUT_MS)
			t = WEB_MAX_TIMEOUT_MS;
		tmo = (unsigned)t;
	}
	if (s_tr == TR_NONE)
		mmb_error("?WEB: no connection");
	n = web_recv_wait(tmp, sizeof(tmp), tmo, &closed);
	if (n < 0)
		n = 0;
	buf_store(buf, tmp, (unsigned)n);
}

/* ---- server commands ------------------------------------------------- */

static void server_send_all(const void *d, unsigned n, unsigned tmo)
{
	const char *p = (const char *)d;
	unsigned left = n;
	unsigned start = mmb_now_ms();
	unsigned last = start;

	while (left)
	{
		int w = mmb_net_srv_send(s_sconn, p, left);

		if (w < 0)
			mmb_error("?WEB: server send failed");
		if (w > 0)
		{
			p += w;
			left -= (unsigned)w;
			last = mmb_now_ms();
			continue;
		}
		if (mmb_now_ms() - last > tmo)
			mmb_error("?WEB: server send timeout");
		mmb_check_break();
		mmb_net_yield();
	}
}

static void cmd_server_read(void)
{
	mmb_arrview buf;
	char tmp[WEB_RX_TMP];
	unsigned tmo = s_timeout_ms;
	unsigned last;
	int n = 0;

	parse_int_buffer(&buf);
	if (*G.p == ',')
	{
		unsigned long t;

		G.p++;
		t = (unsigned long)mmb_as_int(mmb_expr());
		if (t < 1)
			t = 1;
		if (t > WEB_MAX_TIMEOUT_MS)
			t = WEB_MAX_TIMEOUT_MS;
		tmo = (unsigned)t;
	}
	if (s_sconn < 0)
		mmb_error("?WEB: no server connection");
	last = mmb_now_ms();
	for (;;)
	{
		n = mmb_net_srv_recv(s_sconn, tmp, sizeof(tmp));
		if (n > 0)
			break;
		if (n == -1 || n == -2)
		{
			mmb_net_srv_close(s_sconn);
			s_sconn = -1;
			n = 0;
			break;
		}
		if (mmb_now_ms() - last > tmo)
		{
			n = 0;
			break;
		}
		mmb_check_break();
		mmb_net_yield();
	}
	buf_store(buf, tmp, n > 0 ? (unsigned)n : 0);
}

static void cmd_server_send(void)
{
	mmb_arrview buf;
	char tmp[WEB_RX_TMP];
	unsigned n, tmo = s_timeout_ms;

	parse_int_buffer(&buf);
	if (*G.p == ',')
	{
		unsigned long t;

		G.p++;
		t = (unsigned long)mmb_as_int(mmb_expr());
		if (t < 1)
			t = 1;
		if (t > WEB_MAX_TIMEOUT_MS)
			t = WEB_MAX_TIMEOUT_MS;
		tmo = (unsigned)t;
	}
	if (s_sconn < 0)
		mmb_error("?WEB: no server connection");
	n = buf_load(buf, tmp, sizeof(tmp));
	server_send_all(tmp, n, tmo);
}

static const char *reason_phrase(int code)
{
	switch (code)
	{
	case 200: return "OK";
	case 201: return "Created";
	case 204: return "No Content";
	case 301: return "Moved Permanently";
	case 302: return "Found";
	case 304: return "Not Modified";
	case 400: return "Bad Request";
	case 401: return "Unauthorized";
	case 403: return "Forbidden";
	case 404: return "Not Found";
	case 405: return "Method Not Allowed";
	case 500: return "Internal Server Error";
	case 503: return "Service Unavailable";
	default: return "Unknown";
	}
}

static void send_status(int code, const char *ctype, unsigned blen)
{
	char line[64];
	wb_t out;

	wb_init(&out);
	sprintf(line, "HTTP/1.1 %d ", code);
	wb_puts(&out, line);
	wb_puts(&out, reason_phrase(code));
	wb_puts(&out, "\r\n");
	wb_puts(&out, "Content-Type: ");
	wb_puts(&out, ctype);
	wb_puts(&out, "\r\n");
	wb_puts(&out, "Content-Length: ");
	wb_putu(&out, blen);
	wb_puts(&out, "\r\nConnection: close\r\n\r\n");
	server_send_all(out.p, out.n, s_timeout_ms);
	wb_free(&out);
}

static char *read_file(const char *path, unsigned *outn)
{
	int sz = mmb_vfs_size(path);
	char *buf;
	unsigned got = 0;

	if (sz < 0)
		mmb_error("?FILE");
	if ((unsigned)sz > WEB_BODY_MAX)
		mmb_error("?WEB: file too large");
	buf = mmb_str_alloc(sz);
	if (!buf)
		mmb_error("?OUT OF MEMORY");
	if (mmb_vfs_read(path, buf, (unsigned)sz, &got) != 0)
	{
		mmb_str_free(buf);
		mmb_error("?FILE");
	}
	buf[got] = 0;
	*outn = got;
	return buf;
}

static void render_append_value(wb_t *out, mmb_val v)
{
	char tmp[64];

	if (v.type == T_STR)
		wb_puts(out, v.s ? v.s : "");
	else if (v.type == T_INT)
	{
		sprintf(tmp, "%lld", (long long)v.i);
		wb_puts(out, tmp);
	}
	else if (v.type == T_NUM)
	{
		sprintf(tmp, "%g", v.f);
		wb_puts(out, tmp);
	}
	else
		wb_puts(out, "[STRUCT]");
}

static void render_expr(wb_t *out, const char *src, unsigned len)
{
	char buf[MMB_LINE_LEN];
	char tok[MMB_LINE_LEN];
	const char *save;
	const char *a = src;
	const char *b = src + len;
	mmb_val v;

	trim_span(&a, &b);
	if ((unsigned)(b - a) >= sizeof(buf))
		mmb_error("?WEB: PAGE expression too long");
	memcpy(buf, a, (size_t)(b - a));
	buf[b - a] = 0;
	save = G.p;
	mmb_tokenize_text(buf, tok, (int)sizeof(tok));
	G.p = tok;
	v = mmb_expr();
	G.p = save;
	render_append_value(out, v);
}

static char *render_page(const char *src, unsigned n, unsigned *outn)
{
	wb_t out;
	unsigned i = 0;

	wb_init(&out);
	while (i < n)
	{
		if (i + 1 < n && src[i] == '<' && src[i + 1] == '%')
		{
			unsigned j = i + 2;

			while (j + 1 < n && !(src[j] == '%' && src[j + 1] == '>'))
				j++;
			if (j + 1 < n)
			{
				render_expr(&out, src + i + 2, j - (i + 2));
				i = j + 2;
				continue;
			}
			wb_putc(&out, src[i++]);
			continue;
		}
		if (i + 1 < n && src[i] == '{' && src[i + 1] == '{')
		{
			unsigned j = i + 2;

			while (j + 1 < n && !(src[j] == '}' && src[j + 1] == '}'))
				j++;
			if (j + 1 < n)
			{
				render_expr(&out, src + i + 2, j - (i + 2));
				i = j + 2;
				continue;
			}
			wb_putc(&out, src[i++]);
			continue;
		}
		wb_putc(&out, src[i++]);
	}
	if (!out.p)
		wb_put(&out, "", 0);
	*outn = out.n;
	return out.p;
}

/* ---- command --------------------------------------------------------- */

void mmb_cmd_web(void)
{
	if (mmb_match("OPEN"))
	{
		if (mmb_match("TLS"))
		{
			if (!mmb_match("CLIENT"))
				mmb_syntax();
			cmd_client_open(1);
			return;
		}
		if (!mmb_match("TCP"))
			mmb_syntax();
		if (!mmb_match("CLIENT"))
			mmb_syntax();
		cmd_client_open(0);
		return;
	}
	if (mmb_match("CLOSE"))
	{
		if (!mmb_match("TCP"))
			mmb_syntax();
		if (!mmb_match("CLIENT"))
			mmb_syntax();
		tr_close();
		return;
	}
	if (mmb_match("TLS"))
	{
		if (mmb_match("CA"))
		{
			mmb_val f;
			char *buf;
			unsigned n = 0, got = 0;
			int sz;

			f = mmb_expr();
			if (f.type != T_STR)
				mmb_error("?TYPE MISMATCH");
			sz = mmb_vfs_size(f.s);
			if (sz < 0)
				mmb_error("?FILE");
			buf = mmb_str_alloc(sz);
			if (!buf)
				mmb_error("?OUT OF MEMORY");
			if (sz > 0 &&
			    mmb_vfs_read(f.s, buf, (unsigned)sz, &got) != 0)
			{
				mmb_str_free(buf);
				mmb_error("?FILE");
			}
			n = got;
			if (mmb_net_tls_set_ca_pem(buf, (int)n) != 0)
			{
				mmb_str_free(buf);
				mmb_error("?WEB: bad CA file");
			}
			mmb_str_free(buf);
			return;
		}
		if (mmb_match("NOVERIFY"))
		{
			mmb_net_tls_set_verify(0);
			return;
		}
		if (mmb_match("VERIFY"))
		{
			mmb_net_tls_set_verify(1);
			return;
		}
		mmb_syntax();
	}
	if (mmb_match("TIMEOUT"))
	{
		long t = (long)mmb_as_int(mmb_expr());

		if (t < 1)
			t = 1;
		if (t > (long)WEB_MAX_TIMEOUT_MS)
			t = (long)WEB_MAX_TIMEOUT_MS;
		s_timeout_ms = (unsigned)t;
		return;
	}
	if (mmb_match("TCP"))
	{
		if (mmb_match("CLIENT"))
		{
			if (mmb_match("REQUEST"))
				cmd_client_request();
			else if (mmb_match("WRITE"))
				cmd_client_write();
			else if (mmb_match("READ"))
				cmd_client_read();
			else
				mmb_syntax();
			return;
		}
		if (mmb_match("PORT"))
		{
			int port = (int)mmb_as_int(mmb_expr());

			if (port < 1 || port > 65535)
				mmb_error("?WEB: bad port");
			if (s_sconn >= 0)
			{
				mmb_net_srv_close(s_sconn);
				s_sconn = -1;
			}
			if (s_lsn >= 0)
			{
				mmb_net_srv_listen_close(s_lsn);
				s_lsn = -1;
			}
			s_lsn = mmb_net_srv_listen(port);
			if (s_lsn < 0)
				mmb_error("?WEB: cannot listen");
			return;
		}
		if (mmb_match("INTERRUPT"))
		{
			if (s_lsn < 0)
				mmb_error("?WEB: no server port");
			if (s_sconn < 0)
				s_sconn = mmb_net_srv_accept(s_lsn);
			return;
		}
		if (mmb_match("READ"))
		{
			cmd_server_read();
			return;
		}
		if (mmb_match("SEND"))
		{
			cmd_server_send();
			return;
		}
		if (mmb_match("CLOSE"))
		{
			if (s_sconn >= 0)
			{
				mmb_net_srv_close(s_sconn);
				s_sconn = -1;
			}
			return;
		}
		mmb_syntax();
	}
	if (mmb_match("TRANSMIT"))
	{
		char *buf;
		unsigned n = 0;

		if (s_sconn < 0)
			mmb_error("?WEB: no server connection");
		if (mmb_match("CODE"))
		{
			int code;

			(void)mmb_as_int(mmb_expr()); /* cb% handle */
			mmb_skip_sp();
			mmb_expect(',');
			code = (int)mmb_as_int(mmb_expr());
			send_status(code, "text/plain", 0);
			return;
		}
		if (mmb_match("FILE"))
		{
			mmb_val f;
			mmb_val ct;

			(void)mmb_as_int(mmb_expr()); /* cb% handle */
			mmb_skip_sp();
			mmb_expect(',');
			f = mmb_expr();
			if (f.type != T_STR)
				mmb_error("?TYPE MISMATCH");
			mmb_skip_sp();
			mmb_expect(',');
			ct = mmb_expr();
			if (ct.type != T_STR)
				mmb_error("?TYPE MISMATCH");
			buf = read_file(f.s, &n);
			send_status(200,
				    ct.s && ct.s[0] ? ct.s :
						      "application/octet-stream", n);
			server_send_all(buf, n, s_timeout_ms);
			mmb_str_free(buf);
			return;
		}
		if (mmb_match("PAGE"))
		{
			mmb_val f;
			char *rendered;

			(void)mmb_as_int(mmb_expr()); /* cb% handle */
			mmb_skip_sp();
			mmb_expect(',');
			f = mmb_expr();
			if (f.type != T_STR)
				mmb_error("?TYPE MISMATCH");
			mmb_skip_sp();
			if (*G.p == ',')
			{
				G.p++;
				(void)mmb_as_int(mmb_expr()); /* buffersize hint */
			}
			buf = read_file(f.s, &n);
			rendered = render_page(buf, n, &n);
			mmb_str_free(buf);
			send_status(200, "text/html; charset=utf-8", n);
			server_send_all(rendered, n, s_timeout_ms);
			mmb_str_free(rendered);
			return;
		}
		mmb_syntax();
	}
	mmb_syntax();
}

void mmb_web_cleanup(void)
{
	tr_close();
	if (s_sconn >= 0)
	{
		mmb_net_srv_close(s_sconn);
		s_sconn = -1;
	}
	if (s_lsn >= 0)
	{
		mmb_net_srv_listen_close(s_lsn);
		s_lsn = -1;
	}
	if (s_resp_headers)
	{
		mmb_str_free(s_resp_headers);
		s_resp_headers = 0;
		s_resp_headers_len = 0;
	}
	s_status = 0;
}
