/* Bare-metal libc shims.
 *
 * The format core (mmb_vsnprintf / mmb_snprintf) is platform independent and is
 * compiled on every target so it can be unit-tested on the host. The remaining
 * shims are bare-metal only: native builds take them from libc, and defining
 * them there would conflict (and <reent.h> does not exist).
 */
#include <stdarg.h>

typedef struct
{
	char *buf;
	unsigned long cap;
	unsigned long len;
} mmbfmt;

static void ofc(mmbfmt *o, char c)
{
	if (o->cap && o->len + 1 < o->cap)
		o->buf[o->len] = c;
	o->len++;
}

static void ofs_n(mmbfmt *o, const char *s, int n)
{
	int i;

	for (i = 0; i < n; i++)
		ofc(o, s[i]);
}

static void ofpad(mmbfmt *o, char c, int n)
{
	while (n-- > 0)
		ofc(o, c);
}

/* Emit a pre-built body with field-width padding. The zero flag pads after a
 * leading sign ("-0042") or hex prefix ("0x001f") rather than before it. */
static void emit_body(mmbfmt *o, const char *body, int blen, int width,
		      int left, int zero)
{
	int pad = width > blen ? width - blen : 0;
	int i;

	if (left)
		zero = 0;
	if (!left && !zero)
		ofpad(o, ' ', pad);
	if (zero && blen > 1 && (body[0] == '-' || body[0] == '+'))
	{
		ofc(o, body[0]);
		ofpad(o, '0', pad);
		for (i = 1; i < blen; i++)
			ofc(o, body[i]);
	}
	else if (zero && blen > 1 && body[0] == '0' &&
		 (body[1] == 'x' || body[1] == 'X'))
	{
		ofs_n(o, body, 2);
		ofpad(o, '0', pad);
		for (i = 2; i < blen; i++)
			ofc(o, body[i]);
	}
	else
	{
		if (zero)
			ofpad(o, '0', pad);
		ofs_n(o, body, blen);
	}
	if (left)
		ofpad(o, ' ', pad);
}

static int fmt_ull(char *out, unsigned long long v, unsigned base, int upper)
{
	char tmp[24];
	const char *ds = upper ? "0123456789ABCDEF" : "0123456789abcdef";
	int n = 0, i;

	if (v == 0)
		tmp[n++] = '0';
	while (v)
	{
		tmp[n++] = ds[v % base];
		v /= base;
	}
	for (i = 0; i < n; i++)
		out[i] = tmp[n - 1 - i];
	return n;
}

#define MMB_FMT_POW10_MAX 15

static unsigned long long ipow10(int n)
{
	unsigned long long r = 1;

	while (n-- > 0)
		r *= 10;
	return r;
}

/* Fixed notation of |d| (sign stripped by the caller) into out. */
static int fixed_body(char *out, double d, int prec)
{
	char *p = out;
	unsigned long long ip, fi = 0;
	int i;

	if (d < 0)
		d = -d;
	ip = (unsigned long long)d;
	{
		unsigned long long scale = ipow10(prec);
		double scaled = (d - (double)ip) * (double)scale;
		unsigned long long fl = (unsigned long long)scaled;
		double diff = scaled - (double)fl;

		/* Round half to even, matching glibc's default rounding mode. */
		if (diff > 0.5)
			fl++;
		else if (diff == 0.5 && (fl & 1))
			fl++;
		fi = fl;
		if (fi >= scale)
		{
			ip += 1;
			fi = 0;
		}
	}
	{
		char tmp[24];
		int n = 0;

		if (ip == 0)
			tmp[n++] = '0';
		while (ip)
		{
			tmp[n++] = (char)('0' + ip % 10);
			ip /= 10;
		}
		while (n)
			*p++ = tmp[--n];
	}
	if (prec > 0)
	{
		*p++ = '.';
		for (i = prec - 1; i >= 0; i--)
			*p++ = (char)('0' + (int)((fi / ipow10(i)) % 10));
	}
	return (int)(p - out);
}

/* Scientific notation of |d|: mantissa + 'e' + sign + at least 2 exp digits. */
static int sci_body(char *out, double d, int prec, int upper)
{
	char mant[48];
	char expbuf[8];
	char *p = out;
	double a;
	int mlen, i, e = 0;

	if (d < 0)
		d = -d;
	if (d == 0)
		a = 0;
	else
	{
		a = d;
		while (a >= 10.0)
		{
			a /= 10.0;
			e++;
		}
		while (a < 1.0)
		{
			a *= 10.0;
			e--;
		}
	}
	mlen = fixed_body(mant, a, prec);
	/* Rounding can push a mantissa of 9.99.. up to 10.00..: renormalise. */
	if (mlen > 1 && mant[0] == '1' && mant[1] == '0')
	{
		for (i = 1; i < mlen; i++)
			mant[i] = (mant[i] == '.') ? '.' : '0';
		e++;
	}
	for (i = 0; i < mlen; i++)
		*p++ = mant[i];
	*p++ = upper ? 'E' : 'e';
	if (e < 0)
	{
		*p++ = '-';
		e = -e;
	}
	else
		*p++ = '+';
	i = 0;
	if (e == 0)
		expbuf[i++] = '0';
	while (e)
	{
		expbuf[i++] = (char)('0' + e % 10);
		e /= 10;
	}
	if (i < 2)
		expbuf[i++] = '0';
	while (i)
		*p++ = expbuf[--i];
	return (int)(p - out);
}

