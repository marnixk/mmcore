#include "mmb_priv.h"
#include "mmb_version.h"

#ifndef MMB_VERSION
#define MMB_VERSION "dev"
#endif

mmb *g_mmb[MMB_MAX_CONSOLES];
mmb *g_cur;

void mmb_skip_sp(void)
{
	while (*G.p == ' ' || *G.p == '\t' || *G.p == '\r')
		G.p++;
}

int mmb_normalize_newlines(char *buf, int len)
{
	int i, o = 0;

	if (!buf)
		return 0;
	if (len < 0)
	{
		len = 0;
		while (buf[len])
			len++;
	}
	for (i = 0; i < len; i++)
	{
		if (buf[i] == '\r')
			continue;
		buf[o++] = buf[i];
	}
	buf[o] = 0;
	return o;
}

int mmb_is_digit(char c)
{
	return c >= '0' && c <= '9';
}

int mmb_is_ident(char c)
{
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
	       (c >= '0' && c <= '9') || c == '_' || c == '.';
}

void mmb_upper(char *s)
{
	for (; *s; s++)
		if (*s >= 'a' && *s <= 'z')
			*s = (char)(*s - 32);
}

int mmb_keyword_eq(const char *a, const char *b)
{
	while (*a && *b)
	{
		char ca = *a, cb = *b;
		if (ca >= 'a' && ca <= 'z') ca = (char)(ca - 32);
		if (cb >= 'a' && cb <= 'z') cb = (char)(cb - 32);
		if (ca != cb)
			return 0;
		a++;
		b++;
	}
	return *a == 0 && *b == 0;
}

int mmb_match(const char *kw)
{
	const char *save = G.p;
	if (G.opt.profiling && G.running)
		G.prof.match++;
	mmb_skip_sp();
	if ((unsigned char)*G.p == 0x80)
	{
		if (mmb_match_token(kw))
			return 1;
		G.p = save;
		return 0;
	}
	const char *p = G.p;
	const char *k = kw;
	while (*k)
	{
		char a = *p, b = *k;
		if (a >= 'a' && a <= 'z') a = (char)(a - 32);
		if (b >= 'a' && b <= 'z') b = (char)(b - 32);
		if (a != b)
		{
			G.p = save;
			return 0;
		}
		p++;
		k++;
	}
	if (mmb_is_ident(*p))
	{
		G.p = save;
		return 0;
	}
	G.p = p;
	return 1;
}

void mmb_expect(char c)
{
	mmb_skip_sp();
	if (*G.p != c)
		mmb_syntax();
	G.p++;
}

void mmb_ident(char *dst, int dstsz)
{
	int n = 0;
	mmb_skip_sp();
	if (mmb_tok_expand(dst, dstsz))
		return;
	if (!((*G.p >= 'A' && *G.p <= 'Z') || (*G.p >= 'a' && *G.p <= 'z') || *G.p == '_'))
		mmb_syntax();
	while (mmb_is_ident(*G.p) && n < dstsz - 2)
	{
		char c = *G.p++;
		if (c >= 'a' && c <= 'z')
			c = (char)(c - 32);
		dst[n++] = c;
	}
	if (*G.p == '$' || *G.p == '%' || *G.p == '!')
		dst[n++] = *G.p++;
	dst[n] = 0;
}

int mmb_type_suffix(char *name)
{
	int n = (int)strlen(name);
	if (n <= 0)
		return 0;
	char s = name[n - 1];
	if (s == '$')
	{
		name[n - 1] = 0;
		return T_STR;
	}
	if (s == '%')
	{
		name[n - 1] = 0;
		return T_INT;
	}
	if (s == '!')
	{
		name[n - 1] = 0;
		return T_NUM;
	}
	return 0;
}

void mmb_error(const char *msg)
{
	unsigned i;
	for (i = 0; i < sizeof(G.err) - 1 && msg[i]; i++)
		G.err[i] = msg[i];
	if (G.running && G.run_pc >= 0 && G.run_pc < G.nprog)
	{
		const char *p = G.prog[G.run_pc];
		int64_t num = G.prog_num[G.run_pc];
		char nbuf[16];
		int k = 0;
		if (i < sizeof(G.err) - 4)
		{
			G.err[i++] = ' ';
			G.err[i++] = '@';
		}
		if (num <= 0)
			nbuf[k++] = '0';
		else
		{
			char tmp[16];
			int t = 0;
			while (num && t < 15)
			{
				tmp[t++] = (char)('0' + (int)(num % 10));
				num /= 10;
			}
			while (t)
				nbuf[k++] = tmp[--t];
		}
		nbuf[k] = 0;
		for (k = 0; nbuf[k] && i < sizeof(G.err) - 2; k++)
			G.err[i++] = nbuf[k];
		if (i < sizeof(G.err) - 2)
			G.err[i++] = ':';
		while (*p && i < sizeof(G.err) - 1)
			G.err[i++] = *p++;
	}
	G.err[i] = 0;
	if (G.running && !G.error_active && G.on_error_pc >= 0)
	{
		G.error_active = 1;
		G.err_resume_pc = G.run_pc;
		longjmp(G.run_errjmp, 1);
	}
	if (G.running)
		mmb_play_stop();
	longjmp(G.errjmp, 1);
}

void mmb_syntax(void)
{
	mmb_error("?SYNTAX ERROR");
}

void mmb_out(const char *s)
{
	while (*s && G.outn < MMB_OUT_LEN - 1)
		G.out[G.outn++] = *s++;
	G.out[G.outn] = 0;
}

void mmb_out_flush(void)
{
	if (!G.outn)
		return;
	mmb_console_write(G.out);
	G.outn = 0;
	G.out[0] = 0;
}

#define MMB_PRINT_FW 8
#define MMB_PRINT_FH 16

int mmb_print_font_w(void)
{
	return MMB_PRINT_FW;
}

int mmb_print_font_h(void)
{
	return MMB_PRINT_FH;
}

static void mmb_print_put_int(char *buf, int *n, int v)
{
	char tmp[8];
	int i = 0;
	if (v <= 0)
	{
		buf[(*n)++] = '0';
		return;
	}
	while (v > 0 && i < 8)
	{
		tmp[i++] = (char)('0' + (v % 10));
		v /= 10;
	}
	while (i > 0)
		buf[(*n)++] = tmp[--i];
}

static int mmb_print_cursor_seq(char *seq, int seqsz)
{
	int col, row, n = 0;
	if (!seq || seqsz < 12)
		return 0;
	col = G.print_x / MMB_PRINT_FW + 1;
	row = G.print_y / MMB_PRINT_FH + 1;
	seq[n++] = '\x1b';
	seq[n++] = '[';
	mmb_print_put_int(seq, &n, row);
	seq[n++] = ';';
	mmb_print_put_int(seq, &n, col);
	seq[n++] = 'H';
	return n;
}

static void mmb_print_emit_cursor(void)
{
	char seq[24];
	int n;

	n = mmb_print_cursor_seq(seq, sizeof(seq));
	if (n <= 0)
		return;
	/* HDMI only — serial tests expect no cursor ANSI (see CLS tests). */
	if (G.plat && G.plat->write_screen)
		G.plat->write_screen(seq, (unsigned)n);
}

void mmb_print_cursor_goto(int px, int py)
{
	if (px < 0)
		px = 0;
	if (py < 0)
		py = 0;
	G.print_x = px;
	G.print_y = py;
}

void mmb_print_locate_pending(void)
{
	if (!G.print_locate)
		return;
	mmb_print_emit_cursor();
	G.print_locate = 0;
}

