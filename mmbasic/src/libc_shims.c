#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
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

static void fmt_uint(char **o, unsigned v)
{
	char tmp[16];
	int n = 0;

	if (v == 0)
		tmp[n++] = '0';
	while (v && n < 15)
	{
		tmp[n++] = (char)('0' + (v % 10));
		v /= 10;
	}
	while (n)
		*(*o)++ = tmp[--n];
}

int sprintf(char *str, const char *fmt, ...)
{
	va_list ap;
	const char *p;
	char *o = str;

	va_start(ap, fmt);
	for (p = fmt; *p; p++)
	{
		if (*p != '%')
		{
			*o++ = *p;
			continue;
		}
		p++;
		while ((*p >= '0' && *p <= '9') || *p == '.' || *p == '-' || *p == '+')
			p++;
		if (*p == 'd' || *p == 'i' || *p == 'u')
		{
			int v = va_arg(ap, int);
			unsigned u;
			if (*p != 'u' && v < 0)
			{
				*o++ = '-';
				u = (unsigned)(-v);
			}
			else
				u = (unsigned)v;
			fmt_uint(&o, u);
		}
		else if (*p == 'x' || *p == 'X')
		{
			unsigned v = va_arg(ap, unsigned);
			const char *hex = (*p == 'X') ? "0123456789ABCDEF" : "0123456789abcdef";
			char tmp[8];
			int n = 0, i;
			for (i = 0; i < 4; i++)
			{
				tmp[n++] = hex[v & 15];
				v >>= 4;
			}
			while (n)
				*o++ = tmp[--n];
		}
		else if (*p == 'g' || *p == 'G' || *p == 'f' || *p == 'e')
		{
			double d = va_arg(ap, double);
			int neg = 0;
			unsigned ip;
			int frac, i;

			if (d < 0)
			{
				neg = 1;
				d = -d;
			}
			ip = (unsigned)d;
			frac = (int)((d - (double)ip) * 1000000.0 + 0.5);
			if (frac >= 1000000)
			{
				ip++;
				frac = 0;
			}
			if (neg)
				*o++ = '-';
			fmt_uint(&o, ip);
			if (frac)
			{
				char fb[8];
				int n = 0;
				*o++ = '.';
				for (i = 0; i < 6; i++)
				{
					fb[n++] = (char)('0' + (frac % 10));
					frac /= 10;
				}
				while (n && fb[n - 1] == '0')
					n--;
				while (n)
					*o++ = fb[--n];
			}
		}
		else if (*p == 's')
		{
			const char *s = va_arg(ap, const char *);
			if (!s)
				s = "";
			while (*s)
				*o++ = *s++;
		}
		else if (*p == '%')
			*o++ = '%';
		else if (*p)
			*o++ = *p;
	}
	*o = 0;
	va_end(ap);
	return (int)(o - str);
}

struct _reent impure_data;
struct _reent *_impure_ptr = &impure_data;
