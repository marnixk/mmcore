#include "mmb_priv.h"
#include "mmb_version.h"
#include <math.h>

extern mmb_val mmb_load_var(mmb_var *v, int off);
extern int mmb_parse_var_ref(char *name, int *nidx, int *idx);
extern void mmb_do_assign(const char *name, int type_hint, int nidx, int *idx, mmb_val val);

static mmb_val expr_or(void);
static mmb_val expr_primary(void);

static int64_t expr_bit_int(mmb_val v)
{
	double x;
	if (v.type == T_INT)
		return v.i;
	x = mmb_as_float(v);
	return x >= 0.0 ? (int64_t)floor(x + 0.5) : (int64_t)ceil(x - 0.5);
}

static int expr_match_unary(const char *kw, int len)
{
	if ((unsigned char)*G.p != 0x80)
	{
		char c = G.p[len];
		if (c == '$' || c == '%' || c == '!')
			return 0;
	}
	return mmb_match(kw);
}

/* A type suffix or a struct path after a name means the name is a variable,
 * not the named colour constant of the same base name (#1307). */
static int colour_name_suffix(char c)
{
	return c == '$' || c == '%' || c == '!' || c == '.';
}

static int peek_kw(const char *kw)
{
	const char *save = G.p;
	int r = mmb_match(kw);
	G.p = save;
	return r;
}

/* MM.INFO(VERSION) is a number built from the firmware version, as on the
 * CMM2/PicoMite: 5.07.03 -> 5.0703. Non-numeric "dev" builds report 0. */
static double info_version_number(void)
{
	const char *s = MMB_VERSION;
	double v = 0;
	int part = 0, any = 0;
	if (*s == 'v' || *s == 'V')
		s++;
	while (part < 3)
	{
		int n = 0, got = 0;
		while (*s >= '0' && *s <= '9')
		{
			n = n * 10 + (*s - '0');
			s++;
			got = 1;
		}
		if (!got)
			break;
		if (part == 0)
			v = n;
		else if (part == 1)
			v += n / 100.0;
		else
			v += n / 10000.0;
		part++;
		any = 1;
		if (*s != '.')
			break;
		s++;
	}
	return any ? v : 0;
}

/* Parse the one optional MM.INFO/MM.INFO$ argument: either a quoted string or
 * a bareword key such as MODE or HRES. Returns 0 when no key was given. */
