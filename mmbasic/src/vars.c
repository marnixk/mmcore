#include "mmb_priv.h"
#include <string.h>

extern void mmb_do_assign(const char *name, int type_hint, int nidx, int *idx, mmb_val val);

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
			else if (G.vars[i].type == T_STRUCT && G.vars[i].data.blob)
				G.plat->free(G.vars[i].data.blob);
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

int mmb_elem_off(mmb_var *v, int nidx, const int *idx)
{
	if (!v)
		return 0;
	if (G.acc_on)
	{
		if (nidx && v->dims == nidx)
			return offset_of(v, idx);
		return 0;
	}
	if (nidx)
		return offset_of(v, idx);
	return 0;
}

mmb_var *mmb_lookup_struct_var(const char *name)
{
	char nbuf[MMB_MAX_NAME];
	int i;
	strncpy(nbuf, name, MMB_MAX_NAME - 1);
	nbuf[MMB_MAX_NAME - 1] = 0;
	mmb_upper(nbuf);
	mmb_type_suffix(nbuf);
	i = hash_lookup(nbuf, T_STRUCT);
	if (i >= 0)
		return &G.vars[i];
	for (i = 0; i < MMB_MAX_VARS; i++)
		if (G.vars[i].used && G.vars[i].type == T_STRUCT && name_eq(G.vars[i].name, nbuf))
			return &G.vars[i];
	return 0;
}

void mmb_bind_struct_var(const char *name, int sid)
{
	char nbuf[MMB_MAX_NAME];
	int i, slot, sz;
	mmb_sdef *sd;
	strncpy(nbuf, name, MMB_MAX_NAME - 1);
	nbuf[MMB_MAX_NAME - 1] = 0;
	mmb_upper(nbuf);
	mmb_type_suffix(nbuf);
	sd = mmb_struct_def(sid);
	if (!sd)
		mmb_error("?UNKNOWN TYPE");
	for (i = 0; i < MMB_MAX_VARS; i++)
		if (G.vars[i].used && G.vars[i].type == T_STRUCT && name_eq(G.vars[i].name, nbuf))
		{
			if (G.vars[i].struct_idx != sid)
				mmb_error("?TYPE MISMATCH");
			return;
		}
	for (slot = 0; slot < MMB_MAX_VARS; slot++)
		if (!G.vars[slot].used)
			break;
	if (slot >= MMB_MAX_VARS)
		mmb_error("?OUT OF MEMORY");
	sz = sd->total;
	memset(&G.vars[slot], 0, sizeof(G.vars[slot]));
	strncpy(G.vars[slot].name, nbuf, MMB_MAX_NAME - 1);
	G.vars[slot].type = T_STRUCT;
	G.vars[slot].dims = 0;
	G.vars[slot].size = 1;
	G.vars[slot].struct_idx = sid;
	G.vars[slot].used = 1;
	G.vars[slot].unsuffixed = 1;
	G.vars[slot].data.blob = G.plat->alloc((unsigned)sz);
	memset(G.vars[slot].data.blob, 0, (unsigned)sz);
	G.nvars++;
	hash_ins(slot);
}

static mmb_var *try_struct_path(char *nbuf, int nidx, int *idx)
{
	char *dot;
	char base[MMB_MAX_NAME];
	int bl;
	mmb_var *sv;
	if (G.nstruct <= 0)
		return 0;
	dot = strchr(nbuf, '.');
	if (!dot)
		return 0;
	bl = (int)(dot - nbuf);
	if (bl <= 0 || bl >= MMB_MAX_NAME)
		return 0;
	memcpy(base, nbuf, (unsigned)bl);
	base[bl] = 0;
	sv = mmb_lookup_struct_var(base);
	if (!sv)
		return 0;
	if (sv->dims == nidx)
	{
		if (mmb_struct_resolve(sv, dot + 1, 0, 0))
		{
			if (idx && nidx == 0)
				*idx = 0;
			return sv;
		}
	}
	if (sv->dims == 0)
	{
		if (mmb_struct_resolve(sv, dot + 1, nidx, idx))
		{
			if (idx && nidx == 0)
				*idx = 0;
			return sv;
		}
	}
	mmb_error("?UNKNOWN");
	return 0;
}

