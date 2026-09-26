#include "mmb_priv.h"
#include <string.h>

extern int mmb_parse_var_ref(char *name, int *nidx, int *idx);
extern void mmb_do_assign(const char *name, int type_hint, int nidx, int *idx, mmb_val val);

void mmb_cmd_local(void)
{
	G.dim_local = 1;
	mmb_cmd_dim();
	G.dim_local = 0;
}

void mmb_cmd_static(void)
{
	mmb_cmd_dim();
}

void mmb_cmd_error(void)
{
	mmb_skip_sp();
	if (*G.p == 0 || *G.p == ':' || *G.p == '\'')
		mmb_error("?ERROR");
	{
		mmb_val v = mmb_expr();
		if (v.type == T_STR && v.s[0])
			mmb_error(v.s);
		else
			mmb_error("?ERROR");
	}
}

void mmb_cmd_memory(void)
{
	int i, used = 0;
	for (i = 0; i < MMB_MAX_VARS; i++)
		if (G.vars[i].used)
			used++;
	mmb_out("Program: ");
	mmb_outf(0, G.nprog);
	mmb_out(" lines  Variables: ");
	mmb_outf(0, used);
	mmb_out("  Files: ");
	{
		int n = 0;
		for (i = 1; i <= MMB_MAX_FILES; i++)
			if (G.files[i].open)
				n++;
		mmb_outf(0, n);
	}
}

void mmb_cmd_randomize(void)
{
	mmb_skip_sp();
	if (*G.p && *G.p != ':' && *G.p != '\'')
	{
		mmb_val v = mmb_expr();
		G.rnd_seed = (uint32_t)mmb_as_int(v);
		if (G.rnd_seed == 0)
			G.rnd_seed = 0x12345678u;
	}
	else
		G.rnd_seed = (uint32_t)mmb_now_ms() | 1u;
}

static void bump_var(int sign)
{
	char name[MMB_MAX_NAME];
	int nidx, idx[MMB_MAX_DIMS], t;
	mmb_val cur, delta;
	mmb_var *v;
	t = mmb_parse_var_ref(name, &nidx, idx);
	mmb_skip_sp();
	delta = mmb_int_val(1);
	if (*G.p == ',')
	{
		G.p++;
		delta = mmb_expr();
	}
	v = mmb_find_var(name, t, 1, nidx, idx);
	if (!v)
		mmb_syntax();
	cur = mmb_load_var(v, mmb_elem_off(v, nidx, idx));
	if (cur.type == T_STR || cur.type == T_STRUCT)
		mmb_error("?TYPE MISMATCH");
	if (cur.type == T_INT)
		cur = mmb_int_val(cur.i + sign * mmb_as_int(delta));
	else
		cur = mmb_num_val(mmb_as_float(cur) + sign * mmb_as_float(delta));
	mmb_do_assign(name, t, nidx, idx, cur);
}

void mmb_cmd_inc(void)
{
	bump_var(1);
}

void mmb_cmd_dec(void)
{
	bump_var(-1);
}

void mmb_cmd_cat(void)
{
	char name[MMB_MAX_NAME];
	int nidx, idx[MMB_MAX_DIMS], t;
	mmb_val add;
	mmb_var *v;
	t = mmb_parse_var_ref(name, &nidx, idx);
	mmb_skip_sp();
	if (*G.p == ',')
		G.p++;
	add = mmb_expr();
	if (add.type != T_STR)
		mmb_syntax();
	v = mmb_find_var(name, t ? t : T_STR, 1, nidx, idx);
	if (!v || v->type != T_STR)
		mmb_error("?TYPE MISMATCH");
	if (G.acc_on)
	{
		const char *cs;
		int lc, la;
		char *buf;
		mmb_val cur;
		cs = (v->data.s && v->data.s[0]) ? v->data.s[0] : "";
		lc = (int)strlen(cs);
		la = add.s ? (int)strlen(add.s) : 0;
		buf = mmb_tmp_alloc(lc + la + 1);
		if (lc)
			memcpy(buf, cs, (size_t)lc);
		if (la)
			memcpy(buf + lc, add.s, (size_t)la);
		buf[lc + la] = 0;
		cur = mmb_arena_val(buf);
		mmb_do_assign(name, T_STR, nidx, idx, cur);
	}
	else
	{
		int off = mmb_elem_off(v, nidx, idx);
		mmb_str_append(&v->data.s[off], add.s, -1, v->maxlen, v->name);
	}
}

