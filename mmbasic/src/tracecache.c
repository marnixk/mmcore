#include "mmb_priv.h"

#define TC_SLOTS 256
#define TC_CODE  48
#define TC_VARS  8
#define TC_CONST 8
#define TC_STK   8

enum {
	TC_PUSHC = 1,
	TC_PUSHS,
	TC_PUSHA,
	TC_ADD,
	TC_SUB,
	TC_MUL,
	TC_DIV,
	TC_NEG,
	TC_STORES,
	TC_STOREA,
	TC_LT,
	TC_GT,
	TC_LE,
	TC_GE,
	TC_EQ,
	TC_NE,
	TC_JZ,
	TC_END
};

typedef struct {
	const char *key;
	const char *endp;
	uint8_t code[TC_CODE];
	int ncode;
	mmb_var *vp[TC_VARS];
	int nvp;
	double c[TC_CONST];
	int nc;
	int8_t state;
} tc_ent;

static tc_ent tab[TC_SLOTS];
static int tc_compiling;

void mmb_tcache_invalidate(void)
{
	memset(tab, 0, sizeof(tab));
}

static unsigned tc_hash(const char *k)
{
	uintptr_t x = (uintptr_t)k;
	return (unsigned)((x >> 4) ^ (x >> 9)) & (TC_SLOTS - 1);
}

static tc_ent *tc_lookup(const char *key, int create)
{
	unsigned h = tc_hash(key);
	int n;
	for (n = 0; n < 8; n++)
	{
		tc_ent *e = &tab[(h + (unsigned)n) & (TC_SLOTS - 1)];
		if (e->key == key)
			return e;
		if (!e->key)
		{
			if (!create)
				return 0;
			e->key = key;
			e->state = 0;
			return e;
		}
	}
	return 0;
}

static int tc_emit(tc_ent *e, uint8_t op)
{
	if (e->ncode >= TC_CODE)
		return 0;
	e->code[e->ncode++] = op;
	return 1;
}

static int tc_emit1(tc_ent *e, uint8_t op, uint8_t a)
{
	if (e->ncode + 2 > TC_CODE)
		return 0;
	e->code[e->ncode++] = op;
	e->code[e->ncode++] = a;
	return 1;
}

static int tc_emit2(tc_ent *e, uint8_t op, uint8_t a, uint8_t b)
{
	if (e->ncode + 3 > TC_CODE)
		return 0;
	e->code[e->ncode++] = op;
	e->code[e->ncode++] = a;
	e->code[e->ncode++] = b;
	return 1;
}

static int tc_add_var(tc_ent *e, mmb_var *v)
{
	int i;
	if (!v)
		return -1;
	for (i = 0; i < e->nvp; i++)
		if (e->vp[i] == v)
			return i;
	if (e->nvp >= TC_VARS)
		return -1;
	e->vp[e->nvp] = v;
	return e->nvp++;
}

static int tc_add_const(tc_ent *e, double x)
{
	int i;
	for (i = 0; i < e->nc; i++)
		if (e->c[i] == x)
			return i;
	if (e->nc >= TC_CONST)
		return -1;
	e->c[e->nc] = x;
	return e->nc++;
}

static int tc_parse_name(char *name, int *type)
{
	mmb_skip_sp();
	if (!mmb_is_ident(*G.p) || (*G.p >= '0' && *G.p <= '9'))
		return 0;
	mmb_ident(name, MMB_MAX_NAME);
	*type = mmb_type_suffix(name);
	if (mmb_keyword_eq(name, "TIMER") || mmb_keyword_eq(name, "DATE") ||
	    mmb_keyword_eq(name, "TIME"))
		return 0;
	return 1;
}

static int tc_parse_number(double *out)
{
	int is_int = 1;
	double f = 0;
	int64_t i = 0;
	mmb_skip_sp();
	if (!(mmb_is_digit(*G.p) || (G.p[0] == '.' && mmb_is_digit(G.p[1]))))
		return 0;
	while (mmb_is_digit(*G.p))
	{
		int d = *G.p++ - '0';
		i = i * 10 + d;
		f = f * 10.0 + d;
	}
	if (*G.p == '.')
	{
		double p = 0.1;
		is_int = 0;
		G.p++;
		while (mmb_is_digit(*G.p))
		{
			f += (*G.p++ - '0') * p;
			p *= 0.1;
		}
	}
	*out = is_int ? (double)i : f;
	return 1;
}

static int tc_compile_expr(tc_ent *e);