mmb_var *mmb_find_var(const char *name, int type, int create, int nidx, int *idx)
{
	int i;
	char nbuf[MMB_MAX_NAME];
	int t, typed_lookup;
	mmb_var *sv;
	if (G.opt.profiling && G.running)
		G.prof.find_var++;
	G.acc_on = 0;
	strncpy(nbuf, name, MMB_MAX_NAME - 1);
	nbuf[MMB_MAX_NAME - 1] = 0;
	mmb_upper(nbuf);
	sv = try_struct_path(nbuf, nidx, idx);
	if (sv)
		return sv;
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
	if (type == T_STRUCT)
		mmb_error("?UNDECLARED");
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
	/* DIM [SHARED] [INTEGER|FLOAT|STRING] name(d1[,d2...]) [AS type] [, ...] */
	int group = 0;
	mmb_tcache_invalidate();
	mmb_skip_sp();
	(void)mmb_match("SHARED"); /* MMBasic variables are global already */
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
		int struct_idx = -1;
		int fixlen = 0;
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
			if (mmb_match("INTEGER") || mmb_match("INT"))
				type = T_INT;
			else if (mmb_match("STRING"))
				type = T_STR;
			else if (mmb_match("FLOAT"))
				type = T_NUM;
			else
			{
				char tn[MMB_MAX_NAME];
				int sid;
				mmb_ident(tn, sizeof(tn));
				mmb_type_suffix(tn);
				sid = mmb_struct_lookup(tn);
				if (sid < 0)
					mmb_error("?UNKNOWN TYPE");
				type = T_STRUCT;
				struct_idx = sid;
			}
		}
		if (mmb_match("LENGTH"))
		{
			mmb_val lv = mmb_expr();
			fixlen = (int)mmb_as_int(lv);
			if (fixlen < 0)
				fixlen = 0;
			if (fixlen > MMB_MAX_STR)
				fixlen = MMB_MAX_STR;
		}
		if (type == 0)
			type = G.opt.default_type ? G.opt.default_type : T_NUM;
		if (type == T_STRUCT && struct_idx < 0)
			mmb_syntax();

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
		G.vars[slot].struct_idx = struct_idx;
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
		else if (type == T_STRUCT)
		{
			int sz = G.sdef[struct_idx].total;
			G.vars[slot].data.blob = G.plat->alloc((unsigned)n * (unsigned)sz);
			memset(G.vars[slot].data.blob, 0, (unsigned)n * (unsigned)sz);
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
		G.vars[slot].fixlen = (type == T_STR) ? fixlen : 0;
		mmb_skip_sp();
		if (*G.p == '=')
		{
			int ei = 0;
			G.p++;
			mmb_skip_sp();
			if (type == T_STRUCT)
			{
				mmb_sdef *sd = &G.sdef[G.vars[slot].struct_idx];
				int m;
				if (*G.p != '(')
					mmb_syntax();
				G.p++;
				for (m = 0; m < sd->nmem; m++)
				{
					mmb_val init;
					int saved_acc = G.acc_on;
					mmb_skip_sp();
					if (*G.p == ')')
						break;
					init = mmb_expr();
					G.acc_on = 1;
					G.acc_sid = G.vars[slot].struct_idx;
					G.acc_mtype = sd->mem[m].type;
					G.acc_moff = sd->mem[m].offset;
					if (sd->mem[m].type == T_STR)
						G.acc_msize = sd->mem[m].size;
					else if (sd->mem[m].type == T_STRUCT)
					{
						G.acc_sid = sd->mem[m].size;
						G.acc_msize = G.sdef[sd->mem[m].size].total;
					}
					else
						G.acc_msize = 8;
					mmb_struct_store_member(&G.vars[slot], 0, init);
					G.acc_on = saved_acc;
					mmb_skip_sp();
					if (*G.p == ',')
						G.p++;
				}
				mmb_skip_sp();
				if (*G.p == ')')
					G.p++;
			}
			else if (*G.p == '(')
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

static void free_var_storage(mmb_var *v)
{
	int d;
	if (v->type == T_STR && v->data.s)
	{
		for (d = 0; d < v->size; d++)
			if (v->data.s[d])
				G.plat->free(v->data.s[d]);
		G.plat->free(v->data.s);
		v->data.s = 0;
	}
	else if (v->type == T_INT && v->data.i)
	{
		G.plat->free(v->data.i);
		v->data.i = 0;
	}
	else if (v->type == T_NUM && v->data.f)
	{
		G.plat->free(v->data.f);
		v->data.f = 0;
	}
	else if (v->type == T_STRUCT && v->data.blob)
	{
		G.plat->free(v->data.blob);
		v->data.blob = 0;
	}
}

static void alloc_var_storage(mmb_var *v, int n)
{
	int i;
	if (v->type == T_INT)
	{
		v->data.i = G.plat->alloc((unsigned)n * sizeof(int64_t));
		memset(v->data.i, 0, (unsigned)n * sizeof(int64_t));
	}
	else if (v->type == T_STR)
	{
		v->data.s = G.plat->alloc((unsigned)n * sizeof(char *));
		for (i = 0; i < n; i++)
		{
			v->data.s[i] = G.plat->alloc(MMB_MAX_STR + 1);
			v->data.s[i][0] = 0;
		}
	}
	else if (v->type == T_STRUCT)
	{
		int sz = G.sdef[v->struct_idx].total;
		v->data.blob = G.plat->alloc((unsigned)n * (unsigned)sz);
		memset(v->data.blob, 0, (unsigned)n * (unsigned)sz);
	}
	else
	{
		v->data.f = G.plat->alloc((unsigned)n * sizeof(double));
		memset(v->data.f, 0, (unsigned)n * sizeof(double));
	}
}

void mmb_cmd_redim(void)
{
	int preserve = 0;
	int base = G.opt.base;
	mmb_tcache_invalidate();
	mmb_skip_sp();
	(void)mmb_match("SHARED");
	if (mmb_match("PRESERVE"))
		preserve = 1;
	for (;;)
	{
		char name[MMB_MAX_NAME];
		int type, dims = 0, dim[MMB_MAX_DIMS], i, n, slot = -1;
		int struct_idx = -1;
		int old_size = 0, old_dims = 0, old_dim[MMB_MAX_DIMS];
		int is_new = 0;
		mmb_var *v = 0;
		double *of = 0;
		int64_t *oi = 0;
		char **os = 0;
		unsigned char *ob = 0;
		mmb_ident(name, sizeof(name));
		type = mmb_type_suffix(name);
		mmb_skip_sp();
		if (*G.p != '(')
			mmb_syntax();
		G.p++;
		do
		{
			mmb_val dv;
			if (dims >= MMB_MAX_DIMS)
				mmb_syntax();
			dv = mmb_expr();
			dim[dims++] = (int)mmb_as_int(dv);
			mmb_skip_sp();
			if (*G.p == ',')
				G.p++;
			else
				break;
		} while (1);
		mmb_expect(')');
		mmb_skip_sp();
		if (mmb_match("AS"))
		{
			if (mmb_match("INTEGER") || mmb_match("INT"))
				type = T_INT;
			else if (mmb_match("STRING"))
				type = T_STR;
			else if (mmb_match("FLOAT"))
				type = T_NUM;
			else
			{
				char tn[MMB_MAX_NAME];
				int sid;
				mmb_ident(tn, sizeof(tn));
				mmb_type_suffix(tn);
				sid = mmb_struct_lookup(tn);
				if (sid < 0)
					mmb_error("?UNKNOWN TYPE");
				type = T_STRUCT;
				struct_idx = sid;
			}
		}
		if (type == 0)
			type = G.opt.default_type ? G.opt.default_type : T_NUM;
		for (i = 0; i < MMB_MAX_VARS; i++)
			if (G.vars[i].used && name_eq(G.vars[i].name, name) && G.vars[i].type == type)
			{
				v = &G.vars[i];
				break;
			}
		n = elem_count(dim, dims);
		if (n <= 0)
			mmb_error("?INVALID DIMENSION");
		if (!v)
		{
			for (slot = 0; slot < MMB_MAX_VARS; slot++)
				if (!G.vars[slot].used)
					break;
			if (slot >= MMB_MAX_VARS)
				mmb_error("?OUT OF MEMORY");
			v = &G.vars[slot];
			memset(v, 0, sizeof(*v));
			strncpy(v->name, name, MMB_MAX_NAME - 1);
			mmb_upper(v->name);
			v->type = type;
			v->unsuffixed = 1;
			v->struct_idx = struct_idx;
			v->used = 1;
			G.nvars++;
			is_new = 1;
		}
		else
		{
			if (type == T_STRUCT)
			{
				if (struct_idx < 0)
					struct_idx = v->struct_idx;
				else if (struct_idx != v->struct_idx)
					mmb_error("?TYPE MISMATCH");
				v->struct_idx = struct_idx;
			}
			if (preserve)
			{
				if (v->dims != dims)
					mmb_error("?DIMENSION");
				for (i = 0; i < dims - 1; i++)
					if (dim[i] != v->dim[i])
						mmb_error("?DIMENSION");
				old_size = v->size;
				old_dims = v->dims;
				for (i = 0; i < old_dims; i++)
					old_dim[i] = v->dim[i];
				of = v->data.f;
				oi = v->data.i;
				os = v->data.s;
				ob = v->data.blob;
			}
			else
				free_var_storage(v);
		}
		v->dims = dims;
		for (i = 0; i < dims; i++)
			v->dim[i] = dim[i];
		v->size = n;
		v->fixlen = 0;
		if (type == T_STRUCT && v->struct_idx < 0)
			mmb_error("?UNKNOWN TYPE");
		v->data.s = 0;
		alloc_var_storage(v, n);
		G.dim_used = 1;
		if (preserve && old_size > 0)
		{
			int oi_index, sz = type == T_STRUCT ? G.sdef[v->struct_idx].total : 0;
			for (oi_index = 0; oi_index < old_size; oi_index++)
			{
				int rem = oi_index, idx[MMB_MAX_DIMS], inb = 1, noff = 0, stride = 1;
				for (i = old_dims - 1; i >= 0; i--)
				{
					int len = old_dim[i] - base + 1;
					idx[i] = base + rem % len;
					rem /= len;
				}
				for (i = 0; i < dims; i++)
					if (idx[i] < base || idx[i] > dim[i])
						inb = 0;
				if (!inb)
					continue;
				for (i = dims - 1; i >= 0; i--)
				{
					noff += (idx[i] - base) * stride;
					stride *= (dim[i] - base + 1);
				}
				if (type == T_STR)
				{
					strncpy(v->data.s[noff], os[oi_index], MMB_MAX_STR);
					v->data.s[noff][MMB_MAX_STR] = 0;
				}
				else if (type == T_INT)
					v->data.i[noff] = oi[oi_index];
				else if (type == T_NUM)
					v->data.f[noff] = of[oi_index];
				else
					memcpy(v->data.blob + (unsigned)noff * (unsigned)sz,
					       ob + (unsigned)oi_index * (unsigned)sz, (unsigned)sz);
			}
		}
		if (type == T_STR && os)
		{
			for (i = 0; i < old_size; i++)
				if (os[i])
					G.plat->free(os[i]);
			G.plat->free(os);
		}
		else if (type == T_INT && oi)
			G.plat->free(oi);
		else if (type == T_NUM && of)
			G.plat->free(of);
		else if (type == T_STRUCT && ob)
			G.plat->free(ob);
		if (is_new)
			hash_ins((int)(v - G.vars));
		mmb_skip_sp();
		if (*G.p == ',')
		{
			G.p++;
			continue;
		}
		break;
	}
}

void mmb_cmd_common(void)
{
	/* COMMON [SHARED] var[, var...]: variables are already global. */
	char name[MMB_MAX_NAME];
	mmb_skip_sp();
	(void)mmb_match("SHARED");
	for (;;)
	{
		int t;
		mmb_skip_sp();
		if (!mmb_is_ident(*G.p))
			break;
		mmb_ident(name, sizeof(name));
		t = mmb_type_suffix(name);
		(void)mmb_find_var(name, t, 1, 0, 0);
		mmb_skip_sp();
		if (*G.p == ',')
		{
			G.p++;
			continue;
		}
		break;
	}
}

void mmb_cmd_swap(void)
{
	char n1[MMB_MAX_NAME], n2[MMB_MAX_NAME];
	int i1[MMB_MAX_DIMS], i2[MMB_MAX_DIMS], k1 = 0, k2 = 0, t1, t2;
	mmb_val v1, v2;
	mmb_var *va, *vb;
	int o1, o2;
	t1 = mmb_parse_var_ref(n1, &k1, i1);
	mmb_skip_sp();
	mmb_expect(',');
	t2 = mmb_parse_var_ref(n2, &k2, i2);
	va = mmb_find_var(n1, t1, 0, k1, i1);
	vb = mmb_find_var(n2, t2, 0, k2, i2);
	if (!va || !vb)
		mmb_error("?UNKNOWN");
	if (va->type != vb->type || va->type == T_STRUCT)
		mmb_error("?TYPE MISMATCH");
	o1 = mmb_elem_off(va, k1, i1);
	o2 = mmb_elem_off(vb, k2, i2);
	v1 = mmb_load_var(va, o1);
	v2 = mmb_load_var(vb, o2);
	if (v1.type == T_STR)
	{
		v1 = mmb_str_val(v1.s);
		v2 = mmb_str_val(v2.s);
	}
	mmb_do_assign(n1, va->type, k1, i1, v2);
	mmb_do_assign(n2, vb->type, k2, i2, v1);
}

static void store(mmb_var *v, int off, mmb_val val)
{
	if (G.acc_on)
	{
		mmb_struct_store_member(v, off, val);
		G.acc_on = 0;
		return;
	}
	if (v->type == T_STRUCT)
	{
		int sz;
		if (val.type != T_STRUCT || val.struct_idx != v->struct_idx || !val.blob)
			mmb_error("?TYPE MISMATCH");
		sz = G.sdef[v->struct_idx].total;
		memcpy(mmb_struct_elem(v, off), val.blob, (unsigned)sz);
		return;
	}
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
	if (G.acc_on)
	{
		mmb_val r = mmb_struct_load_member(v, off);
		G.acc_on = 0;
		return r;
	}
	if (v->type == T_STRUCT)
	{
		mmb_val out;
		memset(&out, 0, sizeof(out));
		out.type = T_STRUCT;
		out.struct_idx = v->struct_idx;
		out.blob = mmb_struct_elem(v, off);
		return out;
	}
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
	mmb_skip_sp();
	if (*G.p == '.')
	{
		int nl = (int)strlen(name);
		while (*G.p == '.')
		{
			char part[MMB_MAX_NAME];
			int pn;
			if (nl >= MMB_MAX_NAME - 2)
				mmb_error("?SYNTAX ERROR");
			name[nl++] = '.';
			name[nl] = 0;
			G.p++;
			mmb_ident(part, sizeof(part));
			mmb_type_suffix(part);
			pn = (int)strlen(part);
			if (nl + pn >= MMB_MAX_NAME)
				mmb_error("?SYNTAX ERROR");
			memcpy(name + nl, part, (unsigned)pn + 1);
			nl += pn;
			mmb_skip_sp();
		}
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
	off = mmb_elem_off(v, nidx, idx);
	store(v, off, val);
}

void mmb_cmd_clear(void)
{
	mmb_clear_vars(1);
}