/* %g: shortest of %e / %f for the given significant digits, trailing zeros
 * removed unless '#' was given. |d| only; sign handled by caller. */
static int g_body(char *out, double d, int prec, int upper, int alt)
{
	int n, e = 0, dot = -1, epos = -1, i, end, removed;

	if (prec < 0)
		prec = 6;
	else if (prec == 0)
		prec = 1;
	if (prec > MMB_FMT_POW10_MAX)
		prec = MMB_FMT_POW10_MAX;
	if (d < 0)
		d = -d;
	if (d != 0)
	{
		double a = d;

		while (a >= 10.0)
		{
			a /= 10.0;
			e++;
		}
		while (a < 1.0)
		{
			a *= 10.0;
			e--;
		}
	}
	if (e >= -4 && e < prec)
		n = fixed_body(out, d, prec - 1 - e);
	else
		n = sci_body(out, d, prec - 1, upper);
	for (i = 0; i < n; i++)
	{
		if (out[i] == '.')
			dot = i;
		if (out[i] == 'e' || out[i] == 'E')
		{
			epos = i;
			break;
		}
	}
	if (alt)
	{
		/* '#' keeps the decimal point even with no fractional digits. */
		if (dot < 0)
		{
			if (epos >= 0)
			{
				for (i = n; i > epos; i--)
					out[i] = out[i - 1];
				out[epos] = '.';
				n++;
			}
			else
				out[n++] = '.';
		}
		return n;
	}
	if (dot < 0)
		return n;
	end = (epos < 0) ? n : epos;
	{
		int keep = dot;

		for (i = dot + 1; i < end; i++)
			if (out[i] != '0')
				keep = i + 1;
		removed = end - keep;
		if (removed > 0)
		{
			for (i = end; i < n; i++)
				out[i - removed] = out[i];
			n -= removed;
		}
	}
	return n;
}

int mmb_vsnprintf(char *str, unsigned long size, const char *fmt, va_list ap)
{
	mmbfmt o;
	const char *p;

	o.buf = str;
	o.cap = size;
	o.len = 0;
	for (p = fmt; *p; p++)
	{
		int left = 0, zero = 0, plus = 0, space = 0, hash = 0;
		int width = 0, prec = -1, longcnt = 0;
		char conv;

		if (*p != '%')
		{
			ofc(&o, *p);
			continue;
		}
		p++;
		for (;; p++)
		{
			if (*p == '-')
				left = 1;
			else if (*p == '0')
				zero = 1;
			else if (*p == '+')
				plus = 1;
			else if (*p == ' ')
				space = 1;
			else if (*p == '#')
				hash = 1;
			else
				break;
		}
		while (*p >= '0' && *p <= '9')
			width = width * 10 + (*p++ - '0');
		if (*p == '.')
		{
			p++;
			prec = 0;
			while (*p >= '0' && *p <= '9')
				prec = prec * 10 + (*p++ - '0');
		}
		while (*p == 'l')
		{
			longcnt++;
			p++;
		}
		if (*p == 'h')
		{
			p++;
			if (*p == 'h')
				p++;
		}
		else if (*p == 'z' || *p == 'j' || *p == 't')
		{
			longcnt = 1;
			p++;
		}
		conv = *p;
		if (conv == 'd' || conv == 'i' || conv == 'u' || conv == 'x' ||
		    conv == 'X')
		{
			unsigned long long uv;
			int neg = 0, upper = (conv == 'X'), base = 10, i;
			char digits[24], body[80];
			int ndig, blen = 0;

			if (conv == 'd' || conv == 'i')
			{
				long long sv;

				if (longcnt >= 2)
					sv = va_arg(ap, long long);
				else if (longcnt == 1)
					sv = va_arg(ap, long);
				else
					sv = va_arg(ap, int);
				neg = sv < 0;
				uv = neg ? (unsigned long long)(-(sv + 1)) + 1
					 : (unsigned long long)sv;
			}
			else
			{
				if (longcnt >= 2)
					uv = va_arg(ap, unsigned long long);
				else if (longcnt == 1)
					uv = va_arg(ap, unsigned long);
				else
					uv = va_arg(ap, unsigned int);
			}
			if (conv == 'x' || conv == 'X')
				base = 16;
			ndig = (prec == 0 && uv == 0) ? 0
						      : fmt_ull(digits, uv, base, upper);
			if (neg)
				body[blen++] = '-';
			else if (plus)
				body[blen++] = '+';
			else if (space)
				body[blen++] = ' ';
			if (hash && base == 16 && uv != 0)
			{
				body[blen++] = '0';
				body[blen++] = upper ? 'X' : 'x';
			}
			if (prec >= 0)
				for (i = ndig; i < prec; i++)
					body[blen++] = '0';
			for (i = 0; i < ndig; i++)
				body[blen++] = digits[i];
			emit_body(&o, body, blen, width, left, zero && prec < 0);
		}
		else if (conv == 'c')
		{
			char body = (char)va_arg(ap, int);

			emit_body(&o, &body, 1, width, left, 0);
		}
		else if (conv == 's')
		{
			const char *s = va_arg(ap, const char *);
			int slen = 0;

			if (!s)
				s = "";
			while (s[slen])
				slen++;
			if (prec >= 0 && slen > prec)
				slen = prec;
			emit_body(&o, s, slen, width, left, 0);
		}
		else if (conv == 'f' || conv == 'F' || conv == 'e' || conv == 'E' ||
			 conv == 'g' || conv == 'G')
		{
			char body[96];
			int blen = 0;
			double dv = va_arg(ap, double);
			int upper = (conv == 'E' || conv == 'G' || conv == 'F');

			if (dv != dv)
			{
				body[blen++] = 'n';
				body[blen++] = 'a';
				body[blen++] = 'n';
			}
			else if (dv > 1.7976931348623157e308)
			{
				body[blen++] = 'i';
				body[blen++] = 'n';
				body[blen++] = 'f';
			}
			else
			{
				if (dv < 0)
				{
					body[blen++] = '-';
					dv = -dv;
				}
				else if (plus)
					body[blen++] = '+';
				else if (space)
					body[blen++] = ' ';
				if (conv == 'f' || conv == 'F')
				{
					if (prec < 0)
						prec = 6;
					if (prec > MMB_FMT_POW10_MAX)
						prec = MMB_FMT_POW10_MAX;
					blen += fixed_body(body + blen, dv, prec);
				}
				else if (conv == 'e' || conv == 'E')
				{
					if (prec < 0)
						prec = 6;
					if (prec > MMB_FMT_POW10_MAX)
						prec = MMB_FMT_POW10_MAX;
					blen += sci_body(body + blen, dv, prec, upper);
				}
				else
					blen += g_body(body + blen, dv, prec, upper, hash);
			}
			emit_body(&o, body, blen, width, left, zero);
		}
		else if (conv == '%')
			ofc(&o, '%');
		else if (conv)
		{
			ofc(&o, '%');
			ofc(&o, conv);
		}
		else
			ofc(&o, '%');
	}
	if (o.cap)
		o.buf[o.len < o.cap ? o.len : o.cap - 1] = 0;
	return (int)o.len;
}

