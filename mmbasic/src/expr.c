#include "mmb_priv.h"
#include <math.h>

extern mmb_val mmb_load_var(mmb_var *v, int off);
extern int mmb_parse_var_ref(char *name, int *nidx, int *idx);
extern void mmb_do_assign(const char *name, int type_hint, int nidx, int *idx, mmb_val val);

static mmb_val expr_or(void);
static mmb_val expr_primary(void);

static int peek_kw(const char *kw)
{
	const char *save = G.p;
	int r = mmb_match(kw);
	G.p = save;
	return r;
}

/* Functions that need arguments must see '(' so CONST MAX and PRINT MAX*2 work. */
static int match_fun(const char *name)
{
	const char *save = G.p;
	if (!mmb_match(name))
		return 0;
	mmb_skip_sp();
	if (*G.p != '(')
	{
		G.p = save;
		return 0;
	}
	return 1;
}

static mmb_val parse_number(void)
{
	int is_int = 1;
	double f = 0;
	int64_t i = 0;
	mmb_skip_sp();
	if (G.p[0] == '&' && (G.p[1] == 'H' || G.p[1] == 'h'))
	{
		G.p += 2;
		i = 0;
		while ((*G.p >= '0' && *G.p <= '9') ||
		       (*G.p >= 'A' && *G.p <= 'F') ||
		       (*G.p >= 'a' && *G.p <= 'f'))
		{
			int d = *G.p++;
			if (d >= 'a') d -= 32;
			d = d <= '9' ? d - '0' : d - 'A' + 10;
			i = (i << 4) | d;
		}
		return mmb_int_val(i);
	}
	if (G.p[0] == '&' && (G.p[1] == 'B' || G.p[1] == 'b'))
	{
		G.p += 2;
		i = 0;
		while (*G.p == '0' || *G.p == '1')
			i = (i << 1) | (*G.p++ - '0');
		return mmb_int_val(i);
	}
	while (mmb_is_digit(*G.p))
	{
		i = i * 10 + (*G.p - '0');
		f = f * 10.0 + (*G.p - '0');
		G.p++;
	}
	if (*G.p == '.')
	{
		double p = 0.1;
		is_int = 0;
		G.p++;
		while (mmb_is_digit(*G.p))
		{
			f += (*G.p - '0') * p;
			p *= 0.1;
			G.p++;
		}
	}
	if (*G.p == 'E' || *G.p == 'e')
	{
		int es = 1, e = 0;
		is_int = 0;
		G.p++;
		if (*G.p == '-')
		{
			es = -1;
			G.p++;
		}
		else if (*G.p == '+')
			G.p++;
		while (mmb_is_digit(*G.p))
			e = e * 10 + (*G.p++ - '0');
		{
			int k;
			double m = 1;
			for (k = 0; k < e; k++)
				m *= 10.0;
			if (es < 0)
				f /= m;
			else
				f *= m;
		}
	}
	return is_int ? mmb_int_val(i) : mmb_num_val(f);
}

static mmb_val parse_string(void)
{
	char buf[MMB_MAX_STR + 1];
	int n = 0;
	G.p++; /* quote */
	while (*G.p)
	{
		if (*G.p == '"')
		{
			if (G.p[1] == '"')
			{
				if (n < MMB_MAX_STR)
					buf[n++] = '"';
				G.p += 2;
				continue;
			}
			break;
		}
		if (n < MMB_MAX_STR)
			buf[n++] = *G.p;
		G.p++;
	}
	if (*G.p == '"')
		G.p++;
	buf[n] = 0;
	return mmb_str_val(buf);
}

static mmb_val call_args(mmb_val *a, int maxn, int *got)
{
	int n = 0;
	mmb_skip_sp();
	if (*G.p == '(')
	{
		G.p++;
		mmb_skip_sp();
		if (*G.p != ')')
		{
			for (;;)
			{
				if (n >= maxn)
					mmb_syntax();
				a[n++] = mmb_expr();
				mmb_skip_sp();
				if (*G.p == ',')
				{
					G.p++;
					continue;
				}
				break;
			}
		}
		mmb_expect(')');
	}
	*got = n;
	return a[0];
}

static unsigned named_or_fail(const char *n, int *ok)
{
	return mmb_named_colour(n, ok);
}