void mmb_print_track(const char *s, unsigned n)
{
	unsigned i;
	int fw = MMB_PRINT_FW;
	int fh = MMB_PRINT_FH;

	if (!s)
		return;
	for (i = 0; i < n; i++)
	{
		char c = s[i];
		if (c == '\r')
			G.print_x = 0;
		else if (c == '\n')
			G.print_y += fh;
		else if (c == '\t')
		{
			int tab = G.opt.tab > 0 ? G.opt.tab : 8;
			G.print_x = (G.print_x / (fw * tab) + 1) * (fw * tab);
		}
		else
			G.print_x += fw;
	}
}

int mmb_print_try_at(void)
{
	mmb_val vx, vy;
	int mode = 0;

	mmb_skip_sp();
	if (*G.p != '@')
		return 0;
	G.p++;
	mmb_skip_sp();
	mmb_expect('(');
	vx = mmb_expr();
	mmb_skip_sp();
	mmb_expect(',');
	vy = mmb_expr();
	mmb_skip_sp();
	if (*G.p == ',')
	{
		G.p++;
		mode = (int)mmb_as_int(mmb_expr());
	}
	mmb_skip_sp();
	mmb_expect(')');
	mmb_print_cursor_goto((int)mmb_as_int(vx), (int)mmb_as_int(vy));
	G.print_locate = 1;
	(void)mode;
	return 1;
}

void mmb_outf(const char *unused, int64_t n)
{
	char buf[32];
	char *p = buf + sizeof(buf) - 1;
	int neg = 0;
	uint64_t v;
	(void)unused;
	*p = 0;
	if (n < 0)
	{
		neg = 1;
		v = (uint64_t)(-n);
	}
	else
		v = (uint64_t)n;
	if (v == 0)
		*--p = '0';
	while (v)
	{
		*--p = (char)('0' + (v % 10));
		v /= 10;
	}
	if (neg)
		*--p = '-';
	mmb_out(p);
}

void mmb_prof_reset(void)
{
	G.prof.t0_ms = mmb_now_ms();
	G.prof.stmt = 0;
	G.prof.match = 0;
	G.prof.expr = 0;
	G.prof.find_var = 0;
	G.prof.check_break = 0;
	G.prof.gfx_present = 0;
	G.prof.tcache_hit = 0;
	G.prof.tcache_comp = 0;
	G.prof.tcache_bad = 0;
}

void mmb_prof_report(void)
{
	unsigned elapsed;
	if (!G.opt.profiling)
		return;
	elapsed = mmb_now_ms() - G.prof.t0_ms;
	if (G.outn)
		mmb_out("\n");
	mmb_out("[PERF] elapsed=");
	mmb_outf(0, elapsed);
	mmb_out(" ms  statements=");
	mmb_outf(0, G.prof.stmt);
	mmb_out("  match=");
	mmb_outf(0, G.prof.match);
	mmb_out("  expr=");
	mmb_outf(0, G.prof.expr);
	mmb_out("  findvar=");
	mmb_outf(0, G.prof.find_var);
	mmb_out("  break=");
	mmb_outf(0, G.prof.check_break);
	mmb_out("  present=");
	mmb_outf(0, G.prof.gfx_present);
	mmb_out("  tcache=");
	mmb_outf(0, G.prof.tcache_hit);
	mmb_out("/");
	mmb_outf(0, G.prof.tcache_comp);
	mmb_out("/");
	mmb_outf(0, G.prof.tcache_bad);
}

mmb_val mmb_num_val(double f)
{
	mmb_val v;
	memset(&v, 0, sizeof(v));
	v.type = T_NUM;
	v.f = f;
	v.i = (int64_t)f;
	return v;
}

mmb_val mmb_int_val(int64_t i)
{
	mmb_val v;
	memset(&v, 0, sizeof(v));
	v.type = T_INT;
	v.i = i;
	v.f = (double)i;
	return v;
}

/* ------------------------------------------------------------------ *
 * Strings.
 *
 * Persistent string values live in single-owner blocks carved from a
 * size-class pool (no compaction, no GC: a block is freed exactly when
 * its slot is overwritten or cleared).  Expression temporaries live in
 * a chunked bump arena that is reclaimed wholesale at statement and run
 * boundaries.  Both are unbounded apart from available memory.
 * ------------------------------------------------------------------ */

#define MMB_STR_HDR     8
#define MMB_STR_MIN_CLS 4                  /* 16-byte smallest slab class  */
#define MMB_STR_MAX_CLS 16                 /* 64 KiB largest slab class    */
#define MMB_SLAB_SIZE   (64 * 1024)
#define MMB_ARENA_CHUNK (64 * 1024)

typedef struct mmb_freeblk { struct mmb_freeblk *next; } mmb_freeblk;
typedef struct mmb_slab { struct mmb_slab *next; } mmb_slab;
typedef struct mmb_achunk {
	struct mmb_achunk *next;
	unsigned used, size;
} mmb_achunk;

static mmb_freeblk *s_free[MMB_STR_MAX_CLS + 1];
static mmb_slab *s_slabs;
static char *s_slab;
static unsigned s_slab_left;

static mmb_achunk *s_arena;
static mmb_achunk *s_arena_cur;

static char s_empty[1] = { 0 };
#define MMB_EMPTY ((char *)s_empty)

static int log2_ceil(unsigned v)
{
	int c = 0;
	unsigned x = 1;
	while (x < v)
	{
		x <<= 1;
		c++;
	}
	return c;
}

static unsigned blk_total(const char *data)
{
	unsigned t;
	memcpy(&t, data - MMB_STR_HDR, sizeof(t));
	return t;
}

static unsigned blk_cap(const char *data)
{
	return blk_total(data) - MMB_STR_HDR - 1;
}

static char *pool_alloc_raw(int need)
{
	unsigned total;
	if (need < 1)
		need = 1;
	total = (unsigned)need + MMB_STR_HDR;
	if (total <= (1u << MMB_STR_MAX_CLS))
	{
		int cls = log2_ceil(total);
		unsigned bsz;
		char *raw;
		if (cls < MMB_STR_MIN_CLS)
			cls = MMB_STR_MIN_CLS;
		bsz = 1u << cls;
		if (s_free[cls])
		{
			mmb_freeblk *f = s_free[cls];
			s_free[cls] = f->next;
			raw = (char *)f;
		}
		else
		{
			if (s_slab_left < bsz)
			{
				char *ns = (char *)G.plat->alloc(MMB_SLAB_SIZE);
				if (!ns)
					return 0;
				((mmb_slab *)ns)->next = s_slabs;
				s_slabs = (mmb_slab *)ns;
				s_slab = ns + sizeof(mmb_slab);
				s_slab_left = MMB_SLAB_SIZE - (unsigned)sizeof(mmb_slab);
			}
			raw = s_slab;
			s_slab += bsz;
			s_slab_left -= bsz;
		}
		memcpy(raw, &bsz, sizeof(bsz));
		return raw + MMB_STR_HDR;
	}
	{
		char *raw = (char *)G.plat->alloc(total);
		if (!raw)
			return 0;
		memcpy(raw, &total, sizeof(total));
		return raw + MMB_STR_HDR;
	}
}

static void pool_free_raw(char *data)
{
	unsigned total;
	if (!data || data == MMB_EMPTY)
		return;
	total = blk_total(data);
	if (total <= (1u << MMB_STR_MAX_CLS))
	{
		int cls = log2_ceil(total);
		mmb_freeblk *f = (mmb_freeblk *)(data - MMB_STR_HDR);
		if (cls < MMB_STR_MIN_CLS)
			cls = MMB_STR_MIN_CLS;
		f->next = s_free[cls];
		s_free[cls] = f;
	}
	else
		G.plat->free(data - MMB_STR_HDR);
}

