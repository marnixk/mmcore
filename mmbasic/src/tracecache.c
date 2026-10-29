#include "mmb_priv.h"
#include <math.h>

/* Storage lives in the interpreter instance (#1013): the cache is keyed by
 * pointers into that instance's tokenized program and holds pointers to that
 * instance's variables, so a shared table aliased across consoles. The slot
 * struct and capacity constants are in mmb_priv.h. */
#define TC_SLOTS MMB_TC_SLOTS
#define TC_MAX_SLOTS MMB_TC_MAX_SLOTS
#define TC_NEG_SLOTS MMB_TC_NEG_SLOTS
#define TC_NEG_MAX_SLOTS MMB_TC_NEG_MAX_SLOTS
#define TC_CODE  MMB_TC_CODE
#define TC_VARS  MMB_TC_VARS
#define TC_CONST MMB_TC_CONST
#define TC_STK   24
#define TC_PROBES 8
#define tc_ent   mmb_tc_ent
#define tc_neg   mmb_tc_neg

enum {
	TC_PUSHC = 1,
	TC_PUSHS,
	TC_PUSHA,   /* [op, arr, n]: n index values are on the stack */
	TC_ADD,
	TC_SUB,
	TC_MUL,
	TC_DIV,
	TC_IDIV,
	TC_MOD,
	TC_NEG,
	TC_NOT,
	TC_AND,
	TC_OR,
	TC_XOR,
	TC_STORES,
	TC_STOREA,  /* [op, arr, n]: n index values then the value are on the stack */
	TC_LT,
	TC_GT,
	TC_LE,
	TC_GE,
	TC_EQ,
	TC_NE,
	TC_JZ,
	TC_IFBLOCK, /* block IF: pop condition, push IF_BODY/IF_SEARCH */
	TC_GOTO,    /* target is e->branch_pc */
	TC_GOSUB,   /* target is e->branch_pc, pushes the caller frame */
	TC_END
};

/* The active instance's table; see mmb_tc_ent in mmb_priv.h (#1013). */
#define tab (G.tcache)
static int tc_compiling;

/* Both tables start inline (tcache_base/tcneg_base) and move to a heap table
 * only when a program fills the base. A context memset to zero (fresh console,
 * warm reset) lazily restores the inline tables here. */
static void tc_ensure(void)
{
	if (!G.tcache || !G.tcache_cap)
	{
		G.tcache = G.tcache_base;
		G.tcache_cap = TC_SLOTS;
		G.tcache_used = 0;
		G.tcache_hand = 0;
		memset(G.tcache_base, 0, sizeof(G.tcache_base));
	}
	if (!G.tcneg || !G.tcneg_cap)
	{
		G.tcneg = G.tcneg_base;
		G.tcneg_cap = TC_NEG_SLOTS;
		G.tcneg_used = 0;
		memset(G.tcneg_base, 0, sizeof(G.tcneg_base));
	}
}

void mmb_tcache_invalidate(void)
{
	/* Return any grown table so a large program does not pin its high-water
	 * mark for the rest of the console's life (#1369). */
	if (G.tcache && G.tcache != G.tcache_base && G.plat && G.plat->free)
		G.plat->free(G.tcache);
	G.tcache = G.tcache_base;
	G.tcache_cap = TC_SLOTS;
	G.tcache_used = 0;
	G.tcache_hand = 0;
	memset(G.tcache_base, 0, sizeof(G.tcache_base));

	if (G.tcneg && G.tcneg != G.tcneg_base && G.plat && G.plat->free)
		G.plat->free(G.tcneg);
	G.tcneg = G.tcneg_base;
	G.tcneg_cap = TC_NEG_SLOTS;
	G.tcneg_used = 0;
	memset(G.tcneg_base, 0, sizeof(G.tcneg_base));
}

static unsigned tc_hash(const char *k, unsigned cap)
{
	uintptr_t x = (uintptr_t)k;
	return (unsigned)((x >> 4) ^ (x >> 9)) & (cap - 1);
}

/* Look up a compiled statement. Only entries that were fully compiled are in
 * this table; failed compiles live in the negative set. */
static tc_ent *tc_lookup(const char *key)
{
	unsigned cap, h, n;
	if (!G.tcache || !G.tcache_cap)
		return 0;
	cap = G.tcache_cap;
	h = tc_hash(key, cap);
	for (n = 0; n < TC_PROBES; n++)
	{
		tc_ent *e = &tab[(h + n) & (cap - 1)];
		if (e->key == key)
			return e;
		if (!e->key)
			return 0;
	}
	return 0;
}

/* Remove one compiled entry and close the linear-probe hole so entries stored
 * after it stay reachable. A stale binding (LOCAL exit, DIM) is dropped this
 * way rather than by leaking an unreachable slot (#1368, #1369). */
