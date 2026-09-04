#include "mmb_priv.h"

mmb G;

void mmb_skip_sp(void)
{
	while (*G.p == ' ' || *G.p == '\t')
		G.p++;
}

int mmb_is_digit(char c)
{
	return c >= '0' && c <= '9';
}

int mmb_is_ident(char c)
{
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
	       (c >= '0' && c <= '9') || c == '_' || c == '.';
}

void mmb_upper(char *s)
{
	for (; *s; s++)
		if (*s >= 'a' && *s <= 'z')
			*s = (char)(*s - 32);
}

int mmb_keyword_eq(const char *a, const char *b)
{
	while (*a && *b)
	{
		char ca = *a, cb = *b;
		if (ca >= 'a' && ca <= 'z') ca = (char)(ca - 32);
		if (cb >= 'a' && cb <= 'z') cb = (char)(cb - 32);
		if (ca != cb)
			return 0;
		a++;
		b++;
	}
	return *a == 0 && *b == 0;
}

int mmb_match(const char *kw)
{
	const char *save = G.p;
	mmb_skip_sp();
	const char *p = G.p;
	const char *k = kw;
	while (*k)
	{
		char a = *p, b = *k;
		if (a >= 'a' && a <= 'z') a = (char)(a - 32);
		if (b >= 'a' && b <= 'z') b = (char)(b - 32);
		if (a != b)
		{
			G.p = save;
			return 0;
		}
		p++;
		k++;
	}
	if (mmb_is_ident(*p) && *p != '.')
	{
		G.p = save;
		return 0;
	}
	G.p = p;
	return 1;
}

void mmb_expect(char c)
{
	mmb_skip_sp();
	if (*G.p != c)
		mmb_syntax();
	G.p++;
}

void mmb_ident(char *dst, int dstsz)
{
	int n = 0;
	mmb_skip_sp();
	if (!((*G.p >= 'A' && *G.p <= 'Z') || (*G.p >= 'a' && *G.p <= 'z') || *G.p == '_'))
		mmb_syntax();
	while (mmb_is_ident(*G.p) && n < dstsz - 2)
	{
		char c = *G.p++;
		if (c >= 'a' && c <= 'z')
			c = (char)(c - 32);
		dst[n++] = c;
	}
	if (*G.p == '$' || *G.p == '%' || *G.p == '!')
		dst[n++] = *G.p++;
	dst[n] = 0;
}

int mmb_type_suffix(char *name)
{
	int n = (int)strlen(name);
	if (n <= 0)
		return 0;
	char s = name[n - 1];
	if (s == '$')
	{
		name[n - 1] = 0;
		return T_STR;
	}
	if (s == '%')
	{
		name[n - 1] = 0;
		return T_INT;
	}
	if (s == '!')
	{
		name[n - 1] = 0;
		return T_NUM;
	}
	return 0;
}

void mmb_error(const char *msg)
{
	unsigned i;
	for (i = 0; i < sizeof(G.err) - 1 && msg[i]; i++)
		G.err[i] = msg[i];
	G.err[i] = 0;
	longjmp(G.errjmp, 1);
}

void mmb_syntax(void)
{
	mmb_error("?SYNTAX ERROR");
}

void mmb_out(const char *s)
{
	while (*s && G.outn < MMB_OUT_LEN - 1)
		G.out[G.outn++] = *s++;
	G.out[G.outn] = 0;
}

void mmb_outf(const char *unused, int64_t n)
{
	char buf[32];
	char *p = buf + sizeof(buf) - 1;
	int neg = 0;
	uint64_t v;
	(void)unused;
	*p = 0;
	if (n < 0)
	{
		neg = 1;
		v = (uint64_t)(-n);
	}
	else
		v = (uint64_t)n;
	if (v == 0)
		*--p = '0';
	while (v)
	{
		*--p = (char)('0' + (v % 10));
		v /= 10;
	}
	if (neg)
		*--p = '-';
	mmb_out(p);
}

