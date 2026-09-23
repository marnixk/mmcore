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
	char buf[MMB_LINE_LEN];
	int n = 0;
	G.p++; /* quote */
	while (*G.p)
	{
		if (*G.p == '"')
		{
			if (G.p[1] == '"')
			{
				if (n < MMB_LINE_LEN - 1)
					buf[n++] = '"';
				G.p += 2;
				continue;
			}
			break;
		}
		if (n < MMB_LINE_LEN - 1)
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

static void fmt2d(char *p, int v)
{
	p[0] = (char)('0' + (v / 10) % 10);
	p[1] = (char)('0' + v % 10);
}

static int64_t parse_epoch_arg(void)
{
	mmb_skip_sp();
	if (*G.p == '(')
	{
		mmb_val v;
		G.p++;
		mmb_skip_sp();
		if (mmb_match("NOW"))
			v = mmb_int_val(mmb_epoch_now());
		else
			v = mmb_expr();
		mmb_skip_sp();
		mmb_expect(')');
		return mmb_as_int(v);
	}
	if (mmb_match("NOW"))
		return mmb_epoch_now();
	return mmb_as_int(mmb_expr());
}

static int parse_date_fields(const char *s, int *py, int *pmo, int *pd)
{
	int vals[3], nv = 0, i;
	const char *p = s;
	for (i = 0; i < 3; i++)
	{
		int v = 0, got = 0;
		while (*p == ' ' || *p == '\t')
			p++;
		while (*p >= '0' && *p <= '9')
		{
			v = v * 10 + (*p - '0');
			p++;
			got = 1;
		}
		if (!got)
			break;
		vals[nv++] = v;
		while (*p == ' ' || *p == '\t')
			p++;
		if (*p == '-' || *p == '/')
			p++;
		else if (i < 2)
			break;
	}
	if (nv < 3)
		return 0;
	{
		int d = vals[0], mo = vals[1], y = vals[2];
		if (d > 1000)
		{
			int t = d;
			d = y;
			y = t;
		}
		if (y >= 0 && y < 100)
			y += 2000;
		if (d < 1 || d > 31 || mo < 1 || mo > 12 || y < 1902 || y > 2999)
			return 0;
		*py = y;
		*pmo = mo;
		*pd = d;
	}
	return 1;
}

static int parse_time_fields(const char *s, int *ph, int *pmi, int *ps)
{
	int vals[3], nv = 0, i;
	const char *p = s;
	for (i = 0; i < 3; i++)
	{
		int v = 0, got = 0;
		while (*p == ' ' || *p == '\t')
			p++;
		while (*p >= '0' && *p <= '9')
		{
			v = v * 10 + (*p - '0');
			p++;
			got = 1;
		}
		if (!got)
			break;
		vals[nv++] = v;
		while (*p == ' ' || *p == '\t')
			p++;
		if (*p == ':')
			p++;
		else
			break;
	}
	if (nv < 1)
		return 0;
	*ph = vals[0];
	*pmi = nv > 1 ? vals[1] : 0;
	*ps = nv > 2 ? vals[2] : 0;
	if (*ph < 0 || *ph > 23 || *pmi < 0 || *pmi > 59 || *ps < 0 || *ps > 59)
		return 0;
	return 1;
}

static void datetime_str(char *out, int outsz, int64_t e)
{
	int y, mo, d, h, mi, s;
	char *p = out;
	(void)outsz;
	mmb_epoch_break(e + (int64_t)mmb_tz_offset_min() * 60, &y, &mo, &d, &h, &mi, &s);
	fmt2d(p, d); p[2] = '-';
	fmt2d(p + 3, mo); p[5] = '-';
	/* year is always 4 digits in this range */
	p[6] = (char)('0' + (y / 1000) % 10);
	p[7] = (char)('0' + (y / 100) % 10);
	p[8] = (char)('0' + (y / 10) % 10);
	p[9] = (char)('0' + y % 10);
	p[10] = ' ';
	fmt2d(p + 11, h); p[13] = ':';
	fmt2d(p + 14, mi); p[16] = ':';
	fmt2d(p + 17, s);
	p[19] = 0;
}

static int in_charset(const char *set, char c)
{
	for (; *set; set++)
		if (*set == c)
			return 1;
	return 0;
}

static void trim_str(char *dst, const char *src, const char *mask, int left, int right)
{
	int s = 0, e = (int)strlen(src) - 1;
	if (left)
		while (s <= e && in_charset(mask, src[s]))
			s++;
	if (right)
		while (e >= s && in_charset(mask, src[e]))
			e--;
	if (e < s)
	{
		dst[0] = 0;
		return;
	}
	memcpy(dst, src + s, (size_t)(e - s + 1));
	dst[e - s + 1] = 0;
}

static int field_scan(const char *s, int start, const char *delims, const char *quotes)
{
	int i = start;
	while (s[i] && !in_charset(delims, s[i]))
	{
		if (in_charset(quotes, s[i]))
		{
			char q = s[i++];
			while (s[i] && s[i] != q)
				i++;
		}
		if (s[i])
			i++;
	}
	return i;
}

#define MMB_DIR_BUF 4096
static char s_dir_list[MMB_DIR_BUF];
static int s_dir_len;
static int s_dir_pos;
static char s_dir_filter[8] = "ALL";

static int dir_next(const char *filter, char *out, int outsz)
{
	while (s_dir_pos < s_dir_len)
	{
		int start = s_dir_pos, end, len, isdir, take = 1;
		if (s_dir_list[s_dir_pos] == '\n')
		{
			s_dir_pos++;
			continue;
		}
		while (s_dir_pos < s_dir_len && s_dir_list[s_dir_pos] != '\n')
			s_dir_pos++;
		end = s_dir_pos;
		len = end - start;
		isdir = len > 0 && s_dir_list[end - 1] == '/';
		if (filter && filter[0])
		{
			if (mmb_keyword_eq(filter, "DIR") && !isdir)
				take = 0;
			else if (mmb_keyword_eq(filter, "FILE") && isdir)
				take = 0;
		}
		if (take)
		{
			int c;
			if (isdir)
				len--;
			c = len < outsz - 1 ? len : outsz - 1;
			memcpy(out, s_dir_list + start, (unsigned)c);
			out[c] = 0;
			return 1;
		}
	}
	out[0] = 0;
	return 0;
}

static void theme_name_copy(char *dst, int n, const char *src)
{
	int i;
	if (!dst || n < 1)
		return;
	if (!src)
		src = "";
	for (i = 0; src[i] && i < n - 1; i++)
		dst[i] = src[i];
	dst[i] = 0;
}

int mmb_try_function(mmb_val *out)
{
	mmb_val a[8];
	int n = 0, ok;
	unsigned col;
	const char *save = G.p;
	static void *fun_tab[512];
	static int finited;

	if (mmb_try_struct_fun(out))
		return 1;

	if (!finited)
	{
		fun_tab[mmb_kw_id("RGB")] = &&lbl_rgb;
		fun_tab[mmb_kw_id("THEME")] = &&lbl_theme;
		fun_tab[mmb_kw_id("PIXEL")] = &&lbl_pixel;
		fun_tab[mmb_kw_id("LEN")] = &&lbl_len;
		fun_tab[mmb_kw_id("ASC")] = &&lbl_asc;
		fun_tab[mmb_kw_id("CHR$")] = &&lbl_chr;
		fun_tab[mmb_kw_id("STR$")] = &&lbl_str;
		fun_tab[mmb_kw_id("VAL")] = &&lbl_val;
		fun_tab[mmb_kw_id("LEFT$")] = &&lbl_left;
		fun_tab[mmb_kw_id("RIGHT$")] = &&lbl_right;
		fun_tab[mmb_kw_id("MID$")] = &&lbl_mid;
		fun_tab[mmb_kw_id("UCASE$")] = &&lbl_ucase;
		fun_tab[mmb_kw_id("LCASE$")] = &&lbl_lcase;
		fun_tab[mmb_kw_id("SPACE$")] = &&lbl_space;
		fun_tab[mmb_kw_id("LTRIM$")] = &&lbl_ltrim;
		fun_tab[mmb_kw_id("RTRIM$")] = &&lbl_rtrim;
		fun_tab[mmb_kw_id("TRIM$")] = &&lbl_trim;
		fun_tab[mmb_kw_id("BASE$")] = &&lbl_base;
		fun_tab[mmb_kw_id("FIELD$")] = &&lbl_field;
		fun_tab[mmb_kw_id("DATETIME$")] = &&lbl_datetime;
		fun_tab[mmb_kw_id("DAY$")] = &&lbl_day;
		fun_tab[mmb_kw_id("DIR$")] = &&lbl_dir;
		fun_tab[mmb_kw_id("ABS")] = &&lbl_abs;
		fun_tab[mmb_kw_id("INT")] = &&lbl_int;
		fun_tab[mmb_kw_id("FIX")] = &&lbl_fix;
		fun_tab[mmb_kw_id("CINT")] = &&lbl_cint;
		fun_tab[mmb_kw_id("EVAL")] = &&lbl_eval;
		fun_tab[mmb_kw_id("MATH")] = &&lbl_math;
		fun_tab[mmb_kw_id("SQR")] = &&lbl_sqr;
		fun_tab[mmb_kw_id("SQRT")] = &&lbl_sqr;
		fun_tab[mmb_kw_id("SIN")] = &&lbl_sin;
		fun_tab[mmb_kw_id("COS")] = &&lbl_cos;
		fun_tab[mmb_kw_id("TAN")] = &&lbl_tan;
		fun_tab[mmb_kw_id("ATN")] = &&lbl_atn;
		fun_tab[mmb_kw_id("ATN2")] = &&lbl_atn;
		fun_tab[mmb_kw_id("ATAN")] = &&lbl_atn;
		fun_tab[mmb_kw_id("ATAN2")] = &&lbl_atn;
		fun_tab[mmb_kw_id("RND")] = &&lbl_rnd;
		fun_tab[mmb_kw_id("MM.HRES")] = &&lbl_mmhres;
		fun_tab[mmb_kw_id("MM.VRES")] = &&lbl_mmvres;
		fun_tab[mmb_kw_id("MM.HOST.HRES")] = &&lbl_mmhosthres;
		fun_tab[mmb_kw_id("MM.HOST.VRES")] = &&lbl_mmhostvres;
		fun_tab[mmb_kw_id("MM.HPOS")] = &&lbl_mmhpos;
		fun_tab[mmb_kw_id("MM.VPOS")] = &&lbl_mmvpos;
		fun_tab[mmb_kw_id("MM.INFO$")] = &&lbl_mminfo;
		fun_tab[mmb_kw_id("MM.INFO")] = &&lbl_mminfo;
		fun_tab[mmb_kw_id("MM.RUNTIME")] = &&lbl_mmruntime;
		fun_tab[mmb_kw_id("MM.RUNTIME$")] = &&lbl_mmruntime;
		fun_tab[mmb_kw_id("PLAYING")] = &&lbl_playing;
		fun_tab[mmb_kw_id("EOF")] = &&lbl_eof;
		fun_tab[mmb_kw_id("INSTR")] = &&lbl_instr;
		fun_tab[mmb_kw_id("STRING$")] = &&lbl_string;
		fun_tab[mmb_kw_id("HEX$")] = &&lbl_hex;
		fun_tab[mmb_kw_id("OCT$")] = &&lbl_oct;
		fun_tab[mmb_kw_id("BIN$")] = &&lbl_bin;
		fun_tab[mmb_kw_id("DATE$")] = &&lbl_date;
		fun_tab[mmb_kw_id("TIME$")] = &&lbl_time;
		fun_tab[mmb_kw_id("INKEY$")] = &&lbl_inkey;
		fun_tab[mmb_kw_id("KEYDOWN")] = &&lbl_keydown;
		fun_tab[mmb_kw_id("TIMER")] = &&lbl_timer;
		fun_tab[mmb_kw_id("LOF")] = &&lbl_lof;
		fun_tab[mmb_kw_id("CWD$")] = &&lbl_cwd;
		fun_tab[mmb_kw_id("INPUT$")] = &&lbl_input;
		fun_tab[mmb_kw_id("JSON$")] = &&lbl_json;
		fun_tab[mmb_kw_id("JSON_STRINGIFY$")] = &&lbl_json_stringify;
		fun_tab[mmb_kw_id("ACOS")] = &&lbl_acos;
		fun_tab[mmb_kw_id("ACS")] = &&lbl_acos;
		fun_tab[mmb_kw_id("ASIN")] = &&lbl_asin;
		fun_tab[mmb_kw_id("ASN")] = &&lbl_asin;
		fun_tab[mmb_kw_id("LOC")] = &&lbl_loc;
		fun_tab[mmb_kw_id("SGN")] = &&lbl_sgn;
		fun_tab[mmb_kw_id("EXP")] = &&lbl_exp;
		fun_tab[mmb_kw_id("LOG")] = &&lbl_log;
		fun_tab[mmb_kw_id("PI")] = &&lbl_pi;
		fun_tab[mmb_kw_id("DEG")] = &&lbl_deg;
		fun_tab[mmb_kw_id("RAD")] = &&lbl_rad;
		fun_tab[mmb_kw_id("POS")] = &&lbl_pos;
		fun_tab[mmb_kw_id("CSRLIN")] = &&lbl_csrlin;
		fun_tab[mmb_kw_id("BIT")] = &&lbl_bit;
		fun_tab[mmb_kw_id("BYTE")] = &&lbl_byte;
		fun_tab[mmb_kw_id("EPOCH")] = &&lbl_epoch;
		fun_tab[mmb_kw_id("CHOICE")] = &&lbl_choice;
		fun_tab[mmb_kw_id("FORMAT$")] = &&lbl_format;
		fun_tab[mmb_kw_id("BOUND")] = &&lbl_bound;
		fun_tab[mmb_kw_id("TAB")] = &&lbl_tab;
		fun_tab[mmb_kw_id("MM.VER")] = &&lbl_mmver;
		fun_tab[mmb_kw_id("MM.DEVICE$")] = &&lbl_mmdev;
		fun_tab[mmb_kw_id("MM.CMDLINE$")] = &&lbl_mmcmd;
		fun_tab[mmb_kw_id("MAX")] = &&lbl_max;
		fun_tab[mmb_kw_id("MIN")] = &&lbl_min;
		finited = 1;
	}
	if ((unsigned char)*G.p == 0x80)
	{
		int id = (unsigned char)G.p[1] | ((unsigned char)G.p[2] << 8);
		if (id > 0 && id < 512 && fun_tab[id])
		{
			char kn[MMB_MAX_NAME];
			const char *sp = G.p;
			if (mmb_tok_expand(kn, (int)sizeof(kn)))
			{
				mmb_skip_sp();
				if ((*G.p == '.' || *G.p == '(') && mmb_lookup_struct_var(kn))
				{
					G.p = sp;
					return 0;
				}
				/* A CONST may shadow a built-in name: CONST MAX=21
				 * then PRINT MAX*2. Only the bare form is shadowed;
				 * MAX(...) still calls the function. */
				if (*G.p != '(')
				{
					mmb_val cv;
					if (mmb_const_lookup(kn, 0, &cv))
					{
						G.p = sp;
						return 0;
					}
				}
			}
			G.p = sp;
			G.p += 3;
			mmb_skip_sp();
			goto *fun_tab[id];
		}
		goto ident_tail;
	}

	{
		const char *s2 = G.p;
		char name[MMB_MAX_NAME];
		int nn = 0, aid;
		mmb_skip_sp();
		if (mmb_is_ident(*G.p) && !(*G.p >= '0' && *G.p <= '9'))
		{
			while (mmb_is_ident(*G.p) && nn < MMB_MAX_NAME - 2)
			{
				char ch = *G.p++;
				if (ch >= 'a' && ch <= 'z')
					ch = (char)(ch - 32);
				name[nn++] = ch;
			}
			if (*G.p == '$' || *G.p == '%' || *G.p == '!')
				name[nn++] = *G.p++;
			name[nn] = 0;
			aid = mmb_kw_id(name);
			if (aid > 0 && aid < 512 && fun_tab[aid])
			{
				mmb_skip_sp();
				if (*G.p != '(')
				{
					mmb_val cv;
					if (mmb_const_lookup(name, 0, &cv))
					{
						G.p = s2;
						return 0;
					}
				}
				goto *fun_tab[aid];
			}
			G.p = s2;
			goto ident_tail;
		}
		G.p = s2;
	}

	if (mmb_match("RGB"))
	{
	lbl_rgb:
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
			if (n == 4)
				*out = mmb_int_val((int64_t)mmb_rgb_pack_a(r, g, b,
							      (int)mmb_as_int(a[3])));
			else
				*out = mmb_int_val((int64_t)mmb_rgb_pack(r, g, b));
			return 1;
		}
		mmb_syntax();
	}
	if (mmb_match("THEME"))
	{
	lbl_theme:
		char nm[40];
		int nn = 0;
		unsigned rgb;
		unsigned char fidx;
		mmb_skip_sp();
		if (*G.p != '(')
		{
			G.p = save;
			return 0;
		}
		G.p++;
		mmb_skip_sp();
		if (*G.p == '"')
		{
			mmb_val v = mmb_expr();
			if (v.type != T_STR)
				mmb_syntax();
			theme_name_copy(nm, sizeof(nm), v.s);
		}
		else
		{
			const char *sp = G.p;
			while ((mmb_is_ident(*G.p) || *G.p == '$' || *G.p == '%' ||
				*G.p == '!') && nn < (int)sizeof(nm) - 1)
				nm[nn++] = *G.p++;
			nm[nn] = 0;
			mmb_skip_sp();
			if (!(*G.p == ')' && nn > 0 && mmb_editor_theme_field(nm, &fidx)))
			{
				mmb_val v;
				G.p = sp;
				v = mmb_expr();
				if (v.type != T_STR)
					mmb_syntax();
				theme_name_copy(nm, sizeof(nm), v.s);
			}
		}
		mmb_skip_sp();
		mmb_expect(')');
		if (!mmb_editor_theme_rgb(nm, &rgb))
			mmb_syntax();
		*out = mmb_int_val((int64_t)rgb);
		return 1;
	}
	if (mmb_match("PIXEL"))
	{
	lbl_pixel:
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
	lbl_len:
		call_args(a, 1, &n);
		if (n != 1 || a[0].type != T_STR)
			mmb_error("?TYPE MISMATCH");
		*out = mmb_int_val((int64_t)strlen(a[0].s));
		return 1;
	}
	if (mmb_match("ASC"))
	{
	lbl_asc:
		call_args(a, 1, &n);
		if (n != 1 || a[0].type != T_STR)
			mmb_error("?TYPE MISMATCH");
		*out = mmb_int_val(a[0].s[0] ? (unsigned char)a[0].s[0] : 0);
		return 1;
	}
	if (mmb_match("CHR$"))
	{
	lbl_chr:
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
	lbl_str:
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
	lbl_val:
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
	lbl_left:
		int k, i, len;
		char *b;
		call_args(a, 2, &n);
		if (n != 2 || a[0].type != T_STR)
			mmb_error("?TYPE MISMATCH");
		k = (int)mmb_as_int(a[1]);
		if (k < 0)
			k = 0;
		len = (int)strlen(a[0].s);
		if (k > len)
			k = len;
		b = mmb_tmp_alloc(k + 1);
		for (i = 0; i < k; i++)
			b[i] = a[0].s[i];
		b[k] = 0;
		*out = mmb_arena_val(b);
		return 1;
	}
	if (mmb_match("RIGHT$"))
	{
	lbl_right:
		int k, i, len;
		char *b;
		call_args(a, 2, &n);
		if (n != 2 || a[0].type != T_STR)
			mmb_error("?TYPE MISMATCH");
		k = (int)mmb_as_int(a[1]);
		len = (int)strlen(a[0].s);
		if (k > len)
			k = len;
		if (k < 0)
			k = 0;
		b = mmb_tmp_alloc(k + 1);
		for (i = 0; i < k; i++)
			b[i] = a[0].s[len - k + i];
		b[k] = 0;
		*out = mmb_arena_val(b);
		return 1;
	}
	if (mmb_match("MID$"))
	{
	lbl_mid:
		int start, num, i, len;
		char *b;
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
		if (num > len - (start - 1))
			num = len - (start - 1);
		if (num < 0)
			num = 0;
		b = mmb_tmp_alloc(num + 1);
		for (i = 0; i < num; i++)
			b[i] = a[0].s[start - 1 + i];
		b[num] = 0;
		*out = mmb_arena_val(b);
		return 1;
	}
	if (mmb_match("UCASE$"))
	{
	lbl_ucase:
		int len;
		char *b;
		call_args(a, 1, &n);
		if (n != 1 || a[0].type != T_STR)
			mmb_error("?TYPE MISMATCH");
		len = (int)strlen(a[0].s);
		b = mmb_tmp_alloc(len + 1);
		memcpy(b, a[0].s, (size_t)len + 1);
		mmb_upper(b);
		*out = mmb_arena_val(b);
		return 1;
	}
	if (mmb_match("LCASE$"))
	{
	lbl_lcase:
		int i, len;
		char *b;
		call_args(a, 1, &n);
		if (n != 1 || a[0].type != T_STR)
			mmb_error("?TYPE MISMATCH");
		len = (int)strlen(a[0].s);
		b = mmb_tmp_alloc(len + 1);
		memcpy(b, a[0].s, (size_t)len + 1);
		for (i = 0; b[i]; i++)
			if (b[i] >= 'A' && b[i] <= 'Z')
				b[i] = (char)(b[i] + 32);
		*out = mmb_arena_val(b);
		return 1;
	}
	if (mmb_match("SPACE$"))
	{
	lbl_space:
		int k, i;
		char *b;
		call_args(a, 1, &n);
		if (n != 1)
			mmb_syntax();
		k = (int)mmb_as_int(a[0]);
		if (k < 0)
			k = 0;
		b = mmb_tmp_alloc(k + 1);
		for (i = 0; i < k; i++)
			b[i] = ' ';
		b[k] = 0;
		*out = mmb_arena_val(b);
		return 1;
	}
	if (mmb_match("ABS"))
	{
	lbl_abs:
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
	lbl_int:
		call_args(a, 1, &n);
		if (n != 1)
			mmb_syntax();
		*out = mmb_int_val((int64_t)floor(mmb_as_float(a[0])));
		return 1;
	}
	if (mmb_match("FIX"))
	{
	lbl_fix:
		call_args(a, 1, &n);
		if (n != 1)
			mmb_syntax();
		*out = mmb_int_val((int64_t)mmb_as_float(a[0]));
		return 1;
	}
	if (match_fun("CINT"))
	{
	lbl_cint:
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
	lbl_eval:
		char buf[MMB_LINE_LEN];
		const char *savep;

		call_args(a, 1, &n);
		if (n != 1 || a[0].type != T_STR)
			mmb_error("?TYPE MISMATCH");
		strncpy(buf, a[0].s, MMB_LINE_LEN - 1);
		buf[MMB_LINE_LEN - 1] = 0;
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
	lbl_math:
		G.p++;
		return mmb_try_math_fn(out);
	}
	if (mmb_match("SQR") || mmb_match("SQRT"))
	{
	lbl_sqr:
		call_args(a, 1, &n);
		if (n != 1)
			mmb_syntax();
		*out = mmb_num_val(sqrt(mmb_as_float(a[0])));
		return 1;
	}
	if (mmb_match("SIN"))
	{
	lbl_sin:
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
	lbl_cos:
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
	lbl_tan:
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
	if (mmb_match("ATN") || mmb_match("ATN2") || mmb_match("ATAN") || mmb_match("ATAN2"))
	{
	lbl_atn:
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
	lbl_rnd:
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
	lbl_mmhres:
		*out = mmb_int_val(G.gfx.w);
		return 1;
	}
	if (mmb_match("MM.VRES"))
	{
	lbl_mmvres:
		*out = mmb_int_val(G.gfx.h);
		return 1;
	}
	if (mmb_match("MM.HOST.HRES"))
	{
	lbl_mmhosthres:
		*out = mmb_int_val(G.plat && G.plat->host_width ? G.plat->host_width()
							       : G.gfx.w);
		return 1;
	}
	if (mmb_match("MM.HOST.VRES"))
	{
	lbl_mmhostvres:
		*out = mmb_int_val(G.plat && G.plat->host_height ? G.plat->host_height()
								 : G.gfx.h);
		return 1;
	}
	if (mmb_match("MM.HPOS"))
	{
	lbl_mmhpos:
		*out = mmb_int_val(G.print_x);
		return 1;
	}
	if (mmb_match("MM.VPOS"))
	{
	lbl_mmvpos:
		*out = mmb_int_val(G.print_y);
		return 1;
	}
	if (mmb_match("MM.INFO$") || mmb_match("MM.INFO"))
	{
	lbl_mminfo:
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
	lbl_playing:
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
	lbl_eof:
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
		else if (G.files[fn].kind == MMB_FK_TCP)
		{
			int st = mmb_net_tcp_status();
			*out = mmb_int_val((st != 1 || mmb_net_tcp_peer_closed()) ? 1 : 0);
		}
		else
		{
			int sz = mmb_vfs_size(G.files[fn].path);
			*out = mmb_int_val(G.files[fn].pos >= sz ? 1 : 0);
		}
		return 1;
	}
	if (mmb_match("INSTR"))
	{
	lbl_instr:
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
	lbl_string:
		int k, i;
		char *b;
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
		b = mmb_tmp_alloc(k + 1);
		for (i = 0; i < k; i++)
			b[i] = ch;
		b[k] = 0;
		*out = mmb_arena_val(b);
		return 1;
	}
	if (mmb_match("HEX$"))
	{
	lbl_hex:
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
	lbl_oct:
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
	lbl_bin:
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
	if (mmb_match("LTRIM$"))
	{
	lbl_ltrim:
		int len;
		char *b;
		call_args(a, 1, &n);
		if (n != 1 || a[0].type != T_STR)
			mmb_syntax();
		len = (int)strlen(a[0].s ? a[0].s : "");
		b = mmb_tmp_alloc(len + 1);
		trim_str(b, a[0].s ? a[0].s : "", " ", 1, 0);
		*out = mmb_arena_val(b);
		return 1;
	}
	if (mmb_match("RTRIM$"))
	{
	lbl_rtrim:
		int len;
		char *b;
		call_args(a, 1, &n);
		if (n != 1 || a[0].type != T_STR)
			mmb_syntax();
		len = (int)strlen(a[0].s ? a[0].s : "");
		b = mmb_tmp_alloc(len + 1);
		trim_str(b, a[0].s ? a[0].s : "", " ", 0, 1);
		*out = mmb_arena_val(b);
		return 1;
	}
	if (mmb_match("TRIM$"))
	{
	lbl_trim:
		int len;
		char *b;
		const char *mask = " ";
		char where = 'B';
		call_args(a, 3, &n);
		if (n < 1 || a[0].type != T_STR)
			mmb_syntax();
		if (n >= 2 && a[1].type == T_STR && a[1].s[0])
			mask = a[1].s;
		if (n >= 3 && a[2].type == T_STR && a[2].s[0])
			where = a[2].s[0];
		if (where >= 'a' && where <= 'z')
			where = (char)(where - 32);
		if (where != 'L' && where != 'R' && where != 'B')
			mmb_error("?INVALID TRIM");
		len = (int)strlen(a[0].s ? a[0].s : "");
		b = mmb_tmp_alloc(len + 1);
		trim_str(b, a[0].s ? a[0].s : "", mask, where != 'R', where != 'L');
		*out = mmb_arena_val(b);
		return 1;
	}
	if (mmb_match("BASE$"))
	{
	lbl_base:
		char b[72];
		int base, width = 0, k = 0, i;
		uint64_t v;
		call_args(a, 3, &n);
		if (n < 2)
			mmb_syntax();
		base = (int)mmb_as_int(a[0]);
		if (base < 2 || base > 36)
			mmb_error("?INVALID BASE");
		v = (uint64_t)mmb_as_int(a[1]);
		if (n >= 3)
		{
			width = (int)mmb_as_int(a[2]);
			if (width < 0)
				width = 0;
			if (width > (int)sizeof(b) - 1)
				width = (int)sizeof(b) - 1;
		}
		if (v == 0)
			b[k++] = '0';
		while (v)
		{
			int d = (int)(v % (unsigned)base);
			b[k++] = (char)(d < 10 ? '0' + d : 'A' + d - 10);
			v /= (unsigned)base;
		}
		while (k < width)
			b[k++] = '0';
		for (i = 0; i < k / 2; i++)
		{
			char t = b[i];
			b[i] = b[k - 1 - i];
			b[k - 1 - i] = t;
		}
		b[k] = 0;
		*out = mmb_str_val(b);
		return 1;
	}
	if (mmb_match("FIELD$"))
	{
	lbl_field:
		char *b;
		const char *str, *delims = ",", *quotes = "";
		int field, i = 0, j, k = 0;
		call_args(a, 4, &n);
		if (n < 2 || a[0].type != T_STR)
			mmb_syntax();
		str = a[0].s ? a[0].s : "";
		field = (int)mmb_as_int(a[1]);
		if (n >= 3 && a[2].type == T_STR)
			delims = a[2].s;
		if (n >= 4 && a[3].type == T_STR)
			quotes = a[3].s;
		while (--field > 0)
		{
			i = field_scan(str, i, delims, quotes);
			if (!str[i])
				break;
			i++;
		}
		while (str[i] == ' ')
			i++;
		j = field_scan(str, i, delims, quotes);
		while (j > i && str[j - 1] == ' ')
			j--;
		b = mmb_tmp_alloc(j - i + 1);
		for (; i < j; i++)
			b[k++] = str[i];
		b[k] = 0;
		*out = mmb_arena_val(b);
		return 1;
	}
	if (mmb_match("DATETIME$"))
	{
	lbl_datetime:
		char b[24];
		int64_t e = parse_epoch_arg();
		datetime_str(b, sizeof(b), e);
		*out = mmb_str_val(b);
		return 1;
	}
	if (mmb_match("DAY$"))
	{
	lbl_day:
		static const char *const days[8] = {
			"", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"};
		int64_t e;
		int y, mo, d, wd;
		mmb_skip_sp();
		if (*G.p == '(')
		{
			G.p++;
			mmb_skip_sp();
			if (mmb_match("NOW"))
				e = mmb_epoch_now();
			else if (*G.p == '"')
			{
				mmb_val sv = mmb_expr();
				if (!parse_date_fields(sv.s ? sv.s : "", &y, &mo, &d))
					mmb_error("?INVALID DATE");
				e = mmb_epoch_make(y, mo, d, 0, 0, 0);
			}
			else
				e = mmb_as_int(mmb_expr());
			mmb_skip_sp();
			mmb_expect(')');
		}
		else if (mmb_match("NOW"))
			e = mmb_epoch_now();
		else if (*G.p == '"')
		{
			mmb_val sv = mmb_expr();
			if (!parse_date_fields(sv.s ? sv.s : "", &y, &mo, &d))
				mmb_error("?INVALID DATE");
			e = mmb_epoch_make(y, mo, d, 0, 0, 0);
		}
		else
			mmb_error("?INVALID DATE");
		wd = (int)(((e / 86400) % 7 + 4 + 7) % 7);
		*out = mmb_str_val(days[wd == 0 ? 7 : wd]);
		return 1;
	}
	if (mmb_match("DIR$"))
	{
	lbl_dir:
		char b[512];
		const char *spec = 0, *filter = 0;
		mmb_skip_sp();
		if (*G.p == '(')
		{
			G.p++;
			mmb_skip_sp();
			if (*G.p != ')' && *G.p != ',')
			{
				mmb_val sv = mmb_expr();
				if (sv.type == T_STR)
					spec = sv.s;
			}
			mmb_skip_sp();
			if (*G.p == ',')
			{
				mmb_val fv;
				G.p++;
				fv = mmb_expr();
				if (fv.type == T_STR)
					filter = fv.s;
			}
			mmb_skip_sp();
			mmb_expect(')');
		}
		if (spec && spec[0])
		{
			if (mmb_vfs_list(spec, s_dir_list, (int)sizeof(s_dir_list)) != 0)
				s_dir_list[0] = 0;
			s_dir_len = (int)strlen(s_dir_list);
			s_dir_pos = 0;
			strncpy(s_dir_filter, filter && filter[0] ? filter : "ALL", sizeof(s_dir_filter) - 1);
			s_dir_filter[sizeof(s_dir_filter) - 1] = 0;
		}
		dir_next(s_dir_filter, b, (int)sizeof(b));
		*out = mmb_str_val(b);
		return 1;
	}
	if (mmb_match("DATE$"))
	{
	lbl_date:
		mmb_clock_refresh();
		*out = mmb_str_val(G.date_s[0] ? G.date_s : "1-1-26");
		return 1;
	}
	if (mmb_match("TIME$"))
	{
	lbl_time:
		mmb_clock_refresh();
		*out = mmb_str_val(G.time_s[0] ? G.time_s : "12:00:00");
		return 1;
	}
	if (mmb_match("INKEY$"))
	{
	lbl_inkey:
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
	lbl_keydown:
		int narg = 0;
		call_args(a, 1, &narg);
		*out = mmb_int_val(mmb_keydown_get(narg ? (int)mmb_as_int(a[0]) : 0));
		return 1;
	}
	if (mmb_match("TIMER"))
	{
	lbl_timer:
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
	lbl_lof:
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
		if (G.files[fn].kind == MMB_FK_TCP)
		{
			mmb_net_tcp_status();
			*out = mmb_int_val(0);
		}
		else
		{
			sz = mmb_vfs_size(G.files[fn].path);
			*out = mmb_int_val(sz < 0 ? 0 : sz);
		}
		return 1;
	}
	if (mmb_match("CWD$"))
	{
	lbl_cwd:
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
	lbl_input:
		int fn, nch = 0;
		char *b;
		call_args(a, 2, &n);
		if (n != 2 || a[0].type != T_INT && a[0].type != T_NUM)
			mmb_syntax();
		if (a[1].type != T_INT && a[1].type != T_NUM)
			mmb_syntax();
		nch = (int)mmb_as_int(a[0]);
		fn = (int)mmb_as_int(a[1]);
		if (fn < 1 || fn > MMB_MAX_FILES || !G.files[fn].open)
			mmb_error("?FILE");
		{
			int got;
			if (nch < 0)
				nch = 0;
			b = mmb_tmp_alloc(nch + 1);
			got = mmb_file_read(fn, b, nch);
			if (got < 0)
				got = 0;
			b[got] = 0;
			*out = mmb_arena_val(b);
		}
		return 1;
	}
	if (match_fun("JSON$"))
	{
	lbl_json:
		call_args(a, 2, &n);
		if (n != 2 || a[0].type != T_STR || a[1].type != T_STR)
			mmb_error("?TYPE MISMATCH");
		*out = mmb_json_query(a[0].s, a[1].s);
		return 1;
	}
	if (match_fun("JSON_STRINGIFY$"))
	{
	lbl_json_stringify:
		call_args(a, 1, &n);
		if (n != 1)
			mmb_syntax();
		*out = mmb_json_stringify(a[0]);
		return 1;
	}
	if (mmb_match("ACOS") || mmb_match("ACS"))
	{
	lbl_acos:
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
	lbl_asin:
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
	lbl_max:
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
	lbl_min:
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
	lbl_loc:
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
		if (G.files[fn].kind == MMB_FK_TCP)
		{
			int n;
			mmb_net_tcp_status();
			n = mmb_net_tcp_rx_avail();
			if (G.files[fn].ungot >= 0)
				n++;
			*out = mmb_int_val(n);
		}
		else
			*out = mmb_int_val(G.files[fn].pos);
		return 1;
	}
	if (mmb_match("SGN"))
	{
	lbl_sgn:
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
	lbl_exp:
		call_args(a, 1, &n);
		if (n != 1)
			mmb_syntax();
		*out = mmb_num_val(exp(mmb_as_float(a[0])));
		return 1;
	}
	if (mmb_match("LOG"))
	{
	lbl_log:
		call_args(a, 1, &n);
		if (n != 1)
			mmb_syntax();
		*out = mmb_num_val(log(mmb_as_float(a[0])));
		return 1;
	}
	if (mmb_match("PI"))
	{
	lbl_pi:
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
	lbl_deg:
		call_args(a, 1, &n);
		if (n != 1)
			mmb_syntax();
		*out = mmb_num_val(mmb_as_float(a[0]) * 180.0 / 3.14159265358979323846);
		return 1;
	}
	if (match_fun("RAD"))
	{
	lbl_rad:
		call_args(a, 1, &n);
		if (n != 1)
			mmb_syntax();
		*out = mmb_num_val(mmb_as_float(a[0]) * 3.14159265358979323846 / 180.0);
		return 1;
	}
	if (match_fun("POS"))
	{
	lbl_pos:
		call_args(a, 1, &n);
		(void)n;
		*out = mmb_int_val(G.print_x / mmb_print_font_w() + 1);
		return 1;
	}
	if (match_fun("CSRLIN"))
	{
	lbl_csrlin:
		int fh = mmb_print_font_h();
		mmb_skip_sp();
		if (*G.p == '(') /* CSRLIN() is accepted as well as bare CSRLIN */
		{
			G.p++;
			mmb_skip_sp();
			mmb_expect(')');
		}
		*out = mmb_int_val(fh > 0 ? G.print_y / fh + 1 : 1);
		return 1;
	}
	if (match_fun("BIT"))
	{
	lbl_bit:
		call_args(a, 2, &n);
		if (n != 2)
			mmb_syntax();
		{
			int bit = (int)mmb_as_int(a[1]);
			if (bit < 0 || bit > 63)
				mmb_error("?BIT");
			*out = mmb_int_val((mmb_as_int(a[0]) >> bit) & 1);
		}
		return 1;
	}
	if (match_fun("BYTE"))
	{
	lbl_byte:
		int pos;
		char b[2];
		call_args(a, 2, &n);
		if (n != 2 || a[0].type != T_STR)
			mmb_syntax();
		pos = (int)mmb_as_int(a[1]);
		if (pos < 1 || !a[0].s || pos > (int)strlen(a[0].s))
			*out = mmb_int_val(0);
		else
		{
			b[0] = a[0].s[pos - 1];
			b[1] = 0;
			*out = mmb_int_val((unsigned char)b[0]);
		}
		return 1;
	}
	if (mmb_match("EPOCH"))
	{
	lbl_epoch:
		int64_t e = 0;
		mmb_skip_sp();
		if (*G.p == '(')
		{
			G.p++;
			mmb_skip_sp();
			if (mmb_match("NOW"))
				e = mmb_epoch_now();
			else
			{
				mmb_val d = mmb_expr();
				if (d.type != T_STR)
					e = mmb_as_int(d);
				else
				{
					int y, mo, dd, h = 0, mi = 0, s = 0;
					if (!parse_date_fields(d.s ? d.s : "", &y, &mo, &dd))
						mmb_error("?INVALID DATE");
					mmb_skip_sp();
					if (*G.p == ',')
					{
						mmb_val t;
						G.p++;
						t = mmb_expr();
						if (t.type != T_STR || !parse_time_fields(t.s ? t.s : "", &h, &mi, &s))
							mmb_error("?INVALID TIME");
					}
					e = mmb_epoch_make(y, mo, dd, h, mi, s);
				}
			}
			mmb_skip_sp();
			mmb_expect(')');
		}
		else if (mmb_match("NOW"))
			e = mmb_epoch_now();
		else
			mmb_syntax();
		*out = mmb_int_val(e);
		return 1;
	}
	if (match_fun("CHOICE"))
	{
	lbl_choice:
		call_args(a, 3, &n);
		if (n != 3)
			mmb_syntax();
		*out = mmb_as_int(a[0]) ? a[1] : a[2];
		return 1;
	}
	if (match_fun("FORMAT$"))
	{
	lbl_format:
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
	lbl_bound:
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
	lbl_tab:
		call_args(a, 1, &n);
		if (n != 1)
			mmb_syntax();
		{
			int sp = (int)mmb_as_int(a[0]);
			char *buf;
			int i;
			if (sp < 0)
				sp = 0;
			buf = mmb_tmp_alloc(sp + 1);
			for (i = 0; i < sp; i++)
				buf[i] = ' ';
			buf[sp] = 0;
			*out = mmb_arena_val(buf);
		}
		return 1;
	}
	if (mmb_match("MM.VER"))
	{
	lbl_mmver:
		mmb_skip_sp();
		if (*G.p == '(')
		{
			G.p++;
			mmb_expect(')');
		}
		*out = mmb_str_val("1.8");
		return 1;
	}
	if (mmb_match("MM.RUNTIME") || mmb_match("MM.RUNTIME$"))
	{
	lbl_mmruntime:
		mmb_skip_sp();
		if (*G.p == '(')
		{
			G.p++;
			mmb_expect(')');
		}
		*out = mmb_str_val(G.plat && G.plat->runtime ? G.plat->runtime()
							     : "pi");
		return 1;
	}
	if (mmb_match("MM.DEVICE$"))
	{
	lbl_mmdev:
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
	lbl_mmcmd:
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
		off = mmb_elem_off(var, nidx, idx);
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
	if (*G.p == '#')
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
					int la, lb;
					char *buf;
					if (a.type != T_STR || b.type != T_STR)
						mmb_error("?TYPE MISMATCH");
					la = a.s ? (int)strlen(a.s) : 0;
					lb = b.s ? (int)strlen(b.s) : 0;
					buf = mmb_tmp_alloc(la + lb + 1);
					if (la)
						memcpy(buf, a.s, (size_t)la);
					if (lb)
						memcpy(buf + la, b.s, (size_t)lb);
					buf[la + lb] = 0;
					a = mmb_arena_val(buf);
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