void mmb_strpool_reset(void)
{
	int i;
	for (i = 0; i <= MMB_STR_MAX_CLS; i++)
		s_free[i] = 0;
	while (s_slabs)
	{
		mmb_slab *n = s_slabs->next;
		G.plat->free(s_slabs);
		s_slabs = n;
	}
	s_slab = 0;
	s_slab_left = 0;
}

static void str_overflow(const char *what, int need, int max)
{
	char msg[160];
	sprintf(msg, "?OVERFLOW: %s needs %d, LENGTH %d",
		what && what[0] ? what : "string", need, max);
	mmb_error(msg);
}

char *mmb_str_alloc(int n)
{
	char *p;
	if (n < 0)
		n = 0;
	p = pool_alloc_raw(n + 1);
	if (!p)
		mmb_error("?OUT OF MEMORY");
	p[n] = 0;
	return p;
}

void mmb_str_free(char *p)
{
	pool_free_raw(p);
}

char *mmb_str_empty(void)
{
	return MMB_EMPTY;
}

char *mmb_read_line(int hide)
{
	char *line = 0;
	int rc;
	char *empty;
	if (!G.plat || !G.plat->read_line)
	{
		empty = mmb_tmp_alloc(1);
		empty[0] = 0;
		return empty;
	}
	rc = G.plat->read_line(&line, hide);
	if (rc == -2)
	{
		if (line)
			G.plat->free(line);
		mmb_error("?BREAK");
	}
	if (rc != 0 || !line)
	{
		if (line)
			G.plat->free(line);
		empty = mmb_tmp_alloc(1);
		empty[0] = 0;
		return empty;
	}
	{
		int n = (int)strlen(line);
		char *copy = mmb_tmp_alloc(n + 1);
		memcpy(copy, line, (size_t)n + 1);
		G.plat->free(line);
		return copy;
	}
}

/* Copy s[0..len) into *slot, growing (with over-allocation) as needed.
 * Enforces a positive maxlen cap with a diagnostic overflow error. */
char *mmb_str_set(char **slot, const char *s, int len, int maxlen, const char *what)
{
	char *cur;
	if (!slot)
		return 0;
	if (len < 0)
		len = s ? (int)strlen(s) : 0;
	if (maxlen > 0 && len > maxlen)
		str_overflow(what, len, maxlen);
	cur = *slot;
	if (cur == MMB_EMPTY)
		cur = 0;
	if (cur && (int)blk_cap(cur) >= len)
	{
		if (s && s != cur && len)
			memmove(cur, s, (size_t)len);
		cur[len] = 0;
		*slot = cur;
		return cur;
	}
	{
		char *nb = pool_alloc_raw(len + 1);
		if (!nb)
			mmb_error("?OUT OF MEMORY");
		if (s && len)
			memcpy(nb, s, (size_t)len);
		nb[len] = 0;
		pool_free_raw(cur);
		*slot = nb;
		return nb;
	}
}

/* Append s[0..len) to *slot, doubling capacity while it grows. */
char *mmb_str_append(char **slot, const char *s, int len, int maxlen, const char *what)
{
	char *cur;
	int clen;
	if (!slot)
		return 0;
	cur = *slot;
	if (cur == MMB_EMPTY)
		cur = 0;
	clen = cur ? (int)strlen(cur) : 0;
	if (len < 0)
		len = s ? (int)strlen(s) : 0;
	if (maxlen > 0 && clen + len > maxlen)
		str_overflow(what, clen + len, maxlen);
	if (!cur || (int)blk_cap(cur) < clen + len)
	{
		int need = clen + len + 1;
		int cap = cur ? (int)blk_cap(cur) : 0;
		char *nb;
		if (cap && cap * 2 > need)
			need = cap * 2;
		nb = pool_alloc_raw(need);
		if (!nb)
			mmb_error("?OUT OF MEMORY");
		if (clen)
			memcpy(nb, cur, (size_t)clen);
		nb[clen] = 0;
		if (cur)
			pool_free_raw(cur);
		cur = nb;
	}
	if (len)
		memcpy(cur + clen, s, (size_t)len);
	cur[clen + len] = 0;
	*slot = cur;
	return cur;
}

/* Temporary arena: bump-allocate, never free individually. */
char *mmb_tmp_alloc(int n)
{
	unsigned aligned;
	if (n < 1)
		n = 1;
	aligned = ((unsigned)n + 15u) & ~15u;
	for (;;)
	{
		mmb_achunk *c = s_arena_cur;
		if (!c || c->size - c->used < aligned)
		{
			if (c && c->next)
			{
				s_arena_cur = c->next;
				continue;
			}
			{
				unsigned csz = MMB_ARENA_CHUNK;
				mmb_achunk *nc;
				if (aligned + (unsigned)sizeof(mmb_achunk) > csz)
					csz = aligned + (unsigned)sizeof(mmb_achunk);
				nc = (mmb_achunk *)G.plat->alloc(csz);
				if (!nc)
					mmb_error("?OUT OF MEMORY");
				nc->next = 0;
				nc->used = 0;
				nc->size = csz - (unsigned)sizeof(mmb_achunk);
				if (!s_arena)
					s_arena = nc;
				else
				{
					mmb_achunk *t = s_arena;
					while (t->next)
						t = t->next;
					t->next = nc;
				}
				s_arena_cur = nc;
				continue;
			}
		}
		{
			char *p = (char *)c + sizeof(mmb_achunk) + c->used;
			c->used += aligned;
			return p;
		}
	}
}

void mmb_str_reset(void)
{
	mmb_achunk *c;
	for (c = s_arena; c; c = c->next)
		c->used = 0;
	s_arena_cur = s_arena;
}

mmb_val mmb_arena_val(char *p)
{
	mmb_val v;
	memset(&v, 0, sizeof(v));
	v.type = T_STR;
	v.s = p ? p : MMB_EMPTY;
	return v;
}

mmb_val mmb_str_valn(const char *s, int n)
{
	char *p;
	if (!s)
	{
		s = "";
		n = 0;
	}
	if (n < 0)
		n = (int)strlen(s);
	p = mmb_tmp_alloc(n + 1);
	if (n)
		memcpy(p, s, (size_t)n);
	p[n] = 0;
	return mmb_arena_val(p);
}

mmb_val mmb_str_val(const char *s)
{
	return mmb_str_valn(s, s ? (int)strlen(s) : 0);
}

/* Copy a transient value into a persistent pool slot (function returns,
 * SELECT values, GOSUB-saved variables, constants). */
void mmb_val_own(mmb_val *v, char **slot, int maxlen, const char *what)
{
	if (!v || v->type != T_STR || !slot)
		return;
	mmb_str_set(slot, v->s, v->s ? (int)strlen(v->s) : 0, maxlen, what);
	v->s = *slot;
}

double mmb_as_float(mmb_val v)
{
	if (v.type == T_STR || v.type == T_STRUCT)
		mmb_error("?TYPE MISMATCH");
	return v.type == T_INT ? (double)v.i : v.f;
}

int64_t mmb_as_int(mmb_val v)
{
	if (v.type == T_STR || v.type == T_STRUCT)
		mmb_error("?TYPE MISMATCH");
	return v.type == T_INT ? v.i : (int64_t)v.f;
}

void mmb_need_num(mmb_val v)
{
	if (v.type == T_STR)
		mmb_error("?TYPE MISMATCH");
}

unsigned mmb_now_ms(void)
{
	return G.plat && G.plat->millis ? G.plat->millis() : 0;
}