static void tc_remove(tc_ent *victim)
{
	unsigned cap = G.tcache_cap, mask = cap - 1;
	unsigned i = (unsigned)(victim - G.tcache);
	unsigned j = i;

	/* Zero first: a full table has no other hole, and the shift loop needs
	 * one to terminate. */
	G.tcache[i].key = 0;
	for (;;)
	{
		tc_ent *e;
		unsigned home, di, dj;
		j = (j + 1) & mask;
		e = &G.tcache[j];
		if (!e->key)
			break;
		home = tc_hash(e->key, cap);
		di = (i - home) & mask;
		dj = (j - home) & mask;
		if (di <= dj)
		{
			G.tcache[i] = *e;
			i = j;
		}
	}
	memset(&G.tcache[i], 0, sizeof(G.tcache[i]));
	if (G.tcache_used)
		G.tcache_used--;
}

static int tc_grow(void)
{
	unsigned cap = G.tcache_cap, ncap, i, k, mask;
	unsigned used = 0;
	tc_ent *nt;

	if (cap >= TC_MAX_SLOTS || !G.plat || !G.plat->alloc)
		return 0;
	ncap = cap * 2;
	if (ncap > TC_MAX_SLOTS)
		ncap = TC_MAX_SLOTS;
	nt = (tc_ent *)G.plat->alloc(ncap * sizeof(*nt));
	if (!nt)
		return 0;
	memset(nt, 0, ncap * sizeof(*nt));
	mask = ncap - 1;
	for (i = 0; i < cap; i++)
	{
		tc_ent *old = &tab[i];
		unsigned h;
		if (!old->key)
			continue;
		h = tc_hash(old->key, ncap);
		/* Keep the invariant that every stored key is reachable within
		 * TC_PROBES lookups. A rare key that would land further out is
		 * dropped and simply recompiled when it next runs. */
		for (k = 0; k < TC_PROBES; k++)
		{
			tc_ent *dst = &nt[(h + k) & mask];
			if (!dst->key)
			{
				*dst = *old;
				used++;
				break;
			}
		}
	}
	if (G.tcache != G.tcache_base && G.plat->free)
		G.plat->free(G.tcache);
	G.tcache = nt;
	G.tcache_cap = ncap;
	G.tcache_used = used;
	return 1;
}

/* Pick a victim inside the new key's probe window so the replacement stays
 * reachable from its own hash. Second chance clears the window's reference
 * bits before wrapping. Only a table already at TC_MAX_SLOTS gets here. */
static tc_ent *tc_evict(unsigned h)
{
	unsigned cap = G.tcache_cap, mask = cap - 1, n;
	for (n = 0; n < TC_PROBES; n++)
	{
		tc_ent *e = &tab[(h + n) & mask];
		if (!e->key || !e->ref)
			return e;
	}
	for (n = 0; n < TC_PROBES; n++)
		tab[(h + n) & mask].ref = 0;
	return &tab[h & mask];
}

/* Find or reserve a slot for a compiled statement, growing the table when its
 * probe window is full and there is room, otherwise evicting a cold entry. */
static tc_ent *tc_insert(const char *key)
{
	for (;;)
	{
		unsigned cap = G.tcache_cap, mask = cap - 1;
		unsigned h = tc_hash(key, cap), n;
		tc_ent *e;

		for (n = 0; n < TC_PROBES; n++)
		{
			e = &tab[(h + n) & mask];
			if (e->key == key)
				return e;
			if (!e->key)
			{
				memset(e, 0, sizeof(*e));
				e->key = key;
				e->state = 0;
				e->ref = 1;
				G.tcache_used++;
				if (G.tcache_cap < TC_MAX_SLOTS &&
				    G.tcache_used * 4 >= G.tcache_cap * 3 &&
				    tc_grow())
					return tc_lookup(key); /* table moved */
				return e;
			}
		}
		if (G.tcache_cap < TC_MAX_SLOTS && tc_grow())
			continue;
		e = tc_evict(h);
		memset(e, 0, sizeof(*e));
		e->key = key;
		e->state = 0;
		e->ref = 1;
		return e;
	}
}

/* Failed compiles: a compact open-addressed set keyed by the same statement
 * pointer. A retry happens only when G.var_gen changes, matching the old
 * state == -1 slot's fail_gen check (#1368), without consuming 256 bytes. */
static tc_neg *tcneg_find(const char *key)
{
	unsigned cap, h, n;
	if (!G.tcneg || !G.tcneg_cap)
		return 0;
	cap = G.tcneg_cap;
	h = tc_hash(key, cap);
	for (n = 0; n < TC_PROBES; n++)
	{
		tc_neg *e = &G.tcneg[(h + n) & (cap - 1)];
		if (e->key == key)
			return e;
		if (!e->key)
			return 0;
	}
	return 0;
}

