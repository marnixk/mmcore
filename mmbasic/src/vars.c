#include "mmb_priv.h"

static int var_tab[MMB_MAX_VARS];
static int unsuf_tab[MMB_MAX_VARS];

static int name_eq(const char *a, const char *b)
{
	return mmb_keyword_eq(a, b);
}

static unsigned hash_key(const char *n, int type)
{
	unsigned h = 2166136261u;
	while (*n)
	{
		h ^= (unsigned char)*n++;
		h *= 16777619u;
	}
	h ^= (unsigned)type * 0x9e3779b9u;
	return h;
}

static void hash_clear(void)
{
	int i;
	for (i = 0; i < MMB_MAX_VARS; i++)
	{
		var_tab[i] = -1;
		unsuf_tab[i] = -1;
	}
}

static void hash_ins(int vi)
{
	unsigned h;
	int i;
	if (vi < 0 || !G.vars[vi].used)
		return;
	h = hash_key(G.vars[vi].name, G.vars[vi].type) % MMB_MAX_VARS;
	for (i = 0; i < MMB_MAX_VARS; i++)
	{
		int s = (int)((h + (unsigned)i) % MMB_MAX_VARS);
		if (var_tab[s] < 0)
		{
			var_tab[s] = vi;
			break;
		}
	}
	if (G.vars[vi].unsuffixed)
	{
		h = hash_key(G.vars[vi].name, 0) % MMB_MAX_VARS;
		for (i = 0; i < MMB_MAX_VARS; i++)
		{
			int s = (int)((h + (unsigned)i) % MMB_MAX_VARS);
			if (unsuf_tab[s] < 0)
			{
				unsuf_tab[s] = vi;
				break;
			}
		}
	}
}

static int hash_lookup(const char *nbuf, int type)
{
	unsigned h = hash_key(nbuf, type) % MMB_MAX_VARS;
	int i;
	for (i = 0; i < MMB_MAX_VARS; i++)
	{
		int s = (int)((h + (unsigned)i) % MMB_MAX_VARS);
		int vi = var_tab[s];
		if (vi < 0)
			return -1;
		if (!G.vars[vi].used)
			continue;
		if (G.vars[vi].type == type && name_eq(G.vars[vi].name, nbuf))
			return vi;
	}
	return -1;
}

static int unsuf_lookup(const char *nbuf)
{
	unsigned h = hash_key(nbuf, 0) % MMB_MAX_VARS;
	int i;
	for (i = 0; i < MMB_MAX_VARS; i++)
	{
		int s = (int)((h + (unsigned)i) % MMB_MAX_VARS);
		int vi = unsuf_tab[s];
		if (vi < 0)
			return -1;
		if (G.vars[vi].used && G.vars[vi].unsuffixed && name_eq(G.vars[vi].name, nbuf))
			return vi;
	}
	return -1;
}

void mmb_clear_consts(void)
{
	int i;
	for (i = 0; i < MMB_MAX_CONST; i++)
		G.consts[i].used = 0;
	G.nconst = 0;
}

void mmb_const_define(const char *name, int type, mmb_val val)
{
	int i, slot = -1;
	char nbuf[MMB_MAX_NAME];
	strncpy(nbuf, name, MMB_MAX_NAME - 1);
	nbuf[MMB_MAX_NAME - 1] = 0;
	mmb_upper(nbuf);
	if (type == 0)
		type = val.type;
	for (i = 0; i < MMB_MAX_CONST; i++)
	{
		if (G.consts[i].used && name_eq(G.consts[i].name, nbuf))
			return; /* CMM2 allows CONST inside a SUB on each call */
		if (!G.consts[i].used && slot < 0)
			slot = i;
	}
	if (slot < 0)
		mmb_error("?OUT OF MEMORY");
	memset(&G.consts[slot], 0, sizeof(G.consts[slot]));
	strncpy(G.consts[slot].name, nbuf, MMB_MAX_NAME - 1);
	G.consts[slot].type = type;
	G.consts[slot].val = val;
	if (val.type == T_STR)
		mmb_val_own(&G.consts[slot].val, G.consts[slot].s, (int)sizeof(G.consts[slot].s));
	G.consts[slot].used = 1;
	G.nconst++;
}