mmb_val mmb_num_val(double f)
{
	mmb_val v;
	memset(&v, 0, sizeof(v));
	v.type = T_NUM;
	v.f = f;
	v.i = (int64_t)f;
	return v;
}

mmb_val mmb_int_val(int64_t i)
{
	mmb_val v;
	memset(&v, 0, sizeof(v));
	v.type = T_INT;
	v.i = i;
	v.f = (double)i;
	return v;
}

mmb_val mmb_str_val(const char *s)
{
	mmb_val v;
	memset(&v, 0, sizeof(v));
	v.type = T_STR;
	unsigned n = 0;
	if (s)
		while (s[n] && n < MMB_MAX_STR)
		{
			v.s[n] = s[n];
			n++;
		}
	v.s[n] = 0;
	return v;
}

double mmb_as_float(mmb_val v)
{
	if (v.type == T_STR)
		mmb_error("?TYPE MISMATCH");
	return v.type == T_INT ? (double)v.i : v.f;
}

int64_t mmb_as_int(mmb_val v)
{
	if (v.type == T_STR)
		mmb_error("?TYPE MISMATCH");
	return v.type == T_INT ? v.i : (int64_t)v.f;
}

void mmb_need_num(mmb_val v)
{
	if (v.type == T_STR)
		mmb_error("?TYPE MISMATCH");
}

unsigned mmb_now_ms(void)
{
	return G.plat && G.plat->millis ? G.plat->millis() : 0;
}

static void fmt_double(double x, char *buf, int buflen)
{
	int neg = 0, i, n;
	int64_t ip;
	double frac;
	if (buflen < 4)
	{
		buf[0] = 0;
		return;
	}
	if (x < 0)
	{
		neg = 1;
		x = -x;
	}
	ip = (int64_t)x;
	frac = x - (double)ip;
	if (frac < 0)
		frac = 0;
	/* integers print without a decimal (CMM2 PRINT of 5.0 is "5") */
	if (frac < 1e-10 || frac > 1.0 - 1e-10)
	{
		if (frac > 0.5)
			ip++;
		char tmp[40];
		char *p = tmp + sizeof(tmp) - 1;
		uint64_t v = (uint64_t)ip;
		*p = 0;
		if (v == 0)
			*--p = '0';
		while (v)
		{
			*--p = (char)('0' + (v % 10));
			v /= 10;
		}
		if (neg)
			*--p = '-';
		n = 0;
		while (*p && n < buflen - 1)
			buf[n++] = *p++;
		buf[n] = 0;
		return;
	}
	/* up to 6 decimal places, strip trailing zeros */
	char tmp[48];
	char *p = tmp + 24;
	int pos = 0;
	uint64_t v = (uint64_t)ip;
	char ibuf[24];
	int in = 0;
	if (v == 0)
		ibuf[in++] = '0';
	while (v && in < 22)
	{
		ibuf[in++] = (char)('0' + (v % 10));
		v /= 10;
	}
	if (neg)
		tmp[pos++] = '-';
	for (i = in - 1; i >= 0; i--)
		tmp[pos++] = ibuf[i];
	tmp[pos++] = '.';
	for (i = 0; i < 6; i++)
	{
		frac *= 10.0;
		int d = (int)frac;
		if (d < 0)
			d = 0;
		if (d > 9)
			d = 9;
		tmp[pos++] = (char)('0' + d);
		frac -= d;
	}
	while (pos > 0 && tmp[pos - 1] == '0')
		pos--;
	if (pos > 0 && tmp[pos - 1] == '.')
		pos--;
	tmp[pos] = 0;
	n = 0;
	p = tmp;
	while (*p && n < buflen - 1)
		buf[n++] = *p++;
	buf[n] = 0;
}

void mmb_print_val(mmb_val v)
{
	if (v.type == T_STR)
		mmb_out(v.s);
	else if (v.type == T_INT)
		mmb_outf(0, v.i);
	else
	{
		char buf[48];
		fmt_double(v.f, buf, sizeof(buf));
		mmb_out(buf);
	}
}