void mmb_console_write(const char *s)
{
	unsigned n;
	if (!s)
		return;
	n = (unsigned)strlen(s);
	if (!n)
		return;
	if (G.plat && G.plat->write_serial)
		G.plat->write_serial(s, n);
	if (G.plat && G.plat->write_screen)
		G.plat->write_screen(s, n);
}

void mmb_serial_write(const char *s)
{
	unsigned n;
	if (!s || !G.plat || !G.plat->write_serial)
		return;
	n = (unsigned)strlen(s);
	if (n)
		G.plat->write_serial(s, n);
}

static int rgb_dist2(unsigned a, unsigned b)
{
	int dr = (int)((a >> 16) & 255) - (int)((b >> 16) & 255);
	int dg = (int)((a >> 8) & 255) - (int)((b >> 8) & 255);
	int db = (int)(a & 255) - (int)(b & 255);
	return dr * dr + dg * dg + db * db;
}

static int rgb_to_ansi(unsigned rgb, int fg)
{
	static const unsigned pal[] = {
		0x000000, 0xAA0000, 0x00AA00, 0xAA5500,
		0x0000AA, 0xAA00AA, 0x00AAAA, 0xAAAAAA,
		0x555555, 0xFF5555, 0x55FF55, 0xFFFF55,
		0x5555FF, 0xFF55FF, 0x55FFFF, 0xFFFFFF
	};
	static const int fgcode[] = {
		30, 31, 32, 33, 34, 35, 36, 37,
		90, 91, 92, 93, 94, 95, 96, 97
	};
	static const int bgcode[] = {
		40, 41, 42, 43, 44, 45, 46, 47,
		100, 101, 102, 103, 104, 105, 106, 107
	};
	int best = 0, i, d, bd;

	bd = rgb_dist2(rgb, pal[0]);
	for (i = 1; i < 16; i++)
	{
		d = rgb_dist2(rgb, pal[i]);
		if (d < bd)
		{
			bd = d;
			best = i;
		}
	}
	return fg ? fgcode[best] : bgcode[best];
}

static void ansi_put_int(char *seq, int *n, int v)
{
	char tmp[4];
	int i = 0;
	if (v <= 0)
	{
		seq[(*n)++] = '0';
		return;
	}
	while (v > 0 && i < 4)
	{
		tmp[i++] = (char)('0' + (v % 10));
		v /= 10;
	}
	while (i > 0)
		seq[(*n)++] = tmp[--i];
}

void mmb_console_apply_colour(void)
{
	char seq[32];
	int n = 0;

	if (!G.plat || !G.plat->write_screen)
		return;
	/* Circle only honours SGR when the CSI has a single parameter
	 * (`ESC[91m`). `ESC[91;40m` is ignored. Reset first so a leftover
	 * SGR 44 (blue) cannot stick as the paper colour. */
	seq[n++] = '\x1b';
	seq[n++] = '[';
	seq[n++] = '0';
	seq[n++] = 'm';
	seq[n++] = '\x1b';
	seq[n++] = '[';
	ansi_put_int(seq, &n, rgb_to_ansi(G.gfx.fg, 1));
	seq[n++] = 'm';
	seq[n++] = '\x1b';
	seq[n++] = '[';
	ansi_put_int(seq, &n, rgb_to_ansi(G.gfx.bg, 0));
	seq[n++] = 'm';
	G.plat->write_screen(seq, (unsigned)n);
}

void mmb_console_reset_prompt(void)
{
	G.gfx.fg = MMB_DEFAULT_FG;
	G.gfx.bg = MMB_DEFAULT_BG;
	mmb_console_apply_colour();
	mmb_hw_cursor(1);
}

void mmb_hw_cursor(int show)
{
	const char *s = show ? "\x1b[?25h" : "\x1b[?25l";
	if (G.plat && G.plat->write_screen)
		G.plat->write_screen(s, 6);
}

/* Number of character cells across the boot console. */
static int startup_columns(void)
{
	int cols = 80;

	if (G.plat && G.plat->video_cols)
	{
		int v = G.plat->video_cols();
		if (v > 0)
			cols = v;
	}
	return cols;
}

/* Visible width of a string, ignoring ANSI SGR escape sequences. */
static int startup_visible_len(const char *s)
{
	int len = 0;

	while (*s)
	{
		if (s[0] == '\x1b' && s[1] == '[')
		{
			s += 2;
			while (*s && !(*s >= '@' && *s <= '~'))
				s++;
			if (*s)
				s++;
			continue;
		}
		len++;
		s++;
	}
	return len;
}

/* Emit a line centred across the boot console. */
static void startup_centred(const char *s)
{
	int pad = (startup_columns() - startup_visible_len(s)) / 2;
	if (pad < 0)
		pad = 0;
	while (pad-- > 0)
		mmb_console_write(" ");
	mmb_console_write(s);
	mmb_console_write("\n");
}

/* Draw the A:/mmcore.png wordmark centred near the top of the screen. */
static void startup_logo(void)
{
	const unsigned char *file = 0;
	unsigned n = 0;
	uint32_t *pix = 0;
	int w = 0, h = 0, i, j, x0, y0, hw;
	void (*pixel)(int, int, unsigned);

	if (!G.plat)
		return;
	/* Prefer the console's own pixel buffer so the banner newlines that
	 * follow cannot repaint the logo off the screen; fall back to the raw
	 * framebuffer (native hosts share one surface). */
	pixel = G.plat->console_pixel ? G.plat->console_pixel : G.plat->set_pixel;
	if (!pixel)
		return;
	if (mmb_vfs_read_ptr("A:/mmcore.png", &file, &n) != 0 || !file || !n)
		return;
	if (mmb_png_decode_rgba(file, n, &pix, &w, &h) != 0 || !pix)
		return;
	hw = G.plat->hdmi_width ? G.plat->hdmi_width() : w;
	x0 = (hw - w) / 2;
	if (x0 < 0)
		x0 = 0;
	y0 = 8;
	for (j = 0; j < h; j++)
	{
		for (i = 0; i < w; i++)
		{
			uint32_t c = pix[j * w + i];
			if (!(c >> 24))
				continue;
			pixel(x0 + i, y0 + j, c & 0xFFFFFFu);
		}
	}
	G.plat->free(pix);
}

void mmb_print_startup(void)
{
	G.gfx.fg = MMB_DEFAULT_FG;
	G.gfx.bg = MMB_DEFAULT_BG;
	mmb_console_apply_colour();
	if (G.plat && G.plat->fill_screen)
		G.plat->fill_screen(G.gfx.bg);
	mmb_console_apply_colour();

	startup_logo();

	/* Leave the top rows clear for the logo. */
	mmb_console_write("\n\n\n\n");
	mmb_console_write("\x1b[37m");
	startup_centred("mmcore operating system - " MMB_VERSION " - 2026 (c) Marnix Kok");
	startup_centred("MMBasic");
	startup_centred("Type \x1b[97mHELP ME\x1b[37m for a short introduction.");
	mmb_console_write("\n\n");
	mmb_console_write("\x1b[0m");
	mmb_console_apply_colour();
}