int mmb_try_function(mmb_val *out)
{
	mmb_val a[8];
	int n = 0, ok;
	unsigned col;
	const char *save = G.p;

	if ((unsigned char)*G.p != 0x80)
		goto ident_tail;

	if (mmb_match("RGB"))
	{
		call_args(a, 4, &n);
		if (n == 1 && a[0].type == T_STR)
		{
			col = named_or_fail(a[0].s, &ok);
			if (!ok)
				mmb_syntax();
			*out = mmb_int_val((int64_t)col);
			return 1;
		}
		if (n == 1 && a[0].type != T_STR)
		{
			*out = mmb_int_val((int64_t)mmb_colour_from_int(mmb_as_int(a[0])));
			return 1;
		}
		if (n == 3 || n == 4)
		{
			int r = (int)mmb_as_int(a[0]);
			int g = (int)mmb_as_int(a[1]);
			int b = (int)mmb_as_int(a[2]);
			*out = mmb_int_val((int64_t)mmb_rgb_pack(r, g, b));
			return 1;
		}
		mmb_syntax();
	}
	if (mmb_match("PIXEL"))
	{
		int x, y, page = MMB_PAGE_CUR;
		mmb_skip_sp();
		if (*G.p != '(')
		{
			G.p = save;
			return 0;
		}
		G.p++;
		x = (int)mmb_as_int(mmb_expr());
		mmb_skip_sp();
		mmb_expect(',');
		y = (int)mmb_as_int(mmb_expr());
		mmb_skip_sp();
		if (*G.p == ',')
		{
			G.p++;
			mmb_skip_sp();
			if (mmb_match("FRAMEBUFFER"))
				page = MMB_PAGE_FB;
			else
				page = (int)mmb_as_int(mmb_expr());
		}
		mmb_expect(')');
		*out = mmb_int_val((int64_t)mmb_gfx_get_page(x, y, page));
		return 1;
	}
	if (mmb_match("LEN"))
	{
		call_args(a, 1, &n);
		if (n != 1 || a[0].type != T_STR)
			mmb_error("?TYPE MISMATCH");
		*out = mmb_int_val((int64_t)strlen(a[0].s));
		return 1;
	}
	if (mmb_match("ASC"))
	{
		call_args(a, 1, &n);
		if (n != 1 || a[0].type != T_STR)
			mmb_error("?TYPE MISMATCH");
		*out = mmb_int_val(a[0].s[0] ? (unsigned char)a[0].s[0] : 0);
		return 1;
	}
	if (mmb_match("CHR$"))
	{
		char s[2];
		call_args(a, 1, &n);
		if (n != 1)
			mmb_syntax();
		s[0] = (char)mmb_as_int(a[0]);
		s[1] = 0;
		*out = mmb_str_val(s);
		return 1;
	}
	if (mmb_match("STR$"))
	{
		char buf[48];
		call_args(a, 1, &n);
		if (n != 1)
			mmb_syntax();
		G.outn = 0;
		G.out[0] = 0;
		mmb_print_val(a[0]);
		strncpy(buf, G.out, sizeof(buf) - 1);
		buf[sizeof(buf) - 1] = 0;
		G.outn = 0;
		G.out[0] = 0;
		*out = mmb_str_val(buf);
		return 1;
	}
	if (mmb_match("VAL"))
	{
		const char *s;
		call_args(a, 1, &n);
		if (n != 1 || a[0].type != T_STR)
			mmb_error("?TYPE MISMATCH");
		s = a[0].s;
		while (*s == ' ')
			s++;
		*out = mmb_num_val(0);
		{
			int neg = 0;
			double f = 0;
			if (*s == '-')
			{
				neg = 1;
				s++;
			}
			while (*s >= '0' && *s <= '9')
			{
				f = f * 10 + (*s - '0');
				s++;
			}
			if (*s == '.')
			{
				double p = 0.1;
				s++;
				while (*s >= '0' && *s <= '9')
				{
					f += (*s - '0') * p;
					p *= 0.1;
					s++;
				}
			}
			*out = mmb_num_val(neg ? -f : f);
		}
		return 1;
	}
	if (mmb_match("LEFT$"))
	{
		int k, i;
		char b[MMB_MAX_STR + 1];
		call_args(a, 2, &n);
		if (n != 2 || a[0].type != T_STR)
			mmb_error("?TYPE MISMATCH");
		k = (int)mmb_as_int(a[1]);
		if (k < 0)
			k = 0;
		for (i = 0; i < k && a[0].s[i]; i++)
			b[i] = a[0].s[i];
		b[i] = 0;
		*out = mmb_str_val(b);
		return 1;
	}
	if (mmb_match("RIGHT$"))
	{
		int k, i, len;
		char b[MMB_MAX_STR + 1];
		call_args(a, 2, &n);
		if (n != 2 || a[0].type != T_STR)
			mmb_error("?TYPE MISMATCH");
		k = (int)mmb_as_int(a[1]);
		len = (int)strlen(a[0].s);
		if (k > len)
			k = len;
		if (k < 0)
			k = 0;
		for (i = 0; i < k; i++)
			b[i] = a[0].s[len - k + i];
		b[k] = 0;
		*out = mmb_str_val(b);
		return 1;
	}
	if (mmb_match("MID$"))
	{
		int start, num, i, len;
		char b[MMB_MAX_STR + 1];
		call_args(a, 3, &n);
		if ((n != 2 && n != 3) || a[0].type != T_STR)
			mmb_error("?TYPE MISMATCH");
		start = (int)mmb_as_int(a[1]);
		len = (int)strlen(a[0].s);
		num = n == 3 ? (int)mmb_as_int(a[2]) : len;
		if (start < 1)
			start = 1;
		if (num < 0)
			num = 0;
		for (i = 0; i < num && start - 1 + i < len; i++)
			b[i] = a[0].s[start - 1 + i];
		b[i] = 0;
		*out = mmb_str_val(b);
		return 1;
	}
	if (mmb_match("UCASE$"))
	{
		char b[MMB_MAX_STR + 1];
		call_args(a, 1, &n);
		if (n != 1 || a[0].type != T_STR)
			mmb_error("?TYPE MISMATCH");
		strncpy(b, a[0].s, MMB_MAX_STR);
		b[MMB_MAX_STR] = 0;
		mmb_upper(b);
		*out = mmb_str_val(b);
		return 1;
	}
	if (mmb_match("LCASE$"))
	{
		int i;
		char b[MMB_MAX_STR + 1];
		call_args(a, 1, &n);
		if (n != 1 || a[0].type != T_STR)
			mmb_error("?TYPE MISMATCH");
		strncpy(b, a[0].s, MMB_MAX_STR);
		b[MMB_MAX_STR] = 0;
		for (i = 0; b[i]; i++)
			if (b[i] >= 'A' && b[i] <= 'Z')
				b[i] = (char)(b[i] + 32);
		*out = mmb_str_val(b);
		return 1;
	}
	if (mmb_match("SPACE$"))
	{
		int k, i;
		char b[MMB_MAX_STR + 1];
		call_args(a, 1, &n);
		if (n != 1)
			mmb_syntax();
		k = (int)mmb_as_int(a[0]);
		if (k < 0)
			k = 0;
		if (k > MMB_MAX_STR)
			k = MMB_MAX_STR;
		for (i = 0; i < k; i++)
			b[i] = ' ';
		b[k] = 0;
		*out = mmb_str_val(b);
		return 1;
	}
	if (mmb_match("ABS"))
	{
		call_args(a, 1, &n);
		if (n != 1)
			mmb_syntax();
		if (a[0].type == T_INT)
			*out = mmb_int_val(a[0].i < 0 ? -a[0].i : a[0].i);
		else
			*out = mmb_num_val(fabs(mmb_as_float(a[0])));
		return 1;
	}
	if (mmb_match("INT"))
	{
		call_args(a, 1, &n);
		if (n != 1)
			mmb_syntax();
		*out = mmb_int_val((int64_t)floor(mmb_as_float(a[0])));
		return 1;
	}
	if (mmb_match("FIX"))
	{
		call_args(a, 1, &n);
		if (n != 1)
			mmb_syntax();
		*out = mmb_int_val((int64_t)mmb_as_float(a[0]));
		return 1;
	}
	if (match_fun("CINT"))
	{
		double x;
		call_args(a, 1, &n);
		if (n != 1)
			mmb_syntax();
		x = mmb_as_float(a[0]);
		*out = mmb_int_val(x >= 0.0 ? (int64_t)floor(x + 0.5)
					     : (int64_t)ceil(x - 0.5));
		return 1;
	}
	if (match_fun("EVAL"))
	{
		char buf[MMB_MAX_STR + 1];
		const char *savep;

		call_args(a, 1, &n);
		if (n != 1 || a[0].type != T_STR)
			mmb_error("?TYPE MISMATCH");
		strncpy(buf, a[0].s, MMB_MAX_STR);
		buf[MMB_MAX_STR] = 0;
		savep = G.p;
		{
			char tbuf[MMB_LINE_LEN];
			mmb_tokenize_text(buf, tbuf, (int)sizeof(tbuf));
			G.p = tbuf;
			*out = mmb_expr();
		}
		G.p = savep;
		return 1;
	}
	if (match_fun("MATH"))
	{
		G.p++;
		return mmb_try_math_fn(out);
	}
	if (mmb_match("SQR") || mmb_match("SQRT"))
	{
		call_args(a, 1, &n);
		if (n != 1)
			mmb_syntax();
		*out = mmb_num_val(sqrt(mmb_as_float(a[0])));
		return 1;
	}
	if (mmb_match("SIN"))
	{
		double x;
		call_args(a, 1, &n);
		if (n != 1)
			mmb_syntax();
		x = mmb_as_float(a[0]);
		if (G.opt.angle_degrees)
			x *= 3.14159265358979323846 / 180.0;
		*out = mmb_num_val(sin(x));
		return 1;
	}
	if (mmb_match("COS"))
	{
		double x;
		call_args(a, 1, &n);
		if (n != 1)
			mmb_syntax();
		x = mmb_as_float(a[0]);
		if (G.opt.angle_degrees)
			x *= 3.14159265358979323846 / 180.0;
		*out = mmb_num_val(cos(x));
		return 1;
	}
	if (mmb_match("TAN"))
	{
		double x;
		call_args(a, 1, &n);
		if (n != 1)
			mmb_syntax();
		x = mmb_as_float(a[0]);
		if (G.opt.angle_degrees)
			x *= 3.14159265358979323846 / 180.0;
		*out = mmb_num_val(tan(x));
		return 1;
	}
	if (mmb_match("ATN") || mmb_match("ATN2") || mmb_match("ATAN"))
	{
		double x;
		call_args(a, 2, &n);
		if (n == 2)
		{
			x = atan2(mmb_as_float(a[0]), mmb_as_float(a[1]));
		}
		else if (n == 1)
			x = atan(mmb_as_float(a[0]));
		else
			mmb_syntax();
		if (G.opt.angle_degrees)
			x *= 180.0 / 3.14159265358979323846;
		*out = mmb_num_val(x);
		return 1;
	}
	if (mmb_match("RND"))
	{
		if (G.rnd_seed == 0)
			G.rnd_seed = 0x12345678u;
		mmb_skip_sp();
		if (*G.p == '(')
		{
			call_args(a, 1, &n);
			if (n == 1 && mmb_as_int(a[0]) < 0)
				G.rnd_seed = (uint32_t)(-mmb_as_int(a[0]));
		}
		G.rnd_seed = G.rnd_seed * 1664525u + 1013904223u;
		*out = mmb_num_val((G.rnd_seed >> 8) / 16777216.0);
		return 1;
	}
	if (mmb_match("MM.HRES"))
	{
		*out = mmb_int_val(G.gfx.w);
		return 1;
	}
	if (mmb_match("MM.VRES"))
	{
		*out = mmb_int_val(G.gfx.h);
		return 1;
	}
	if (mmb_match("MM.INFO$") || mmb_match("MM.INFO"))
	{
		call_args(a, 2, &n);
		if (n >= 1)
		{
			char key[40];
			if (a[0].type == T_STR)
				strncpy(key, a[0].s, sizeof(key) - 1);
			else
				key[0] = 0;
			mmb_upper(key);
			if (mmb_keyword_eq(key, "MODE") || peek_kw("MODE"))
			{
				/* MM.INFO(MODE) -> 1.8 style */
				*out = mmb_num_val(G.gfx.mode + G.gfx.bits / 100.0);
				return 1;
			}
			if (mmb_keyword_eq(key, "HRES"))
			{
				*out = mmb_int_val(G.gfx.w);
				return 1;
			}
			if (mmb_keyword_eq(key, "AUDIO") || peek_kw("AUDIO"))
			{
				*out = mmb_str_val(G.opt.audio_target ? "HDMI" : "JACK");
				return 1;
			}
		}
		/* MM.INFO(MODE) with keyword inside parens already consumed by call_args as expr - handle MODE ident */
		*out = mmb_num_val(G.gfx.mode + G.gfx.bits / 100.0);
		return 1;
	}
	if (mmb_match("PLAYING"))
	{
		mmb_skip_sp();
		if (*G.p == '(')
		{
			G.p++;
			mmb_expect(')');
		}
		*out = mmb_int_val(G.audio.playing && !G.audio.paused ? 1 : 0);
		return 1;
	}
	if (mmb_match("EOF"))
	{
		int fn;
		mmb_skip_sp();
		mmb_expect('(');
		mmb_skip_sp();
		if (*G.p == '#')
			G.p++;
		fn = (int)mmb_as_int(mmb_expr());
		mmb_expect(')');
		if (fn < 1 || fn > MMB_MAX_FILES || !G.files[fn].open)
			*out = mmb_int_val(1);
		else
		{
			int sz = mmb_vfs_size(G.files[fn].path);
			*out = mmb_int_val(G.files[fn].pos >= sz ? 1 : 0);
		}
		return 1;
	}
	if (mmb_match("INSTR"))
	{
		int start = 1, i, j, len, nlen;
		char *hay = "", *ndl = "";
		call_args(a, 3, &n);
		/* CMM2: INSTR([start,] haystack$, needle$) */
		if (n == 3)
		{
			if (a[1].type != T_STR || a[2].type != T_STR)
				mmb_error("?TYPE MISMATCH");
			start = (int)mmb_as_int(a[0]);
			hay = a[1].s;
			ndl = a[2].s;
		}
		else if (n == 2)
		{
			if (a[0].type != T_STR || a[1].type != T_STR)
				mmb_error("?TYPE MISMATCH");
			hay = a[0].s;
			ndl = a[1].s;
		}
		else
			mmb_error("?TYPE MISMATCH");
		if (start < 1)
			start = 1;
		len = (int)strlen(hay);
		nlen = (int)strlen(ndl);
		if (nlen == 0)
		{
			*out = mmb_int_val(start);
			return 1;
		}
		for (i = start - 1; i <= len - nlen; i++)
		{
			for (j = 0; j < nlen && hay[i + j] == ndl[j]; j++)
				;
			if (j == nlen)
			{
				*out = mmb_int_val(i + 1);
				return 1;
			}
		}
		*out = mmb_int_val(0);
		return 1;
	}
	if (mmb_match("STRING$"))
	{
		int k, i;
		char b[MMB_MAX_STR + 1];
		char ch = ' ';
		call_args(a, 2, &n);
		if (n != 2)
			mmb_syntax();
		k = (int)mmb_as_int(a[0]);
		if (a[1].type == T_STR && a[1].s[0])
			ch = a[1].s[0];
		else
			ch = (char)mmb_as_int(a[1]);
		if (k < 0)
			k = 0;
		if (k > MMB_MAX_STR)
			k = MMB_MAX_STR;
		for (i = 0; i < k; i++)
			b[i] = ch;
		b[k] = 0;
		*out = mmb_str_val(b);
		return 1;
	}
	if (mmb_match("HEX$"))
	{
		char b[32];
		int64_t v;
		int i, nhex;
		call_args(a, 1, &n);
		if (n != 1)
			mmb_syntax();
		v = mmb_as_int(a[0]);
		if (v < 0)
			v = -v;
		b[31] = 0;
		nhex = 0;
		if (v == 0)
			b[nhex++] = '0';
		while (v)
		{
			int d = (int)(v & 15);
			b[nhex++] = (char)(d < 10 ? '0' + d : 'A' + d - 10);
			v >>= 4;
		}
		for (i = 0; i < nhex / 2; i++)
		{
			char t = b[i];
			b[i] = b[nhex - 1 - i];
			b[nhex - 1 - i] = t;
		}
		b[nhex] = 0;
		*out = mmb_str_val(b);
		return 1;
	}
	if (mmb_match("OCT$"))
	{
		char b[32];
		char *p;
		int64_t v;
		call_args(a, 1, &n);
		if (n != 1)
			mmb_syntax();
		v = mmb_as_int(a[0]);
		if (v < 0)
			v = -v;
		p = b + sizeof(b) - 1;
		*p = 0;
		if (v == 0)
			*--p = '0';
		while (v)
		{
			*--p = (char)('0' + (v % 8));
			v /= 8;
		}
		*out = mmb_str_val(p);
		return 1;
	}
	if (mmb_match("BIN$"))
	{
		char b[66];
		int64_t v;
		int i, nb;
		call_args(a, 1, &n);
		if (n != 1)
			mmb_syntax();
		v = mmb_as_int(a[0]);
		if (v < 0)
			v = -v;
		nb = 0;
		if (v == 0)
			b[nb++] = '0';
		while (v)
		{
			b[nb++] = (char)('0' + (v & 1));
			v >>= 1;
		}
		for (i = 0; i < nb / 2; i++)
		{
			char t = b[i];
			b[i] = b[nb - 1 - i];
			b[nb - 1 - i] = t;
		}
		b[nb] = 0;
		*out = mmb_str_val(b);
		return 1;
	}
	if (mmb_match("DATE$"))
	{
		mmb_clock_refresh();
		*out = mmb_str_val(G.date_s[0] ? G.date_s : "1-1-26");
		return 1;
	}
	if (mmb_match("TIME$"))
	{
		mmb_clock_refresh();
		*out = mmb_str_val(G.time_s[0] ? G.time_s : "12:00:00");
		return 1;
	}
	if (mmb_match("INKEY$"))
	{
		int c;
		char b[2];
		mmb_skip_sp();
		if (*G.p == '(')
		{
			G.p++;
			mmb_expect(')');
		}
		c = mmb_inkey_pop();
		if (c < 0)
			*out = mmb_str_val("");
		else
		{
			b[0] = (char)c;
			b[1] = 0;
			*out = mmb_str_val(b);
		}
		return 1;
	}
	if (match_fun("KEYDOWN"))
	{
		int narg = 0;
		call_args(a, 1, &narg);
		*out = mmb_int_val(mmb_keydown_get(narg ? (int)mmb_as_int(a[0]) : 0));
		return 1;
	}
	if (mmb_match("TIMER"))
	{
		mmb_skip_sp();
		if (*G.p == '(')
		{
			G.p++;
			mmb_expect(')');
		}
		*out = mmb_int_val((int64_t)mmb_now_ms() - G.timer_base);
		return 1;
	}
	if (mmb_match("LOF"))
	{
		int fn, sz;
		mmb_skip_sp();
		mmb_expect('(');
		mmb_skip_sp();
		if (*G.p == '#')
			G.p++;
		fn = (int)mmb_as_int(mmb_expr());
		mmb_expect(')');
		if (fn < 1 || fn > MMB_MAX_FILES || !G.files[fn].open)
			mmb_error("?FILE");
		sz = mmb_vfs_size(G.files[fn].path);
		*out = mmb_int_val(sz < 0 ? 0 : sz);
		return 1;
	}
	if (mmb_match("CWD$"))
	{
		mmb_skip_sp();
		if (*G.p == '(')
		{
			G.p++;
			mmb_expect(')');
		}
		*out = mmb_str_val(mmb_vfs_cwd());
		return 1;
	}
	if (mmb_match("INPUT$"))
	{
		int fn, nch = 0;
		char b[MMB_MAX_STR + 1];
		call_args(a, 2, &n);
		if (n != 2 || a[0].type != T_INT && a[0].type != T_NUM)
			mmb_syntax();
		if (a[1].type != T_INT && a[1].type != T_NUM)
			mmb_syntax();
		fn = (int)mmb_as_int(a[0]);
		nch = (int)mmb_as_int(a[1]);
		if (fn < 1 || fn > MMB_MAX_FILES || !G.files[fn].open)
			mmb_error("?FILE");
		{
			unsigned got = 0;
			if (nch < 0)
				nch = 0;
			if (nch > MMB_MAX_STR)
				nch = MMB_MAX_STR;
			if (mmb_vfs_read_at(G.files[fn].path, (unsigned)G.files[fn].pos,
					    b, (unsigned)nch, &got) != 0)
				got = 0;
			G.files[fn].pos += (int)got;
			b[got] = 0;
			*out = mmb_str_val(b);
		}
		return 1;
	}
	if (mmb_match("ACOS") || mmb_match("ACS"))
	{
		call_args(a, 1, &n);
		if (n != 1)
			mmb_syntax();
		{
			double x = acos(mmb_as_float(a[0]));
			if (G.opt.angle_degrees)
				x *= 180.0 / 3.14159265358979323846;
			*out = mmb_num_val(x);
		}
		return 1;
	}
	if (mmb_match("ASIN") || mmb_match("ASN"))
	{
		call_args(a, 1, &n);
		if (n != 1)
			mmb_syntax();
		{
			double x = asin(mmb_as_float(a[0]));
			if (G.opt.angle_degrees)
				x *= 180.0 / 3.14159265358979323846;
			*out = mmb_num_val(x);
		}
		return 1;
	}
	if (match_fun("MAX"))
	{
		call_args(a, 8, &n);
		if (n < 1)
			mmb_syntax();
		{
			double m = mmb_as_float(a[0]);
			int i;
			for (i = 1; i < n; i++)
				if (mmb_as_float(a[i]) > m)
					m = mmb_as_float(a[i]);
			*out = mmb_num_val(m);
		}
		return 1;
	}
	if (match_fun("MIN"))
	{
		call_args(a, 8, &n);
		if (n < 1)
			mmb_syntax();
		{
			double m = mmb_as_float(a[0]);
			int i;
			for (i = 1; i < n; i++)
				if (mmb_as_float(a[i]) < m)
					m = mmb_as_float(a[i]);
			*out = mmb_num_val(m);
		}
		return 1;
	}
	if (mmb_match("LOC"))
	{
		int fn;
		mmb_skip_sp();
		mmb_expect('(');
		mmb_skip_sp();
		if (*G.p == '#')
			G.p++;
		fn = (int)mmb_as_int(mmb_expr());
		mmb_expect(')');
		if (fn < 1 || fn > MMB_MAX_FILES || !G.files[fn].open)
			mmb_error("?FILE");
		*out = mmb_int_val(G.files[fn].pos);
		return 1;
	}
	if (mmb_match("SGN"))
	{
		call_args(a, 1, &n);
		if (n != 1)
			mmb_syntax();
		{
			double x = mmb_as_float(a[0]);
			*out = mmb_int_val(x > 0 ? 1 : (x < 0 ? -1 : 0));
		}
		return 1;
	}
	if (mmb_match("EXP"))
	{
		call_args(a, 1, &n);
		if (n != 1)
			mmb_syntax();
		*out = mmb_num_val(exp(mmb_as_float(a[0])));
		return 1;
	}
	if (mmb_match("LOG"))
	{
		call_args(a, 1, &n);
		if (n != 1)
			mmb_syntax();
		*out = mmb_num_val(log(mmb_as_float(a[0])));
		return 1;
	}
	if (mmb_match("PI"))
	{
		mmb_skip_sp();
		if (*G.p == '(')
		{
			G.p++;
			mmb_expect(')');
		}
		*out = mmb_num_val(3.14159265358979323846);
		return 1;
	}
	if (match_fun("DEG"))
	{
		call_args(a, 1, &n);
		if (n != 1)
			mmb_syntax();
		*out = mmb_num_val(mmb_as_float(a[0]) * 180.0 / 3.14159265358979323846);
		return 1;
	}
	if (match_fun("RAD"))
	{
		call_args(a, 1, &n);
		if (n != 1)
			mmb_syntax();
		*out = mmb_num_val(mmb_as_float(a[0]) * 3.14159265358979323846 / 180.0);
		return 1;
	}
	if (match_fun("POS"))
	{
		call_args(a, 1, &n);
		(void)n;
		*out = mmb_int_val(1);
		return 1;
	}
	if (match_fun("CHOICE"))
	{
		call_args(a, 3, &n);
		if (n != 3)
			mmb_syntax();
		*out = mmb_as_int(a[0]) ? a[1] : a[2];
		return 1;
	}
	if (match_fun("FORMAT$"))
	{
		call_args(a, 2, &n);
		if (n < 1)
			mmb_syntax();
		{
			char buf[48];
			mmb_val num = a[0];
			int64_t v, neg = 0;
			int k = 0;
			char tmp[32];
			if (n >= 1 && a[0].type == T_STR && n >= 2)
				num = a[1];
			v = mmb_as_int(num);
			if (v < 0)
			{
				neg = 1;
				v = -v;
			}
			if (v == 0)
				tmp[k++] = '0';
			while (v && k < 30)
			{
				tmp[k++] = (char)('0' + (int)(v % 10));
				v /= 10;
			}
			if (neg)
				tmp[k++] = '-';
			{
				int i;
				for (i = 0; i < k; i++)
					buf[i] = tmp[k - 1 - i];
				buf[k] = 0;
			}
			*out = mmb_str_val(buf);
		}
		return 1;
	}
	if (match_fun("BOUND"))
	{
		char name[MMB_MAX_NAME];
		int dim = 1, i;
		mmb_var *v;
		mmb_skip_sp();
		mmb_expect('(');
		mmb_ident(name, sizeof(name));
		mmb_type_suffix(name);
		mmb_skip_sp();
		if (*G.p == '(')
		{
			G.p++;
			mmb_skip_sp();
			mmb_expect(')');
		}
		mmb_skip_sp();
		if (*G.p == ',')
		{
			G.p++;
			dim = (int)mmb_as_int(mmb_expr());
		}
		mmb_expect(')');
		v = 0;
		for (i = 0; i < MMB_MAX_VARS; i++)
			if (G.vars[i].used && mmb_keyword_eq(G.vars[i].name, name))
			{
				v = &G.vars[i];
				break;
			}
		if (!v)
			mmb_error("?UNDECLARED");
		if (dim < 1)
			dim = 1;
		if (dim > v->dims)
			*out = mmb_int_val(0);
		else
			*out = mmb_int_val(v->dim[dim - 1]);
		return 1;
	}
	if (match_fun("TAB"))
	{
		call_args(a, 1, &n);
		if (n != 1)
			mmb_syntax();
		{
			int sp = (int)mmb_as_int(a[0]);
			char buf[MMB_MAX_STR + 1];
			int i;
			if (sp < 0)
				sp = 0;
			if (sp > MMB_MAX_STR)
				sp = MMB_MAX_STR;
			for (i = 0; i < sp; i++)
				buf[i] = ' ';
			buf[sp] = 0;
			*out = mmb_str_val(buf);
		}
		return 1;
	}
	if (mmb_match("MM.VER"))
	{
		mmb_skip_sp();
		if (*G.p == '(')
		{
			G.p++;
			mmb_expect(')');
		}
		*out = mmb_str_val("1.8");
		return 1;
	}
	if (mmb_match("MM.DEVICE$"))
	{
		mmb_skip_sp();
		if (*G.p == '(')
		{
			G.p++;
			mmb_expect(')');
		}
		*out = mmb_str_val("Colour Maximite 2");
		return 1;
	}
	if (mmb_match("MM.CMDLINE$"))
	{
		mmb_skip_sp();
		if (*G.p == '(')
		{
			G.p++;
			mmb_expect(')');
		}
		*out = mmb_str_val(G.current_prog[0] ? G.current_prog : "");
		return 1;
	}
ident_tail:
	if (mmb_try_user_function(out))
		return 1;

	/* named colours as identifiers */
	{
		char name[32];
		const char *save2 = G.p;
		int ok = 0;
		unsigned col;
		mmb_skip_sp();
		if (mmb_tok_expand(name, (int)sizeof(name)))
		{
			col = mmb_named_colour(name, &ok);
			if (ok)
			{
				*out = mmb_int_val((int64_t)col);
				return 1;
			}
			G.p = save2;
		}
		else
		{
			int i = 0;
			while (mmb_is_ident(*G.p) && i < 31)
				name[i++] = *G.p++;
			name[i] = 0;
			if (i)
			{
				col = mmb_named_colour(name, &ok);
				if (ok)
				{
					*out = mmb_int_val((int64_t)col);
					return 1;
				}
			}
			G.p = save;
		}
	}
	(void)save;
	return 0;
}