int mmb_snprintf(char *str, unsigned long size, const char *fmt, ...)
{
	va_list ap;
	int r;

	va_start(ap, fmt);
	r = mmb_vsnprintf(str, size, fmt, ap);
	va_end(ap);
	return r;
}

#if defined(MMB_PLATFORM_POSIX)
/* Native builds get all of these from libc; the bare-metal shims below would
 * conflict (and <reent.h> does not exist). */
#else
#include <stdio.h>
#include <stdlib.h>
#include <reent.h>

div_t div(int n, int d)
{
	div_t r;
	r.quot = d ? n / d : 0;
	r.rem = d ? n % d : 0;
	return r;
}

int fflush(FILE *f)
{
	(void)f;
	return 0;
}

void rewind(FILE *f)
{
	(void)f;
}

void exit(int c)
{
	(void)c;
	for (;;)
		;
}

double strtod(const char *nptr, char **endptr)
{
	const char *s = nptr;
	int neg = 0;
	double f = 0, p = 0.1;

	if (!s)
	{
		if (endptr)
			*endptr = 0;
		return 0;
	}
	while (*s == ' ' || *s == '\t')
		s++;
	if (*s == '-')
	{
		neg = 1;
		s++;
	}
	else if (*s == '+')
		s++;
	while (*s >= '0' && *s <= '9')
	{
		f = f * 10.0 + (*s - '0');
		s++;
	}
	if (*s == '.')
	{
		s++;
		while (*s >= '0' && *s <= '9')
		{
			f += (*s - '0') * p;
			p *= 0.1;
			s++;
		}
	}
	if (*s == 'e' || *s == 'E')
	{
		int eneg = 0, e = 0;
		double m = 1.0;

		s++;
		if (*s == '-')
		{
			eneg = 1;
			s++;
		}
		else if (*s == '+')
			s++;
		while (*s >= '0' && *s <= '9')
		{
			e = e * 10 + (*s - '0');
			s++;
		}
		while (e-- > 0)
			m *= 10.0;
		if (eneg)
			f /= m;
		else
			f *= m;
	}
	if (endptr)
		*endptr = (char *)s;
	return neg ? -f : f;
}

int sprintf(char *str, const char *fmt, ...)
{
	va_list ap;
	int r;

	va_start(ap, fmt);
	r = mmb_vsnprintf(str, (unsigned long)-1, fmt, ap);
	va_end(ap);
	return r;
}

struct _reent impure_data;
struct _reent *_impure_ptr = &impure_data;
#endif /* !MMB_PLATFORM_POSIX */
