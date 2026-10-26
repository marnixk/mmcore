#include "mmb_priv.h"
#include <stdio.h>
#include <string.h>

extern void mmb_do_assign(const char *name, int type_hint, int nidx, int *idx, mmb_val val);

static void free_var_storage(mmb_var *v);
static void alloc_var_storage(mmb_var *v, int n);
static void hash_ins(int vi);
static void hash_rebuild(void);
static int mmb_local_save(int slot, int existed);
static void var_sig_update(int slot);

/* Per-slot identity of the current binding, and the generation it had when a
 * returning LOCAL frame released it.  A LOCAL re-created in the same slot with
 * the same name/type/dims is the same binding, so its trace/resolve cache
 * entries can come back instead of forcing a recompile on every call
 * (#1328).  Nothing else revives a generation: any other binding bumps it. */
static unsigned *slot_sig;
static unsigned *reuse_gen;
/* Generations handed to recycled slots live in a separate range so freeing a
 * LOCAL can invalidate those slots without advancing G.var_gen (which would
 * force the trace cache to retry a statement it already knows it cannot
 * compile, rescanning the variable table on every call). */
static unsigned free_gen_seq;

/* Stable hash of everything a trace-cache entry assumes about a binding. */
static unsigned var_binding_sig(const mmb_var *v)
{
	unsigned h = 2166136261u;
	const unsigned char *p;
	int i;
	if (!v->name[0])
		return 0;
	for (p = (const unsigned char *)v->name; *p; p++)
	{
		h ^= *p;
		h *= 16777619u;
	}
#define MIX_SIG(x)                                     \
	do                                             \
	{                                              \
		unsigned _x = (unsigned)(x);           \
		int _b;                                \
		for (_b = 0; _b < 4; _b++)             \
		{                                      \
			h ^= (_x >> (8 * _b)) & 0xffu; \
			h *= 16777619u;                \
		}                                      \
	} while (0)
	MIX_SIG(v->type);
	MIX_SIG(v->dims);
	MIX_SIG(v->size);
	MIX_SIG(v->struct_idx);
	MIX_SIG(v->unsuffixed);
	MIX_SIG(v->maxlen);
	for (i = 0; i < v->dims && i < MMB_MAX_DIMS; i++)
		MIX_SIG(v->dim[i]);
#undef MIX_SIG
	return h ? h : 1;
}

static void var_sig_update(int slot)
{
	if (slot_sig)
		slot_sig[slot] = var_binding_sig(&G.vars[slot]);
	if (reuse_gen)
		reuse_gen[slot] = 0;
}

/* Take a fresh binding generation for `slot` (#1368).  Any trace-cache or
 * resolve-cache entry holding this mmb_var* is then stale, but only that
 * entry: entries for untouched slots stay valid.  Must be called after the
 * slot's fields (including any memset) are set for the new binding. */
static unsigned var_bind_gen(int slot)
{
	G.var_gen++;
	G.vars[slot].gen = G.var_gen;
	var_sig_update(slot);
	return G.var_gen;
}

/* Bind a fresh LOCAL into a recycled slot.  When the slot's last binding was
 * the same name/type/dims and it was released by a LOCAL return, put back the
 * generation it had then: the cached entries still describe exactly this
 * binding, so a helper called in a loop keeps its compiled body.  A different
 * binding (or a slot that was never a matching LOCAL) gets a new generation
 * as usual. */
static void var_bind_local(int slot)
{
	unsigned sig = var_binding_sig(&G.vars[slot]);
	if (slot_sig && reuse_gen && sig != 0 && slot_sig[slot] == sig &&
	    reuse_gen[slot] != 0)
	{
		G.vars[slot].gen = reuse_gen[slot];
		reuse_gen[slot] = 0;
	}
	else
	{
		G.var_gen++;
		G.vars[slot].gen = G.var_gen;
	}
	if (slot_sig)
		slot_sig[slot] = sig;
	if (reuse_gen)
		reuse_gen[slot] = 0;
}

/* Variable-name hash tables (#1303). The variable table grows past 512
 * entries, so the hash is heap-allocated and doubles when the load factor
 * would pass ~0.5: capacity is always at least twice the number of used
 * entries (and at least MMB_HASH_MIN), kept in step with vars_cap. */
#define MMB_HASH_MIN 512
static int *var_tab;
static int *unsuf_tab;
static unsigned hash_cap;

/* Free-slot list (#1328).  vars_free_slot() used to scan the variable table
 * for the first unused slot, so creating a LOCAL's fresh slot cost O(number
 * of globals) and LOCAL/JOIN grew with the program.  Slots released by a
 * returning LOCAL frame are kept on a LIFO list; alloc_high bounds the
 * never-yet-handed-out prefix so the common path is O(1). */
static int *free_next;
static unsigned free_cap;
static int free_head = -1;
static unsigned alloc_high;

/* Direct-mapped resolve cache (#1002): parsing a statement re-resolves every
 * variable token to an mmb_var* on each execution.  Key by the source-token
 * address (unique per occurrence) plus the operand shape, so a repeat
 * execution of the same statement reuses the binding.  Program text changes
 * and table moves/rehashes bump the generation, invalidating all entries.  A
 * single slot rebound by LOCAL or DIM keeps the entry address valid but bumps
 * the variable's own generation, which is checked before the cached pointer is
 * returned (#1368). */
#define MMB_RC_SIZE 256
typedef struct {
	const char *ref;
	unsigned gen;
	int type;
	int nidx;
	mmb_var *v;
	unsigned vgen; /* v->gen when cached (#1368) */
	/* The resolved name is compared on a hit so a reused transient parse
	 * buffer (EXECUTE/EVAL) at the same address cannot return a stale
	 * binding for different text. */
	char name[MMB_MAX_NAME];
} mmb_rc_ent;
static mmb_rc_ent rc_tab[MMB_RC_SIZE];
static unsigned rc_gen = 1;