static void fmt_double(double x, char *buf, int buflen)
{
	int neg = 0, i, n;
	int64_t ip;
	double frac;
	if (buflen < 4)
	{
		buf[0] = 0;
		return;
	}
	if (x < 0)
	{
		neg = 1;
		x = -x;
	}
	ip = (int64_t)x;
	frac = x - (double)ip;
	if (frac < 0)
		frac = 0;
	/* integers print without a decimal (CMM2 PRINT of 5.0 is "5") */
	if (frac < 1e-10 || frac > 1.0 - 1e-10)
	{
		if (frac > 0.5)
			ip++;
		char tmp[40];
		char *p = tmp + sizeof(tmp) - 1;
		uint64_t v = (uint64_t)ip;
		*p = 0;
		if (v == 0)
			*--p = '0';
		while (v)
		{
			*--p = (char)('0' + (v % 10));
			v /= 10;
		}
		if (neg)
			*--p = '-';
		n = 0;
		while (*p && n < buflen - 1)
			buf[n++] = *p++;
		buf[n] = 0;
		return;
	}
	/* up to 6 decimal places, strip trailing zeros */
	char tmp[48];
	char *p = tmp + 24;
	int pos = 0;
	uint64_t v = (uint64_t)ip;
	char ibuf[24];
	int in = 0;
	if (v == 0)
		ibuf[in++] = '0';
	while (v && in < 22)
	{
		ibuf[in++] = (char)('0' + (v % 10));
		v /= 10;
	}
	if (neg)
		tmp[pos++] = '-';
	for (i = in - 1; i >= 0; i--)
		tmp[pos++] = ibuf[i];
	tmp[pos++] = '.';
	for (i = 0; i < 6; i++)
	{
		frac *= 10.0;
		int d = (int)frac;
		if (d < 0)
			d = 0;
		if (d > 9)
			d = 9;
		tmp[pos++] = (char)('0' + d);
		frac -= d;
	}
	while (pos > 0 && tmp[pos - 1] == '0')
		pos--;
	if (pos > 0 && tmp[pos - 1] == '.')
		pos--;
	tmp[pos] = 0;
	n = 0;
	p = tmp;
	while (*p && n < buflen - 1)
		buf[n++] = *p++;
	buf[n] = 0;
}

void mmb_print_val(mmb_val v)
{
	if (v.type == T_STR)
		mmb_out(v.s ? v.s : "");
	else if (v.type == T_INT)
		mmb_outf(0, v.i);
	else if (v.type == T_STRUCT)
		mmb_out("[STRUCT]");
	else
	{
		char buf[48];
		fmt_double(v.f, buf, sizeof(buf));
		mmb_out(buf);
	}
}

int mmb_opt_repeat_first(void)
{
	return G.opt.repeat_first > 0 ? G.opt.repeat_first : MMB_REPEAT_FIRST_DEFAULT;
}

int mmb_opt_repeat_next(void)
{
	return G.opt.repeat_next > 0 ? G.opt.repeat_next : MMB_REPEAT_NEXT_DEFAULT;
}

int mmb_opt_wifi_debug(void)
{
	return G.opt.wifi_debug != 0;
}

int mmb_opt_console_serial(void)
{
	return G.opt.console == 1 || G.opt.console == 3;
}

int mmb_opt_console_screen(void)
{
	return G.opt.console == 2 || G.opt.console == 3;
}

const char *mmb_prompt(void)
{
	static char buf[140];
	const char *cwd;
	int n;

	if (!G.opt.prompt)
		return "> ";
	cwd = mmb_vfs_cwd();
	n = 0;
	while (cwd[n] && n < (int)sizeof(buf) - 3)
	{
		buf[n] = cwd[n];
		n++;
	}
	buf[n++] = '>';
	buf[n++] = ' ';
	buf[n] = 0;
	return buf;
}

const char *mmb_opt_wifi_country(void)
{
	if (G.opt.wifi_country[0] && G.opt.wifi_country[1])
		return G.opt.wifi_country;
	return "US";
}

/* Circle hostap driver_circle.cpp: associate is refused unless the
 * country is on this list. UK is not listed; GB is. */
static const char k_wifi_countries[][3] = {
	"AD","AE","AF","AI","AL","AM","AN","AR","AS","AT","AU","AW","AZ",
	"BA","BB","BD","BE","BF","BG","BH","BL","BM","BN","BO","BR","BS",
	"BT","BY","BZ","CA","CF","CH","CI","CL","CN","CO","CR","CU","CX",
	"CY","CZ","DE","DK","DM","DO","DZ","EC","EE","EG","ES","ET","FI",
	"FM","FR","GB","GD","GE","GF","GH","GL","GP","GR","GT","GU","GY",
	"HK","HN","HR","HT","HU","ID","IE","IL","IN","IR","IS","IT","JM",
	"JO","JP","KE","KH","KN","KP","KR","KW","KY","KZ","LB","LC","LI",
	"LK","LS","LT","LU","LV","MA","MC","MD","ME","MF","MH","MK","MN",
	"MO","MP","MQ","MR","MT","MU","MV","MW","MX","MY","NG","NI","NL",
	"NO","NP","NZ","OM","PA","PE","PF","PG","PH","PK","PL","PM","PR",
	"PT","PW","PY","QA","RE","RO","RS","RU","RW","SA","SE","SG","SI",
	"SK","SN","SR","SV","SY","TC","TD","TG","TH","TN","TR","TT","TW",
	"TZ","UA","UG","US","UY","UZ","VC","VE","VI","VN","VU","WF","WS",
	"YE","YT","ZA","ZW"
};

int mmb_wifi_country_normalize(const char *s, char out[3])
{
	char a, b;
	unsigned i;

	if (!s || !s[0] || !s[1] || s[2] || !out)
		return 0;
	a = s[0];
	b = s[1];
	if (a >= 'a' && a <= 'z')
		a = (char)(a - 32);
	if (b >= 'a' && b <= 'z')
		b = (char)(b - 32);
	if (a < 'A' || a > 'Z' || b < 'A' || b > 'Z')
		return 0;
	if (a == 'U' && b == 'K')
	{
		a = 'G';
		b = 'B';
	}
	for (i = 0; i < sizeof k_wifi_countries / sizeof k_wifi_countries[0]; i++)
	{
		if (k_wifi_countries[i][0] == a && k_wifi_countries[i][1] == b)
		{
			out[0] = a;
			out[1] = b;
			out[2] = 0;
			return 1;
		}
	}
	return 0;
}

/* ---- timezone names (#524) -------------------------------------------- */

int mmb_tz_offset_min(void)
{
	return G.opt.tz_offset_min;
}

/* A pragmatic fixed-offset table: standard time, no DST rules. Covers the
 * zones a Pi owner is likely to name; anything else can use UTC+HH:MM. */
typedef struct {
	const char *name;
	int offset_min;
} tz_entry;