int mmb_const_lookup(const char *name, int type, mmb_val *out)
{
	int i;
	char nbuf[MMB_MAX_NAME];
	int t;
	strncpy(nbuf, name, MMB_MAX_NAME - 1);
	nbuf[MMB_MAX_NAME - 1] = 0;
	mmb_upper(nbuf);
	t = mmb_type_suffix(nbuf);
	if (t)
		type = t;
	for (i = 0; i < MMB_MAX_CONST; i++)
	{
		if (G.consts[i].used && name_eq(G.consts[i].name, nbuf))
		{
			if (type && G.consts[i].type != type)
				return 0;
			*out = G.consts[i].val;
			return 1;
		}
	}
	return 0;
}

void mmb_clear_vars(int keep_options)
{
	int i, d;
	(void)keep_options;
	for (i = 0; i < MMB_MAX_VARS; i++)
	{
		if (G.vars[i].used)
		{
			if (G.vars[i].type == T_STR && G.vars[i].data.s)
			{
				for (d = 0; d < G.vars[i].size; d++)
					G.plat->free(G.vars[i].data.s[d]);
				G.plat->free(G.vars[i].data.s);
			}
			else if (G.vars[i].type == T_INT && G.vars[i].data.i)
				G.plat->free(G.vars[i].data.i);
			else if (G.vars[i].type == T_NUM && G.vars[i].data.f)
				G.plat->free(G.vars[i].data.f);
		}
		memset(&G.vars[i], 0, sizeof(G.vars[i]));
	}
	G.nvars = 0;
	G.dim_used = 0;
	hash_clear();
	mmb_tcache_invalidate();
}

static int elem_count(const int *dim, int ndims)
{
	int n = 1, i;
	for (i = 0; i < ndims; i++)
		n *= (dim[i] - G.opt.base + 1);
	return n;
}

static int offset_of(mmb_var *v, const int *idx)
{
	int off = 0, i, stride = 1;
	for (i = v->dims - 1; i >= 0; i--)
	{
		if (idx[i] < G.opt.base || idx[i] > v->dim[i])
			mmb_error("?INDEX OUT OF BOUNDS");
		off += (idx[i] - G.opt.base) * stride;
		stride *= (v->dim[i] - G.opt.base + 1);
	}
	return off;
}

int mmb_var_offset(mmb_var *v, int nidx, const int *idx)
{
	if (!v || nidx == 0)
		return 0;
	return offset_of(v, idx);
}

mmb_var *mmb_find_var(const char *name, int type, int create, int nidx, int *idx)
{
	int i;
	char nbuf[MMB_MAX_NAME];
	int t, typed_lookup;
	if (G.opt.profiling && G.running)
		G.prof.find_var++;
	strncpy(nbuf, name, MMB_MAX_NAME - 1);
	nbuf[MMB_MAX_NAME - 1] = 0;
	mmb_upper(nbuf);
	t = mmb_type_suffix(nbuf);
	typed_lookup = (type != 0) || t;
	if (t)
		type = t;
	else if (type == 0)
	{
		int ui = unsuf_lookup(nbuf);
		if (ui >= 0)
			type = G.vars[ui].type;
		if (type == 0)
			type = G.opt.default_type;
	}
	if (type == 0 && G.opt.explicit)
		mmb_error("?EXPLICIT");
	if (type == 0)
		type = T_NUM;

	i = hash_lookup(nbuf, type);
	if (i < 0)
	{
		for (i = 0; i < MMB_MAX_VARS; i++)
			if (G.vars[i].used && G.vars[i].type == type && name_eq(G.vars[i].name, nbuf))
			{
				hash_ins(i);
				break;
			}
		if (i >= MMB_MAX_VARS)
			i = -1;
	}
	if (i >= 0)
	{
		if (nidx != G.vars[i].dims)
		{
			if (nidx == 0 && G.vars[i].dims > 0)
				mmb_error("?ARRAY");
			if (nidx > 0 && G.vars[i].dims == 0)
				mmb_error("?NOT AN ARRAY");
			if (nidx != G.vars[i].dims)
				mmb_error("?SUBSCRIPT");
		}
		if (idx && nidx == 0)
			*idx = 0;
		return &G.vars[i];
	}

	if (!create)
		return 0;
	if (G.opt.explicit)
		mmb_error("?UNDECLARED");
	if (nidx > 0)
		mmb_error("?UNDECLARED"); /* arrays must be DIMmed */

	for (i = 0; i < MMB_MAX_VARS; i++)
		if (!G.vars[i].used)
			break;
	if (i >= MMB_MAX_VARS)
		mmb_error("?OUT OF MEMORY");

	memset(&G.vars[i], 0, sizeof(G.vars[i]));
	strncpy(G.vars[i].name, nbuf, MMB_MAX_NAME - 1);
	G.vars[i].type = type;
	G.vars[i].dims = 0;
	G.vars[i].size = 1;
	G.vars[i].used = 1;
	G.vars[i].unsuffixed = !typed_lookup;
	if (type == T_INT)
	{
		G.vars[i].data.i = G.plat->alloc(sizeof(int64_t));
		G.vars[i].data.i[0] = 0;
	}
	else if (type == T_STR)
	{
		G.vars[i].data.s = G.plat->alloc(sizeof(char *));
		G.vars[i].data.s[0] = G.plat->alloc(MMB_MAX_STR + 1);
		G.vars[i].data.s[0][0] = 0;
	}
	else
	{
		G.vars[i].data.f = G.plat->alloc(sizeof(double));
		G.vars[i].data.f[0] = 0;
	}
	G.nvars++;
	hash_ins(i);
	if (idx)
		*idx = 0;
	return &G.vars[i];
}