void mmb_resolve_invalidate(void)
{
	rc_gen++;
}

static mmb_rc_ent *rc_slot(const char *ref)
{
	uintptr_t x = (uintptr_t)ref;
	/* Mix low bits so tokens a byte or two apart (``A=A+1``) do not thrash
	 * the same slot. */
	return &rc_tab[(x ^ (x >> 8) ^ (x >> 16)) & (MMB_RC_SIZE - 1)];
}

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
	unsigned i;
	for (i = 0; i < hash_cap; i++)
	{
		var_tab[i] = -1;
		unsuf_tab[i] = -1;
	}
}

/* Grow the hash tables so `need_vars` entries keep the load factor at or
 * below 0.5, then rebuild. Called before a new variable is inserted. */
static void hash_ensure(unsigned need_vars)
{
	unsigned want = 2 * need_vars;
	unsigned cap = hash_cap > 0 ? hash_cap : MMB_HASH_MIN;
	int *nt, *nu;
	if (want <= hash_cap)
		return;
	while (cap < want)
		cap *= 2;
	nt = (int *)G.plat->alloc(cap * sizeof(int));
	nu = (int *)G.plat->alloc(cap * sizeof(int));
	if (!nt || !nu)
	{
		if (nt)
			G.plat->free(nt);
		if (nu)
			G.plat->free(nu);
		mmb_error("?OUT OF MEMORY");
	}
	if (var_tab)
		G.plat->free(var_tab);
	if (unsuf_tab)
		G.plat->free(unsuf_tab);
	var_tab = nt;
	unsuf_tab = nu;
	hash_cap = cap;
	hash_rebuild();
}

/* Drop the hash tables (NEW / teardown); they regrow on the next insert. */
static void hash_release(void)
{
	if (var_tab)
		G.plat->free(var_tab);
	if (unsuf_tab)
		G.plat->free(unsuf_tab);
	var_tab = 0;
	unsuf_tab = 0;
	hash_cap = 0;
}

static void hash_rebuild(void)
{
	int i;
	hash_clear();
	for (i = 0; i < G.vars_cap; i++)
		if (G.vars[i].used)
			hash_ins(i);
}