static int tcneg_grow(void)
{
	unsigned cap = G.tcneg_cap, ncap, i, k, mask;
	unsigned used = 0;
	tc_neg *nt;

	if (cap >= TC_NEG_MAX_SLOTS || !G.plat || !G.plat->alloc)
		return 0;
	ncap = cap * 2;
	if (ncap > TC_NEG_MAX_SLOTS)
		ncap = TC_NEG_MAX_SLOTS;
	nt = (tc_neg *)G.plat->alloc(ncap * sizeof(*nt));
	if (!nt)
		return 0;
	memset(nt, 0, ncap * sizeof(*nt));
	mask = ncap - 1;
	for (i = 0; i < cap; i++)
	{
		tc_neg *old = &G.tcneg[i];
		unsigned h;
		if (!old->key)
			continue;
		h = tc_hash(old->key, ncap);
		for (k = 0; k < TC_PROBES; k++)
		{
			tc_neg *dst = &nt[(h + k) & mask];
			if (!dst->key)
			{
				*dst = *old;
				used++;
				break;
			}
		}
	}
	if (G.tcneg != G.tcneg_base && G.plat->free)
		G.plat->free(G.tcneg);
	G.tcneg = nt;
	G.tcneg_cap = ncap;
	G.tcneg_used = used;
	return 1;
}

static void tcneg_add(const char *key, unsigned gen)
{
	for (;;)
	{
		unsigned cap = G.tcneg_cap, mask = cap - 1;
		unsigned h = tc_hash(key, cap), n;
		tc_neg *e;

		for (n = 0; n < TC_PROBES; n++)
		{
			e = &G.tcneg[(h + n) & mask];
			if (e->key == key)
			{
				e->fail_gen = gen;
				return;
			}
			if (!e->key)
			{
				e->key = key;
				e->fail_gen = gen;
				G.tcneg_used++;
				if (G.tcneg_cap < TC_NEG_MAX_SLOTS &&
				    G.tcneg_used * 4 >= G.tcneg_cap * 3)
					tcneg_grow();
				return;
			}
		}
		if (G.tcneg_cap < TC_NEG_MAX_SLOTS && tcneg_grow())
			continue;
		/* At capacity: prefer dropping a statement that failed under an
		 * older binding set, whose retry is already due. */
		e = &G.tcneg[h & mask];
		for (n = 0; n < TC_PROBES; n++)
		{
			tc_neg *cand = &G.tcneg[(h + n) & mask];
			if (cand->key && cand->fail_gen != G.var_gen)
			{
				e = cand;
				break;
			}
		}
		e->key = key;
		e->fail_gen = gen;
		return;
	}
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
	e->vgen[e->nvp] = v->gen;
	return e->nvp++;
}

/* Constants keep their T_INT/T_NUM type.  `\\`, MOD, AND/OR and NOT round or
 * truncate differently depending on it, so folding 1 and 1.0 into one slot
 * would change results (#1370). */
static int tc_add_const(tc_ent *e, mmb_val v)
{
	int i;
	if (v.type != T_INT && v.type != T_NUM)
		return -1;
	for (i = 0; i < e->nc; i++)
	{
		if (e->ctype[i] != v.type)
			continue;
		if (v.type == T_INT ? e->ci[i] == v.i : e->c[i] == v.f)
			return i;
	}
	if (e->nc >= TC_CONST)
		return -1;
	e->ctype[e->nc] = (uint8_t)v.type;
	if (v.type == T_INT)
		e->ci[e->nc] = v.i;
	else
		e->c[e->nc] = v.f;
	return e->nc++;
}

/* A dotted name is a struct member only when the part before the first dot
 * names a declared struct variable. CMM2 also allows plain variables whose
 * name contains a dot (e.g. ``DIM POS.X(100)``), which this cache can still
 * compile. Resolving a member array needs its parsed index values, which the
 * scalar/array compiler here does not carry (it passes a NULL index vector to
 * mmb_find_var()), so struct members are left to the interpreter's own parser.
 * mmb_ident() folds '.' into the identifier, hence this explicit split. */
static int tc_is_struct_member(const char *name)
{
	char base[MMB_MAX_NAME];
	const char *dot = strchr(name, '.');
	int n;
	if (!dot)
		return 0;
	n = (int)(dot - name);
	if (n <= 0 || n >= MMB_MAX_NAME)
		return 0;
	memcpy(base, name, (size_t)n);
	base[n] = 0;
	return mmb_lookup_struct_var(base) != 0;
}

static int tc_parse_name(char *name, int *type)
{
	mmb_skip_sp();
	if (!mmb_is_ident_start(*G.p))
		return 0;
	mmb_ident(name, MMB_MAX_NAME);
	*type = mmb_type_suffix(name);
	if (mmb_keyword_eq(name, "TIMER") || mmb_keyword_eq(name, "DATE") ||
	    mmb_keyword_eq(name, "TIME") ||
	    mmb_keyword_eq(name, "MM.BRIGHTNESS"))
		return 0;
	if (tc_is_struct_member(name))
		return 0;
	return 1;
}