static mmb_val expr_primary(void)
{
	mmb_val v;
	mmb_skip_sp();
	if (*G.p == '"')
		return parse_string();
	if (mmb_is_digit(*G.p) || (G.p[0] == '.' && mmb_is_digit(G.p[1])) || G.p[0] == '&')
		return parse_number();
	if (*G.p == '(')
	{
		G.p++;
		v = mmb_expr();
		mmb_expect(')');
		return v;
	}
	if (mmb_try_function(&v))
		return v;
	/* variable */
	{
		char name[MMB_MAX_NAME];
		int nidx = 0, idx[MMB_MAX_DIMS], off = 0, t;
		mmb_var *var;
		mmb_val cv;
		t = mmb_parse_var_ref(name, &nidx, idx);
		if (nidx == 0 && mmb_const_lookup(name, t, &cv))
			return cv;
		var = mmb_find_var(name, t, 1, nidx, idx);
		if (nidx)
		{
			int i;
			/* idx overwritten? recompute */
			off = 0;
			{
				int stride = 1;
				for (i = var->dims - 1; i >= 0; i--)
				{
					if (idx[i] < G.opt.base || idx[i] > var->dim[i])
						mmb_error("?INDEX OUT OF BOUNDS");
					off += (idx[i] - G.opt.base) * stride;
					stride *= (var->dim[i] - G.opt.base + 1);
				}
			}
		}
		return mmb_load_var(var, off);
	}
}

