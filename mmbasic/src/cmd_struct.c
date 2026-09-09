#include "mmb_priv.h"
#include <string.h>

static int align8(int off)
{
	return (off + 7) & ~7;
}

static int has_numeric_mem(const mmb_sdef *d)
{
	int i;
	for (i = 0; i < d->nmem; i++)
		if (d->mem[i].type == T_INT || d->mem[i].type == T_NUM ||
		    d->mem[i].type == T_STRUCT)
			return 1;
	return 0;
}

static int mem_elem_size(const mmb_smem *m)
{
	if (m->type == T_INT || m->type == T_NUM)
		return 8;
	if (m->type == T_STR)
		return m->size + 1;
	if (m->type == T_STRUCT && m->size >= 0 && m->size < G.nstruct)
		return G.sdef[m->size].total;
	return 0;
}

void mmb_struct_clear(void)
{
	memset(G.sdef, 0, sizeof(G.sdef));
	G.nstruct = 0;
	G.acc_on = 0;
}

int mmb_struct_lookup(const char *name)
{
	int i;
	for (i = 0; i < G.nstruct; i++)
		if (G.sdef[i].used && mmb_keyword_eq(G.sdef[i].name, name))
			return i;
	return -1;
}

mmb_sdef *mmb_struct_def(int idx)
{
	if (idx < 0 || idx >= G.nstruct || !G.sdef[idx].used)
		return 0;
	return &G.sdef[idx];
}

unsigned char *mmb_struct_elem(mmb_var *v, int off)
{
	if (!v || v->type != T_STRUCT || !v->data.blob)
		return 0;
	return v->data.blob + (unsigned)off * (unsigned)G.sdef[v->struct_idx].total;
}

static int parse_member_type(int *type, int *size)
{
	char tn[MMB_MAX_NAME];
	int sid;
	mmb_skip_sp();
	if (mmb_match("INTEGER") || mmb_match("INT"))
	{
		*type = T_INT;
		*size = 8;
		return 1;
	}
	if (mmb_match("FLOAT"))
	{
		*type = T_NUM;
		*size = 8;
		return 1;
	}
	if (mmb_match("STRING"))
	{
		*type = T_STR;
		*size = MMB_MAX_STR;
		mmb_skip_sp();
		if (mmb_match("LENGTH"))
		{
			mmb_val v = mmb_expr();
			*size = (int)mmb_as_int(v);
			if (*size < 0)
				*size = 0;
			if (*size > MMB_MAX_STR)
				*size = MMB_MAX_STR;
		}
		return 1;
	}
	mmb_ident(tn, sizeof(tn));
	mmb_type_suffix(tn);
	sid = mmb_struct_lookup(tn);
	if (sid < 0)
		return 0;
	*type = T_STRUCT;
	*size = sid;
	return 1;
}

static void finalize_type(mmb_sdef *d)
{
	if (has_numeric_mem(d))
		d->total = align8(d->total);
}

static int parse_one_member(mmb_sdef *d)
{
	char name[MMB_MAX_NAME];
	int type = 0, size = 0, dims = 0, dim[MMB_MAX_DIMS], i, count = 1;
	int off, esz;
	mmb_smem *m;
	if (d->nmem >= MMB_MAX_STRUCT_MEMBERS)
		mmb_error("?TOO MANY MEMBERS");
	mmb_ident(name, sizeof(name));
	mmb_type_suffix(name);
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
	if (!mmb_match("AS"))
		mmb_syntax();
	if (!parse_member_type(&type, &size))
		mmb_error("?UNKNOWN TYPE");
	for (i = 0; i < dims; i++)
	{
		int n = dim[i] - G.opt.base + 1;
		if (n <= 0)
			mmb_error("?INVALID DIMENSION");
		count *= n;
	}
	off = d->total;
	if (type == T_INT || type == T_NUM || type == T_STRUCT)
		off = align8(off);
	esz = 0;
	if (type == T_INT || type == T_NUM)
		esz = 8;
	else if (type == T_STR)
		esz = size + 1;
	else if (type == T_STRUCT)
		esz = G.sdef[size].total;
	m = &d->mem[d->nmem];
	memset(m, 0, sizeof(*m));
	strncpy(m->name, name, MMB_MAX_NAME - 1);
	mmb_upper(m->name);
	m->type = type;
	m->size = size;
	m->offset = off;
	m->dims = dims;
	m->count = count;
	for (i = 0; i < dims; i++)
		m->dim[i] = dim[i];
	d->total = off + esz * count;
	d->nmem++;
	return 1;
}

static int line_starts_kw(const char *body, const char *kw)
{
	const char *save = G.p;
	int r;
	G.p = body;
	mmb_skip_sp();
	r = mmb_match(kw);
	G.p = save;
	return r;
}

static int line_is_end_type(const char *body)
{
	const char *save = G.p;
	int r = 0;
	G.p = body;
	mmb_skip_sp();
	if (mmb_match("END") && mmb_match("TYPE"))
		r = 1;
	G.p = save;
	return r;
}