/* Mirrors expr.c's parse_number(), including the &H / &B forms and exponent,
 * so a literal the interpreter accepts keeps the same type and value here. */
static int tc_parse_number(mmb_val *out)
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
			if (d >= 'a')
				d -= 32;
			d = d <= '9' ? d - '0' : d - 'A' + 10;
			i = (i << 4) | d;
		}
		*out = mmb_int_val(i);
		return 1;
	}
	if (G.p[0] == '&' && (G.p[1] == 'B' || G.p[1] == 'b'))
	{
		G.p += 2;
		i = 0;
		while (*G.p == '0' || *G.p == '1')
			i = (i << 1) | (*G.p++ - '0');
		*out = mmb_int_val(i);
		return 1;
	}
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
	if (*G.p == 'E' || *G.p == 'e')
	{
		int es = 1, ex = 0;
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
			ex = ex * 10 + (*G.p++ - '0');
		{
			int k;
			double m = 1;
			for (k = 0; k < ex; k++)
				m *= 10.0;
			if (es < 0)
				f /= m;
			else
				f *= m;
		}
	}
	*out = is_int ? mmb_int_val(i) : mmb_num_val(f);
	return 1;
}

static int tc_compile_expr(tc_ent *e);

/* name(variable) is array access only when `name` is not a user FUNCTION.
 * A function's scalar return variable would otherwise be mistaken for an
 * array and mmb_find_var() would fault with "?NOT AN ARRAY". */
static int tc_is_user_func(const char *name)
{
	/* The SUB name index makes this O(1) at compile time (#1418). */
	return mmb_sub_is_func(name);
}

/* Non-throwing variable lookup for the trace-cache compiler. mmb_find_var()
 * raises a BASIC error when a name is used with the wrong number of subscripts
 * (an array index variable such as `a(qq(0))` arrives here with nidx == 0, so
 * mmb_find_var() would raise ?ARRAY). Compilation must never raise: an
 * unsupported shape has to return -1 and let the interpreter parse the
 * statement (#1310). Type resolution mirrors mmb_find_var(): an explicit
 * suffix wins, then an existing unsuffixed binding, then OPTION's default. */
static mmb_var *tc_find_var(const char *name, int type)
{
	/* Resolved through the #1303 name hash, so a compile is O(1) in the
	 * number of globals rather than two linear sweeps (#1385). */
	return mmb_lookup_var_nothrow(name, type);
}

/* Create a scalar on the LHS path, but only in shapes where mmb_find_var()
 * cannot raise. On any doubt the compiler bails and the interpreter creates
 * the variable while executing the statement. */
static mmb_var *tc_create_scalar(const char *name, int type)
{
	int is_colour;
	if (type == T_STR || type == T_STRUCT || strchr(name, '.'))
		return 0;
	if (G.opt.explicit || mmb_is_builtin_func(name))
		return 0;
	mmb_named_colour(name, &is_colour);
	if (is_colour)
		return 0;
	/* Bail when the lazily allocated table is absent or full: creating the
	 * variable here could allocate/grow it and invalidate the trace cache
	 * mid-compile (#1303). The interpreter creates it instead. */
	if (!G.vars || G.nvars >= G.vars_cap)
		return 0;
	return mmb_find_var(name, type, 1, 0, 0);
}

/* Compile a comma-separated index list, evaluating every element in source
 * order (the interpreter evaluates subscripts before the right-hand side).
 * Returns the index count (>=1), or 0 when a subscript is outside the shapes
 * this compiler can prove safe.  The values are pushed on the replay stack;
 * TC_PUSHA/TC_STOREA consume them. */
static int tc_compile_indices(tc_ent *e, int *nidx)
{
	int n = 0;
	for (;;)
	{
		if (n >= MMB_MAX_DIMS)
			return 0;
		if (!tc_compile_expr(e))
			return 0;
		n++;
		mmb_skip_sp();
		if (*G.p == ',')
		{
			G.p++;
			continue;
		}
		break;
	}
	*nidx = n;
	return 1;
}

/* expr_unary()'s keyword test: `NOT%` is a variable, not an operator, so a
 * type suffix immediately after the keyword means this is not NOT. */
static int tc_match_unary(const char *kw)
{
	mmb_skip_sp();
	if ((unsigned char)*G.p != 0x80)
	{
		char c = G.p[strlen(kw)];
		if (c == '$' || c == '%' || c == '!')
			return 0;
	}
	return mmb_match(kw);
}