static mmb_val expr_unary(void)
{
	mmb_skip_sp();
	if (*G.p == '-')
	{
		G.p++;
		{
			mmb_val v = expr_unary();
			if (v.type == T_INT)
				return mmb_int_val(-v.i);
			return mmb_num_val(-mmb_as_float(v));
		}
	}
	if (*G.p == '+')
	{
		G.p++;
		return expr_unary();
	}
	mmb_skip_sp();
	if (((unsigned char)*G.p == 0x80 || *G.p == 'N' || *G.p == 'n') && mmb_match("NOT"))
	{
		mmb_val v = expr_unary();
		return mmb_int_val(mmb_as_int(v) ? 0 : 1);
	}
	return expr_primary();
}

static mmb_val expr_pow(void)
{
	mmb_val a = expr_unary();
	mmb_skip_sp();
	if (*G.p == '^')
	{
		G.p++;
		{
			mmb_val b = expr_unary();
			return mmb_num_val(pow(mmb_as_float(a), mmb_as_float(b)));
		}
	}
	return a;
}

static mmb_val expr_mul(void)
{
	mmb_val a = expr_pow();
	for (;;)
	{
		mmb_skip_sp();
		if (*G.p == '*')
		{
			G.p++;
			{
				mmb_val b = expr_pow();
				if (a.type == T_INT && b.type == T_INT)
					a = mmb_int_val(a.i * b.i);
				else
					a = mmb_num_val(mmb_as_float(a) * mmb_as_float(b));
			}
		}
		else if (*G.p == '/')
		{
			G.p++;
			{
				mmb_val b = expr_pow();
				double d = mmb_as_float(b);
				a = mmb_num_val(d == 0 ? 0 : mmb_as_float(a) / d);
			}
		}
		else if (*G.p == '\\')
		{
			G.p++;
			{
				mmb_val b = expr_pow();
				int64_t d = mmb_as_int(b);
				a = mmb_int_val(d == 0 ? 0 : mmb_as_int(a) / d);
			}
		}
		else if (((unsigned char)*G.p == 0x80 || *G.p == 'M' || *G.p == 'm') && mmb_match("MOD"))
		{
			mmb_val b = expr_pow();
			int64_t d = mmb_as_int(b);
			a = mmb_int_val(d == 0 ? 0 : mmb_as_int(a) % d);
		}
		else
			break;
	}
	return a;
}