static void parse_type_block(int start_pc)
{
	const char *save = G.p;
	char tname[MMB_MAX_NAME];
	mmb_sdef *d;
	int i, slot;
	G.p = mmb_tok_line(start_pc);
	mmb_skip_sp();
	if (!mmb_match("TYPE"))
	{
		G.p = save;
		return;
	}
	mmb_ident(tname, sizeof(tname));
	mmb_type_suffix(tname);
	mmb_upper(tname);
	if (mmb_struct_lookup(tname) >= 0)
		mmb_error("?ALREADY DECLARED");
	if (G.nstruct >= MMB_MAX_STRUCT_TYPES)
		mmb_error("?OUT OF MEMORY");
	slot = G.nstruct++;
	d = &G.sdef[slot];
	memset(d, 0, sizeof(*d));
	strncpy(d->name, tname, MMB_MAX_NAME - 1);
	d->used = 1;
	for (i = start_pc + 1; i < G.nprog; i++)
	{
		const char *ln = mmb_tok_line(i);
		G.p = ln;
		mmb_skip_sp();
		if (*G.p == 0 || *G.p == '\'')
			continue;
		if (line_is_end_type(ln))
			break;
		if (line_starts_kw(ln, "TYPE"))
			mmb_error("?END TYPE");
		parse_one_member(d);
	}
	if (d->nmem <= 0)
		mmb_error("?SYNTAX ERROR");
	finalize_type(d);
	G.p = save;
}

void mmb_struct_prepare(void)
{
	int i;
	int save_base = G.opt.base;
	const char *save = G.p;
	mmb_struct_clear();
	for (i = 0; i < G.nprog; i++)
	{
		G.p = mmb_tok_line(i);
		mmb_skip_sp();
		if (mmb_match("OPTION") && mmb_match("BASE"))
			G.opt.base = (int)mmb_as_int(mmb_expr());
	}
	for (i = 0; i < G.nprog; i++)
	{
		if (line_starts_kw(mmb_tok_line(i), "TYPE"))
			parse_type_block(i);
	}
	G.opt.base = save_base;
	G.p = save;
}

static int find_end_type_pc(int start)
{
	int i;
	for (i = start + 1; i < G.nprog; i++)
		if (line_is_end_type(mmb_tok_line(i)))
			return i;
	return G.nprog;
}

void mmb_cmd_type(void)
{
	if (G.running)
		G.branch_pc = find_end_type_pc(G.run_pc) + 1;
	else
		mmb_error("?TYPE");
}

void mmb_cmd_end_type(void)
{
}

static int find_member(const mmb_sdef *d, const char *name)
{
	int i;
	for (i = 0; i < d->nmem; i++)
		if (mmb_keyword_eq(d->mem[i].name, name))
			return i;
	return -1;
}

static int member_index_off(const mmb_smem *m, int nidx, const int *idx)
{
	int off = 0, i, stride = 1, esz;
	if (nidx != m->dims)
	{
		if (nidx == 0 && m->dims > 0)
			mmb_error("?ARRAY");
		if (nidx > 0 && m->dims == 0)
			mmb_error("?NOT AN ARRAY");
		mmb_error("?SUBSCRIPT");
	}
	if (nidx == 0)
		return 0;
	esz = mem_elem_size(m);
	for (i = m->dims - 1; i >= 0; i--)
	{
		if (idx[i] < G.opt.base || idx[i] > m->dim[i])
			mmb_error("?INDEX OUT OF BOUNDS");
		off += (idx[i] - G.opt.base) * stride;
		stride *= (m->dim[i] - G.opt.base + 1);
	}
	return off * esz;
}