void mmb_cmd_dim(void)
{
	/* DIM [INTEGER|FLOAT|STRING] name(d1[,d2...]) [AS type] [, ...] */
	int group = 0;
	mmb_tcache_invalidate();
	mmb_skip_sp();
	if (mmb_match("INTEGER") || mmb_match("INT"))
		group = T_INT;
	else if (mmb_match("STRING"))
		group = T_STR;
	else if (mmb_match("FLOAT"))
		group = T_NUM;
	for (;;)
	{
		char name[MMB_MAX_NAME];
		int type, dims = 0, dim[MMB_MAX_DIMS], i, n, slot, had_suffix;
		int idxdummy[MMB_MAX_DIMS];
		mmb_ident(name, sizeof(name));
		{
			int sl = (int)strlen(name);
			had_suffix = sl && (name[sl - 1] == '$' || name[sl - 1] == '%' || name[sl - 1] == '!');
		}
		type = mmb_type_suffix(name);
		if (group)
			type = group;
		mmb_skip_sp();
		if (*G.p == '(')
		{
			G.p++;
			do
			{
				mmb_val v;
				if (dims >= MMB_MAX_DIMS)
					mmb_syntax();
				v = mmb_expr();
				dim[dims++] = (int)mmb_as_int(v);
				mmb_skip_sp();
				if (*G.p == ',')
					G.p++;
				else
					break;
			} while (1);
			mmb_expect(')');
		}
		mmb_skip_sp();
		if (mmb_match("AS"))
		{
			if (mmb_match("INTEGER"))
				type = T_INT;
			else if (mmb_match("STRING"))
				type = T_STR;
			else if (mmb_match("FLOAT"))
				type = T_NUM;
			else
				mmb_syntax();
		}
		if (mmb_match("LENGTH"))
		{
			mmb_val lv = mmb_expr();
			(void)lv; /* accepted; strings stay MMB_MAX_STR */
		}
		if (type == 0)
			type = G.opt.default_type ? G.opt.default_type : T_NUM;

		for (i = 0; i < MMB_MAX_VARS; i++)
			if (G.vars[i].used && name_eq(G.vars[i].name, name) && G.vars[i].type == type)
			{
				if (G.dim_local)
				{
					slot = i;
					goto dim_init;
				}
				mmb_error("?ALREADY DECLARED");
			}

		for (slot = 0; slot < MMB_MAX_VARS; slot++)
			if (!G.vars[slot].used)
				break;
		if (slot >= MMB_MAX_VARS)
			mmb_error("?OUT OF MEMORY");

		memset(&G.vars[slot], 0, sizeof(G.vars[slot]));
		strncpy(G.vars[slot].name, name, MMB_MAX_NAME - 1);
		mmb_upper(G.vars[slot].name);
		G.vars[slot].type = type;
		G.vars[slot].dims = dims;
		for (i = 0; i < dims; i++)
			G.vars[slot].dim[i] = dim[i];
		n = dims ? elem_count(dim, dims) : 1;
		if (n <= 0)
			mmb_error("?INVALID DIMENSION");
		G.vars[slot].size = n;
		G.vars[slot].used = 1;
		G.vars[slot].unsuffixed = !had_suffix;
		if (type == T_INT)
		{
			G.vars[slot].data.i = G.plat->alloc((unsigned)n * sizeof(int64_t));
			memset(G.vars[slot].data.i, 0, (unsigned)n * sizeof(int64_t));
		}
		else if (type == T_STR)
		{
			G.vars[slot].data.s = G.plat->alloc((unsigned)n * sizeof(char *));
			for (i = 0; i < n; i++)
			{
				G.vars[slot].data.s[i] = G.plat->alloc(MMB_MAX_STR + 1);
				G.vars[slot].data.s[i][0] = 0;
			}
		}
		else
		{
			G.vars[slot].data.f = G.plat->alloc((unsigned)n * sizeof(double));
			memset(G.vars[slot].data.f, 0, (unsigned)n * sizeof(double));
		}
		G.nvars++;
		hash_ins(slot);
		if (dims)
			G.dim_used = 1;
		(void)idxdummy;
	dim_init:
		mmb_skip_sp();
		if (*G.p == '=')
		{
			int ei = 0;
			G.p++;
			mmb_skip_sp();
			if (*G.p == '(')
			{
				G.p++;
				while (ei < G.vars[slot].size)
				{
					mmb_val init;
					mmb_skip_sp();
					if (*G.p == ')')
						break;
					init = mmb_expr();
					if (type == T_STR)
					{
						if (init.type != T_STR)
							mmb_error("?TYPE MISMATCH");
						strncpy(G.vars[slot].data.s[ei], init.s, MMB_MAX_STR);
					}
					else if (type == T_INT)
						G.vars[slot].data.i[ei] = mmb_as_int(init);
					else
						G.vars[slot].data.f[ei] = mmb_as_float(init);
					ei++;
					mmb_skip_sp();
					if (*G.p == ',')
					{
						G.p++;
						continue;
					}
					break;
				}
				mmb_skip_sp();
				if (*G.p == ')')
					G.p++;
			}
			else
			{
				mmb_val init;
				init = mmb_expr();
				if (type == T_STR)
				{
					if (init.type != T_STR)
						mmb_error("?TYPE MISMATCH");
					strncpy(G.vars[slot].data.s[0], init.s, MMB_MAX_STR);
				}
				else if (type == T_INT)
					G.vars[slot].data.i[0] = mmb_as_int(init);
				else
					G.vars[slot].data.f[0] = mmb_as_float(init);
			}
			mmb_skip_sp();
		}
		if (*G.p == ',')
		{
			G.p++;
			continue;
		}
		break;
	}
}