static mmb_val expr_add(void)
{
	mmb_val a = expr_mul();
	for (;;)
	{
		mmb_skip_sp();
		if (*G.p == '+')
		{
			G.p++;
			{
				mmb_val b = expr_mul();
				if (a.type == T_STR || b.type == T_STR)
				{
					char buf[MMB_MAX_STR + 1];
					if (a.type != T_STR || b.type != T_STR)
						mmb_error("?TYPE MISMATCH");
					strncpy(buf, a.s, MMB_MAX_STR);
					buf[MMB_MAX_STR] = 0;
					strncat(buf, b.s, MMB_MAX_STR - strlen(buf));
					a = mmb_str_val(buf);
				}
				else if (a.type == T_INT && b.type == T_INT)
					a = mmb_int_val(a.i + b.i);
				else
					a = mmb_num_val(mmb_as_float(a) + mmb_as_float(b));
			}
		}
		else if (*G.p == '-')
		{
			G.p++;
			{
				mmb_val b = expr_mul();
				if (a.type == T_INT && b.type == T_INT)
					a = mmb_int_val(a.i - b.i);
				else
					a = mmb_num_val(mmb_as_float(a) - mmb_as_float(b));
			}
		}
		else
			break;
	}
	return a;
}

static mmb_val expr_rel(void)
{
	mmb_val a = expr_add();
	mmb_skip_sp();
	{
		int op = 0; /* 1= 2<> 3< 4> 5<= 6>= */
		if (G.p[0] == '<' && G.p[1] == '>')
		{
			op = 2;
			G.p += 2;
		}
		else if (G.p[0] == '<' && G.p[1] == '=')
		{
			op = 5;
			G.p += 2;
		}
		else if (G.p[0] == '>' && G.p[1] == '=')
		{
			op = 6;
			G.p += 2;
		}
		else if (*G.p == '=')
		{
			op = 1;
			G.p++;
		}
		else if (*G.p == '<')
		{
			op = 3;
			G.p++;
		}
		else if (*G.p == '>')
		{
			op = 4;
			G.p++;
		}
		if (op)
		{
			mmb_val b = expr_add();
			int r = 0;
			if (a.type == T_STR || b.type == T_STR)
			{
				int c = strcmp(a.s, b.s);
				if (op == 1) r = c == 0;
				else if (op == 2) r = c != 0;
				else if (op == 3) r = c < 0;
				else if (op == 4) r = c > 0;
				else if (op == 5) r = c <= 0;
				else r = c >= 0;
			}
			else
			{
				double x = mmb_as_float(a), y = mmb_as_float(b);
				if (op == 1) r = x == y;
				else if (op == 2) r = x != y;
				else if (op == 3) r = x < y;
				else if (op == 4) r = x > y;
				else if (op == 5) r = x <= y;
				else r = x >= y;
			}
			return mmb_int_val(r);
		}
	}
	return a;
}