/* Handler name for ON MOUSECLICK / ON MOUSEMOVE: a quoted string or a bare
 * SUB name. An empty tail clears the handler, mirroring ON KEY. */
static void on_named_sub(char *dst, int cap)
{
	mmb_skip_sp();
	if (*G.p == 0 || *G.p == ':' || *G.p == '\'')
	{
		dst[0] = 0;
		return;
	}
	if (*G.p == '"')
	{
		mmb_val v = mmb_expr();
		const char *s;
		int i;
		if (v.type != T_STR)
			mmb_syntax();
		s = v.s ? v.s : "";
		for (i = 0; i < cap - 1 && s[i]; i++)
			dst[i] = s[i];
		dst[i] = 0;
		return;
	}
	mmb_ident(dst, cap);
	mmb_type_suffix(dst);
}

void mmb_cmd_on(void)
{
	mmb_val v;
	int n, i, gosub, target;
	mmb_skip_sp();
	if (mmb_match("ERROR"))
	{
		G.error_active = 0;
		mmb_skip_sp();
		if (*G.p == 0 || *G.p == ':' || *G.p == '\'' || *G.p == ',')
		{
			G.on_error_pc = -1;
			return;
		}
		if (mmb_match("GOTO") || mmb_match("THEN"))
		{
			mmb_skip_sp();
			G.on_error_pc = mmb_parse_target();
			return;
		}
		G.on_error_pc = -1;
		return;
	}
	if (mmb_match("KEY"))
	{
		mmb_skip_sp();
		if (*G.p == 0 || *G.p == ':' || *G.p == '\'')
		{
			G.on_key[0] = 0;
			return;
		}
		mmb_ident(G.on_key, sizeof(G.on_key));
		mmb_type_suffix(G.on_key);
		return;
	}
	if (mmb_match("MOUSECLICK"))
	{
		on_named_sub(G.on_mouseclick, sizeof(G.on_mouseclick));
		return;
	}
	if (mmb_match("MOUSEMOVE"))
	{
		on_named_sub(G.on_mousemove, sizeof(G.on_mousemove));
		return;
	}
	v = mmb_expr();
	n = (int)mmb_as_int(v);
	mmb_skip_sp();
	if (mmb_match("GOSUB"))
		gosub = 1;
	else if (mmb_match("GOTO"))
		gosub = 0;
	else
		mmb_syntax();
	i = 1;
	target = -1;
	for (;;)
	{
		mmb_val ln = mmb_expr();
		if (i == n)
			target = (int)mmb_as_int(ln);
		mmb_skip_sp();
		if (*G.p == ',')
		{
			G.p++;
			i++;
			continue;
		}
		break;
	}
	if (target < 0)
		return;
	if (gosub)
	{
		if (G.gosub_sp >= MMB_MAX_GOSUB)
			mmb_error("?GOSUB");
		G.gosub_stack[G.gosub_sp] = G.run_pc + 1;
		G.gosub_event[G.gosub_sp] = 0;
		G.gosub_nsave[G.gosub_sp] = 0;
		G.gosub_sp++;
	}
	G.branch_pc = mmb_find_line_pc(target);
}

void mmb_cmd_mid(void)
{
	char name[MMB_MAX_NAME];
	int nidx, idx[MMB_MAX_DIMS], t, start, ncopy, i, slen, rlen, off, needed;
	mmb_val cur, repl, sv, lv;
	mmb_var *v;
	char *buf;
	const char *cs;
	int have_n = 0;
	mmb_skip_sp();
	mmb_expect('(');
	t = mmb_parse_var_ref(name, &nidx, idx);
	mmb_skip_sp();
	mmb_expect(',');
	sv = mmb_expr();
	start = (int)mmb_as_int(sv);
	ncopy = 0;
	mmb_skip_sp();
	if (*G.p == ',')
	{
		G.p++;
		lv = mmb_expr();
		ncopy = (int)mmb_as_int(lv);
		have_n = 1;
	}
	mmb_expect(')');
	mmb_skip_sp();
	mmb_expect('=');
	repl = mmb_expr();
	if (repl.type != T_STR)
		mmb_syntax();
	v = mmb_find_var(name, t ? t : T_STR, 1, nidx, idx);
	if (!v || v->type != T_STR)
		mmb_error("?TYPE MISMATCH");
	off = mmb_elem_off(v, nidx, idx);
	cs = v->data.s && v->data.s[off] ? v->data.s[off] : "";
	slen = (int)strlen(cs);
	if (start < 1)
		start = 1;
	if (ncopy < 0)
		ncopy = 0;
	rlen = (int)strlen(repl.s);
	if (!have_n)
		ncopy = rlen;
	if (rlen < ncopy)
		ncopy = rlen;
	needed = slen;
	if (start - 1 + ncopy > needed)
		needed = start - 1 + ncopy;
	buf = mmb_tmp_alloc(needed + 1);
	if (slen)
		memcpy(buf, cs, (size_t)slen);
	for (i = 0; i < ncopy; i++)
	{
		int dest = start - 1 + i;
		if (dest >= slen)
		{
			while (slen < dest)
				buf[slen++] = ' ';
			buf[dest] = repl.s[i];
			slen = dest + 1;
		}
		else
			buf[dest] = repl.s[i];
	}
	buf[slen] = 0;
	cur = mmb_arena_val(buf);
	mmb_do_assign(name, T_STR, nidx, idx, cur);
}