static int tc_compile_primary(tc_ent *e)
{
	char name[MMB_MAX_NAME];
	mmb_val num;
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
		mmb_var *arr;
		int ai, nidx;
		if (tc_is_user_func(name))
			return 0;
		G.p++;
		if (!tc_compile_indices(e, &nidx))
			return 0;
		mmb_skip_sp();
		if (*G.p != ')')
			return 0;
		G.p++;
		if (type == T_STR || type == T_STRUCT)
			return 0;
		arr = tc_find_var(name, type);
		if (!arr || arr->dims != nidx || arr->type == T_STR ||
		    arr->type == T_STRUCT || G.acc_on)
			return 0;
		ai = tc_add_var(e, arr);
		if (ai < 0)
			return 0;
		return tc_emit2(e, TC_PUSHA, (uint8_t)ai, (uint8_t)nidx);
	}
	{
		mmb_val cv;
		mmb_var *v;
		int vi;
		if (mmb_const_lookup(name, type, &cv))
		{
			int ci;
			if (cv.type != T_INT && cv.type != T_NUM)
				return 0;
			ci = tc_add_const(e, cv);
			if (ci < 0)
				return 0;
			return tc_emit1(e, TC_PUSHC, (uint8_t)ci);
		}
		if (type == T_STR || type == T_STRUCT)
			return 0;
		v = tc_find_var(name, type);
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
	{
		G.p++;
		return tc_compile_unary(e);
	}
	if (*G.p == '#')
	{
		G.p++;
		return tc_compile_unary(e);
	}
	if (tc_match_unary("NOT"))
	{
		if (!tc_compile_unary(e))
			return 0;
		return tc_emit(e, TC_NOT);
	}
	return tc_compile_primary(e);
}

/* *, /, \\ and MOD share expr_mul()'s precedence and left associativity. */
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
		else if (*G.p == '\\')
			op = TC_IDIV;
		else if (mmb_match("MOD"))
			op = TC_MOD;
		else
			break;
		if (op != TC_MOD)
			G.p++;
		if (!tc_compile_unary(e))
			return 0;
		if (!tc_emit(e, op))
			return 0;
	}
	return 1;
}

static int tc_compile_add(tc_ent *e)
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

/* expr_rel() takes at most one comparison; a chained `<` is a syntax error in
 * the interpreter, and this compiler falls back so the same error surfaces. */
static int tc_compile_rel(tc_ent *e)
{
	uint8_t op;
	if (!tc_compile_add(e))
		return 0;
	mmb_skip_sp();
	if (G.p[0] == '<' && G.p[1] == '>')
	{
		G.p += 2;
		op = TC_NE;
	}
	else if (G.p[0] == '<' && G.p[1] == '=')
	{
		G.p += 2;
		op = TC_LE;
	}
	else if (G.p[0] == '>' && G.p[1] == '=')
	{
		G.p += 2;
		op = TC_GE;
	}
	else if (*G.p == '<')
	{
		G.p++;
		op = TC_LT;
	}
	else if (*G.p == '>')
	{
		G.p++;
		op = TC_GT;
	}
	else if (*G.p == '=')
	{
		G.p++;
		op = TC_EQ;
	}
	else
		return 1;
	if (!tc_compile_add(e))
		return 0;
	return tc_emit(e, op);
}

static int tc_compile_and(tc_ent *e)
{
	if (!tc_compile_rel(e))
		return 0;
	for (;;)
	{
		mmb_skip_sp();
		if (!mmb_match("AND"))
			break;
		if (!tc_compile_rel(e))
			return 0;
		if (!tc_emit(e, TC_AND))
			return 0;
	}
	return 1;
}

/* AND/OR/XOR all evaluate both operands: MMBasic has no short-circuit, so a
 * right-hand side that raises (?INDEX OUT OF BOUNDS) still does. */
static int tc_compile_expr(tc_ent *e)
{
	if (!tc_compile_and(e))
		return 0;
	for (;;)
	{
		uint8_t op;
		mmb_skip_sp();
		if (mmb_match("OR"))
			op = TC_OR;
		else if (mmb_match("XOR"))
			op = TC_XOR;
		else
			break;
		if (!tc_compile_and(e))
			return 0;
		if (!tc_emit(e, op))
			return 0;
	}
	return 1;
}

static int tc_compile_store(tc_ent *e, mmb_var *dst, int nidx)
{
	int di;
	di = tc_add_var(e, dst);
	if (di < 0)
		return 0;
	if (nidx == 0)
		return tc_emit1(e, TC_STORES, (uint8_t)di);
	return tc_emit2(e, TC_STOREA, (uint8_t)di, (uint8_t)nidx);
}

static int tc_at_stmt_end(void)
{
	mmb_skip_sp();
	return *G.p == 0 || *G.p == ':' || *G.p == '\'';
}