int mmb_struct_resolve(mmb_var *v, const char *path, int nidx, const int *idx)
{
	char buf[MMB_MAX_NAME], part[MMB_MAX_NAME];
	const char *p;
	int sid, nest = 0, n, mi, moff;
	const mmb_sdef *d;
	const mmb_smem *m;
	if (!v || v->type != T_STRUCT)
		return 0;
	sid = v->struct_idx;
	moff = 0;
	strncpy(buf, path, sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = 0;
	mmb_upper(buf);
	p = buf;
	G.acc_on = 0;
	if (!buf[0])
	{
		G.acc_on = 0;
		G.acc_sid = sid;
		G.acc_mtype = T_STRUCT;
		G.acc_moff = 0;
		G.acc_msize = G.sdef[sid].total;
		return 1;
	}
	while (*p)
	{
		n = 0;
		while (*p && *p != '.' && n < MMB_MAX_NAME - 1)
			part[n++] = *p++;
		part[n] = 0;
		if (*p == '.')
			p++;
		if (sid < 0 || sid >= G.nstruct)
			return 0;
		d = &G.sdef[sid];
		mi = find_member(d, part);
		if (mi < 0)
			return 0;
		m = &d->mem[mi];
		moff += m->offset;
		if (*p)
		{
			if (m->type != T_STRUCT)
				return 0;
			if (++nest > MMB_MAX_STRUCT_NEST)
				mmb_error("?NESTING");
			if (m->dims)
				moff += member_index_off(m, 0, 0);
			sid = m->size;
			continue;
		}
		if (m->dims)
			moff += member_index_off(m, nidx, idx);
		else if (nidx)
			mmb_error("?NOT AN ARRAY");
		G.acc_on = 1;
		G.acc_sid = v->struct_idx;
		G.acc_mtype = m->type;
		G.acc_moff = moff;
		G.acc_msize = m->type == T_STR ? m->size :
			      (m->type == T_STRUCT ? G.sdef[m->size].total : 8);
		if (m->type == T_STRUCT)
			G.acc_sid = m->size;
		return 1;
	}
	return 0;
}

static void pack_str(unsigned char *p, int max, const char *s)
{
	int n = 0;
	if (!s)
		s = "";
	while (s[n] && n < max)
		n++;
	p[0] = (unsigned char)n;
	if (n)
		memcpy(p + 1, s, (unsigned)n);
	if (n < max)
		memset(p + 1 + n, 0, (unsigned)(max - n));
}

static mmb_val unpack_str(const unsigned char *p, int max)
{
	int n = p[0];
	char tmp[MMB_MAX_STR + 1];
	if (n > max)
		n = max;
	if (n > MMB_MAX_STR)
		n = MMB_MAX_STR;
	if (n)
		memcpy(tmp, p + 1, (unsigned)n);
	tmp[n] = 0;
	return mmb_str_val(tmp);
}

void mmb_struct_store_member(mmb_var *v, int eoff, mmb_val val)
{
	unsigned char *base = mmb_struct_elem(v, eoff);
	if (!base)
		mmb_error("?TYPE MISMATCH");
	base += G.acc_moff;
	if (G.acc_mtype == T_INT)
	{
		int64_t x = mmb_as_int(val);
		memcpy(base, &x, 8);
	}
	else if (G.acc_mtype == T_NUM)
	{
		double x = mmb_as_float(val);
		memcpy(base, &x, 8);
	}
	else if (G.acc_mtype == T_STR)
	{
		if (val.type != T_STR)
			mmb_error("?TYPE MISMATCH");
		pack_str(base, G.acc_msize, val.s);
	}
	else if (G.acc_mtype == T_STRUCT)
	{
		if (val.type != T_STRUCT || val.struct_idx != G.acc_sid)
			mmb_error("?TYPE MISMATCH");
		if (val.blob)
			memcpy(base, val.blob, (unsigned)G.acc_msize);
	}
}

mmb_val mmb_struct_load_member(mmb_var *v, int eoff)
{
	unsigned char *base = mmb_struct_elem(v, eoff);
	mmb_val out;
	memset(&out, 0, sizeof(out));
	if (!base)
		mmb_error("?TYPE MISMATCH");
	base += G.acc_moff;
	if (G.acc_mtype == T_INT)
	{
		int64_t x = 0;
		memcpy(&x, base, 8);
		return mmb_int_val(x);
	}
	if (G.acc_mtype == T_NUM)
	{
		double x = 0;
		memcpy(&x, base, 8);
		return mmb_num_val(x);
	}
	if (G.acc_mtype == T_STR)
		return unpack_str(base, G.acc_msize);
	out.type = T_STRUCT;
	out.struct_idx = G.acc_sid;
	out.blob = base;
	return out;
}

static mmb_var *parse_struct_var(int *eoff, int want_member)
{
	char name[MMB_MAX_NAME];
	int nidx, idx[MMB_MAX_DIMS], t;
	mmb_var *v;
	G.acc_on = 0;
	t = mmb_parse_var_ref(name, &nidx, idx);
	v = mmb_find_var(name, t, 0, nidx, idx);
	if (!v || v->type != T_STRUCT)
		mmb_error("?TYPE MISMATCH");
	*eoff = 0;
	if (nidx)
		*eoff = mmb_var_offset(v, nidx, idx);
	if (want_member && !G.acc_on)
		mmb_error("?SYNTAX ERROR");
	if (!want_member && G.acc_on)
		mmb_error("?TYPE MISMATCH");
	return v;
}

static void copy_struct(mmb_var *dst, int doff, mmb_var *src, int soff)
{
	int sz;
	if (dst->struct_idx != src->struct_idx)
		mmb_error("?TYPE MISMATCH");
	sz = G.sdef[dst->struct_idx].total;
	memcpy(mmb_struct_elem(dst, doff), mmb_struct_elem(src, soff), (unsigned)sz);
}

static int at_name(void)
{
	unsigned char c = (unsigned char)*G.p;
	return c == 0x80 || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

static int parse_empty_member(char *aname, int asz, char *mname, int msz)
{
	const char *save = G.p;
	mmb_skip_sp();
	if (!at_name())
	{
		G.p = save;
		return 0;
	}
	mmb_ident(aname, asz);
	mmb_type_suffix(aname);
	mmb_skip_sp();
	if (*G.p != '(')
	{
		G.p = save;
		return 0;
	}
	G.p++;
	mmb_skip_sp();
	if (*G.p != ')')
	{
		G.p = save;
		return 0;
	}
	G.p++;
	mmb_skip_sp();
	if (*G.p != '.')
	{
		G.p = save;
		return 0;
	}
	G.p++;
	mmb_ident(mname, msz);
	mmb_type_suffix(mname);
	return 1;
}

static mmb_var *find_struct_array(const char *name)
{
	int i;
	for (i = 0; i < MMB_MAX_VARS; i++)
		if (G.vars[i].used && G.vars[i].type == T_STRUCT &&
		    G.vars[i].dims > 0 && mmb_keyword_eq(G.vars[i].name, name))
			return &G.vars[i];
	return 0;
}

static int member_meta(mmb_var *v, const char *mname, mmb_smem **mout)
{
	int mi = find_member(&G.sdef[v->struct_idx], mname);
	if (mi < 0)
		mmb_error("?UNKNOWN");
	*mout = &G.sdef[v->struct_idx].mem[mi];
	return mi;
}

static void cmd_copy(void)
{
	char n1[MMB_MAX_NAME], n2[MMB_MAX_NAME];
	int a1 = 0, a2 = 0;
	mmb_var *s, *d;
	int i, n;
	if (parse_empty_member(n1, sizeof(n1), n2, sizeof(n2)))
		mmb_syntax();
	mmb_ident(n1, sizeof(n1));
	mmb_type_suffix(n1);
	mmb_skip_sp();
	if (*G.p == '(')
	{
		G.p++;
		mmb_skip_sp();
		if (*G.p == ')')
		{
			a1 = 1;
			G.p++;
		}
		else
			mmb_syntax();
	}
	if (!mmb_match("TO"))
		mmb_syntax();
	mmb_ident(n2, sizeof(n2));
	mmb_type_suffix(n2);
	mmb_skip_sp();
	if (*G.p == '(')
	{
		G.p++;
		mmb_skip_sp();
		if (*G.p == ')')
		{
			a2 = 1;
			G.p++;
		}
		else
			mmb_syntax();
	}
	s = mmb_find_var(n1, T_STRUCT, 0, a1 ? 1 : 0, 0);
	d = mmb_find_var(n2, T_STRUCT, 0, a2 ? 1 : 0, 0);
	if (!s)
		s = mmb_find_var(n1, 0, 0, 0, 0);
	if (!d)
		d = mmb_find_var(n2, 0, 0, 0, 0);
	if (!s || !d || s->type != T_STRUCT || d->type != T_STRUCT)
		mmb_error("?TYPE MISMATCH");
	if (a1 != a2)
		mmb_error("?SYNTAX ERROR");
	if (a1)
	{
		n = s->size < d->size ? s->size : d->size;
		for (i = 0; i < n; i++)
			copy_struct(d, i, s, i);
	}
	else
		copy_struct(d, 0, s, 0);
}

static int cmp_blob(const unsigned char *a, const unsigned char *b, mmb_smem *m, int flags)
{
	int r = 0;
	if (m->type == T_INT)
	{
		int64_t xa = 0, xb = 0;
		memcpy(&xa, a, 8);
		memcpy(&xb, b, 8);
		r = xa < xb ? -1 : xa > xb ? 1 : 0;
	}
	else if (m->type == T_NUM)
	{
		double xa = 0, xb = 0;
		memcpy(&xa, a, 8);
		memcpy(&xb, b, 8);
		r = xa < xb ? -1 : xa > xb ? 1 : 0;
	}
	else if (m->type == T_STR)
	{
		int na = a[0], nb = b[0], n, i;
		const char *sa = (const char *)(a + 1);
		const char *sb = (const char *)(b + 1);
		if ((flags & 4) && na == 0 && nb != 0)
			r = 1;
		else if ((flags & 4) && nb == 0 && na != 0)
			r = -1;
		else
		{
			n = na < nb ? na : nb;
			for (i = 0; i < n; i++)
			{
				unsigned char ca = (unsigned char)sa[i];
				unsigned char cb = (unsigned char)sb[i];
				if (flags & 2)
				{
					if (ca >= 'a' && ca <= 'z')
						ca = (unsigned char)(ca - 32);
					if (cb >= 'a' && cb <= 'z')
						cb = (unsigned char)(cb - 32);
				}
				if (ca != cb)
				{
					r = ca < cb ? -1 : 1;
					break;
				}
			}
			if (r == 0)
				r = na < nb ? -1 : na > nb ? 1 : 0;
		}
	}
	if (flags & 1)
		r = -r;
	return r;
}

static void cmd_sort(void)
{
	char aname[MMB_MAX_NAME], mname[MMB_MAX_NAME];
	mmb_var *v;
	mmb_smem *m;
	int flags = 0, i, j, sz;
	unsigned char tmp[MMB_STRUCT_RET_MAX];
	if (!parse_empty_member(aname, sizeof(aname), mname, sizeof(mname)))
		mmb_syntax();
	mmb_skip_sp();
	if (*G.p == ',')
	{
		G.p++;
		flags = (int)mmb_as_int(mmb_expr());
	}
	v = find_struct_array(aname);
	if (!v)
		mmb_error("?ARRAY");
	member_meta(v, mname, &m);
	sz = G.sdef[v->struct_idx].total;
	if (sz > MMB_STRUCT_RET_MAX)
		mmb_error("?OVERFLOW");
	for (i = 1; i < v->size; i++)
	{
		memcpy(tmp, mmb_struct_elem(v, i), (unsigned)sz);
		j = i;
		while (j > 0 &&
		       cmp_blob(mmb_struct_elem(v, j - 1) + m->offset, tmp + m->offset, m, flags) > 0)
		{
			memcpy(mmb_struct_elem(v, j), mmb_struct_elem(v, j - 1), (unsigned)sz);
			j--;
		}
		memcpy(mmb_struct_elem(v, j), tmp, (unsigned)sz);
	}
}

static void cmd_extract(void)
{
	char aname[MMB_MAX_NAME], mname[MMB_MAX_NAME], dname[MMB_MAX_NAME];
	mmb_var *v, *d;
	mmb_smem *m;
	int i, n;
	if (!parse_empty_member(aname, sizeof(aname), mname, sizeof(mname)))
		mmb_syntax();
	mmb_skip_sp();
	mmb_expect(',');
	mmb_ident(dname, sizeof(dname));
	mmb_type_suffix(dname);
	mmb_skip_sp();
	if (*G.p == '(')
	{
		G.p++;
		mmb_expect(')');
	}
	v = find_struct_array(aname);
	if (!v)
		mmb_error("?ARRAY");
	member_meta(v, mname, &m);
	d = mmb_find_var(dname, m->type == T_STR ? T_STR : (m->type == T_INT ? T_INT : T_NUM), 0, 1, 0);
	if (!d || d->dims == 0)
		mmb_error("?ARRAY");
	n = v->size < d->size ? v->size : d->size;
	for (i = 0; i < n; i++)
	{
		unsigned char *p = mmb_struct_elem(v, i) + m->offset;
		if (m->type == T_INT)
		{
			int64_t x = 0;
			memcpy(&x, p, 8);
			d->data.i[i] = x;
		}
		else if (m->type == T_NUM)
		{
			double x = 0;
			memcpy(&x, p, 8);
			d->data.f[i] = x;
		}
		else if (m->type == T_STR)
		{
			mmb_val s = unpack_str(p, m->size);
			strncpy(d->data.s[i], s.s, MMB_MAX_STR);
		}
	}
}

static void cmd_insert(void)
{
	char aname[MMB_MAX_NAME], mname[MMB_MAX_NAME], sname[MMB_MAX_NAME];
	mmb_var *v, *s;
	mmb_smem *m;
	int i, n;
	mmb_ident(sname, sizeof(sname));
	mmb_type_suffix(sname);
	mmb_skip_sp();
	if (*G.p == '(')
	{
		G.p++;
		mmb_expect(')');
	}
	mmb_skip_sp();
	mmb_expect(',');
	if (!parse_empty_member(aname, sizeof(aname), mname, sizeof(mname)))
		mmb_syntax();
	v = find_struct_array(aname);
	if (!v)
		mmb_error("?ARRAY");
	member_meta(v, mname, &m);
	s = mmb_find_var(sname, m->type == T_STR ? T_STR : (m->type == T_INT ? T_INT : T_NUM), 0, 1, 0);
	if (!s || s->dims == 0)
		mmb_error("?ARRAY");
	n = v->size < s->size ? v->size : s->size;
	for (i = 0; i < n; i++)
	{
		unsigned char *p = mmb_struct_elem(v, i) + m->offset;
		if (m->type == T_INT)
		{
			int64_t x = s->type == T_INT ? s->data.i[i] : (int64_t)s->data.f[i];
			memcpy(p, &x, 8);
		}
		else if (m->type == T_NUM)
		{
			double x = s->type == T_INT ? (double)s->data.i[i] : s->data.f[i];
			memcpy(p, &x, 8);
		}
		else if (m->type == T_STR)
			pack_str(p, m->size, s->data.s[i]);
	}
}

static void print_one(mmb_var *v, int eoff)
{
	const mmb_sdef *d = &G.sdef[v->struct_idx];
	int i;
	mmb_out(d->name);
	mmb_out(" ");
	for (i = 0; i < d->nmem; i++)
	{
		const mmb_smem *m = &d->mem[i];
		unsigned char *p = mmb_struct_elem(v, eoff) + m->offset;
		if (i)
			mmb_out(" ");
		mmb_out(m->name);
		mmb_out("=");
		if (m->type == T_INT)
		{
			int64_t x = 0;
			memcpy(&x, p, 8);
			mmb_print_val(mmb_int_val(x));
		}
		else if (m->type == T_NUM)
		{
			double x = 0;
			memcpy(&x, p, 8);
			mmb_print_val(mmb_num_val(x));
		}
		else if (m->type == T_STR)
			mmb_print_val(unpack_str(p, m->size));
		else
			mmb_out("[STRUCT]");
	}
}

static void cmd_print(void)
{
	char name[MMB_MAX_NAME];
	int whole = 0, i;
	mmb_var *v;
	mmb_ident(name, sizeof(name));
	mmb_type_suffix(name);
	mmb_skip_sp();
	if (*G.p == '(')
	{
		G.p++;
		mmb_skip_sp();
		if (*G.p == ')')
		{
			whole = 1;
			G.p++;
		}
		else
			mmb_syntax();
	}
	v = mmb_find_var(name, T_STRUCT, 0, whole ? 1 : 0, 0);
	if (!v)
		v = mmb_find_var(name, 0, 0, 0, 0);
	if (!v || v->type != T_STRUCT)
		mmb_error("?TYPE MISMATCH");
	if (whole)
	{
		for (i = 0; i < v->size; i++)
		{
			if (i)
				mmb_out("\n");
			print_one(v, i);
		}
	}
	else
		print_one(v, 0);
}

static void cmd_clear(void)
{
	char name[MMB_MAX_NAME];
	int whole = 0, i;
	mmb_var *v;
	int sz;
	mmb_ident(name, sizeof(name));
	mmb_type_suffix(name);
	mmb_skip_sp();
	if (*G.p == '(')
	{
		G.p++;
		mmb_skip_sp();
		if (*G.p == ')')
		{
			whole = 1;
			G.p++;
		}
		else
			mmb_syntax();
	}
	v = mmb_find_var(name, T_STRUCT, 0, whole ? 1 : 0, 0);
	if (!v)
		v = mmb_find_var(name, 0, 0, 0, 0);
	if (!v || v->type != T_STRUCT)
		mmb_error("?TYPE MISMATCH");
	sz = G.sdef[v->struct_idx].total;
	if (whole)
	{
		for (i = 0; i < v->size; i++)
			memset(mmb_struct_elem(v, i), 0, (unsigned)sz);
	}
	else
		memset(mmb_struct_elem(v, 0), 0, (unsigned)sz);
}

static void cmd_swap(void)
{
	int e1, e2, sz;
	mmb_var *a, *b;
	unsigned char tmp[MMB_STRUCT_RET_MAX];
	a = parse_struct_var(&e1, 0);
	mmb_skip_sp();
	mmb_expect(',');
	b = parse_struct_var(&e2, 0);
	if (a->struct_idx != b->struct_idx)
		mmb_error("?TYPE MISMATCH");
	sz = G.sdef[a->struct_idx].total;
	if (sz > MMB_STRUCT_RET_MAX)
		mmb_error("?OVERFLOW");
	memcpy(tmp, mmb_struct_elem(a, e1), (unsigned)sz);
	memcpy(mmb_struct_elem(a, e1), mmb_struct_elem(b, e2), (unsigned)sz);
	memcpy(mmb_struct_elem(b, e2), tmp, (unsigned)sz);
}

static int file_write_at(int fn, const void *data, unsigned n)
{
	int sz;
	unsigned char *buf;
	unsigned got = 0;
	if (fn < 1 || fn > MMB_MAX_FILES || !G.files[fn].open)
		mmb_error("?FILE");
	if (G.files[fn].mode == 0)
		mmb_error("?FILE");
	sz = mmb_vfs_size(G.files[fn].path);
	if (sz < 0)
		sz = 0;
	if (G.files[fn].pos >= sz)
	{
		if (mmb_vfs_write(G.files[fn].path, data, n, 1) != 0)
			mmb_error("?FILE");
		G.files[fn].pos += (int)n;
		return 0;
	}
	buf = G.plat->alloc((unsigned)sz + n + 16);
	if (!buf)
		mmb_error("?OUT OF MEMORY");
	if (sz && mmb_vfs_read_at(G.files[fn].path, 0, buf, (unsigned)sz, &got) != 0)
	{
		G.plat->free(buf);
		mmb_error("?FILE");
	}
	if (G.files[fn].pos + (int)n > sz)
	{
		memset(buf + sz, 0, (unsigned)(G.files[fn].pos + (int)n - sz));
		sz = G.files[fn].pos + (int)n;
	}
	memcpy(buf + G.files[fn].pos, data, n);
	if (G.files[fn].pos + (int)n > sz)
		sz = G.files[fn].pos + (int)n;
	if (mmb_vfs_write(G.files[fn].path, buf, (unsigned)sz, 0) != 0)
	{
		G.plat->free(buf);
		mmb_error("?FILE");
	}
	G.plat->free(buf);
	G.files[fn].pos += (int)n;
	return 0;
}

static void cmd_save_load(int save)
{
	int fn, whole = 0, one = 0, idxv = 0, i, n, sz;
	mmb_var *v;
	char name[MMB_MAX_NAME];
	mmb_skip_sp();
	if (*G.p == '#')
		G.p++;
	fn = (int)mmb_as_int(mmb_expr());
	mmb_skip_sp();
	mmb_expect(',');
	if (fn < 1 || fn > MMB_MAX_FILES || !G.files[fn].open)
		mmb_error("?FILE");
	mmb_ident(name, sizeof(name));
	mmb_type_suffix(name);
	mmb_skip_sp();
	if (*G.p == '(')
	{
		G.p++;
		mmb_skip_sp();
		if (*G.p == ')')
		{
			whole = 1;
			G.p++;
		}
		else
		{
			idxv = (int)mmb_as_int(mmb_expr());
			mmb_expect(')');
			one = 1;
		}
	}
	v = mmb_find_var(name, T_STRUCT, 0, (whole || one || 0), 0);
	if (!v)
	{
		int dummy[MMB_MAX_DIMS];
		dummy[0] = idxv;
		v = mmb_find_var(name, 0, 0, one ? 1 : 0, dummy);
	}
	if (!v || v->type != T_STRUCT)
		mmb_error("?TYPE MISMATCH");
	sz = G.sdef[v->struct_idx].total;
	if (whole)
	{
		n = v->size;
		for (i = 0; i < n; i++)
		{
			if (save)
				file_write_at(fn, mmb_struct_elem(v, i), (unsigned)sz);
			else
			{
				unsigned got = 0;
				if (mmb_vfs_read_at(G.files[fn].path, (unsigned)G.files[fn].pos,
						    mmb_struct_elem(v, i), (unsigned)sz, &got) != 0 ||
				    (int)got != sz)
					mmb_error("?FILE");
				G.files[fn].pos += sz;
			}
		}
		return;
	}
	i = one ? (idxv - G.opt.base) : 0;
	if (i < 0 || i >= v->size)
		mmb_error("?INDEX OUT OF BOUNDS");
	if (save)
		file_write_at(fn, mmb_struct_elem(v, i), (unsigned)sz);
	else
	{
		unsigned got = 0;
		if (mmb_vfs_read_at(G.files[fn].path, (unsigned)G.files[fn].pos,
				    mmb_struct_elem(v, i), (unsigned)sz, &got) != 0 ||
		    (int)got != sz)
			mmb_error("?FILE");
		G.files[fn].pos += sz;
	}
}

void mmb_cmd_struct(void)
{
	if (mmb_match("COPY"))
		cmd_copy();
	else if (mmb_match("SORT"))
		cmd_sort();
	else if (mmb_match("EXTRACT"))
		cmd_extract();
	else if (mmb_match("INSERT"))
		cmd_insert();
	else if (mmb_match("CLEAR"))
		cmd_clear();
	else if (mmb_match("SWAP"))
		cmd_swap();
	else if (mmb_match("PRINT"))
		cmd_print();
	else if (mmb_match("SAVE"))
		cmd_save_load(1);
	else if (mmb_match("LOAD"))
		cmd_save_load(0);
	else
		mmb_syntax();
}

void mmb_cmd_list_type(void)
{
	char name[MMB_MAX_NAME];
	int i, j, only = 0;
	mmb_skip_sp();
	if (*G.p && *G.p != ':' && *G.p != '\'')
	{
		mmb_ident(name, sizeof(name));
		mmb_type_suffix(name);
		only = 1;
	}
	for (i = 0; i < G.nstruct; i++)
	{
		mmb_sdef *d = &G.sdef[i];
		if (only && !mmb_keyword_eq(d->name, name))
			continue;
		if (i && !only)
			mmb_out("\n");
		mmb_out("TYPE ");
		mmb_out(d->name);
		mmb_out("\n");
		for (j = 0; j < d->nmem; j++)
		{
			mmb_smem *m = &d->mem[j];
			mmb_out("  ");
			mmb_out(m->name);
			if (m->dims)
			{
				int k;
				mmb_out("(");
				for (k = 0; k < m->dims; k++)
				{
					if (k)
						mmb_out(",");
					mmb_outf(0, m->dim[k]);
				}
				mmb_out(")");
			}
			mmb_out(" AS ");
			if (m->type == T_INT)
				mmb_out("INTEGER");
			else if (m->type == T_NUM)
				mmb_out("FLOAT");
			else if (m->type == T_STR)
			{
				mmb_out("STRING");
				if (m->size != MMB_MAX_STR)
				{
					mmb_out(" LENGTH ");
					mmb_outf(0, m->size);
				}
			}
			else if (m->type == T_STRUCT)
				mmb_out(G.sdef[m->size].name);
			mmb_out("\n");
		}
		mmb_out("END TYPE");
		if (only)
			break;
	}
}

static int fun_sizeof(mmb_val *out)
{
	char n[MMB_MAX_NAME];
	int sid;
	mmb_val a;
	a = mmb_expr();
	if (a.type != T_STR)
		mmb_syntax();
	strncpy(n, a.s, sizeof(n) - 1);
	n[sizeof(n) - 1] = 0;
	sid = mmb_struct_lookup(n);
	if (sid < 0)
		mmb_error("?UNKNOWN TYPE");
	*out = mmb_int_val(G.sdef[sid].total);
	return 1;
}

static int fun_offset(mmb_val *out)
{
	char tn[MMB_MAX_NAME], en[MMB_MAX_NAME];
	int sid, mi;
	mmb_val a, b;
	a = mmb_expr();
	mmb_skip_sp();
	mmb_expect(',');
	b = mmb_expr();
	if (a.type != T_STR || b.type != T_STR)
		mmb_syntax();
	strncpy(tn, a.s, sizeof(tn) - 1);
	strncpy(en, b.s, sizeof(en) - 1);
	sid = mmb_struct_lookup(tn);
	if (sid < 0)
		mmb_error("?UNKNOWN TYPE");
	mi = find_member(&G.sdef[sid], en);
	if (mi < 0)
		mmb_error("?UNKNOWN");
	*out = mmb_int_val(G.sdef[sid].mem[mi].offset);
	return 1;
}

static int fun_type(mmb_val *out)
{
	char tn[MMB_MAX_NAME], en[MMB_MAX_NAME];
	int sid, mi, t;
	mmb_val a, b;
	a = mmb_expr();
	mmb_skip_sp();
	mmb_expect(',');
	b = mmb_expr();
	if (a.type != T_STR || b.type != T_STR)
		mmb_syntax();
	strncpy(tn, a.s, sizeof(tn) - 1);
	strncpy(en, b.s, sizeof(en) - 1);
	sid = mmb_struct_lookup(tn);
	if (sid < 0)
		mmb_error("?UNKNOWN TYPE");
	mi = find_member(&G.sdef[sid], en);
	if (mi < 0)
		mmb_error("?UNKNOWN");
	t = G.sdef[sid].mem[mi].type;
	if (t == T_NUM)
		*out = mmb_int_val(1);
	else if (t == T_STR)
		*out = mmb_int_val(2);
	else if (t == T_INT)
		*out = mmb_int_val(4);
	else
		*out = mmb_int_val(0);
	return 1;
}

static int fun_find(mmb_val *out)
{
	char aname[MMB_MAX_NAME], mname[MMB_MAX_NAME];
	mmb_var *v;
	mmb_smem *m;
	mmb_val want;
	int start = G.opt.base, i;
	if (!parse_empty_member(aname, sizeof(aname), mname, sizeof(mname)))
		mmb_syntax();
	mmb_skip_sp();
	mmb_expect(',');
	want = mmb_expr();
	mmb_skip_sp();
	if (*G.p == ',')
	{
		G.p++;
		mmb_skip_sp();
		if (*G.p && *G.p != ')')
			start = (int)mmb_as_int(mmb_expr());
	}
	v = find_struct_array(aname);
	if (!v)
		mmb_error("?ARRAY");
	member_meta(v, mname, &m);
	for (i = start - G.opt.base; i < v->size; i++)
	{
		unsigned char *p = mmb_struct_elem(v, i) + m->offset;
		int hit = 0;
		if (m->type == T_INT)
		{
			int64_t x = 0;
			memcpy(&x, p, 8);
			hit = x == mmb_as_int(want);
		}
		else if (m->type == T_NUM)
		{
			double x = 0;
			memcpy(&x, p, 8);
			hit = x == mmb_as_float(want);
		}
		else if (m->type == T_STR && want.type == T_STR)
		{
			mmb_val s = unpack_str(p, m->size);
			hit = mmb_keyword_eq(s.s, want.s) ||
			      (s.s && want.s && strcmp(s.s, want.s) == 0);
		}
		if (hit)
		{
			*out = mmb_int_val(i + G.opt.base);
			return 1;
		}
	}
	*out = mmb_int_val(-1);
	return 1;
}

int mmb_try_struct_fun(mmb_val *out)
{
	if (!mmb_match("STRUCT"))
		return 0;
	mmb_skip_sp();
	mmb_expect('(');
	mmb_skip_sp();
	if (mmb_match("SIZEOF"))
		fun_sizeof(out);
	else if (mmb_match("OFFSET"))
		fun_offset(out);
	else if (mmb_match("TYPE"))
		fun_type(out);
	else if (mmb_match("FIND"))
		fun_find(out);
	else
		mmb_syntax();
	mmb_skip_sp();
	mmb_expect(')');
	return 1;
}