static void do_lset_rset(int right)
{
	char name[MMB_MAX_NAME];
	int nidx = 0, idx[MMB_MAX_DIMS], t, width, slen, i, off = 0;
	mmb_val v;
	mmb_var *var;
	char *buf;
	t = mmb_parse_var_ref(name, &nidx, idx);
	mmb_skip_sp();
	mmb_expect('=');
	v = mmb_expr();
	if (v.type != T_STR)
		mmb_error("?TYPE MISMATCH");
	var = mmb_find_var(name, t ? t : T_STR, 1, nidx, idx);
	if (!var || var->type != T_STR)
		mmb_error("?TYPE MISMATCH");
	off = mmb_elem_off(var, nidx, idx);
	width = var->maxlen;
	if (width <= 0)
	{
		int cur = (int)strlen(var->data.s[off]);
		int vlen = (int)strlen(v.s);
		width = cur > vlen ? cur : vlen;
	}
	buf = mmb_tmp_alloc(width + 1);
	for (i = 0; i < width; i++)
		buf[i] = ' ';
	buf[width] = 0;
	slen = (int)strlen(v.s);
	if (slen > width)
		slen = width;
	if (right)
		memcpy(buf + (width - slen), v.s, (size_t)slen);
	else
		memcpy(buf, v.s, (size_t)slen);
	mmb_do_assign(name, T_STR, nidx, idx, mmb_str_val(buf));
}

void mmb_cmd_lset(void)
{
	do_lset_rset(0);
}

void mmb_cmd_rset(void)
{
	do_lset_rset(1);
}

void mmb_cmd_bit(void)
{
	char name[MMB_MAX_NAME];
	int nidx = 0, idx[MMB_MAX_DIMS], t, bit, off;
	mmb_val bv, val;
	mmb_var *v;
	int64_t x;
	mmb_skip_sp();
	mmb_expect('(');
	t = mmb_parse_var_ref(name, &nidx, idx);
	mmb_skip_sp();
	mmb_expect(',');
	bv = mmb_expr();
	bit = (int)mmb_as_int(bv);
	mmb_skip_sp();
	mmb_expect(')');
	mmb_skip_sp();
	mmb_expect('=');
	val = mmb_expr();
	if (bit < 0 || bit > 63)
		mmb_error("?BIT");
	v = mmb_find_var(name, t ? t : T_INT, 0, nidx, idx);
	if (!v || v->type != T_INT)
		mmb_error("?TYPE MISMATCH");
	off = mmb_elem_off(v, nidx, idx);
	x = v->data.i[off];
	if (mmb_as_int(val))
		x |= ((int64_t)1 << bit);
	else
		x &= ~((int64_t)1 << bit);
	v->data.i[off] = x;
}