static const tz_entry k_timezones[] = {
	{ "UTC", 0 }, { "GMT", 0 }, { "Zulu", 0 },
	{ "Atlantic/Reykjavik", 0 }, { "Europe/London", 0 }, { "Europe/Dublin", 0 },
	{ "Europe/Lisbon", 0 },
	{ "Europe/Amsterdam", 60 }, { "Europe/Paris", 60 }, { "Europe/Berlin", 60 },
	{ "Europe/Madrid", 60 }, { "Europe/Rome", 60 }, { "Europe/Brussels", 60 },
	{ "Europe/Vienna", 60 }, { "Europe/Stockholm", 60 }, { "Europe/Oslo", 60 },
	{ "Europe/Copenhagen", 60 }, { "Europe/Prague", 60 }, { "Europe/Warsaw", 60 },
	{ "Europe/Zurich", 60 }, { "Europe/Budapest", 60 }, { "Europe/Belgrade", 60 },
	{ "Africa/Casablanca", 60 }, { "Africa/Lagos", 60 },
	{ "Europe/Athens", 120 }, { "Europe/Helsinki", 120 }, { "Europe/Kyiv", 120 },
	{ "Europe/Bucharest", 120 }, { "Europe/Riga", 120 }, { "Europe/Sofia", 120 },
	{ "Europe/Tallinn", 120 }, { "Europe/Vilnius", 120 },
	{ "Africa/Johannesburg", 120 }, { "Africa/Cairo", 120 }, { "Asia/Jerusalem", 120 },
	{ "Europe/Moscow", 180 }, { "Europe/Istanbul", 180 }, { "Africa/Nairobi", 180 },
	{ "Asia/Riyadh", 180 },
	{ "Asia/Tehran", 210 },
	{ "Asia/Dubai", 240 }, { "Indian/Mauritius", 240 },
	{ "Asia/Kabul", 270 },
	{ "Asia/Karachi", 300 }, { "Asia/Tashkent", 300 },
	{ "Asia/Kolkata", 330 }, { "Asia/Calcutta", 330 },
	{ "Asia/Kathmandu", 345 },
	{ "Asia/Dhaka", 360 }, { "Asia/Almaty", 360 },
	{ "Asia/Bangkok", 420 }, { "Asia/Jakarta", 420 }, { "Asia/Ho_Chi_Minh", 420 },
	{ "Asia/Shanghai", 480 }, { "Asia/Hong_Kong", 480 }, { "Asia/Singapore", 480 },
	{ "Asia/Manila", 480 }, { "Asia/Taipei", 480 }, { "Asia/Kuala_Lumpur", 480 },
	{ "Australia/Perth", 480 },
	{ "Asia/Tokyo", 540 }, { "Asia/Seoul", 540 },
	{ "Australia/Adelaide", 570 },
	{ "Australia/Brisbane", 600 }, { "Australia/Sydney", 600 },
	{ "Australia/Melbourne", 600 }, { "Australia/Hobart", 600 },
	{ "Pacific/Guam", 600 },
	{ "Pacific/Auckland", 720 }, { "Pacific/Fiji", 720 },
	{ "Pacific/Honolulu", -600 },
	{ "America/Anchorage", -540 },
	{ "America/Los_Angeles", -480 }, { "America/Vancouver", -480 },
	{ "America/Denver", -420 }, { "America/Phoenix", -420 },
	{ "America/Chicago", -360 }, { "America/Winnipeg", -360 },
	{ "America/Mexico_City", -360 },
	{ "America/New_York", -300 }, { "America/Toronto", -300 },
	{ "America/Detroit", -300 }, { "America/Bogota", -300 },
	{ "America/Lima", -300 }, { "America/Panama", -300 },
	{ "America/Caracas", -240 },
	{ "America/Sao_Paulo", -180 }, { "America/Buenos_Aires", -180 },
	{ "America/Argentina/Buenos_Aires", -180 },
};

static int tz_name_match(const char *a, const char *b)
{
	char ca, cb;

	while (*a && *b)
	{
		ca = *a;
		cb = *b;
		if (ca >= 'a' && ca <= 'z')
			ca = (char)(ca - 32);
		if (cb >= 'a' && cb <= 'z')
			cb = (char)(cb - 32);
		if (ca != cb)
			return 0;
		a++;
		b++;
	}
	return *a == 0 && *b == 0;
}

static void tz_copy(char *out, int outcap, const char *s)
{
	int i = 0;

	if (!out || outcap <= 0)
		return;
	while (s[i] && i + 1 < outcap)
	{
		out[i] = s[i];
		i++;
	}
	out[i] = 0;
}

/* Parse "UTC", "GMT", "UTC+2", "GMT-5:30", "Europe/Amsterdam", a bare
 * "+HH:MM" / "-HH", or a built-in IANA name. Writes the canonical name and
 * the minutes east of UTC. Returns 1 on success, 0 on failure. */
int mmb_timezone_normalize(const char *s, int *offset_min, char *out, int outcap)
{
	const char *p;
	int sign = 1, hh = 0, mm = 0, have = 0;
	unsigned i;

	if (!s || !out || outcap <= 0)
		return 0;
	while (*s == ' ' || *s == '\t')
		s++;
	for (i = 0; i < sizeof k_timezones / sizeof k_timezones[0]; i++)
	{
		if (tz_name_match(s, k_timezones[i].name))
		{
			if (offset_min)
				*offset_min = k_timezones[i].offset_min;
			tz_copy(out, outcap, k_timezones[i].name);
			return 1;
		}
	}
	p = s;
	if ((p[0] == 'U' || p[0] == 'u') && (p[1] == 'T' || p[1] == 't') &&
	    (p[2] == 'C' || p[2] == 'c'))
		p += 3;
	else if ((p[0] == 'G' || p[0] == 'g') && (p[1] == 'M' || p[1] == 'm') &&
		 (p[2] == 'T' || p[2] == 't'))
		p += 3;
	if (*p == '+' || *p == '-')
		sign = (*p == '-') ? -1 : 1;
	while (*p == ' ' || *p == '\t')
		p++;
	if (*p == '+')
		p++;
	else if (*p == '-')
		sign = -1, p++;
	if (*p < '0' || *p > '9')
		return 0;
	while (*p >= '0' && *p <= '9')
	{
		hh = hh * 10 + (*p - '0');
		p++;
		have = 1;
	}
	if (*p == ':')
	{
		p++;
		if (*p < '0' || *p > '9')
			return 0;
		while (*p >= '0' && *p <= '9')
		{
			mm = mm * 10 + (*p - '0');
			p++;
		}
	}
	while (*p == ' ')
		p++;
	if (!have || *p != 0 || hh > 14 || mm > 59)
		return 0;
	if (hh == 14 && mm != 0)
		return 0;
	if (offset_min)
		*offset_min = sign * (hh * 60 + mm);
	if (outcap > 0)
	{
		char tmp[24];
		int n = 0;
		const char *u = "UTC";

		while (*u && n < (int)sizeof(tmp) - 8)
			tmp[n++] = *u++;
		if (sign * (hh * 60 + mm) != 0)
		{
			tmp[n++] = sign < 0 ? '-' : '+';
			tmp[n++] = (char)('0' + hh / 10);
			tmp[n++] = (char)('0' + hh % 10);
			if (mm)
			{
				tmp[n++] = ':';
				tmp[n++] = (char)('0' + mm / 10);
				tmp[n++] = (char)('0' + mm % 10);
			}
		}
		tmp[n] = 0;
		tz_copy(out, outcap, tmp);
	}
	return 1;
}

static const int k_mdays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

static int parse_int_part(const char **ps)
{
	int n = 0;
	const char *p = *ps;
	while (*p == ' ' || *p == '\t')
		p++;
	if (*p < '0' || *p > '9')
		return -1;
	while (*p >= '0' && *p <= '9')
		n = n * 10 + (*p++ - '0');
	*ps = p;
	return n;
}

static void clock_norm(void)
{
	unsigned now = mmb_now_ms();
	int add;

	if (now < G.clk_ms)
		G.clk_ms = now;
	add = (int)((now - G.clk_ms) / 1000u);
	if (add <= 0)
		return;
	G.clk_ms += (unsigned)add * 1000u;
	G.clk_s += add;
	while (G.clk_s >= 60)
	{
		G.clk_s -= 60;
		G.clk_mi++;
	}
	while (G.clk_mi >= 60)
	{
		G.clk_mi -= 60;
		G.clk_h++;
	}
	while (G.clk_h >= 24)
	{
		G.clk_h -= 24;
		G.clk_d++;
	}
	for (;;)
	{
		int md, mo = G.clk_mo;
		if (mo < 1)
			mo = 1;
		if (mo > 12)
			mo = 12;
		md = k_mdays[mo - 1];
		if (mo == 2 && ((G.clk_y % 4) == 0))
			md = 29;
		if (G.clk_d <= md)
			break;
		G.clk_d -= md;
		G.clk_mo++;
		if (G.clk_mo > 12)
		{
			G.clk_mo = 1;
			G.clk_y++;
			if (G.clk_y > 99)
				G.clk_y = 0;
		}
	}
}