static int tc_compile_primary(tc_ent *e)
{
	char name[MMB_MAX_NAME];
	double num;
	int type;
	mmb_skip_sp();
	if (*G.p == '(')
	{
		G.p++;
		if (!tc_compile_expr(e))
			return 0;
		mmb_skip_sp();
		if (*G.p != ')')
			return 0;
		G.p++;
		return 1;
	}
	if (tc_parse_number(&num))
	{
		int ci = tc_add_const(e, num);
		if (ci < 0)
			return 0;
		return tc_emit1(e, TC_PUSHC, (uint8_t)ci);
	}
	if (!tc_parse_name(name, &type))
		return 0;
	mmb_skip_sp();
	if (*G.p == '(')
	{
		char iname[MMB_MAX_NAME];
		mmb_var *arr, *idx;
		int ai, ii, itype;
		G.p++;
		if (!tc_parse_name(iname, &itype))
			return 0;
		mmb_skip_sp();
		if (*G.p != ')')
			return 0;
		G.p++;
		if (type == T_STR || type == T_STRUCT)
			return 0;
		arr = mmb_find_var(name, type, 0, 1, 0);
		idx = mmb_find_var(iname, itype, 0, 0, 0);
		if (!arr || !idx || arr->dims != 1 || idx->dims != 0 ||
		    arr->type == T_STR || arr->type == T_STRUCT || G.acc_on ||
		    idx->type == T_STR || idx->type == T_STRUCT)
			return 0;
		ai = tc_add_var(e, arr);
		ii = tc_add_var(e, idx);
		if (ai < 0 || ii < 0)
			return 0;
		return tc_emit2(e, TC_PUSHA, (uint8_t)ai, (uint8_t)ii);
	}
	{
		mmb_val cv;
		mmb_var *v;
		int vi;
		if (mmb_const_lookup(name, type, &cv))
		{
			int ci = tc_add_const(e, mmb_as_float(cv));
			if (ci < 0)
				return 0;
			return tc_emit1(e, TC_PUSHC, (uint8_t)ci);
		}
		if (type == T_STR || type == T_STRUCT)
			return 0;
		v = mmb_find_var(name, type, 0, 0, 0);
		if (!v || v->dims != 0 || v->type == T_STR || v->type == T_STRUCT || G.acc_on)
		{
			G.acc_on = 0;
			return 0;
		}
		vi = tc_add_var(e, v);
		if (vi < 0)
			return 0;
		return tc_emit1(e, TC_PUSHS, (uint8_t)vi);
	}
}

static int tc_compile_unary(tc_ent *e)
{
	mmb_skip_sp();
	if (*G.p == '-')
	{
		G.p++;
		if (!tc_compile_unary(e))
			return 0;
		return tc_emit(e, TC_NEG);
	}
	if (*G.p == '+')
		G.p++;
	return tc_compile_primary(e);
}

static int tc_compile_term(tc_ent *e)
{
	if (!tc_compile_unary(e))
		return 0;
	for (;;)
	{
		uint8_t op;
		mmb_skip_sp();
		if (*G.p == '*')
			op = TC_MUL;
		else if (*G.p == '/')
			op = TC_DIV;
		else
			break;
		G.p++;
		if (!tc_compile_unary(e))
			return 0;
		if (!tc_emit(e, op))
			return 0;
	}
	return 1;
}

static int tc_compile_expr(tc_ent *e)
{
	if (!tc_compile_term(e))
		return 0;
	for (;;)
	{
		uint8_t op;
		mmb_skip_sp();
		if (*G.p == '+')
			op = TC_ADD;
		else if (*G.p == '-')
			op = TC_SUB;
		else
			break;
		G.p++;
		if (!tc_compile_term(e))
			return 0;
		if (!tc_emit(e, op))
			return 0;
	}
	return 1;
}

static int tc_compile_store(tc_ent *e, mmb_var *dst, mmb_var *idx)
{
	int di, ii;
	di = tc_add_var(e, dst);
	if (di < 0)
		return 0;
	if (!idx)
		return tc_emit1(e, TC_STORES, (uint8_t)di);
	ii = tc_add_var(e, idx);
	if (ii < 0)
		return 0;
	return tc_emit2(e, TC_STOREA, (uint8_t)di, (uint8_t)ii);
}

static int tc_at_stmt_end(void)
{
	mmb_skip_sp();
	return *G.p == 0 || *G.p == ':' || *G.p == '\'';
}