void mmb_cmd_byte(void)
{
	char name[MMB_MAX_NAME];
	int nidx = 0, idx[MMB_MAX_DIMS], t, pos, off, len, k, final;
	mmb_val pv, val;
	mmb_var *v;
	const char *s;
	char *nb;
	mmb_skip_sp();
	mmb_expect('(');
	t = mmb_parse_var_ref(name, &nidx, idx);
	mmb_skip_sp();
	mmb_expect(',');
	pv = mmb_expr();
	pos = (int)mmb_as_int(pv);
	mmb_skip_sp();
	mmb_expect(')');
	mmb_skip_sp();
	mmb_expect('=');
	val = mmb_expr();
	if (pos < 1)
		mmb_error("?BYTE");
	v = mmb_find_var(name, t ? t : T_STR, 0, nidx, idx);
	if (!v || v->type != T_STR)
		mmb_error("?TYPE MISMATCH");
	off = mmb_elem_off(v, nidx, idx);
	s = v->data.s[off] ? v->data.s[off] : "";
	len = (int)strlen(s);
	final = len > pos ? len : pos;
	nb = mmb_tmp_alloc(final + 1);
	if (len)
		memcpy(nb, s, (size_t)len);
	for (k = len; k < pos - 1; k++)
		nb[k] = ' ';
	nb[pos - 1] = (char)(mmb_as_int(val) & 0xff);
	nb[final] = 0;
	mmb_str_set(&v->data.s[off], nb, final, v->maxlen, v->name);
}

void mmb_cmd_sort(void)
{
	char name[MMB_MAX_NAME];
	int t, i, j, n;
	mmb_var *v = 0;
	mmb_ident(name, sizeof(name));
	t = mmb_type_suffix(name);
	mmb_skip_sp();
	if (*G.p == '(')
	{
		G.p++;
		mmb_skip_sp();
		mmb_expect(')');
	}
	for (i = 0; i < MMB_MAX_VARS; i++)
		if (G.vars[i].used && mmb_keyword_eq(G.vars[i].name, name) &&
		    (t == 0 || G.vars[i].type == t))
		{
			v = &G.vars[i];
			break;
		}
	if (!v || v->dims < 1)
		mmb_error("?ARRAY");
	n = v->size;
	for (i = 0; i < n - 1; i++)
		for (j = 0; j < n - 1 - i; j++)
		{
			int swap = 0;
			if (v->type == T_STR)
				swap = strcmp(v->data.s[j], v->data.s[j + 1]) > 0;
			else if (v->type == T_INT)
				swap = v->data.i[j] > v->data.i[j + 1];
			else
				swap = v->data.f[j] > v->data.f[j + 1];
			if (swap)
			{
				if (v->type == T_STR)
				{
					char *tmp = v->data.s[j];
					v->data.s[j] = v->data.s[j + 1];
					v->data.s[j + 1] = tmp;
				}
				else if (v->type == T_INT)
				{
					int64_t tmp = v->data.i[j];
					v->data.i[j] = v->data.i[j + 1];
					v->data.i[j + 1] = tmp;
				}
				else
				{
					double tmp = v->data.f[j];
					v->data.f[j] = v->data.f[j + 1];
					v->data.f[j + 1] = tmp;
				}
			}
		}
}

void mmb_cmd_settick(void)
{
	int period, slot = 0;
	char name[MMB_MAX_NAME];
	period = (int)mmb_as_int(mmb_expr());
	mmb_skip_sp();
	if (*G.p == ',')
		G.p++;
	mmb_skip_sp();
	if (*G.p == 0 || *G.p == ':' || *G.p == '\'')
	{
		G.tick[0].period = 0;
		G.tick[0].sub[0] = 0;
		return;
	}
	mmb_ident(name, sizeof(name));
	mmb_type_suffix(name);
	mmb_skip_sp();
	if (*G.p == ',')
	{
		G.p++;
		slot = (int)mmb_as_int(mmb_expr());
	}
	if (slot < 0 || slot >= MMB_MAX_TICK)
		slot = 0;
	if (period <= 0)
	{
		G.tick[slot].period = 0;
		G.tick[slot].sub[0] = 0;
		return;
	}
	G.tick[slot].period = period;
	strncpy(G.tick[slot].sub, name, MMB_MAX_NAME - 1);
	G.tick[slot].sub[MMB_MAX_NAME - 1] = 0;
	G.tick[slot].last = mmb_now_ms();
}

void mmb_cmd_resume(void)
{
	int target;
	if (!G.error_active)
	{
		G.error_active = 1; /* stop mmb_error trapping this one */
		mmb_error("?RESUME");
	}
	mmb_skip_sp();
	if (mmb_match("NEXT"))
		target = G.err_resume_pc + 1;
	else if (*G.p && *G.p != ':' && *G.p != '\'')
		target = mmb_parse_target();
	else
		target = G.err_resume_pc;
	G.error_active = 0;
	G.branch_pc = target;
}
