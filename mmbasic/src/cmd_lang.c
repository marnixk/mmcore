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
	mmb_val add, cur;
	mmb_var *v;
	char buf[MMB_MAX_STR + 1];
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
	strncpy(buf, v->data.s && v->data.s[0] ? v->data.s[0] : "", sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = 0;
	{
		int n = (int)strlen(buf);
		int i;
		for (i = 0; add.s[i] && n + 1 < (int)sizeof(buf); i++)
			buf[n++] = add.s[i];
		buf[n] = 0;
	}
	cur = mmb_str_val(buf);
	mmb_do_assign(name, T_STR, nidx, idx, cur);
}

void mmb_cmd_on(void)
{
	mmb_val v;
	int n, i, gosub, target;
	mmb_skip_sp();
	if (mmb_match("ERROR"))
	{
		while (*G.p && *G.p != ':' && *G.p != '\'')
			G.p++;
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
	int nidx, idx[MMB_MAX_DIMS], t, start, ncopy, i, slen, rlen;
	mmb_val cur, repl, sv, lv;
	mmb_var *v;
	char buf[MMB_MAX_STR + 1];
	mmb_skip_sp();
	mmb_expect('(');
	t = mmb_parse_var_ref(name, &nidx, idx);
	mmb_skip_sp();
	mmb_expect(',');
	sv = mmb_expr();
	start = (int)mmb_as_int(sv);
	ncopy = MMB_MAX_STR;
	mmb_skip_sp();
	if (*G.p == ',')
	{
		G.p++;
		lv = mmb_expr();
		ncopy = (int)mmb_as_int(lv);
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
	strncpy(buf, v->data.s && v->data.s[0] ? v->data.s[0] : "", sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = 0;
	slen = (int)strlen(buf);
	if (start < 1)
		start = 1;
	if (ncopy < 0)
		ncopy = 0;
	rlen = (int)strlen(repl.s);
	if (rlen < ncopy)
		ncopy = rlen;
	for (i = 0; i < ncopy && start - 1 + i < MMB_MAX_STR; i++)
	{
		int dest = start - 1 + i;
		if (dest >= slen)
		{
			while (slen < dest)
				buf[slen++] = ' ';
			buf[dest] = repl.s[i];
			slen = dest + 1;
			buf[slen] = 0;
		}
		else
			buf[dest] = repl.s[i];
	}
	cur = mmb_str_val(buf);
	mmb_do_assign(name, T_STR, nidx, idx, cur);
}

static void do_lset_rset(int right)
{
	char name[MMB_MAX_NAME];
	int nidx = 0, idx[MMB_MAX_DIMS], t, width, slen, i, off = 0;
	mmb_val v;
	mmb_var *var;
	char buf[MMB_MAX_STR + 1];
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
	width = var->fixlen;
	if (width <= 0)
	{
		int cur = (int)strlen(var->data.s[off]);
		int vlen = (int)strlen(v.s);
		width = cur > vlen ? cur : vlen;
	}
	if (width > MMB_MAX_STR)
		width = MMB_MAX_STR;
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
	int nidx = 0, idx[MMB_MAX_DIMS], t, pos, off, len, k;
	mmb_val pv, val;
	mmb_var *v;
	char *s;
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
	if (pos < 1 || pos > MMB_MAX_STR)
		mmb_error("?BYTE");
	v = mmb_find_var(name, t ? t : T_STR, 0, nidx, idx);
	if (!v || v->type != T_STR)
		mmb_error("?TYPE MISMATCH");
	off = mmb_elem_off(v, nidx, idx);
	s = v->data.s[off];
	len = (int)strlen(s);
	if (pos - 1 >= len)
	{
		for (k = len; k < pos - 1 && k < MMB_MAX_STR; k++)
			s[k] = ' ';
		s[pos] = 0;
	}
	s[pos - 1] = (char)(mmb_as_int(val) & 0xff);
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
