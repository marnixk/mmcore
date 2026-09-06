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
	if (mmb_is_ident(*p))
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
	if (G.running && G.run_pc >= 0 && G.run_pc < G.nprog)
	{
		const char *p = G.prog[G.run_pc];
		int64_t num = G.prog_num[G.run_pc];
		char nbuf[16];
		int k = 0;
		if (i < sizeof(G.err) - 4)
		{
			G.err[i++] = ' ';
			G.err[i++] = '@';
		}
		if (num <= 0)
			nbuf[k++] = '0';
		else
		{
			char tmp[16];
			int t = 0;
			while (num && t < 15)
			{
				tmp[t++] = (char)('0' + (int)(num % 10));
				num /= 10;
			}
			while (t)
				nbuf[k++] = tmp[--t];
		}
		nbuf[k] = 0;
		for (k = 0; nbuf[k] && i < sizeof(G.err) - 2; k++)
			G.err[i++] = nbuf[k];
		if (i < sizeof(G.err) - 2)
			G.err[i++] = ':';
		while (*p && i < sizeof(G.err) - 1)
			G.err[i++] = *p++;
	}
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

void mmb_console_write(const char *s)
{
	unsigned n;
	if (!s)
		return;
	n = (unsigned)strlen(s);
	if (!n)
		return;
	if (G.plat && G.plat->write_serial)
		G.plat->write_serial(s, n);
	if (G.plat && G.plat->write_screen)
		G.plat->write_screen(s, n);
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

int mmb_opt_repeat_first(void)
{
	return G.opt.repeat_first > 0 ? G.opt.repeat_first : 600;
}

int mmb_opt_repeat_next(void)
{
	return G.opt.repeat_next > 0 ? G.opt.repeat_next : 150;
}

int mmb_opt_wifi_debug(void)
{
	return G.opt.wifi_debug != 0;
}

int mmb_opt_console_serial(void)
{
	return G.opt.console == 1 || G.opt.console == 3;
}

int mmb_opt_console_screen(void)
{
	return G.opt.console == 2 || G.opt.console == 3;
}

const char *mmb_prompt(void)
{
	static char buf[140];
	const char *cwd;
	int n;

	if (!G.opt.prompt)
		return "> ";
	cwd = mmb_vfs_cwd();
	n = 0;
	while (cwd[n] && n < (int)sizeof(buf) - 3)
	{
		buf[n] = cwd[n];
		n++;
	}
	buf[n++] = '>';
	buf[n++] = ' ';
	buf[n] = 0;
	return buf;
}

const char *mmb_opt_wifi_country(void)
{
	if (G.opt.wifi_country[0] && G.opt.wifi_country[1])
		return G.opt.wifi_country;
	return "US";
}

static const int k_mdays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

static int parse_int_part(const char **ps)
{
	int n = 0;
	const char *p = *ps;
	while (*p == ' ' || *p == '\t')
		p++;
	if (*p < '0' || *p > '9')
		return -1;
	while (*p >= '0' && *p <= '9')
		n = n * 10 + (*p++ - '0');
	*ps = p;
	return n;
}

static void clock_norm(void)
{
	unsigned now = mmb_now_ms();
	int add;

	if (now < G.clk_ms)
		G.clk_ms = now;
	add = (int)((now - G.clk_ms) / 1000u);
	if (add <= 0)
		return;
	G.clk_ms += (unsigned)add * 1000u;
	G.clk_s += add;
	while (G.clk_s >= 60)
	{
		G.clk_s -= 60;
		G.clk_mi++;
	}
	while (G.clk_mi >= 60)
	{
		G.clk_mi -= 60;
		G.clk_h++;
	}
	while (G.clk_h >= 24)
	{
		G.clk_h -= 24;
		G.clk_d++;
	}
	for (;;)
	{
		int md, mo = G.clk_mo;
		if (mo < 1)
			mo = 1;
		if (mo > 12)
			mo = 12;
		md = k_mdays[mo - 1];
		if (mo == 2 && ((G.clk_y % 4) == 0))
			md = 29;
		if (G.clk_d <= md)
			break;
		G.clk_d -= md;
		G.clk_mo++;
		if (G.clk_mo > 12)
		{
			G.clk_mo = 1;
			G.clk_y++;
			if (G.clk_y > 99)
				G.clk_y = 0;
		}
	}
}

static void fmt2(char *p, int n)
{
	p[0] = (char)('0' + (n / 10) % 10);
	p[1] = (char)('0' + n % 10);
}

void mmb_clock_refresh(void)
{
	char *p;
	int n;
	clock_norm();
	p = G.date_s;
	n = G.clk_d;
	if (n >= 10)
		*p++ = (char)('0' + n / 10);
	*p++ = (char)('0' + n % 10);
	*p++ = '-';
	n = G.clk_mo;
	if (n >= 10)
		*p++ = (char)('0' + n / 10);
	*p++ = (char)('0' + n % 10);
	*p++ = '-';
	fmt2(p, G.clk_y);
	p[2] = 0;
	fmt2(G.time_s, G.clk_h);
	G.time_s[2] = ':';
	fmt2(G.time_s + 3, G.clk_mi);
	G.time_s[5] = ':';
	fmt2(G.time_s + 6, G.clk_s);
	G.time_s[8] = 0;
}

void mmb_clock_init(void)
{
	G.clk_d = 1;
	G.clk_mo = 1;
	G.clk_y = 26;
	G.clk_h = 12;
	G.clk_mi = 0;
	G.clk_s = 0;
	G.clk_ms = mmb_now_ms();
	mmb_clock_refresh();
}

int mmb_clock_set_date(const char *s)
{
	const char *p = s;
	int d, m, y;
	if (!s)
		return -1;
	d = parse_int_part(&p);
	while (*p == ' ')
		p++;
	if (*p != '-' && *p != '/')
		return -1;
	p++;
	m = parse_int_part(&p);
	while (*p == ' ')
		p++;
	if (*p != '-' && *p != '/')
		return -1;
	p++;
	y = parse_int_part(&p);
	if (d < 1 || d > 31 || m < 1 || m > 12 || y < 0)
		return -1;
	if (y >= 100)
		y %= 100;
	clock_norm();
	G.clk_d = d;
	G.clk_mo = m;
	G.clk_y = y;
	G.clk_ms = mmb_now_ms();
	mmb_clock_refresh();
	return 0;
}

int mmb_clock_set_time(const char *s)
{
	const char *p = s;
	int h, mi, sec;
	if (!s)
		return -1;
	h = parse_int_part(&p);
	while (*p == ' ')
		p++;
	if (*p != ':')
		return -1;
	p++;
	mi = parse_int_part(&p);
	sec = 0;
	while (*p == ' ')
		p++;
	if (*p == ':')
	{
		p++;
		sec = parse_int_part(&p);
	}
	if (h < 0 || h > 23 || mi < 0 || mi > 59 || sec < 0 || sec > 59)
		return -1;
	clock_norm();
	G.clk_h = h;
	G.clk_mi = mi;
	G.clk_s = sec;
	G.clk_ms = mmb_now_ms();
	mmb_clock_refresh();
	return 0;
}

void mmb_inkey_push(int c)
{
	if (c <= 0 || c > 255)
		return;
	if (G.inkey_n >= MMB_INKEY)
		return;
	G.inkey_q[G.inkey_w] = c;
	G.inkey_w = (G.inkey_w + 1) % MMB_INKEY;
	G.inkey_n++;
}

int mmb_inkey_pop(void)
{
	int c;
	if (G.inkey_n <= 0)
		return -1;
	c = G.inkey_q[G.inkey_r];
	G.inkey_r = (G.inkey_r + 1) % MMB_INKEY;
	G.inkey_n--;
	return c;
}

void mmb_keydown_set(const int *codes, int n)
{
	int i;
	if (n < 0)
		n = 0;
	if (n > 6)
		n = 6;
	G.nkeydown = n;
	for (i = 0; i < n; i++)
		G.keydown[i] = codes[i];
}

int mmb_keydown_get(int n)
{
	if (n == 0)
		return G.nkeydown;
	if (n < 1 || n > G.nkeydown)
		return 0;
	return G.keydown[n - 1];
}