static void store(mmb_var *v, int off, mmb_val val)
{
	if (v->type == T_STR)
	{
		if (val.type != T_STR)
			mmb_error("?TYPE MISMATCH");
		strncpy(v->data.s[off], val.s ? val.s : "", MMB_MAX_STR);
		v->data.s[off][MMB_MAX_STR] = 0;
	}
	else if (v->type == T_INT)
	{
		if (val.type == T_STR)
			mmb_error("?TYPE MISMATCH");
		v->data.i[off] = mmb_as_int(val);
	}
	else
	{
		if (val.type == T_STR)
			mmb_error("?TYPE MISMATCH");
		v->data.f[off] = mmb_as_float(val);
	}
}

mmb_val mmb_load_var(mmb_var *v, int off)
{
	if (v->type == T_STR)
		return mmb_str_val(v->data.s[off]);
	if (v->type == T_INT)
		return mmb_int_val(v->data.i[off]);
	return mmb_num_val(v->data.f[off]);
}

void mmb_assign_from_parse(void); /* defined in expr.c via parse of LET / implied LET */

int mmb_parse_var_ref(char *name, int *nidx, int *idx)
{
	int t;
	mmb_ident(name, MMB_MAX_NAME);
	t = mmb_type_suffix(name);
	*nidx = 0;
	mmb_skip_sp();
	if (*G.p == '(')
	{
		G.p++;
		do
		{
			mmb_val v;
			if (*nidx >= MMB_MAX_DIMS)
				mmb_syntax();
			v = mmb_expr();
			idx[*nidx] = (int)mmb_as_int(v);
			(*nidx)++;
			mmb_skip_sp();
			if (*G.p == ',')
				G.p++;
			else
				break;
		} while (1);
		mmb_expect(')');
	}
	return t;
}

void mmb_do_assign(const char *name, int type_hint, int nidx, int *idx, mmb_val val)
{
	int off = 0;
	int idxcopy[MMB_MAX_DIMS];
	int i;
	mmb_val dummy;
	mmb_var *v;
	if (nidx == 0 && mmb_const_lookup(name, type_hint, &dummy))
		mmb_error("?CONST");
	for (i = 0; i < nidx; i++)
		idxcopy[i] = idx[i];
	v = mmb_find_var(name, type_hint, 1, nidx, idxcopy);
	if (nidx)
		off = offset_of(v, idx);
	store(v, off, val);
}

void mmb_cmd_clear(void)
{
	mmb_clear_vars(1);
}
