#include "mmb_priv.h"

mmb G;

void mmb_skip_sp(void)
{
	while (*G.p == ' ' || *G.p == '\t' || *G.p == '\r')
		G.p++;
}

int mmb_normalize_newlines(char *buf, int len)
{
	int i, o = 0;

	if (!buf)
		return 0;
	if (len < 0)
	{
		len = 0;
		while (buf[len])
			len++;
	}
	for (i = 0; i < len; i++)
	{
		if (buf[i] == '\r')
			continue;
		buf[o++] = buf[i];
	}
	buf[o] = 0;
	return o;
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
	if (G.opt.profiling && G.running)
		G.prof.match++;
	mmb_skip_sp();
	if ((unsigned char)*G.p == 0x80)
	{
		if (mmb_match_token(kw))
			return 1;
		G.p = save;
		return 0;
	}
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
	if (mmb_tok_expand(dst, dstsz))
		return;
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
	if (G.running)
		mmb_play_stop();
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

void mmb_out_flush(void)
{
	if (!G.outn)
		return;
	mmb_console_write(G.out);
	G.outn = 0;
	G.out[0] = 0;
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

void mmb_prof_reset(void)
{
	G.prof.t0_ms = mmb_now_ms();
	G.prof.stmt = 0;
	G.prof.match = 0;
	G.prof.expr = 0;
	G.prof.find_var = 0;
	G.prof.check_break = 0;
	G.prof.gfx_present = 0;
}

void mmb_prof_report(void)
{
	unsigned elapsed;
	if (!G.opt.profiling)
		return;
	elapsed = mmb_now_ms() - G.prof.t0_ms;
	if (G.outn)
		mmb_out("\n");
	mmb_out("[PERF] elapsed=");
	mmb_outf(0, elapsed);
	mmb_out(" ms  statements=");
	mmb_outf(0, G.prof.stmt);
	mmb_out("  match=");
	mmb_outf(0, G.prof.match);
	mmb_out("  expr=");
	mmb_outf(0, G.prof.expr);
	mmb_out("  findvar=");
	mmb_outf(0, G.prof.find_var);
	mmb_out("  break=");
	mmb_outf(0, G.prof.check_break);
	mmb_out("  present=");
	mmb_outf(0, G.prof.gfx_present);
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

static int rgb_dist2(unsigned a, unsigned b)
{
	int dr = (int)((a >> 16) & 255) - (int)((b >> 16) & 255);
	int dg = (int)((a >> 8) & 255) - (int)((b >> 8) & 255);
	int db = (int)(a & 255) - (int)(b & 255);
	return dr * dr + dg * dg + db * db;
}

static int rgb_to_ansi(unsigned rgb, int fg)
{
	static const unsigned pal[] = {
		0x000000, 0xAA0000, 0x00AA00, 0xAA5500,
		0x0000AA, 0xAA00AA, 0x00AAAA, 0xAAAAAA,
		0x555555, 0xFF5555, 0x55FF55, 0xFFFF55,
		0x5555FF, 0xFF55FF, 0x55FFFF, 0xFFFFFF
	};
	static const int fgcode[] = {
		30, 31, 32, 33, 34, 35, 36, 37,
		90, 91, 92, 93, 94, 95, 96, 97
	};
	static const int bgcode[] = {
		40, 41, 42, 43, 44, 45, 46, 47,
		100, 101, 102, 103, 104, 105, 106, 107
	};
	int best = 0, i, d, bd;

	bd = rgb_dist2(rgb, pal[0]);
	for (i = 1; i < 16; i++)
	{
		d = rgb_dist2(rgb, pal[i]);
		if (d < bd)
		{
			bd = d;
			best = i;
		}
	}
	return fg ? fgcode[best] : bgcode[best];
}

static void ansi_put_int(char *seq, int *n, int v)
{
	char tmp[4];
	int i = 0;
	if (v <= 0)
	{
		seq[(*n)++] = '0';
		return;
	}
	while (v > 0 && i < 4)
	{
		tmp[i++] = (char)('0' + (v % 10));
		v /= 10;
	}
	while (i > 0)
		seq[(*n)++] = tmp[--i];
}

void mmb_console_apply_colour(void)
{
	char seq[24];
	int n = 0;

	if (!G.plat || !G.plat->write_screen)
		return;
	/* Circle only honours SGR when the CSI has a single parameter
	 * (`ESC[91m`). `ESC[91;40m` is ignored. */
	seq[n++] = '\x1b';
	seq[n++] = '[';
	ansi_put_int(seq, &n, rgb_to_ansi(G.gfx.fg, 1));
	seq[n++] = 'm';
	seq[n++] = '\x1b';
	seq[n++] = '[';
	ansi_put_int(seq, &n, rgb_to_ansi(G.gfx.bg, 0));
	seq[n++] = 'm';
	G.plat->write_screen(seq, (unsigned)n);
}

void mmb_console_reset_prompt(void)
{
	G.gfx.fg = 0x808080u;
	G.gfx.bg = 0;
	mmb_console_apply_colour();
}

void mmb_print_startup(void)
{
	mmb_console_write("\x1b[37m");
	mmb_console_write("Copyright 2011-2026 Geoff Graham\n");
	mmb_console_write("Copyright 2016-2026 Peter Mather\n");
	mmb_console_write("Adapted and extended by Marnix Kok\n");
	mmb_console_write("\n");
	mmb_console_write("Type \x1b[97mHELP\x1b[37m to get started.\n");
	mmb_console_write("\n\n");
	mmb_console_write("\x1b[0m");
	mmb_console_apply_colour();
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
	return G.opt.repeat_first > 0 ? G.opt.repeat_first : MMB_REPEAT_FIRST_DEFAULT;
}

int mmb_opt_repeat_next(void)
{
	return G.opt.repeat_next > 0 ? G.opt.repeat_next : MMB_REPEAT_NEXT_DEFAULT;
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

/* Circle hostap driver_circle.cpp: associate is refused unless the
 * country is on this list. UK is not listed; GB is. */
static const char k_wifi_countries[][3] = {
	"AD","AE","AF","AI","AL","AM","AN","AR","AS","AT","AU","AW","AZ",
	"BA","BB","BD","BE","BF","BG","BH","BL","BM","BN","BO","BR","BS",
	"BT","BY","BZ","CA","CF","CH","CI","CL","CN","CO","CR","CU","CX",
	"CY","CZ","DE","DK","DM","DO","DZ","EC","EE","EG","ES","ET","FI",
	"FM","FR","GB","GD","GE","GF","GH","GL","GP","GR","GT","GU","GY",
	"HK","HN","HR","HT","HU","ID","IE","IL","IN","IR","IS","IT","JM",
	"JO","JP","KE","KH","KN","KP","KR","KW","KY","KZ","LB","LC","LI",
	"LK","LS","LT","LU","LV","MA","MC","MD","ME","MF","MH","MK","MN",
	"MO","MP","MQ","MR","MT","MU","MV","MW","MX","MY","NG","NI","NL",
	"NO","NP","NZ","OM","PA","PE","PF","PG","PH","PK","PL","PM","PR",
	"PT","PW","PY","QA","RE","RO","RS","RU","RW","SA","SE","SG","SI",
	"SK","SN","SR","SV","SY","TC","TD","TG","TH","TN","TR","TT","TW",
	"TZ","UA","UG","US","UY","UZ","VC","VE","VI","VN","VU","WF","WS",
	"YE","YT","ZA","ZW"
};

int mmb_wifi_country_normalize(const char *s, char out[3])
{
	char a, b;
	unsigned i;

	if (!s || !s[0] || !s[1] || s[2] || !out)
		return 0;
	a = s[0];
	b = s[1];
	if (a >= 'a' && a <= 'z')
		a = (char)(a - 32);
	if (b >= 'a' && b <= 'z')
		b = (char)(b - 32);
	if (a < 'A' || a > 'Z' || b < 'A' || b > 'Z')
		return 0;
	if (a == 'U' && b == 'K')
	{
		a = 'G';
		b = 'B';
	}
	for (i = 0; i < sizeof k_wifi_countries / sizeof k_wifi_countries[0]; i++)
	{
		if (k_wifi_countries[i][0] == a && k_wifi_countries[i][1] == b)
		{
			out[0] = a;
			out[1] = b;
			out[2] = 0;
			return 1;
		}
	}
	return 0;
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