static int tc_compile_let_from_lhs(tc_ent *e)
{
	char name[MMB_MAX_NAME];
	mmb_var *dst, *idx = 0;
	int type;
	if (!tc_parse_name(name, &type))
		return 0;
	mmb_skip_sp();
	if (*G.p == '(')
	{
		char iname[MMB_MAX_NAME];
		int itype;
		G.p++;
		if (!tc_parse_name(iname, &itype))
			return 0;
		mmb_skip_sp();
		if (*G.p != ')')
			return 0;
		G.p++;
		if (type == T_STR || type == T_STRUCT)
			return 0;
		dst = mmb_find_var(name, type, 0, 1, 0);
		idx = mmb_find_var(iname, itype, 0, 0, 0);
		if (!dst || !idx || dst->dims != 1 || idx->dims != 0 ||
		    dst->type == T_STR || dst->type == T_STRUCT || G.acc_on ||
		    idx->type == T_STR || idx->type == T_STRUCT)
			return 0;
	}
	else
	{
		if (type == T_STR || type == T_STRUCT)
			return 0;
		dst = mmb_find_var(name, type, G.opt.explicit ? 0 : 1, 0, 0);
		if (!dst || dst->dims != 0 || dst->type == T_STR || dst->type == T_STRUCT || G.acc_on)
		{
			G.acc_on = 0;
			return 0;
		}
	}
	mmb_skip_sp();
	if (*G.p != '=')
		return 0;
	G.p++;
	if (!tc_compile_expr(e))
		return 0;
	if (!tc_at_stmt_end())
		return 0;
	return tc_compile_store(e, dst, idx);
}

static int tc_relop(uint8_t *op)
{
	mmb_skip_sp();
	if (G.p[0] == '<' && G.p[1] == '>')
	{
		G.p += 2;
		*op = TC_NE;
		return 1;
	}
	if (G.p[0] == '<' && G.p[1] == '=')
	{
		G.p += 2;
		*op = TC_LE;
		return 1;
	}
	if (G.p[0] == '>' && G.p[1] == '=')
	{
		G.p += 2;
		*op = TC_GE;
		return 1;
	}
	if (*G.p == '<')
	{
		G.p++;
		*op = TC_LT;
		return 1;
	}
	if (*G.p == '>')
	{
		G.p++;
		*op = TC_GT;
		return 1;
	}
	if (*G.p == '=')
	{
		G.p++;
		*op = TC_EQ;
		return 1;
	}
	return 0;
}

static int tc_compile_if(tc_ent *e)
{
	uint8_t op;
	if (!tc_compile_expr(e))
		return 0;
	if (!tc_relop(&op))
		return 0;
	if (!tc_compile_expr(e))
		return 0;
	if (!tc_emit(e, op))
		return 0;
	if (!tc_emit(e, TC_JZ))
		return 0;
	if (!mmb_match("THEN"))
		return 0;
	mmb_skip_sp();
	if (tc_at_stmt_end())
		return 0;
	return tc_compile_let_from_lhs(e);
}

static double tc_load_scl(mmb_var *v)
{
	if (v->type == T_INT)
		return (double)v->data.i[0];
	return v->data.f[0];
}

static double tc_load_arr(mmb_var *arr, mmb_var *idx)
{
	int64_t i = idx->type == T_INT ? idx->data.i[0] : (int64_t)idx->data.f[0];
	int off = (int)i - G.opt.base;
	if (off < 0 || off >= arr->size)
		mmb_error("?INDEX OUT OF BOUNDS");
	if (arr->type == T_INT)
		return (double)arr->data.i[off];
	return arr->data.f[off];
}

static void tc_store_scl(mmb_var *v, double x)
{
	if (v->type == T_INT)
		v->data.i[0] = (int64_t)x;
	else
		v->data.f[0] = x;
}

static void tc_store_arr(mmb_var *arr, mmb_var *idx, double x)
{
	int64_t i = idx->type == T_INT ? idx->data.i[0] : (int64_t)idx->data.f[0];
	int off = (int)i - G.opt.base;
	if (off < 0 || off >= arr->size)
		mmb_error("?INDEX OUT OF BOUNDS");
	if (arr->type == T_INT)
		arr->data.i[off] = (int64_t)x;
	else
		arr->data.f[off] = x;
}