static void hash_ins(int vi)
{
	unsigned h;
	unsigned i;
	if (vi < 0 || !hash_cap || !G.vars[vi].used)
		return;
	h = hash_key(G.vars[vi].name, G.vars[vi].type) % hash_cap;
	for (i = 0; i < hash_cap; i++)
	{
		unsigned s = (h + i) % hash_cap;
		if (var_tab[s] < 0)
		{
			var_tab[s] = vi;
			break;
		}
	}
	if (G.vars[vi].unsuffixed)
	{
		h = hash_key(G.vars[vi].name, 0) % hash_cap;
		for (i = 0; i < hash_cap; i++)
		{
			unsigned s = (h + i) % hash_cap;
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
	unsigned h;
	unsigned i;
	if (!hash_cap)
		return -1;
	h = hash_key(nbuf, type) % hash_cap;
	for (i = 0; i < hash_cap; i++)
	{
		unsigned s = (h + i) % hash_cap;
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
	unsigned h;
	unsigned i;
	if (!hash_cap)
		return -1;
	h = hash_key(nbuf, 0) % hash_cap;
	for (i = 0; i < hash_cap; i++)
	{
		unsigned s = (h + i) % hash_cap;
		int vi = unsuf_tab[s];
		if (vi < 0)
			return -1;
		if (G.vars[vi].used && G.vars[vi].unsuffixed && name_eq(G.vars[vi].name, nbuf))
			return vi;
	}
	return -1;
}

/* Remove `vi` from one open-addressed table.  Backward-shift the rest of the
 * probe cluster into the hole so entries stored after it stay reachable. */
static void hash_unlink(int *tab, unsigned home, int vi, int unsuf)
{
	unsigned mask = hash_cap - 1;
	unsigned i = home, j;
	while (tab[i] != vi)
	{
		if (tab[i] < 0)
			return;
		i = (i + 1) & mask;
	}
	tab[i] = -1;
	j = i;
	for (;;)
	{
		int w;
		unsigned hw;
		j = (j + 1) & mask;
		w = tab[j];
		if (w < 0)
			break;
		hw = unsuf ? hash_key(G.vars[w].name, 0) % hash_cap
			   : hash_key(G.vars[w].name, G.vars[w].type) % hash_cap;
		/* The hole at i is on w's probe path from hw to j, so w can move
		 * back into it without becoming unreachable. */
		if (((j - hw) & mask) >= ((j - i) & mask))
		{
			tab[i] = w;
			tab[j] = -1;
			i = j;
		}
	}
}

/* Drop the hash entries for a slot that is being freed (LOCAL return).  A
 * wholesale hash_rebuild() here was O(vars_cap) per call (#1328). */
static void hash_del(int vi)
{
	unsigned h;
	if (vi < 0 || vi >= G.vars_cap || !G.vars[vi].used)
		return;
	if (!hash_cap)
		return;
	h = hash_key(G.vars[vi].name, G.vars[vi].type) % hash_cap;
	hash_unlink(var_tab, h, vi, 0);
	if (G.vars[vi].unsuffixed)
	{
		h = hash_key(G.vars[vi].name, 0) % hash_cap;
		hash_unlink(unsuf_tab, h, vi, 1);
	}
}

/* Free slot in the CONST table, doubling it when full (#1303). */
static int consts_free_slot(void)
{
	int i;
	if (!G.consts)
	{
		G.consts = (mmb_const *)G.plat->alloc((unsigned)MMB_MAX_CONST * sizeof(mmb_const));
		if (!G.consts)
			mmb_error("?OUT OF MEMORY");
		memset(G.consts, 0, (unsigned)MMB_MAX_CONST * sizeof(mmb_const));
		G.consts_cap = MMB_MAX_CONST;
	}
	for (i = 0; i < G.consts_cap; i++)
		if (!G.consts[i].used)
			return i;
	{
		mmb_const *nc = (mmb_const *)G.plat->alloc((unsigned)G.consts_cap * 2 * sizeof(mmb_const));
		if (!nc)
			mmb_error("?OUT OF MEMORY");
		memset(nc, 0, (unsigned)G.consts_cap * 2 * sizeof(mmb_const));
		memcpy(nc, G.consts, (unsigned)G.consts_cap * sizeof(mmb_const));
		G.plat->free(G.consts);
		G.consts = nc;
		G.consts_cap *= 2;
		return G.consts_cap / 2;
	}
}

void mmb_clear_consts(void)
{
	int i;
	for (i = 0; i < G.consts_cap; i++)
	{
		if (G.consts[i].used && G.consts[i].val.type == T_STR)
		{
			mmb_str_free(G.consts[i].s);
			G.consts[i].s = 0;
		}
		G.consts[i].used = 0;
	}
	G.nconst = 0;
	if (G.consts)
	{
		G.plat->free(G.consts);
		G.consts = 0;
	}
	G.consts_cap = 0;
	mmb_resolve_invalidate();
}

void mmb_const_define(const char *name, int type, mmb_val val)
{
	int i, slot;
	char nbuf[MMB_MAX_NAME];
	strncpy(nbuf, name, MMB_MAX_NAME - 1);
	nbuf[MMB_MAX_NAME - 1] = 0;
	mmb_upper(nbuf);
	if (type == 0)
		type = val.type;
	for (i = 0; i < G.consts_cap; i++)
	{
		if (G.consts[i].used && name_eq(G.consts[i].name, nbuf))
			return; /* CMM2 allows CONST inside a SUB on each call */
	}
	slot = consts_free_slot();
	memset(&G.consts[slot], 0, sizeof(G.consts[slot]));
	strncpy(G.consts[slot].name, nbuf, MMB_MAX_NAME - 1);
	G.consts[slot].type = type;
	G.consts[slot].val = val;
	if (val.type == T_STR)
		mmb_val_own(&G.consts[slot].val, &G.consts[slot].s, 0, nbuf);
	G.consts[slot].used = 1;
	G.nconst++;
	mmb_resolve_invalidate();
}

int mmb_const_lookup(const char *name, int type, mmb_val *out)
{
	int i;
	char nbuf[MMB_MAX_NAME];
	int t;
	/* Most programs declare no constants; skip the copy/uppercase/scan on
	 * every scalar read and write (#993). */
	if (G.nconst <= 0)
		return 0;
	strncpy(nbuf, name, MMB_MAX_NAME - 1);
	nbuf[MMB_MAX_NAME - 1] = 0;
	mmb_upper(nbuf);
	t = mmb_type_suffix(nbuf);
	if (t)
		type = t;
	for (i = 0; i < G.consts_cap; i++)
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

/* Free slot in the variable table, allocating it on first use and doubling
 * it when full (#1303). Growth moves the table, so both the trace cache and
 * the resolve cache (which hold mmb_var*) are invalidated. */
static int vars_free_slot(void)
{
	int slot = -1;
	int i;
	if (!G.vars)
	{
		G.vars = (mmb_var *)G.plat->alloc((unsigned)MMB_MAX_VARS * sizeof(mmb_var));
		if (!G.vars)
			mmb_error("?OUT OF MEMORY");
		memset(G.vars, 0, (unsigned)MMB_MAX_VARS * sizeof(mmb_var));
		G.vars_cap = MMB_MAX_VARS;
		free_next = (int *)G.plat->alloc((unsigned)MMB_MAX_VARS * sizeof(int));
		if (!free_next)
			mmb_error("?OUT OF MEMORY");
		memset(free_next, 0xff, (unsigned)MMB_MAX_VARS * sizeof(int));
		free_cap = MMB_MAX_VARS;
		free_head = -1;
		alloc_high = 0;
		slot_sig = (unsigned *)G.plat->alloc((unsigned)MMB_MAX_VARS * sizeof(unsigned));
		reuse_gen = (unsigned *)G.plat->alloc((unsigned)MMB_MAX_VARS * sizeof(unsigned));
		if (!slot_sig || !reuse_gen)
			mmb_error("?OUT OF MEMORY");
		memset(slot_sig, 0, (unsigned)MMB_MAX_VARS * sizeof(unsigned));
		memset(reuse_gen, 0, (unsigned)MMB_MAX_VARS * sizeof(unsigned));
	}
	if (free_head >= 0)
	{
		slot = free_head;
		free_head = free_next[slot];
		free_next[slot] = -1;
	}
	else if (alloc_high < G.vars_cap)
		slot = (int)alloc_high++;
	if (slot < 0)
	{
		mmb_var *old = G.vars;
		mmb_var *nv = (mmb_var *)G.plat->alloc((unsigned)G.vars_cap * 2 * sizeof(mmb_var));
		int *nf;
		if (!nv)
			mmb_error("?OUT OF MEMORY");
		memset(nv, 0, (unsigned)G.vars_cap * 2 * sizeof(mmb_var));
		memcpy(nv, G.vars, (unsigned)G.vars_cap * sizeof(mmb_var));
		G.plat->free(G.vars);
		G.vars = nv;
		G.vars_cap *= 2;
		nf = (int *)G.plat->alloc((unsigned)G.vars_cap * sizeof(int));
		if (!nf)
			mmb_error("?OUT OF MEMORY");
		memset(nf, 0xff, (unsigned)G.vars_cap * sizeof(int));
		memcpy(nf, free_next, (unsigned)free_cap * sizeof(int));
		G.plat->free(free_next);
		free_next = nf;
		free_cap = G.vars_cap;
		{
			unsigned *ns = (unsigned *)G.plat->alloc((unsigned)G.vars_cap * sizeof(unsigned));
			unsigned *nr = (unsigned *)G.plat->alloc((unsigned)G.vars_cap * sizeof(unsigned));
			if (!ns || !nr)
				mmb_error("?OUT OF MEMORY");
			memset(ns, 0, (unsigned)G.vars_cap * sizeof(unsigned));
			memset(nr, 0, (unsigned)G.vars_cap * sizeof(unsigned));
			if (slot_sig)
			{
				memcpy(ns, slot_sig, (unsigned)(G.vars_cap / 2) * sizeof(unsigned));
				G.plat->free(slot_sig);
			}
			if (reuse_gen)
			{
				memcpy(nr, reuse_gen, (unsigned)(G.vars_cap / 2) * sizeof(unsigned));
				G.plat->free(reuse_gen);
			}
			slot_sig = ns;
			reuse_gen = nr;
		}
		/* An active FOR loop keeps a live mmb_var* across its body; growth
		 * moved the table, so rebase every loop variable (#1303). */
		for (i = 0; i < G.for_sp; i++)
			if (G.forstack[i].vp)
				G.forstack[i].vp = nv + (G.forstack[i].vp - old);
		mmb_tcache_invalidate();
		mmb_resolve_invalidate();
		slot = (int)alloc_high++; /* first fresh slot of the doubled table */
	}
	/* Keep the name hash load factor at or below 0.5 so the insert that
	 * follows this call cannot overflow the probe (#1303). */
	hash_ensure((unsigned)G.nvars + 1);
	return slot;
}

void mmb_clear_vars(int keep_options)
{
	int i, d;
	(void)keep_options;
	for (i = 0; i < G.vars_cap; i++)
	{
		if (G.vars[i].used)
		{
			if (G.vars[i].type == T_STR && G.vars[i].data.s)
			{
				for (d = 0; d < G.vars[i].size; d++)
					mmb_str_free(G.vars[i].data.s[d]);
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
	if (G.vars)
	{
		G.plat->free(G.vars);
		G.vars = 0;
	}
	G.vars_cap = 0;
	G.nvars = 0;
	G.dim_used = 0;
	if (free_next)
	{
		G.plat->free(free_next);
		free_next = 0;
	}
	if (slot_sig)
	{
		G.plat->free(slot_sig);
		slot_sig = 0;
	}
	if (reuse_gen)
	{
		G.plat->free(reuse_gen);
		reuse_gen = 0;
	}
	free_cap = 0;
	free_head = -1;
	alloc_high = 0;
	free_gen_seq = 0;
	hash_release();
	mmb_tcache_invalidate();
	mmb_resolve_invalidate();
}

static int elem_count(const int *dim, int ndims)
{
	int n = 1, i;
	for (i = 0; i < ndims; i++)
		n *= (dim[i] - G.opt.base + 1);
	return n;
}

/* Row-major element strides: stride[dims-1] = 1, stride[i] is the product of
 * the lengths of the dimensions after i. Depends on the current OPTION BASE,
 * so a var records which base its cached strides belong to. */
static void set_var_stride(mmb_var *v)
{
	int i;
	v->stride_base = G.opt.base;
	if (v->dims <= 0)
		return;
	v->stride[v->dims - 1] = 1;
	for (i = v->dims - 2; i >= 0; i--)
		v->stride[i] = v->stride[i + 1] * (v->dim[i + 1] - G.opt.base + 1);
}

static int offset_of(mmb_var *v, const int *idx)
{
	int off = 0, i;
	if (v->dims > 0 && v->stride_base != G.opt.base)
		set_var_stride(v);
	for (i = v->dims - 1; i >= 0; i--)
	{
		if (idx[i] < G.opt.base || idx[i] > v->dim[i])
			mmb_error("?INDEX OUT OF BOUNDS");
		off += (idx[i] - G.opt.base) * v->stride[i];
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
		if (G.acc_base_nidx && v->dims == G.acc_base_nidx)
			return offset_of(v, G.acc_base_idx);
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
	for (i = 0; i < G.vars_cap; i++)
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
	for (i = 0; i < G.vars_cap; i++)
		if (G.vars[i].used && G.vars[i].type == T_STRUCT && name_eq(G.vars[i].name, nbuf))
		{
			if (G.vars[i].struct_idx != sid)
				mmb_error("?TYPE MISMATCH");
			return;
		}
	slot = vars_free_slot();
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
	var_bind_gen(slot);
	hash_ins(slot);
	mmb_resolve_invalidate();
}

static mmb_var *try_struct_path(char *nbuf, int nidx, int *idx)
{
	char *dot;
	char base[MMB_MAX_NAME], bname[MMB_MAX_NAME];
	int bl, bnidx = 0, bidx[MMB_MAX_DIMS], i, embedded, ok;
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
	embedded = mmb_decode_part(base, bl, bname, &bnidx, bidx);
	sv = mmb_lookup_struct_var(bname);
	if (!sv)
		return 0;
	G.acc_base_nidx = 0;
	ok = 0;
	if (embedded)
	{
		if (sv->dims == 0)
			mmb_error("?NOT AN ARRAY");
		if (sv->dims != bnidx)
			mmb_error("?SUBSCRIPT");
		G.acc_base_nidx = bnidx;
		for (i = 0; i < bnidx; i++)
			G.acc_base_idx[i] = bidx[i];
		ok = mmb_struct_resolve(sv, dot + 1, nidx, idx);
	}
	else if (sv->dims == nidx)
	{
		G.acc_base_nidx = nidx;
		for (i = 0; i < nidx; i++)
			G.acc_base_idx[i] = idx ? idx[i] : 0;
		ok = mmb_struct_resolve(sv, dot + 1, 0, 0);
	}
	else if (sv->dims == 0)
	{
		ok = mmb_struct_resolve(sv, dot + 1, nidx, idx);
	}
	if (ok)
	{
		if (idx && nidx == 0)
			*idx = 0;
		return sv;
	}
	mmb_error("?UNKNOWN");
	return 0;
}

/* #1304: a user identifier whose base name (type suffix stripped) collides
 * with a built-in function or a named colour constant can never be reached in
 * an expression - built-ins and colours resolve before the variable table -
 * so declaring one used to fail only later, at the call site, or silently
 * shadowed the wrong way round. Reject it at declaration time with a message
 * naming the conflicting built-in.
 *
 * Command-only keywords (DIR, DRAW, COLOR, ...) are deliberately NOT
 * reserved: they never produce a value in an expression and real apps use
 * them as parameter names (mmcore-apps tetris `sub gm.rotate(dir)`, sudoku
 * `local i, y, draw`, syntaxshock `local near_x, far_x, color`). Suffixed
 * names (grey%, name$) are exempt: the suffix makes them unambiguous to the
 * tokeniser and expression parser.
 *
 * Commands that end the interpreter (QUIT, END, STOP, REBOOT, RESTART,
 * SHUTDOWN) are also rejected: a bare assignment to such a name is dispatched
 * as the command before it can reach the variable table (#1334). */
/* #1334: commands that terminate or restart the interpreter are dispatched
 * before an assignment is even recognised, so a variable declared with one of
 * these names silently runs the command when it is later assigned:
 *
 *     LOCAL quit : quit = 0     ' exits without a message
 *
 * Only this terminating set is reserved here. Command-only keywords that do
 * not end the session (DIR, DRAW, COLOR, ...) stay usable as names; real apps
 * pass them as parameters (#1304). */
static int is_terminating_command(const char *nbuf)
{
	static const char *const names[] = {
		"QUIT", "END", "STOP", "REBOOT", "RESTART", "SHUTDOWN", 0
	};
	int i;
	for (i = 0; names[i]; i++)
		if (strcmp(nbuf, names[i]) == 0)
			return 1;
	return 0;
}

/* #1337: command keywords whose token is dispatched before an assignment is
 * recognised. A bare assignment shaped like `quit = 0` otherwise runs the
 * command instead of storing the value. This is the terminating set above
 * plus the command-only keywords that hijack an assignment without ending the
 * session (`run = 0` reruns the program, `new = 0` clears it, `chain = 0`
 * loads a file). Command syntax such as `RUN "file"` is untouched: callers
 * only consult this when the keyword token is followed by `=`. */
int mmb_command_hijacks_assignment(const char *name)
{
	static const char *const extra[] = { "RUN", "NEW", "CHAIN", 0 };
	int i;
	if (is_terminating_command(name))
		return 1;
	for (i = 0; extra[i]; i++)
		if (strcmp(name, extra[i]) == 0)
			return 1;
	return 0;
}

void mmb_check_user_name(const char *name)
{
	char nbuf[MMB_MAX_NAME];
	char msg[MMB_MAX_NAME + 48];
	int n, ok;
	if (!name || !*name)
		return;
	strncpy(nbuf, name, MMB_MAX_NAME - 1);
	nbuf[MMB_MAX_NAME - 1] = 0;
	mmb_upper(nbuf);
	n = (int)strlen(nbuf);
	if (n > 0 && (nbuf[n - 1] == '$' || nbuf[n - 1] == '%' || nbuf[n - 1] == '!'))
		return;
	if (mmb_is_builtin_func(nbuf))
	{
		snprintf(msg, sizeof(msg), "?NAME CONFLICT: %s is a built-in function", nbuf);
		mmb_error(msg);
	}
	mmb_named_colour(nbuf, &ok);
	if (ok)
	{
		snprintf(msg, sizeof(msg), "?NAME CONFLICT: %s is a built-in constant", nbuf);
		mmb_error(msg);
	}
	if (is_terminating_command(nbuf))
	{
		snprintf(msg, sizeof(msg), "?NAME CONFLICT: %s is a reserved command", nbuf);
		mmb_error(msg);
	}
}

int mmb_bare_name_type(const char *name)
{
	int g, k;
	/* Walk the call frames from innermost outwards. A frame's LOCALs shadow
	 * everything outside it; the SUB/FUNCTION parameters of that frame come
	 * next. This keeps `x` bound to the parameter declared AS FLOAT in the
	 * callee even when an earlier same-named parameter was AS INTEGER, and
	 * to a LOCAL x even when a global/param x of another type exists
	 * (#1315). */
	for (g = G.gosub_sp - 1; g >= 0; g--)
	{
		int si;
		for (k = 0; k < G.gosub_nlocal[g]; k++)
		{
			int slot = G.gosub_local[g][k].slot;
			mmb_var *v;
			if (slot < 0 || slot >= G.vars_cap)
				continue;
			v = &G.vars[slot];
			if (v->used && v->unsuffixed && mmb_keyword_eq(v->name, name))
				return v->type;
		}
		si = G.gosub_sub[g];
		if (si < 0 || si >= G.nsubs || !G.subs[si].used)
			continue;
		for (k = 0; k < G.subs[si].nargs; k++)
		{
			char abuf[MMB_MAX_NAME];
			int sl;
			if (G.subs[si].arg_sid[k] >= 0)
				continue;
			strncpy(abuf, G.subs[si].args[k], MMB_MAX_NAME - 1);
			abuf[MMB_MAX_NAME - 1] = 0;
			sl = (int)strlen(abuf);
			/* A suffixed parameter (x$, x%) is a different variable from
			 * the bare name, so it must not capture bare lookups. */
			if (sl && (abuf[sl - 1] == '$' || abuf[sl - 1] == '%' ||
				   abuf[sl - 1] == '!'))
				continue;
			mmb_upper(abuf);
			if (mmb_keyword_eq(abuf, name))
				return G.subs[si].arg_type[k];
		}
	}
	return -1;
}

/* Same resolution rules as mmb_find_var(), but never raises and never creates:
 * a miss is simply "not an existing variable".  The trace-cache compiler uses
 * this so a (re)compile is O(1) in the number of globals instead of scanning
 * the whole table (#1385). */
mmb_var *mmb_lookup_var_nothrow(const char *name, int type)
{
	char nbuf[MMB_MAX_NAME];
	int t, i;
	G.acc_on = 0;
	strncpy(nbuf, name, MMB_MAX_NAME - 1);
	nbuf[MMB_MAX_NAME - 1] = 0;
	mmb_upper(nbuf);
	/* The variable table is lazily allocated now (#1303), so it can be
	 * NULL before the first variable exists. */
	if (!G.vars)
		return 0;
	t = mmb_type_suffix(nbuf);
	if (t)
		type = t;
	else if (type == 0)
	{
		int at = mmb_bare_name_type(nbuf);
		if (at >= 0)
		{
			/* Mirror mmb_find_var(): a bare parameter name inside a
			 * SUB/FUNCTION resolves with the declared type of that
			 * parameter, not the first same-named unsuffixed variable
			 * (#1315). */
			type = at ? at : G.opt.default_type;
		}
		else
		{
			int ui = unsuf_lookup(nbuf);
			if (ui >= 0)
				type = G.vars[ui].type;
		}
		if (type == 0 && (G.opt.default_type == T_STR ||
				  G.opt.default_type == T_INT))
			type = G.opt.default_type;
		if (type == 0)
			type = T_NUM;
	}
	i = hash_lookup(nbuf, type);
	return i >= 0 ? &G.vars[i] : 0;
}

mmb_var *mmb_find_var(const char *name, int type, int create, int nidx, int *idx)
{
	int i;
	char nbuf[MMB_MAX_NAME];
	int t, typed_lookup, had_suffix, sl, bare;
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
	if (strchr(nbuf, '('))
		mmb_error("?UNKNOWN");
	/* A suffix ($/%/!) makes the name unambiguous to the parser, so it
	 * stays exempt from the built-in name check below (#1304). The
	 * assignment parser strips the suffix before calling here, so `bare`
	 * (no type hint from the caller) is the other half of the signal:
	 * grey% = 1 arrives with type T_INT, grey = 0 with type 0. */
	sl = (int)strlen(nbuf);
	had_suffix = sl > 0 &&
		     (nbuf[sl - 1] == '$' || nbuf[sl - 1] == '%' || nbuf[sl - 1] == '!');
	bare = (type == 0);
	t = mmb_type_suffix(nbuf);
	typed_lookup = (type != 0) || t;
	if (t)
		type = t;
	else if (type == 0)
	{
		int at = mmb_bare_name_type(nbuf);
		if (at >= 0)
		{
			/* Inside a SUB/FUNCTION a bare name that is one of its
			 * parameters denotes that parameter's declared type, not the
			 * first same-named unsuffixed variable (#1315). */
			type = at ? at : G.opt.default_type;
		}
		else
		{
			int ui = unsuf_lookup(nbuf);
			if (ui >= 0)
				type = G.vars[ui].type;
			if (type == 0)
				type = G.opt.default_type;
		}
	}
	if (type == 0 && G.opt.explicit)
		mmb_error("?EXPLICIT");
	if (type == 0)
		type = T_NUM;

	/* The hash is authoritative: mmb_init() clears it, and every in-place
	 * binding change re-inserts or rebuilds it, so a miss means the
	 * variable does not exist (#998). */
	i = hash_lookup(nbuf, type);
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
	/* Declaring (or implicitly creating) a variable with a built-in's name
	 * would only fail later, where the token is parsed as that function, or
	 * silently resolve to the built-in constant (#1304). Built-in function
	 * names were always rejected here; named colours are rejected only for
	 * unsuffixed bindings (grey% = 1 stays allowed, as DIM grey% was). */
	if (!had_suffix && (bare || mmb_is_builtin_func(nbuf)))
		mmb_check_user_name(nbuf);
	if (type == T_STRUCT)
		mmb_error("?UNDECLARED");
	if (G.opt.explicit)
		mmb_error("?UNDECLARED");
	if (nidx > 0)
		mmb_error("?UNDECLARED"); /* arrays must be DIMmed */

	i = vars_free_slot();

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
		G.vars[i].data.s[0] = mmb_str_empty();
	}
	else
	{
		G.vars[i].data.f = G.plat->alloc(sizeof(double));
		G.vars[i].data.f[0] = 0;
	}
	G.nvars++;
	var_bind_gen(i);
	hash_ins(i);
	if (idx)
		*idx = 0;
	return &G.vars[i];
}

mmb_var *mmb_find_var_ref(const char *ref, const char *name, int type, int create,
			  int nidx, int *idx)
{
	mmb_rc_ent *e;
	mmb_var *v;
	/* Only stable, non-struct program tokens qualify: immediate-mode text
	 * moves through a reused buffer, and a struct member path needs the
	 * accumulator context that resides in G, not on the token. */
	if (!ref || !G.running || G.acc_on || strchr(name, '.'))
		return mmb_find_var(name, type, create, nidx, idx);
	e = rc_slot(ref);
	if (e->ref == ref && e->gen == rc_gen && e->type == type &&
	    e->nidx == nidx && e->v->gen == e->vgen && strcmp(e->name, name) == 0)
		return e->v;
	v = mmb_find_var(name, type, create, nidx, idx);
	if (!v)
		return 0; /* never cache a miss: a later create would be stale */
	e->ref = ref;
	e->gen = rc_gen;
	e->type = type;
	e->nidx = nidx;
	e->v = v;
	e->vgen = v->gen;
	strncpy(e->name, name, MMB_MAX_NAME - 1);
	e->name[MMB_MAX_NAME - 1] = 0;
	return v;
}

void mmb_cmd_dim(void)
{
	/* DIM [SHARED] [INTEGER|FLOAT|STRING] name(d1[,d2...]) [AS type] [, ...] */
	int group = 0;
	/* No wholesale cache flush here (#1368): creating a slot leaves every
	 * existing binding at the same address, and rebinding one (LOCAL
	 * shadow) bumps only that slot's generation.  vars_free_slot() still
	 * invalidates both caches when it has to move the table. */
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
		int maxlen = 0;
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
			maxlen = (int)mmb_as_int(lv);
			if (maxlen < 0)
				maxlen = 0;
		}
		if (type == 0)
			type = G.opt.default_type ? G.opt.default_type : T_NUM;
		if (type == T_STRUCT && struct_idx < 0)
			mmb_syntax();
		/* Built-in names (CHOICE, ...) and named colours (GREY, ...) are
		 * reserved unless a type suffix makes them an ordinary identifier
		 * (CHOICE% is not a keyword token in the CMM2 tokeniser). Struct
		 * variables may shadow a built-in: the expression parser resolves
		 * x(...)/x.y through the struct before the function token. */
		if (!had_suffix && type != T_STRUCT)
			mmb_check_user_name(name);

		/* The name hash is authoritative, so an exact name+type lookup is
		 * O(1) instead of scanning the whole variable table (#1328).  LOCAL
		 * runs this on every call of a SUB/FUNCTION with locals, so the
		 * linear scan made its cost grow with the number of globals. */
		i = hash_lookup(name, type);
		if (i >= 0)
		{
			if (G.dim_local)
			{
				int r = mmb_local_save(i, 1);
				if (r < 0)
					mmb_error("?OUT OF MEMORY");
				slot = i;
				if (r == 0)
					goto dim_init; /* already local here, or no frame */
				goto dim_reinit;
			}
			mmb_error("?ALREADY DECLARED");
		}

		slot = vars_free_slot();
		if (G.dim_local && mmb_local_save(slot, 0) < 0)
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
		alloc_var_storage(&G.vars[slot], n);
		G.nvars++;
		if (G.dim_local)
			var_bind_local(slot);
		else
			var_bind_gen(slot);
		hash_ins(slot);
		if (dims)
			G.dim_used = 1;
		(void)idxdummy;
		goto dim_init;
	dim_reinit:
		/* A scoped LOCAL shadows an existing binding whose storage was
		 * detached into this frame's save record, so rebind it fresh.
		 * The slot address stays put but its meaning changed, so bump the
		 * generation: cached entries for the old binding fall back to the
		 * interpreter instead of reading the scoped variable (#1368). */
		{
			int oldunsuf = G.vars[slot].unsuffixed;
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
			alloc_var_storage(&G.vars[slot], n);
			var_bind_gen(slot);
			if (oldunsuf != G.vars[slot].unsuffixed)
				hash_rebuild();
			if (dims)
				G.dim_used = 1;
			(void)idxdummy;
		}
	dim_init:
		G.vars[slot].maxlen = (type == T_STR) ? maxlen : 0;
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
						mmb_str_set(&G.vars[slot].data.s[ei],
							   init.s, -1, G.vars[slot].maxlen,
							   G.vars[slot].name);
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
					mmb_str_set(&G.vars[slot].data.s[0],
						   init.s, -1, G.vars[slot].maxlen,
						   G.vars[slot].name);
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
				mmb_str_free(v->data.s[d]);
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
	set_var_stride(v);
	if (v->type == T_INT)
	{
		v->data.i = G.plat->alloc((unsigned)n * sizeof(int64_t));
		memset(v->data.i, 0, (unsigned)n * sizeof(int64_t));
	}
	else if (v->type == T_STR)
	{
		v->data.s = G.plat->alloc((unsigned)n * sizeof(char *));
		for (i = 0; i < n; i++)
			v->data.s[i] = mmb_str_empty();
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

/* Record the binding of `slot` for the current SUB/FUNCTION frame so it can be
 * restored when the frame returns.  Returns 1 when the caller should rebind the
 * slot as a fresh scoped variable, 0 when nothing needs saving (no active frame,
 * or the name is already a local of this frame), and -1 when the frame's local
 * table is full.  `existed` says whether the slot held a real binding. */
static int mmb_local_save(int slot, int existed)
{
	int g, k, i;
	mmb_localsave *ls;
	mmb_var *v;
	if (G.gosub_sp <= 0)
		return 0;
	g = G.gosub_sp - 1;
	for (k = 0; k < G.gosub_nlocal[g]; k++)
		if (G.gosub_local[g][k].slot == slot)
			return 0;
	if (G.gosub_nlocal[g] >= MMB_MAX_LOCALS)
		return -1;
	ls = &G.gosub_local[g][G.gosub_nlocal[g]++];
	ls->slot = slot;
	ls->existed = existed;
	if (!existed)
		return 1;
	v = &G.vars[slot];
	ls->type = v->type;
	ls->dims = v->dims;
	for (i = 0; i < MMB_MAX_DIMS; i++)
		ls->dim[i] = v->dim[i];
	ls->size = v->size;
	ls->struct_idx = v->struct_idx;
	ls->unsuffixed = v->unsuffixed;
	ls->maxlen = v->maxlen;
	ls->data = v->data.f;
	return 1;
}

void mmb_vars_rehash(void)
{
	hash_rebuild();
	mmb_resolve_invalidate();
}

void mmb_local_restore(int g)
{
	int k, i;
	if (G.gosub_nlocal[g] <= 0)
		return;
	for (k = G.gosub_nlocal[g] - 1; k >= 0; k--)
	{
		mmb_localsave *ls = &G.gosub_local[g][k];
		mmb_var *v;
		if (ls->slot < 0 || ls->slot >= G.vars_cap)
			continue;
		v = &G.vars[ls->slot];
		free_var_storage(v);
		if (ls->existed)
		{
			v->type = ls->type;
			v->dims = ls->dims;
			for (i = 0; i < MMB_MAX_DIMS; i++)
				v->dim[i] = ls->dim[i];
			v->size = ls->size;
			v->struct_idx = ls->struct_idx;
			v->unsuffixed = ls->unsuffixed;
			v->maxlen = ls->maxlen;
			v->data.f = ls->data;
			v->used = 1;
			set_var_stride(v);
		}
		else
		{
			/* The scoped binding never existed outside this frame, so drop
			 * just its name entries and recycle the slot.  A full
			 * hash_rebuild() here was O(vars_cap) per LOCAL call (#1328).
			 * Keep the binding signature and its old generation so the
			 * same LOCAL next call can revive the cached body; the new
			 * generation makes every entry stale in the meantime. */
			unsigned oldgen = v->gen;
			unsigned sig = var_binding_sig(v);
			hash_del(ls->slot);
			memset(v, 0, sizeof(*v));
			G.nvars--;
			v->gen = 0x80000000u + (++free_gen_seq);
			if (slot_sig)
				slot_sig[ls->slot] = sig;
			if (reuse_gen)
				reuse_gen[ls->slot] = oldgen;
			free_next[ls->slot] = free_head;
			free_head = ls->slot;
			continue;
		}
		/* The restored binding is a different meaning than the scoped one:
		 * bump the slot generation so any cached pointer to it falls back
		 * to the interpreter (#1368).  Entries for variables this frame did
		 * not touch stay valid, so no wholesale trace/resolve flush is
		 * needed. */
		var_bind_gen(ls->slot);
	}
	G.gosub_nlocal[g] = 0;
}

void mmb_cmd_redim(void)
{
	int preserve = 0;
	int base = G.opt.base;
	mmb_tcache_invalidate();
	mmb_resolve_invalidate();
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
		for (i = 0; i < G.vars_cap; i++)
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
			slot = vars_free_slot();
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
		v->maxlen = 0;
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
					mmb_str_set(&v->data.s[noff], os[oi_index], -1,
						   v->maxlen, v->name);
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
					mmb_str_free(os[i]);
			G.plat->free(os);
		}
		else if (type == T_INT && oi)
			G.plat->free(oi);
		else if (type == T_NUM && of)
			G.plat->free(of);
		else if (type == T_STRUCT && ob)
			G.plat->free(ob);
		if (is_new)
		{
			var_bind_gen((int)(v - G.vars));
			hash_ins((int)(v - G.vars));
		}
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
		if (!mmb_is_ident_start(*G.p))
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
		mmb_str_set(&v->data.s[off], val.s ? val.s : "", -1,
			   v->maxlen, v->name);
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

static int ref_name_char(char c)
{
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
	       (c >= '0' && c <= '9') || c == '_';
}

static int parse_ref_seg(char *out, int outsz)
{
	int n = 0;
	mmb_skip_sp();
	if (mmb_tok_expand(out, outsz))
	{
		n = (int)strlen(out);
		if (*G.p == '$' || *G.p == '%' || *G.p == '!')
			out[n++] = *G.p++;
		out[n] = 0;
		return 1;
	}
	if (!mmb_is_ident_start(*G.p))
		return 0;
	while (ref_name_char(*G.p) && n < outsz - 2)
	{
		char c = *G.p++;
		if (c >= 'a' && c <= 'z')
			c = (char)(c - 32);
		out[n++] = c;
	}
	if (*G.p == '$' || *G.p == '%' || *G.p == '!')
		out[n++] = *G.p++;
	out[n] = 0;
	return 1;
}

static void parse_ref_indices(int *nidx, int *idx)
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

static void ref_append(char *name, int *nl, const char *s)
{
	int n = (int)strlen(s);
	if (*nl + n >= MMB_MAX_NAME)
		mmb_error("?SYNTAX ERROR");
	memcpy(name + *nl, s, (unsigned)n);
	*nl += n;
	name[*nl] = 0;
}

static void ref_append_idx(char *name, int *nl, const int *idx, int nidx)
{
	char part[MMB_MAX_DIMS * 13 + 4];
	int k = 0, i;
	part[k++] = '(';
	for (i = 0; i < nidx; i++)
	{
		char tmp[16];
		int m = 0, neg = 0;
		int v = idx[i];
		if (v < 0)
		{
			neg = 1;
			v = -v;
		}
		do
		{
			tmp[m++] = (char)('0' + v % 10);
			v /= 10;
		} while (v);
		if (neg)
			part[k++] = '-';
		while (m)
			part[k++] = tmp[--m];
		if (i + 1 < nidx)
			part[k++] = ',';
	}
	part[k++] = ')';
	part[k] = 0;
	ref_append(name, nl, part);
}

int mmb_parse_var_ref(char *name, int *nidx, int *idx)
{
	int t = 0, nl = 0;
	name[0] = 0;
	*nidx = 0;
	for (;;)
	{
		char seg[MMB_MAX_NAME];
		int snidx = 0, sidx[MMB_MAX_DIMS], i;
		if (!parse_ref_seg(seg, sizeof(seg)))
			mmb_syntax();
		t = mmb_type_suffix(seg);
		mmb_skip_sp();
		while (*G.p == '(')
		{
			parse_ref_indices(&snidx, sidx);
			mmb_skip_sp();
		}
		if (*G.p == '.')
		{
			ref_append(name, &nl, seg);
			if (snidx)
				ref_append_idx(name, &nl, sidx, snidx);
			ref_append(name, &nl, ".");
			G.p++;
			continue;
		}
		ref_append(name, &nl, seg);
		*nidx = snidx;
		for (i = 0; i < snidx; i++)
			idx[i] = sidx[i];
		break;
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

void mmb_do_assign_ref(const char *ref, const char *name, int type_hint, int nidx,
		       int *idx, mmb_val val)
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
	v = mmb_find_var_ref(ref, name, type_hint, 1, nidx, idxcopy);
	off = mmb_elem_off(v, nidx, idx);
	store(v, off, val);
}

void mmb_cmd_clear(void)
{
	mmb_clear_vars(1);
}