static int tc_compile_let_from_lhs(tc_ent *e)
{
	char name[MMB_MAX_NAME];
	mmb_var *dst = 0;
	int type, nidx = 0;
	if (!tc_parse_name(name, &type))
		return 0;
	mmb_skip_sp();
	if (*G.p == '(')
	{
		G.p++;
		if (!tc_compile_indices(e, &nidx))
			return 0;
		mmb_skip_sp();
		if (*G.p != ')')
			return 0;
		G.p++;
		if (type == T_STR || type == T_STRUCT)
			return 0;
		dst = tc_find_var(name, type);
		if (!dst || dst->dims != nidx || dst->type == T_STR ||
		    dst->type == T_STRUCT || G.acc_on)
			return 0;
	}
	else
	{
		if (type == T_STR || type == T_STRUCT)
			return 0;
		dst = tc_find_var(name, type);
		if (!dst)
			dst = tc_create_scalar(name, type);
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
	return tc_compile_store(e, dst, nidx);
}

/* Non-throwing equivalents of core.c's find_label_pc()/mmb_find_line_pc().
 * Compilation may not raise: an unknown target is just a statement this
 * compiler declines, and the interpreter reports the real ?LABEL/?LINE error
 * when the branch actually runs. */
static int tc_find_label_safe(const char *name)
{
	int i;
	for (i = 0; i < G.nlabels; i++)
		if (G.labels[i].used && mmb_keyword_eq(G.labels[i].name, name))
			return G.labels[i].pc;
	return -1;
}

static int tc_find_line_safe(int num)
{
	int lo = 0, hi = G.nprog - 1;
	while (lo <= hi)
	{
		int mid = (lo + hi) >> 1;
		if (G.prog_num[mid] == num)
			return mid;
		if (G.prog_num[mid] < num)
			lo = mid + 1;
		else
			hi = mid - 1;
	}
	return -1;
}

/* Resolve a GOTO/GOSUB target exactly like core.c's parse_target(): a bare
 * identifier is a label, anything numeric is a line number.  Only the simple
 * single-token forms are cached; an expression target keeps its side effects
 * and stays in the interpreter. */
static int tc_parse_target(int *pc)
{
	mmb_skip_sp();
	if (mmb_is_digit(*G.p))
	{
		int num = 0;
		while (mmb_is_digit(*G.p))
			num = num * 10 + (*G.p++ - '0');
		mmb_skip_sp();
		if (*G.p != 0 && *G.p != '\'')
			return 0;
		*pc = tc_find_line_safe(num);
		return *pc >= 0;
	}
	if (mmb_is_ident_start(*G.p))
	{
		char name[MMB_MAX_NAME];
		mmb_ident(name, sizeof(name));
		mmb_type_suffix(name);
		mmb_skip_sp();
		if (*G.p != 0 && *G.p != '\'')
			return 0;
		*pc = tc_find_label_safe(name);
		return *pc >= 0;
	}
	return 0;
}

static int tc_compile_if(tc_ent *e)
{
	int pc;
	if (!tc_compile_expr(e))
		return 0;
	if (!mmb_match("THEN"))
		return 0;
	mmb_skip_sp();
	/* A THEN at end of line opens a multi-line block IF. Evaluate its
	 * condition here and push the same IF_BODY/IF_SEARCH frame the
	 * interpreter would have pushed (#1419). */
	if (tc_at_stmt_end())
		return tc_emit(e, TC_IFBLOCK);
	if (!tc_emit(e, TC_JZ))
		return 0;
	if (mmb_match("GOTO"))
	{
		if (!tc_parse_target(&pc))
			return 0;
		e->branch_pc = pc;
		return tc_emit(e, TC_GOTO);
	}
	if (mmb_match("GOSUB"))
	{
		if (!tc_parse_target(&pc))
			return 0;
		e->branch_pc = pc;
		return tc_emit(e, TC_GOSUB);
	}
	if (!tc_compile_let_from_lhs(e))
		return 0;
	/* Only a one-statement THEN arm can be replayed here: the interpreter
	 * owns the rest of the line for `... : ...` and `ELSE` arms (#1276). */
	mmb_skip_sp();
	return *G.p == 0 || *G.p == '\'';
}

static mmb_val tc_load_var(mmb_var *v, int off)
{
	if (v->type == T_INT)
		return mmb_int_val(v->data.i[off]);
	return mmb_num_val(v->data.f[off]);
}

static void tc_store_var(mmb_var *v, int off, mmb_val x)
{
	if (v->type == T_INT)
		v->data.i[off] = mmb_as_int(x);
	else
		v->data.f[off] = mmb_as_float(x);
}

/* Subscript conversion matches mmb_parse_var_ref(): (int)mmb_as_int(v), i.e.
 * fractional indices truncate toward zero.  mmb_var_offset() then applies the
 * same base/stride and bounds rules as the interpreter (#1002). */
static int tc_elem_off(mmb_var *arr, const mmb_val *idx, int n)
{
	int iv[MMB_MAX_DIMS];
	int k;
	for (k = 0; k < n; k++)
		iv[k] = (int)mmb_as_int(idx[k]);
	return mmb_var_offset(arr, n, iv);
}

/* expr.c's expr_bit_int(): T_INT keeps its value, a float rounds half away
 * from zero before the bit operation (#1311). */
static int64_t tc_bit_int(mmb_val v)
{
	double x;
	if (v.type == T_INT)
		return v.i;
	x = mmb_as_float(v);
	return x >= 0.0 ? (int64_t)floor(x + 0.5) : (int64_t)ceil(x - 0.5);
}

/* A cached entry is only replayable while every variable it was compiled
 * against still has the same binding (#1368).  LOCAL shadows/restores and new
 * DIMs bump the affected slot's generation; a mismatch means the slow
 * interpreter path must re-resolve and recompile, so the cache can never read
 * a slot that was freed and reused for a different name, type or dims. */
static int tc_entry_valid(const tc_ent *e)
{
	int i;
	for (i = 0; i < e->nvp; i++)
		if (!e->vp[i] || e->vp[i]->gen != e->vgen[i])
			return 0;
	return 1;
}

/* Replay an entry. With `out` non-NULL (a bare condition compiled by
 * mmb_tcache_try_cond), the single value left on the stack is returned. */
static int tc_replay_out(tc_ent *e, mmb_val *out)
{
	mmb_val stk[TC_STK];
	int sp = 0;
	int pc = 0;
	if (!tc_entry_valid(e))
		return 0;
	while (pc < e->ncode)
	{
		uint8_t op = e->code[pc++];
		if (op == TC_END)
			break;
		if (op == TC_JZ)
		{
			if (sp < 1)
				return 0;
			if (mmb_as_int(stk[--sp]) == 0)
				break;
			continue;
		}
		if (op == TC_IFBLOCK)
		{
			if (sp < 1)
				return 0;
			mmb_if_push_block(mmb_as_int(stk[--sp]) != 0);
			break;
		}
		if (op == TC_GOTO)
		{
			G.branch_pc = e->branch_pc;
			break;
		}
		if (op == TC_GOSUB)
		{
			gosub_push_return();
			G.branch_pc = e->branch_pc;
			break;
		}
		if (op == TC_PUSHC)
		{
			uint8_t ci;
			if (sp >= TC_STK || pc >= e->ncode)
				return 0;
			ci = e->code[pc++];
			if (ci >= e->nc)
				return 0;
			stk[sp++] = e->ctype[ci] == T_INT ? mmb_int_val(e->ci[ci])
							  : mmb_num_val(e->c[ci]);
			continue;
		}
		if (op == TC_PUSHS)
		{
			uint8_t vi;
			if (sp >= TC_STK || pc >= e->ncode)
				return 0;
			vi = e->code[pc++];
			if (vi >= e->nvp)
				return 0;
			stk[sp++] = tc_load_var(e->vp[vi], 0);
			continue;
		}
		if (op == TC_PUSHA)
		{
			uint8_t ai, n, k;
			mmb_val iv[MMB_MAX_DIMS];
			int off;
			if (pc + 1 >= e->ncode)
				return 0;
			ai = e->code[pc++];
			n = e->code[pc++];
			if (ai >= e->nvp || n < 1 || n > MMB_MAX_DIMS || sp < n)
				return 0;
			for (k = n; k-- > 0;)
				iv[k] = stk[--sp];
			off = tc_elem_off(e->vp[ai], iv, n);
			stk[sp++] = tc_load_var(e->vp[ai], off);
			continue;
		}
		if (op == TC_STORES)
		{
			uint8_t vi;
			if (sp < 1 || pc >= e->ncode)
				return 0;
			vi = e->code[pc++];
			if (vi >= e->nvp)
				return 0;
			tc_store_var(e->vp[vi], 0, stk[--sp]);
			continue;
		}
		if (op == TC_STOREA)
		{
			uint8_t ai, n, k;
			mmb_val iv[MMB_MAX_DIMS], val;
			int off;
			if (pc + 1 >= e->ncode)
				return 0;
			ai = e->code[pc++];
			n = e->code[pc++];
			if (ai >= e->nvp || n < 1 || n > MMB_MAX_DIMS || sp < n + 1)
				return 0;
			val = stk[--sp];
			for (k = n; k-- > 0;)
				iv[k] = stk[--sp];
			off = tc_elem_off(e->vp[ai], iv, n);
			tc_store_var(e->vp[ai], off, val);
			continue;
		}
		if (op == TC_NEG)
		{
			if (sp < 1)
				return 0;
			if (stk[sp - 1].type == T_INT)
				stk[sp - 1] = mmb_int_val(-stk[sp - 1].i);
			else
				stk[sp - 1] = mmb_num_val(-stk[sp - 1].f);
			continue;
		}
		if (op == TC_NOT)
		{
			if (sp < 1)
				return 0;
			stk[sp - 1] = mmb_int_val(mmb_as_float(stk[sp - 1]) != 0.0 ? 0 : 1);
			continue;
		}
		if (op == TC_ADD || op == TC_SUB || op == TC_MUL || op == TC_DIV ||
		    op == TC_IDIV || op == TC_MOD || op == TC_AND ||
		    op == TC_OR || op == TC_XOR ||
		    op == TC_LT || op == TC_GT || op == TC_LE || op == TC_GE ||
		    op == TC_EQ || op == TC_NE)
		{
			mmb_val x, y, r;
			if (sp < 2)
				return 0;
			y = stk[--sp];
			x = stk[--sp];
			if (op == TC_ADD)
			{
				if (x.type == T_INT && y.type == T_INT)
					r = mmb_int_val(x.i + y.i);
				else
					r = mmb_num_val(mmb_as_float(x) + mmb_as_float(y));
			}
			else if (op == TC_SUB)
			{
				if (x.type == T_INT && y.type == T_INT)
					r = mmb_int_val(x.i - y.i);
				else
					r = mmb_num_val(mmb_as_float(x) - mmb_as_float(y));
			}
			else if (op == TC_MUL)
			{
				if (x.type == T_INT && y.type == T_INT)
					r = mmb_int_val(x.i * y.i);
				else
					r = mmb_num_val(mmb_as_float(x) * mmb_as_float(y));
			}
			else if (op == TC_DIV)
			{
				double d = mmb_as_float(y);
				r = mmb_num_val(d == 0 ? 0 : mmb_as_float(x) / d);
			}
			else if (op == TC_IDIV)
			{
				int64_t d = mmb_as_int(y);
				r = mmb_int_val(d == 0 ? 0 : mmb_as_int(x) / d);
			}
			else if (op == TC_MOD)
			{
				int64_t d = mmb_as_int(y);
				r = mmb_int_val(d == 0 ? 0 : mmb_as_int(x) % d);
			}
			else if (op == TC_AND)
				r = mmb_int_val(tc_bit_int(x) & tc_bit_int(y));
			else if (op == TC_OR)
				r = mmb_int_val(tc_bit_int(x) | tc_bit_int(y));
			else if (op == TC_XOR)
				r = mmb_int_val(tc_bit_int(x) ^ tc_bit_int(y));
			else
			{
				double a = mmb_as_float(x), b = mmb_as_float(y);
				int truth;
				if (op == TC_LT)
					truth = a < b;
				else if (op == TC_GT)
					truth = a > b;
				else if (op == TC_LE)
					truth = a <= b;
				else if (op == TC_GE)
					truth = a >= b;
				else if (op == TC_EQ)
					truth = a == b;
				else
					truth = a != b;
				r = mmb_int_val(truth);
			}
			stk[sp++] = r;
			continue;
		}
		return 0;
	}
	G.p = e->endp;
	if (out)
	{
		if (sp != 1)
			return 0;
		*out = stk[0];
	}
	return 1;
}

static int tc_replay(tc_ent *e)
{
	return tc_replay_out(e, 0);
}

static int tc_try(int kind, mmb_val *out)
{
	const char *key;
	const char *save;
	tc_ent *e;
	tc_ent tmp;
	tc_neg *neg;
	int ok;

	if (!G.running || !G.opt.tracecache || tc_compiling)
		return 0;
	tc_ensure();
	key = G.p;
	e = tc_lookup(key);
	if (e)
	{
		if (e->state > 0 && tc_entry_valid(e))
		{
			e->ref = 1;
			if (G.opt.profiling)
				G.prof.tcache_hit++;
			return tc_replay_out(e, kind == 2 ? out : 0);
		}
		/* Stale binding: drop the entry so the recompile below cannot be
		 * shadowed by a pointer that was freed and reused (#1368). */
		tc_remove(e);
	}
	neg = tcneg_find(key);
	if (neg && neg->fail_gen == G.var_gen)
	{
		/* Known-bad for this binding set: do not retry on every run. */
		return 0;
	}
	save = G.p;
	memset(&tmp, 0, sizeof(tmp));
	tmp.key = key;
	tc_compiling = 1;
	if (kind == 1)
		ok = tc_compile_if(&tmp) && tc_emit(&tmp, TC_END);
	else if (kind == 2)
		ok = tc_compile_expr(&tmp) && tc_emit(&tmp, TC_END);
	else
		ok = tc_compile_let_from_lhs(&tmp) && tc_emit(&tmp, TC_END);
	tc_compiling = 0;
	if (!ok)
	{
		G.p = save;
		tcneg_add(key, G.var_gen);
		if (G.opt.profiling)
			G.prof.tcache_bad++;
		return 0;
	}
	tmp.endp = G.p;
	tmp.state = 1;
	tmp.ref = 1;
	e = tc_insert(key);
	if (e)
		*e = tmp;
	else
		e = &tmp;
	G.p = save;
	if (G.opt.profiling)
		G.prof.tcache_comp++;
	return tc_replay_out(e, kind == 2 ? out : 0);
}

int mmb_tcache_try_let(void)
{
	return tc_try(0, 0);
}

int mmb_tcache_try_if(void)
{
	return tc_try(1, 0);
}

int mmb_tcache_try_cond(mmb_val *out)
{
	return tc_try(2, out);
}