static int info_key_arg(char *key, int keysz)
{
	key[0] = 0;
	mmb_skip_sp();
	if (*G.p != '(')
		return 0;
	G.p++;
	mmb_skip_sp();
	if (*G.p != ')')
	{
		if (*G.p == '"')
		{
			mmb_val sv = mmb_expr();
			if (sv.type == T_STR)
			{
				strncpy(key, sv.s ? sv.s : "", (size_t)keysz - 1);
				key[keysz - 1] = 0;
			}
		}
		else
			mmb_ident(key, keysz);
	}
	mmb_skip_sp();
	mmb_expect(')');
	return key[0] != 0;
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

#define MMB_FUN_TAB(X) \
	X("RGB", lbl_rgb) \
	X("THEME", lbl_theme) \
	X("PIXEL", lbl_pixel) \
	X("LEN", lbl_len) \
	X("ASC", lbl_asc) \
	X("CHR$", lbl_chr) \
	X("STR$", lbl_str) \
	X("VAL", lbl_val) \
	X("LEFT$", lbl_left) \
	X("RIGHT$", lbl_right) \
	X("MID$", lbl_mid) \
	X("UCASE$", lbl_ucase) \
	X("LCASE$", lbl_lcase) \
	X("SPACE$", lbl_space) \
	X("LTRIM$", lbl_ltrim) \
	X("RTRIM$", lbl_rtrim) \
	X("TRIM$", lbl_trim) \
	X("BASE$", lbl_base) \
	X("FIELD$", lbl_field) \
	X("DATETIME$", lbl_datetime) \
	X("DAY$", lbl_day) \
	X("DIR$", lbl_dir) \
	X("ABS", lbl_abs) \
	X("INT", lbl_int) \
	X("FIX", lbl_fix) \
	X("CINT", lbl_cint) \
	X("EVAL", lbl_eval) \
	X("MATH", lbl_math) \
	X("SQR", lbl_sqr) \
	X("SQRT", lbl_sqr) \
	X("SIN", lbl_sin) \
	X("COS", lbl_cos) \
	X("TAN", lbl_tan) \
	X("ATN", lbl_atn) \
	X("ATN2", lbl_atn) \
	X("ATAN", lbl_atn) \
	X("ATAN2", lbl_atn) \
	X("RND", lbl_rnd) \
	X("MM.HRES", lbl_mmhres) \
	X("MM.VRES", lbl_mmvres) \
	X("MM.HOST.HRES", lbl_mmhosthres) \
	X("MM.HOST.VRES", lbl_mmhostvres) \
	X("MM.HPOS", lbl_mmhpos) \
	X("MM.VPOS", lbl_mmvpos) \
	X("MM.INFO$", lbl_mminfo) \
	X("MM.INFO", lbl_mminfo) \
	X("MM.RUNTIME", lbl_mmruntime) \
	X("MM.RUNTIME$", lbl_mmruntime) \
	X("PLAYING", lbl_playing) \
	X("EOF", lbl_eof) \
	X("INSTR", lbl_instr) \
	X("STRING$", lbl_string) \
	X("HEX$", lbl_hex) \
	X("OCT$", lbl_oct) \
	X("BIN$", lbl_bin) \
	X("DATE$", lbl_date) \
	X("TIME$", lbl_time) \
	X("INKEY$", lbl_inkey) \
	X("KEYDOWN", lbl_keydown) \
	X("TIMER", lbl_timer) \
	X("LOF", lbl_lof) \
	X("CWD$", lbl_cwd) \
	X("INPUT$", lbl_input) \
	X("JSON$", lbl_json) \
	X("JSON_STRINGIFY$", lbl_json_stringify) \
	X("ACOS", lbl_acos) \
	X("ACS", lbl_acos) \
	X("ASIN", lbl_asin) \
	X("ASN", lbl_asin) \
	X("LOC", lbl_loc) \
	X("SGN", lbl_sgn) \
	X("EXP", lbl_exp) \
	X("LOG", lbl_log) \
	X("PI", lbl_pi) \
	X("DEG", lbl_deg) \
	X("RAD", lbl_rad) \
	X("POS", lbl_pos) \
	X("CSRLIN", lbl_csrlin) \
	X("BIT", lbl_bit) \
	X("BYTE", lbl_byte) \
	X("EPOCH", lbl_epoch) \
	X("CHOICE", lbl_choice) \
	X("FORMAT$", lbl_format) \
	X("BOUND", lbl_bound) \
	X("TAB", lbl_tab) \
	X("MM.VER", lbl_mmver) \
	X("MM.DEVICE$", lbl_mmdev) \
	X("MM.CMDLINE$", lbl_mmcmd) \
	X("TDF.WIDTH", lbl_tdfwidth) \
	X("TDF.NAME$", lbl_tdfname) \
	X("TDF.TYPE%", lbl_tdftype) \
	X("TDF.SPACING%", lbl_tdfspacing) \
	X("TDF.HEIGHT", lbl_tdfheight) \
	X("TDF.VARIANTS%", lbl_tdfvariants) \
	X("TDF.VARIANT%", lbl_tdfvariant) \
	X("TDF.VARIANTNAME$", lbl_tdfvariantname) \
	X("MAX", lbl_max) \
	X("MIN", lbl_min) \
	X("MM.BATTERY%", lbl_mmbattery) \
	X("MM.BRIGHTNESS%", lbl_mmbrightness) \
	X("MM.ERRNO", lbl_mmerrno) \
	X("MM.ERRMSG$", lbl_mmerrmsg)

static const char *const builtin_func_names[] = {
#define MMB_FUN_NAME(n, l) n,
	MMB_FUN_TAB(MMB_FUN_NAME)
#undef MMB_FUN_NAME
	0
};

int mmb_is_builtin_func(const char *name)
{
	const char *const *p;
	if (!name || !*name)
		return 0;
	for (p = builtin_func_names; *p; p++)
		if (mmb_keyword_eq(name, *p))
			return 1;
	return 0;
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
#define MMB_FUN_ENTRY(n, l) fun_tab[mmb_kw_id(n)] = &&l;
		MMB_FUN_TAB(MMB_FUN_ENTRY)
#undef MMB_FUN_ENTRY
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
				/* Defensive: a CONST may no longer shadow a built-in
				 * name (mmb_check_user_name rejects the declaration,
				 * #1304), but keep honouring a shadowing const if one
				 * ever exists: only the bare form is shadowed;
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
		if (mmb_is_ident_start(*G.p))
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
		int save;
		call_args(a, 1, &n);
		if (n != 1)
			mmb_syntax();
		/* PRINT accumulates its whole statement in G.out (#991). Format
		 * the value after any pending output instead of clobbering it, so
		 * `PRINT "x";STR$(5)` keeps the "x". */
		save = G.outn;
		mmb_print_val(a[0]);
		strncpy(buf, G.out + save, sizeof(buf) - 1);
		buf[sizeof(buf) - 1] = 0;
		G.outn = save;
		G.out[save] = 0;
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
		char key[MMB_MAX_NAME];
		if (!info_key_arg(key, (int)sizeof(key)))
		{
			*out = mmb_num_val(G.gfx.mode + G.gfx.bits / 100.0);
			return 1;
		}
		mmb_upper(key);
		if (mmb_keyword_eq(key, "MODE"))
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
		if (mmb_keyword_eq(key, "VRES"))
		{
			*out = mmb_int_val(G.gfx.h);
			return 1;
		}
		if (mmb_keyword_eq(key, "VERSION"))
		{
			*out = mmb_num_val(info_version_number());
			return 1;
		}
		if (mmb_keyword_eq(key, "AUDIO"))
		{
			*out = mmb_str_val(G.opt.audio_target ? "HDMI" : "JACK");
			return 1;
		}
		if (mmb_keyword_eq(key, "MIXGAP"))
		{
			*out = mmb_int_val((int)mmb_audio_mix_gap_ms());
			return 1;
		}
		if (mmb_keyword_eq(key, "UNDERRUN"))
		{
			*out = mmb_int_val((int)mmb_audio_underruns());
			return 1;
		}
		if (mmb_keyword_eq(key, "FIRSTVOL"))
		{
			/* Gain applied to the first buffer after play began (#1113). */
			*out = mmb_int_val((int)mmb_audio_first_mix_vol());
			return 1;
		}
		if (mmb_keyword_eq(key, "AUDIORESET"))
		{
			*out = mmb_int_val((int)mmb_audio_resets());
			return 1;
		}
		if (mmb_keyword_eq(key, "MP3BUF"))
		{
			/* Peak MP3 decoder-buffer allocation (#1150). */
			*out = mmb_int_val((int)mmb_audio_mp3_buffer_peak());
			return 1;
		}
		if (mmb_keyword_eq(key, "MP3RATE"))
		{
			/* Source sample rate of the last MP3 (#1247). */
			*out = mmb_int_val((int)mmb_audio_mp3_rate());
			return 1;
		}
		if (mmb_keyword_eq(key, "MP3SRC"))
		{
			/* Source-rate frames decoded from the last MP3 (#1247). */
			*out = mmb_int_val((int)mmb_audio_mp3_src_frames());
			return 1;
		}
		if (mmb_keyword_eq(key, "MP3OUT"))
		{
			/* 44100 Hz frames mixed from the last MP3 (#1247). */
			*out = mmb_int_val((int)mmb_audio_mp3_out_frames());
			return 1;
		}
		/* CMM2/PicoMite reject an unknown key; never fall back to MODE. */
		mmb_error("?SYNTAX ERROR");
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
		*out = mmb_int_val(g_audio.playing && !g_audio.paused ? 1 : 0);
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
		char filter_buf[8];
		const char *spec = 0, *filter = 0;
		filter_buf[0] = 0;
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
				G.p++;
				mmb_skip_sp();
				if (peek_kw("FILE") || peek_kw("DIR") || peek_kw("ALL"))
					mmb_ident(filter_buf, (int)sizeof(filter_buf));
				else
				{
					mmb_val fv = mmb_expr();
					if (fv.type == T_STR)
					{
						strncpy(filter_buf, fv.s ? fv.s : "",
							sizeof(filter_buf) - 1);
						filter_buf[sizeof(filter_buf) - 1] = 0;
					}
				}
				if (filter_buf[0])
					filter = filter_buf;
			}
			mmb_skip_sp();
			mmb_expect(')');
		}
		if (spec && spec[0])
		{
			/* DIR$() keeps its own 4 KB listing buffer; truncation is not
			 * surfaced here (out of scope for #693). */
			if (mmb_vfs_list(spec, s_dir_list, (int)sizeof(s_dir_list), 0) != 0)
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
	if (mmb_match("MM.BATTERY%"))
	{
	lbl_mmbattery:
		mmb_skip_sp();
		if (*G.p == '(')
		{
			G.p++;
			mmb_expect(')');
		}
		/* Native Linux reports the first battery's charge; Pi/Circle,
		 * Windows, macOS and any other target report 100 (no battery). */
		*out = mmb_int_val((int64_t)mmb_battery_percent());
		return 1;
	}
	if (mmb_match("MM.BRIGHTNESS%"))
	{
	lbl_mmbrightness:
		mmb_skip_sp();
		if (*G.p == '(')
		{
			G.p++;
			mmb_expect(')');
		}
#if defined(MMB_PLATFORM_POSIX)
		/* Native Linux (Chromebook ISO / framebuffer): read the panel
		 * backlight percentage, falling back to 100 when none exists. */
		*out = mmb_int_val(mmb_brightness_get());
#else
		/* Pi/Circle and other bare-metal targets: no controllable
		 * backlight. */
		*out = mmb_int_val(100);
#endif
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
		for (i = 0; i < G.vars_cap; i++)
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
	if (mmb_match("MM.ERRNO"))
	{
	lbl_mmerrno:
		mmb_skip_sp();
		if (*G.p == '(')
		{
			G.p++;
			mmb_expect(')');
		}
		*out = mmb_int_val(G.mm_errno);
		return 1;
	}
	if (mmb_match("MM.ERRMSG$"))
	{
	lbl_mmerrmsg:
		mmb_skip_sp();
		if (*G.p == '(')
		{
			G.p++;
			mmb_expect(')');
		}
		*out = mmb_str_val(G.mm_errmsg);
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
	if (mmb_match("TDF.WIDTH"))
	{
	lbl_tdfwidth:
		mmb_tdf_fn_width(out);
		return 1;
	}
	if (mmb_match("TDF.NAME$"))
	{
	lbl_tdfname:
		mmb_tdf_fn_name(out);
		return 1;
	}
	if (mmb_match("TDF.TYPE%"))
	{
	lbl_tdftype:
		mmb_tdf_fn_type(out);
		return 1;
	}
	if (mmb_match("TDF.SPACING%"))
	{
	lbl_tdfspacing:
		mmb_tdf_fn_spacing(out);
		return 1;
	}
	if (mmb_match("TDF.HEIGHT"))
	{
	lbl_tdfheight:
		mmb_tdf_fn_height(out);
		return 1;
	}
	if (mmb_match("TDF.VARIANTS%"))
	{
	lbl_tdfvariants:
		mmb_tdf_fn_variants(out);
		return 1;
	}
	if (mmb_match("TDF.VARIANT%"))
	{
	lbl_tdfvariant:
		mmb_tdf_fn_variant(out);
		return 1;
	}
	if (mmb_match("TDF.VARIANTNAME$"))
	{
	lbl_tdfvariantname:
		mmb_tdf_fn_variantname(out);
		return 1;
	}
ident_tail:
	if (mmb_try_user_function(out))
		return 1;

	/* named colours as identifiers. A type suffix or a struct path after the
	 * name means this is a variable, not the colour constant (#1307). */
	{
		char name[32];
		const char *save2 = G.p;
		int ok = 0;
		unsigned col;
		mmb_skip_sp();
		if (mmb_tok_expand(name, (int)sizeof(name)))
		{
			col = mmb_named_colour(name, &ok);
			if (ok && !colour_name_suffix(*G.p))
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
				if (ok && !colour_name_suffix(*G.p))
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
		const char *ref = G.p;
		t = mmb_parse_var_ref(name, &nidx, idx);
		if (nidx == 0 && mmb_const_lookup(name, t, &cv))
			return cv;
		var = mmb_find_var_ref(ref, name, t, 1, nidx, idx);
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
	if (((unsigned char)*G.p == 0x80 || *G.p == 'N' || *G.p == 'n') &&
	    expr_match_unary("NOT", 3))
	{
		mmb_val v = expr_unary();
		return mmb_int_val(mmb_as_float(v) != 0.0 ? 0 : 1);
	}
	if (((unsigned char)*G.p == 0x80 || *G.p == 'I' || *G.p == 'i') &&
	    expr_match_unary("INV", 3))
	{
		mmb_val v = expr_unary();
		return mmb_int_val(~expr_bit_int(v));
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

static mmb_val expr_shift(void)
{
	mmb_val a = expr_add();
	for (;;)
	{
		mmb_skip_sp();
		if (G.p[0] == '<' && G.p[1] == '<')
		{
			mmb_val b;
			int64_t n, x;
			G.p += 2;
			b = expr_add();
			n = expr_bit_int(b);
			x = expr_bit_int(a);
			if (n <= 0)
				a = mmb_int_val(x);
			else if (n >= 64)
				a = mmb_int_val(0);
			else
				a = mmb_int_val((int64_t)((uint64_t)x << n));
		}
		else if (G.p[0] == '>' && G.p[1] == '>')
		{
			mmb_val b;
			int64_t n, x;
			G.p += 2;
			b = expr_add();
			n = expr_bit_int(b);
			x = expr_bit_int(a);
			if (n <= 0)
				a = mmb_int_val(x);
			else if (n >= 64)
				a = mmb_int_val(x < 0 ? -1 : 0);
			else
				a = mmb_int_val(x >> n);
		}
		else
			break;
	}
	return a;
}

static mmb_val expr_rel(void)
{
	mmb_val a = expr_shift();
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
			a = mmb_int_val(expr_bit_int(a) & expr_bit_int(b));
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
			a = mmb_int_val(expr_bit_int(a) | expr_bit_int(b));
		}
		else if (((unsigned char)*G.p == 0x80 || *G.p == 'X' || *G.p == 'x') && mmb_match("XOR"))
		{
			mmb_val b = expr_and();
			a = mmb_int_val(expr_bit_int(a) ^ expr_bit_int(b));
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