/* Civil calendar <-> days since 1970-01-01 (Howard Hinnant's algorithms). */
static int64_t days_from_civil(int y, int m, int d)
{
	int64_t era, doe, yoe, doy;
	y -= (m <= 2);
	era = (y >= 0 ? y : y - 399) / 400;
	yoe = (int64_t)(y - era * 400);
	doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + doe - 719468;
}

static void civil_from_days(int64_t z, int *py, int *pm, int *pd)
{
	int64_t era, doe, yoe, doy, mp;
	int y;
	unsigned d, m;
	z += 719468;
	era = (z >= 0 ? z : z - 146096) / 146097;
	doe = z - era * 146097;
	yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	y = (int)(yoe + era * 400);
	doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	mp = (5 * doy + 2) / 153;
	d = (unsigned)(doy - (153 * mp + 2) / 5 + 1);
	m = (unsigned)(mp + (mp < 10 ? 3 : -9));
	y += (m <= 2);
	*py = y;
	*pm = (int)m;
	*pd = (int)d;
}

int64_t mmb_epoch_make(int y, int mo, int d, int h, int mi, int s)
{
	return days_from_civil(y, mo, d) * 86400 + (int64_t)h * 3600 + (int64_t)mi * 60 + s;
}

void mmb_epoch_break(int64_t e, int *py, int *pmo, int *pd, int *ph, int *pmi, int *ps)
{
	int64_t days = e / 86400;
	int64_t rem = e % 86400;
	if (rem < 0)
	{
		rem += 86400;
		days--;
	}
	civil_from_days(days, py, pmo, pd);
	*ph = (int)(rem / 3600);
	rem %= 3600;
	*pmi = (int)(rem / 60);
	*ps = (int)(rem % 60);
}

int64_t mmb_epoch_now(void)
{
	mmb_clock_refresh();
	return mmb_epoch_make(2000 + G.clk_y, G.clk_mo, G.clk_d, G.clk_h, G.clk_mi, G.clk_s);
}

static void fmt2(char *p, int n)
{
	p[0] = (char)('0' + (n / 10) % 10);
	p[1] = (char)('0' + n % 10);
}

/* G.clk_* holds UTC. G.date_s/G.time_s are the local (timezone-adjusted)
 * strings shown by DATE$/TIME$. */
void mmb_clock_refresh(void)
{
	char *p;
	int n, y, mo, d, h, mi, sec;

	clock_norm();
	{
		int64_t utc = mmb_epoch_make(2000 + G.clk_y, G.clk_mo, G.clk_d,
					     G.clk_h, G.clk_mi, G.clk_s);
		mmb_epoch_break(utc + (int64_t)mmb_tz_offset_min() * 60,
				&y, &mo, &d, &h, &mi, &sec);
	}
	p = G.date_s;
	n = d;
	if (n >= 10)
		*p++ = (char)('0' + n / 10);
	*p++ = (char)('0' + n % 10);
	*p++ = '-';
	n = mo;
	if (n >= 10)
		*p++ = (char)('0' + n / 10);
	*p++ = (char)('0' + n % 10);
	*p++ = '-';
	if (y >= 2000)
		y -= 2000;
	fmt2(p, y);
	p[2] = 0;
	fmt2(G.time_s, h);
	G.time_s[2] = ':';
	fmt2(G.time_s + 3, mi);
	G.time_s[5] = ':';
	fmt2(G.time_s + 6, sec);
	G.time_s[8] = 0;
}

void mmb_clock_init(void)
{
	G.clk_d = 1;
	G.clk_mo = 1;
	G.clk_y = 26;
	G.clk_h = 12;
	G.clk_mi = 0;
	G.clk_s = 0;
	G.clk_ms = mmb_now_ms();
	mmb_clock_refresh();
}

/* Set the clock from a UTC Unix epoch (NTP). Also stamps the hardware wall
 * clock so FAT file timestamps track it, when the platform supports it. */
int mmb_clock_set_epoch(int64_t utc_epoch)
{
	int y, mo, d, h, mi, sec;

	if (utc_epoch < 0)
		return -1;
	mmb_epoch_break(utc_epoch, &y, &mo, &d, &h, &mi, &sec);
	if (y < 2000 || y > 2099)
		return -1;
	G.clk_y = y - 2000;
	G.clk_mo = mo;
	G.clk_d = d;
	G.clk_h = h;
	G.clk_mi = mi;
	G.clk_s = sec;
	G.clk_ms = mmb_now_ms();
	mmb_clock_refresh();
	if (G.plat && G.plat->set_wall_clock)
		G.plat->set_wall_clock((long long)utc_epoch, mmb_tz_offset_min());
	return 0;
}

int mmb_clock_set_date(const char *s)
{
	const char *p = s;
	int d, m, y, h, mi, sec;
	int yy, mo, dd, hh, mm2, ss;
	int64_t local, utc;
	if (!s)
		return -1;
	d = parse_int_part(&p);
	while (*p == ' ')
		p++;
	if (*p != '-' && *p != '/')
		return -1;
	p++;
	m = parse_int_part(&p);
	while (*p == ' ')
		p++;
	if (*p != '-' && *p != '/')
		return -1;
	p++;
	y = parse_int_part(&p);
	if (d < 1 || d > 31 || m < 1 || m > 12 || y < 0)
		return -1;
	if (y < 100)
		y += 2000;
	clock_norm();
	/* keep the current local time-of-day, replace the date */
	local = mmb_epoch_make(2000 + G.clk_y, G.clk_mo, G.clk_d,
			       G.clk_h, G.clk_mi, G.clk_s) +
		(int64_t)mmb_tz_offset_min() * 60;
	mmb_epoch_break(local, &yy, &mo, &dd, &h, &mi, &sec);
	local = mmb_epoch_make(y, m, d, h, mi, sec);
	utc = local - (int64_t)mmb_tz_offset_min() * 60;
	mmb_epoch_break(utc, &yy, &mo, &dd, &hh, &mm2, &ss);
	G.clk_y = (yy - 2000) % 100;
	G.clk_mo = mo;
	G.clk_d = dd;
	G.clk_h = hh;
	G.clk_mi = mm2;
	G.clk_s = ss;
	G.clk_ms = mmb_now_ms();
	mmb_clock_refresh();
	return 0;
}

int mmb_clock_set_time(const char *s)
{
	const char *p = s;
	int h, mi, sec, y, mo, d, hh, mm2, ss;
	int64_t utc, local;
	if (!s)
		return -1;
	h = parse_int_part(&p);
	while (*p == ' ')
		p++;
	if (*p != ':')
		return -1;
	p++;
	mi = parse_int_part(&p);
	sec = 0;
	while (*p == ' ')
		p++;
	if (*p == ':')
	{
		p++;
		sec = parse_int_part(&p);
	}
	if (h < 0 || h > 23 || mi < 0 || mi > 59 || sec < 0 || sec > 59)
		return -1;
	clock_norm();
	utc = mmb_epoch_make(2000 + G.clk_y, G.clk_mo, G.clk_d,
			     G.clk_h, G.clk_mi, G.clk_s);
	local = utc + (int64_t)mmb_tz_offset_min() * 60;
	mmb_epoch_break(local, &y, &mo, &d, &hh, &mm2, &ss);
	local = mmb_epoch_make(y, mo, d, h, mi, sec);
	utc = local - (int64_t)mmb_tz_offset_min() * 60;
	mmb_epoch_break(utc, &y, &mo, &d, &hh, &mm2, &ss);
	G.clk_y = (y - 2000) % 100;
	G.clk_mo = mo;
	G.clk_d = d;
	G.clk_h = hh;
	G.clk_mi = mm2;
	G.clk_s = ss;
	G.clk_ms = mmb_now_ms();
	mmb_clock_refresh();
	return 0;
}