static int tc_replay(tc_ent *e)
{
	double stk[TC_STK];
	int sp = 0;
	int pc = 0;
	while (pc < e->ncode)
	{
		uint8_t op = e->code[pc++];
		if (op == TC_END)
			break;
		if (op == TC_JZ)
		{
			if (sp < 1)
				return 0;
			if (stk[--sp] == 0.0)
				break;
			continue;
		}
		if (op == TC_PUSHC)
		{
			if (sp >= TC_STK || pc >= e->ncode)
				return 0;
			stk[sp++] = e->c[e->code[pc++]];
			continue;
		}
		if (op == TC_PUSHS)
		{
			if (sp >= TC_STK || pc >= e->ncode)
				return 0;
			stk[sp++] = tc_load_scl(e->vp[e->code[pc++]]);
			continue;
		}
		if (op == TC_PUSHA)
		{
			uint8_t a, i;
			if (sp >= TC_STK || pc + 1 >= e->ncode)
				return 0;
			a = e->code[pc++];
			i = e->code[pc++];
			stk[sp++] = tc_load_arr(e->vp[a], e->vp[i]);
			continue;
		}
		if (op == TC_STORES)
		{
			if (sp < 1 || pc >= e->ncode)
				return 0;
			tc_store_scl(e->vp[e->code[pc++]], stk[--sp]);
			continue;
		}
		if (op == TC_STOREA)
		{
			uint8_t a, i;
			if (sp < 1 || pc + 1 >= e->ncode)
				return 0;
			a = e->code[pc++];
			i = e->code[pc++];
			tc_store_arr(e->vp[a], e->vp[i], stk[--sp]);
			continue;
		}
		if (op == TC_NEG)
		{
			if (sp < 1)
				return 0;
			stk[sp - 1] = -stk[sp - 1];
			continue;
		}
		if (op == TC_ADD || op == TC_SUB || op == TC_MUL || op == TC_DIV ||
		    op == TC_LT || op == TC_GT || op == TC_LE || op == TC_GE ||
		    op == TC_EQ || op == TC_NE)
		{
			double x, y, r = 0;
			if (sp < 2)
				return 0;
			y = stk[--sp];
			x = stk[--sp];
			if (op == TC_ADD)
				r = x + y;
			else if (op == TC_SUB)
				r = x - y;
			else if (op == TC_MUL)
				r = x * y;
			else if (op == TC_DIV)
				r = x / y;
			else if (op == TC_LT)
				r = x < y ? 1.0 : 0.0;
			else if (op == TC_GT)
				r = x > y ? 1.0 : 0.0;
			else if (op == TC_LE)
				r = x <= y ? 1.0 : 0.0;
			else if (op == TC_GE)
				r = x >= y ? 1.0 : 0.0;
			else if (op == TC_EQ)
				r = x == y ? 1.0 : 0.0;
			else
				r = x != y ? 1.0 : 0.0;
			stk[sp++] = r;
			continue;
		}
		return 0;
	}
	G.p = e->endp;
	return 1;
}

static int tc_try(int is_if)
{
	const char *key;
	const char *save;
	tc_ent *e;
	tc_ent tmp;
	if (!G.running || !G.opt.tracecache || tc_compiling)
		return 0;
	key = G.p;
	e = tc_lookup(key, 1);
	if (!e)
		return 0;
	if (e->state < 0)
		return 0;
	if (e->state > 0)
	{
		if (G.opt.profiling)
			G.prof.tcache_hit++;
		return tc_replay(e);
	}
	save = G.p;
	memset(&tmp, 0, sizeof(tmp));
	tmp.key = key;
	tc_compiling = 1;
	if (is_if)
	{
		if (!tc_compile_if(&tmp) || !tc_emit(&tmp, TC_END))
		{
			tc_compiling = 0;
			G.p = save;
			e->state = -1;
			if (G.opt.profiling)
				G.prof.tcache_bad++;
			return 0;
		}
	}
	else if (!tc_compile_let_from_lhs(&tmp) || !tc_emit(&tmp, TC_END))
	{
		tc_compiling = 0;
		G.p = save;
		e->state = -1;
		if (G.opt.profiling)
			G.prof.tcache_bad++;
		return 0;
	}
	tc_compiling = 0;
	tmp.endp = G.p;
	tmp.state = 1;
	*e = tmp;
	G.p = save;
	if (G.opt.profiling)
		G.prof.tcache_comp++;
	return tc_replay(e);
}

int mmb_tcache_try_let(void)
{
	return tc_try(0);
}

int mmb_tcache_try_if(void)
{
	return tc_try(1);
}