static mmb_val expr_and(void)
{
	mmb_val a = expr_rel();
	for (;;)
	{
		mmb_skip_sp();
		if (!(((unsigned char)*G.p == 0x80 || *G.p == 'A' || *G.p == 'a') && mmb_match("AND")))
			break;
		{
			mmb_val b = expr_rel();
			a = mmb_int_val(mmb_as_int(a) && mmb_as_int(b));
		}
	}
	return a;
}

static mmb_val expr_or(void)
{
	mmb_val a = expr_and();
	for (;;)
	{
		mmb_skip_sp();
		if (((unsigned char)*G.p == 0x80 || *G.p == 'O' || *G.p == 'o') && mmb_match("OR"))
		{
			mmb_val b = expr_and();
			a = mmb_int_val(mmb_as_int(a) || mmb_as_int(b));
		}
		else if (((unsigned char)*G.p == 0x80 || *G.p == 'X' || *G.p == 'x') && mmb_match("XOR"))
		{
			mmb_val b = expr_and();
			a = mmb_int_val((mmb_as_int(a) != 0) ^ (mmb_as_int(b) != 0));
		}
		else
			break;
	}
	return a;
}

mmb_val mmb_expr(void)
{
	if (G.opt.profiling && G.running)
		G.prof.expr++;
	return expr_or();
}