#define MMB_KEY_UP        0x80
#define MMB_KEY_DOWN      0x81
#define MMB_KEY_LEFT      0x82
#define MMB_KEY_RIGHT     0x83
#define MMB_KEY_INSERT    0x84
#define MMB_KEY_HOME      0x86
#define MMB_KEY_END       0x87
#define MMB_KEY_PGUP      0x88
#define MMB_KEY_PGDN      0x89
#define MMB_KEY_F1        0x91
#define MMB_KEY_SHIFT_TAB 0x9F

static int inkey_at(int i)
{
	if (i < 0 || i >= G.inkey_n)
		return -1;
	return G.inkey_q[(G.inkey_r + i) % MMB_INKEY];
}

static void inkey_drop(int n)
{
	if (n <= 0)
		return;
	if (n > G.inkey_n)
		n = G.inkey_n;
	G.inkey_r = (G.inkey_r + n) % MMB_INKEY;
	G.inkey_n -= n;
}

static void inkey_poll(void)
{
	if (G.plat && G.plat->poll_input)
		G.plat->poll_input();
	if (G.running && G.plat && G.plat->take_break && G.plat->take_break())
	{
		mmb_play_stop();
		mmb_error("?BREAK");
	}
}

static int inkey_wait_n(int n, unsigned timeout_ms)
{
	unsigned start;

	if (G.inkey_n >= n)
		return 1;
	if (!G.plat || !G.plat->millis || !timeout_ms)
		return G.inkey_n >= n;
	start = mmb_now_ms();
	while (G.inkey_n < n)
	{
		if ((mmb_now_ms() - start) >= timeout_ms)
			return 0;
		inkey_poll();
	}
	return 1;
}

static int inkey_map_arrow(int c)
{
	static const int arrows[] = {
		MMB_KEY_UP, MMB_KEY_DOWN, MMB_KEY_RIGHT, MMB_KEY_LEFT
	};

	if (c < 'A' || c > 'D')
		return -1;
	return arrows[c - 'A'];
}

static int inkey_map_tilde(int p0)
{
	if (p0 == 1 || p0 == 7)
		return MMB_KEY_HOME;
	if (p0 == 2)
		return MMB_KEY_INSERT;
	if (p0 == 3)
		return 0x7F;
	if (p0 == 4 || p0 == 8)
		return MMB_KEY_END;
	if (p0 == 5)
		return MMB_KEY_PGUP;
	if (p0 == 6)
		return MMB_KEY_PGDN;
	if (p0 >= 11 && p0 <= 15)
		return MMB_KEY_F1 + (p0 - 11);
	if (p0 >= 17 && p0 <= 19)
		return MMB_KEY_F1 + 5 + (p0 - 17);
	if (p0 >= 20 && p0 <= 21)
		return MMB_KEY_F1 + 8 + (p0 - 20);
	if (p0 >= 23 && p0 <= 24)
		return MMB_KEY_F1 + 10 + (p0 - 23);
	if (p0 >= 25 && p0 <= 26)
		return (MMB_KEY_F1 + 2) + 0x20 + (p0 - 25);
	if (p0 >= 28 && p0 <= 29)
		return (MMB_KEY_F1 + 4) + 0x20 + (p0 - 28);
	if (p0 >= 31 && p0 <= 34)
		return (MMB_KEY_F1 + 6) + 0x20 + (p0 - 31);
	return -1;
}

static int inkey_csi_param0(int end)
{
	int n = 0, i, seen = 0;

	for (i = 2; i < end; i++)
	{
		int c = inkey_at(i);
		if (c == ';')
			break;
		if (c >= '0' && c <= '9')
		{
			seen = 1;
			n = n * 10 + (c - '0');
		}
	}
	return seen ? n : 0;
}

static int inkey_csi_end(void)
{
	int i;

	if (G.inkey_n < 3)
		return -1;
	if (inkey_at(1) != '[')
		return -1;
	if (inkey_at(2) == '[')
		return G.inkey_n >= 4 ? 3 : -1;
	for (i = 2; i < G.inkey_n && i < 16; i++)
	{
		int c = inkey_at(i);
		if (c >= 0x40 && c <= 0x7E)
			return i;
		if (c < 0x20 || c > 0x3F)
			return -2;
	}
	return -1;
}

static int inkey_map_csi(int end)
{
	int final, p0, mapped;

	if (end < 2)
		return -1;
	final = inkey_at(end);
	if (inkey_at(2) == '[')
	{
		if (final >= 'A' && final <= 'E')
			return MMB_KEY_F1 + (final - 'A');
		return -1;
	}
	mapped = inkey_map_arrow(final);
	if (mapped >= 0)
		return mapped;
	if (final == 'H')
		return MMB_KEY_HOME;
	if (final == 'F')
		return MMB_KEY_END;
	if (final == 'Z')
		return MMB_KEY_SHIFT_TAB;
	if (final != '~')
		return -1;
	p0 = inkey_csi_param0(end);
	return inkey_map_tilde(p0);
}

void mmb_inkey_push(int c)
{
	if (c <= 0 || c > 255)
		return;
	if (G.inkey_n >= MMB_INKEY)
		return;
	G.inkey_q[G.inkey_w] = c;
	G.inkey_w = (G.inkey_w + 1) % MMB_INKEY;
	G.inkey_n++;
}

int mmb_inkey_pop(void)
{
	int c, mapped, end;

	if (G.inkey_n <= 0)
		return -1;
	c = inkey_at(0);
	if (c != 0x1b)
	{
		inkey_drop(1);
		return c;
	}

	if (!inkey_wait_n(2, 30))
	{
		inkey_drop(1);
		return 0x1b;
	}

	c = inkey_at(1);
	if (c == 'O')
	{
		if (!inkey_wait_n(3, 50))
		{
			inkey_drop(1);
			return 0x1b;
		}
		c = inkey_at(2);
		mapped = inkey_map_arrow(c);
		if (mapped < 0 && c >= 'P' && c <= 'T')
			mapped = MMB_KEY_F1 + (c - 'P');
		if (mapped >= 0)
		{
			inkey_drop(3);
			return mapped;
		}
		inkey_drop(1);
		return 0x1b;
	}
	if (c != '[')
	{
		inkey_drop(1);
		return 0x1b;
	}

	if (!inkey_wait_n(3, 50))
	{
		inkey_drop(1);
		return 0x1b;
	}

	end = inkey_csi_end();
	if (end < 0)
	{
		unsigned start;

		if (!G.plat || !G.plat->millis)
		{
			inkey_drop(1);
			return 0x1b;
		}
		start = mmb_now_ms();
		while (end == -1 && (mmb_now_ms() - start) < 90)
		{
			inkey_poll();
			end = inkey_csi_end();
		}
	}
	if (end >= 2)
	{
		mapped = inkey_map_csi(end);
		if (mapped >= 0)
		{
			inkey_drop(end + 1);
			return mapped;
		}
	}
	inkey_drop(1);
	return 0x1b;
}

void mmb_keydown_set(const int *codes, int n)
{
	int i;
	if (n < 0)
		n = 0;
	if (n > 6)
		n = 6;
	G.nkeydown = n;
	for (i = 0; i < n; i++)
		G.keydown[i] = codes[i];
}

int mmb_keydown_get(int n)
{
	if (n == 0)
		return G.nkeydown;
	if (n < 1 || n > G.nkeydown)
		return 0;
	return G.keydown[n - 1];
}
