#include "mmb_priv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(MMB_PLATFORM_POSIX)
#include <time.h>
#endif

/* Opt-in per-statement profiler (MMB_STMTPROF).
 *
 * Unset, empty, "0", or "off": nothing is timed and the interpreter hot path
 * is one predictable load of mmb_stmtprof_active.
 * "1" / "on": write C:/.mmcore-stmtprof.csv (and sibling tables) at END.
 * Any other value is a host path (POSIX) or a drive path (C:/...).
 *
 * Time is exclusive: a SUB/FUNCTION body and check_break are their own
 * records, not part of the caller. raw_ns is a cycle counter converted with
 * the run's CLOCK_MONOTONIC span. busy_ns subtracts the measured cost of the
 * two timer reads that bracket each record.
 */

int mmb_stmtprof_active;

#define SP_CAP 16384
#define SP_MAX 48
#define SP_PROBE 24
#define SP_NAMES 768
#define SP_NAME_LEN 40

typedef struct {
	const char *site;
	uint64_t ticks;
	uint32_t count;
	uint32_t uncached;
	int line;
	int pc;
	int is_break;
	char kind[24];
	char stmt[80];
	char src[100];
} sp_bucket;

typedef struct {
	const char *site;
	uint64_t t0;
	uint64_t ticks;
	uint32_t uncached;
	int line;
	int pc;
	int is_break;
} sp_frame;

typedef struct {
	char name[SP_NAME_LEN];
	uint32_t count;
} sp_name;

static int s_enabled;
static int s_active;
static char s_path[256];
static sp_bucket *s_tab;
static sp_name *s_subs;
static int s_nsubs;
static uint32_t s_builtin[512];
static sp_frame s_stack[SP_MAX];
static int s_depth;
static uint64_t s_overflow_ticks;
static uint32_t s_overflow_count;
/* Distinct addresses for per-line check_break buckets. Not program text. */
static char s_break_key[8192];
static uint64_t s_run_ticks0, s_run_ns0;
static uint64_t s_overhead_ticks;

static uint64_t ticks(void)
{
#if defined(__x86_64__)
	unsigned lo, hi, aux;
	__asm__ __volatile__("rdtscp" : "=a"(lo), "=d"(hi), "=c"(aux));
	return ((uint64_t)hi << 32) | lo;
#elif defined(__aarch64__)
	uint64_t v;
	__asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(v));
	return v;
#elif defined(MMB_PLATFORM_POSIX)
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#else
	return mmb_now_us() * 1000ull;
#endif
}

static uint64_t wall_ns(void)
{
#if defined(MMB_PLATFORM_POSIX)
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#else
	return mmb_now_us() * 1000ull;
#endif
}

static int env_off(const char *v)
{
	if (!v || !v[0] || strcmp(v, "0") == 0)
		return 1;
	if ((v[0] == 'o' || v[0] == 'O') && (v[1] == 'f' || v[1] == 'F') &&
	    (v[2] == 'f' || v[2] == 'F') && v[3] == 0)
		return 1;
	return 0;
}

static int env_on(const char *v)
{
	return strcmp(v, "1") == 0 ||
	       ((v[0] == 'o' || v[0] == 'O') && (v[1] == 'n' || v[1] == 'N') &&
		v[2] == 0) ||
	       ((v[0] == 'y' || v[0] == 'Y') && (v[1] == 'e' || v[1] == 'E') &&
		(v[2] == 's' || v[2] == 'S') && v[3] == 0) ||
	       ((v[0] == 't' || v[0] == 'T') && (v[1] == 'r' || v[1] == 'R') &&
		(v[2] == 'u' || v[2] == 'U') && (v[3] == 'e' || v[3] == 'E') &&
		v[4] == 0);
}

void mmb_stmtprof_env_init(void)
{
	const char *env = getenv("MMB_STMTPROF");

	s_enabled = 0;
	s_active = 0;
	mmb_stmtprof_active = 0;
	s_path[0] = 0;
	if (env_off(env))
		return;
	s_enabled = 1;
	if (env_on(env))
		snprintf(s_path, sizeof s_path, "C:/.mmcore-stmtprof.csv");
	else
		snprintf(s_path, sizeof s_path, "%s", env);
}

static uint64_t measure_overhead(void)
{
	enum { N = 200000 };
	volatile uint64_t sink = 0;
	uint64_t t0, t1, a, b, empty, both;
	int i;

	t0 = ticks();
	for (i = 0; i < N; i++)
		sink += (uint64_t)i;
	t1 = ticks();
	empty = t1 - t0;
	t0 = ticks();
	for (i = 0; i < N; i++)
	{
		a = ticks();
		b = ticks();
		sink += a + b + (uint64_t)i;
	}
	t1 = ticks();
	both = t1 - t0;
	if (both <= empty)
		return 0;
	return (both - empty) / (uint64_t)N;
}

void mmb_stmtprof_run_begin(void)
{
	if (!s_enabled)
		return;
	if (s_active)
		mmb_stmtprof_run_end();
	s_tab = (sp_bucket *)calloc(SP_CAP, sizeof(sp_bucket));
	s_subs = (sp_name *)calloc(SP_NAMES, sizeof(sp_name));
	if (!s_tab || !s_subs)
	{
		free(s_tab);
		free(s_subs);
		s_tab = 0;
		s_subs = 0;
		return;
	}
	memset(s_builtin, 0, sizeof s_builtin);
	s_nsubs = 0;
	s_depth = 0;
	s_overflow_ticks = 0;
	s_overflow_count = 0;
	s_overhead_ticks = measure_overhead();
	s_run_ticks0 = ticks();
	s_run_ns0 = wall_ns();
	s_active = 1;
	mmb_stmtprof_active = 1;
}

static int line_no(int pc)
{
	if (pc < 0 || pc >= G.nprog || !G.prog_num)
		return 0;
	return G.prog_num[pc];
}

static int push_frame(const char *site, int is_break)
{
	sp_frame *fr;
	uint64_t now;

	if (s_depth >= SP_MAX)
		return 0;
	now = ticks();
	if (s_depth > 0)
		s_stack[s_depth - 1].ticks += now - s_stack[s_depth - 1].t0;
	fr = &s_stack[s_depth++];
	memset(fr, 0, sizeof *fr);
	fr->t0 = now;
	fr->site = site;
	fr->line = line_no(G.run_pc);
	fr->pc = G.run_pc;
	fr->is_break = is_break;
	return 1;
}

int mmb_stmtprof_enter_slow(void)
{
	const char *site;
	if (!mmb_stmtprof_active)
		return 0;
	site = G.stmt_start ? G.stmt_start : G.p;
	if (!site)
		return 0;
	return push_frame(site, 0);
}

int mmb_stmtprof_enter_break(void)
{
	int pc;
	if (!mmb_stmtprof_active || !G.running)
		return 0;
	pc = G.run_pc;
	if (pc < 0 || pc >= (int)sizeof s_break_key)
		pc = 0;
	return push_frame(&s_break_key[pc], 1);
}

void mmb_stmtprof_note_uncached(void)
{
	if (!mmb_stmtprof_active || s_depth <= 0)
		return;
	s_stack[s_depth - 1].uncached++;
}

static unsigned mix_ptr(uintptr_t x)
{
	x ^= x >> 30;
	x *= 0xbf58476d1ce4e5b9ull;
	x ^= x >> 27;
	x *= 0x94d049bb133111ebull;
	x ^= x >> 31;
	return (unsigned)x;
}

static void snippet(const char *p, char *out, int n)
{
	int o = 0;
	int q = 0;

	if (!p)
	{
		out[0] = 0;
		return;
	}
	while (*p && o < n - 1)
	{
		unsigned char c = (unsigned char)*p;
		if (!q && (c == ':' || c == '\''))
			break;
		if (c == 0x80)
		{
			int id = (unsigned char)p[1] | ((unsigned char)p[2] << 8);
			const char *kw = mmb_kw_name(id);
			if (o && out[o - 1] != ' ' && o < n - 1)
				out[o++] = ' ';
			while (*kw && o < n - 1)
				out[o++] = *kw++;
			p += 3;
			continue;
		}
		if (c == '"')
			q = !q;
		out[o++] = (char)c;
		p++;
	}
	while (o > 0 && out[o - 1] == ' ')
		o--;
	out[o] = 0;
}

static void kind_of(const char *p, char *out, int n)
{
	char buf[80];

	out[0] = 0;
	if (!p)
		return;
	if ((unsigned char)*p == 0x80)
	{
		int id = (unsigned char)p[1] | ((unsigned char)p[2] << 8);
		const char *kw = mmb_kw_name(id);
		const char *q;
		snprintf(out, (size_t)n, "%s", kw);
		if (strcmp(kw, "END") != 0 && strcmp(kw, "EXIT") != 0)
			return;
		q = p + 3;
		while (*q == ' ' || *q == '\t')
			q++;
		if ((unsigned char)*q == 0x80)
		{
			int id2 = (unsigned char)q[1] | ((unsigned char)q[2] << 8);
			snprintf(out, (size_t)n, "%s %s", kw, mmb_kw_name(id2));
		}
		return;
	}
	snippet(p, buf, (int)sizeof buf);
	if (strchr(buf, '='))
		snprintf(out, (size_t)n, "LET");
	else
		snprintf(out, (size_t)n, "CALL");
}

static void source_of(int pc, char *out, int n)
{
	const char *s;
	int i = 0;

	out[0] = 0;
	if (pc < 0 || pc >= G.nprog || !G.prog || !G.prog[pc])
		return;
	s = G.prog[pc];
	while (*s && i < n - 1)
	{
		char c = *s++;
		if (c == '\t')
			c = ' ';
		if (c == '\n' || c == '\r')
			break;
		out[i++] = c;
	}
	out[i] = 0;
}

static void fill_bucket(sp_bucket *b, const sp_frame *fr)
{
	b->site = fr->site;
	b->line = fr->line;
	b->pc = fr->pc;
	b->is_break = fr->is_break;
	if (fr->is_break)
	{
		snprintf(b->kind, sizeof b->kind, "BREAK");
		snprintf(b->stmt, sizeof b->stmt, "(check_break)");
	}
	else
	{
		kind_of(fr->site, b->kind, (int)sizeof b->kind);
		snippet(fr->site, b->stmt, (int)sizeof b->stmt);
	}
	source_of(fr->pc, b->src, (int)sizeof b->src);
}

static void bucket_add(const sp_frame *fr)
{
	unsigned h, n;
	sp_bucket *slot = 0;

	if (!s_tab || !fr->site)
	{
		s_overflow_ticks += fr->ticks;
		s_overflow_count++;
		return;
	}
	h = mix_ptr((uintptr_t)fr->site) & (SP_CAP - 1);
	for (n = 0; n < SP_PROBE; n++)
	{
		sp_bucket *b = &s_tab[(h + n) & (SP_CAP - 1)];
		if (b->site == fr->site)
		{
			b->ticks += fr->ticks;
			b->count++;
			b->uncached += fr->uncached;
			return;
		}
		if (!b->site)
		{
			slot = b;
			break;
		}
	}
	if (!slot)
	{
		s_overflow_ticks += fr->ticks;
		s_overflow_count++;
		return;
	}
	fill_bucket(slot, fr);
	slot->ticks = fr->ticks;
	slot->count = 1;
	slot->uncached = fr->uncached;
}

void mmb_stmtprof_leave(void)
{
	sp_frame *fr;
	uint64_t now;

	if (!mmb_stmtprof_active || s_depth <= 0)
		return;
	now = ticks();
	fr = &s_stack[s_depth - 1];
	fr->ticks += now - fr->t0;
	bucket_add(fr);
	s_depth--;
	if (s_depth > 0)
		s_stack[s_depth - 1].t0 = now;
}

void mmb_stmtprof_hit_builtin(int id)
{
	if (!mmb_stmtprof_active)
		return;
	if (id > 0 && id < 512)
		s_builtin[id]++;
}

void mmb_stmtprof_hit_sub(const char *name)
{
	int i;
	char buf[SP_NAME_LEN];

	if (!mmb_stmtprof_active || !name || !name[0] || !s_subs)
		return;
	snprintf(buf, sizeof buf, "%s", name);
	for (i = 0; buf[i]; i++)
		if (buf[i] >= 'a' && buf[i] <= 'z')
			buf[i] = (char)(buf[i] - 32);
	for (i = 0; i < s_nsubs; i++)
		if (strcmp(s_subs[i].name, buf) == 0)
		{
			s_subs[i].count++;
			return;
		}
	if (s_nsubs >= SP_NAMES)
		return;
	snprintf(s_subs[s_nsubs].name, SP_NAME_LEN, "%s", buf);
	s_subs[s_nsubs].count = 1;
	s_nsubs++;
}

int mmb_stmtprof_depth(void)
{
	return mmb_stmtprof_active ? s_depth : 0;
}

void mmb_stmtprof_set_depth(int depth)
{
	if (!mmb_stmtprof_active)
		return;
	if (depth < 0)
		depth = 0;
	if (depth > s_depth)
		depth = s_depth;
	s_depth = depth;
	if (s_depth > 0)
		s_stack[s_depth - 1].t0 = ticks();
}

static int cmp_bucket(const void *va, const void *vb)
{
	const sp_bucket *a = *(const sp_bucket *const *)va;
	const sp_bucket *b = *(const sp_bucket *const *)vb;
	if (a->ticks < b->ticks)
		return 1;
	if (a->ticks > b->ticks)
		return -1;
	return 0;
}

static int cmp_name(const void *va, const void *vb)
{
	const sp_name *a = va;
	const sp_name *b = vb;
	if (a->count < b->count)
		return 1;
	if (a->count > b->count)
		return -1;
	return strcmp(a->name, b->name);
}

typedef struct {
	char *p;
	unsigned n, cap;
} sp_buf;

static void badd(sp_buf *b, const char *s)
{
	unsigned m = (unsigned)strlen(s);
	if (b->n + m + 1 > b->cap)
	{
		unsigned c = b->cap ? b->cap * 2u : 8192u;
		char *np;
		while (c < b->n + m + 1)
			c *= 2u;
		np = (char *)realloc(b->p, c);
		if (!np)
			return;
		b->p = np;
		b->cap = c;
	}
	memcpy(b->p + b->n, s, m);
	b->n += m;
	b->p[b->n] = 0;
}

static void badd_quoted(sp_buf *b, const char *s)
{
	badd(b, "\"");
	for (; s && *s; s++)
	{
		char tmp[3];
		if (*s == '"')
		{
			badd(b, "\"\"");
			continue;
		}
		tmp[0] = (*s == '\n' || *s == '\r') ? ' ' : *s;
		tmp[1] = 0;
		badd(b, tmp);
	}
	badd(b, "\"");
}

static void sibling_path(const char *path, const char *tag, char *out, int n)
{
	const char *slash = strrchr(path, '/');
	const char *dot = strrchr(path, '.');

	if (dot && (!slash || dot > slash))
		snprintf(out, (size_t)n, "%.*s.%s%s", (int)(dot - path), path, tag,
			 dot);
	else
		snprintf(out, (size_t)n, "%s.%s.csv", path, tag);
}

static int write_bytes(const char *path, const char *data, unsigned n)
{
#if defined(MMB_PLATFORM_POSIX)
	if (path[0] == '/' || (path[0] == '.' && path[1] == '/'))
	{
		FILE *fp = fopen(path, "w");
		if (!fp)
			return -1;
		if (n && fwrite(data, 1, n, fp) != n)
		{
			fclose(fp);
			return -1;
		}
		fclose(fp);
		return 0;
	}
#endif
	return mmb_vfs_write(path, data, n, 0) < 0 ? -1 : 0;
}

static uint64_t ticks_to_ns(uint64_t dt, uint64_t span_ticks, uint64_t span_ns)
{
	if (!span_ticks)
		return 0;
	return (uint64_t)(((__uint128_t)dt * span_ns) / span_ticks);
}

static uint64_t busy_ticks(uint64_t total, uint32_t count)
{
	__uint128_t sub = (__uint128_t)s_overhead_ticks * count;
	if (sub >= total)
		return 0;
	return (uint64_t)(total - sub);
}

void mmb_stmtprof_run_end(void)
{
	uint64_t span_ticks, span_ns, now_ticks, now_ns;
	sp_bucket **order = 0;
	int norder = 0;
	int i;
	sp_buf body;
	char line[512];
	char subpath[300], bipath[300];
	uint64_t sum_raw = 0, sum_busy = 0, sum_pause = 0, sum_break = 0;

	if (!s_active)
		return;
	now_ticks = ticks();
	now_ns = wall_ns();
	mmb_stmtprof_active = 0;
	s_active = 0;
	s_depth = 0;
	span_ticks = now_ticks - s_run_ticks0;
	span_ns = now_ns > s_run_ns0 ? now_ns - s_run_ns0 : 0;

	memset(&body, 0, sizeof body);
	order = (sp_bucket **)malloc(sizeof(sp_bucket *) * SP_CAP);
	if (s_tab && order)
	{
		for (i = 0; i < SP_CAP; i++)
			if (s_tab[i].site)
				order[norder++] = &s_tab[i];
		qsort(order, (size_t)norder, sizeof order[0], cmp_bucket);
	}
	for (i = 0; i < norder; i++)
	{
		uint64_t raw = ticks_to_ns(order[i]->ticks, span_ticks, span_ns);
		uint64_t busy = ticks_to_ns(busy_ticks(order[i]->ticks, order[i]->count),
					    span_ticks, span_ns);
		sum_raw += raw;
		sum_busy += busy;
		if (order[i]->is_break)
			sum_break += raw;
		else if (strcmp(order[i]->kind, "PAUSE") == 0)
			sum_pause += raw;
	}
	snprintf(line, sizeof line,
		 "# mmcore-stmtprof version=1\n"
		 "# elapsed_ns=%llu cycle_span=%llu ns_per_cycle_num=%llu ns_per_cycle_den=%llu\n"
		 "# overhead_cycles=%llu records=%d overflow_records=%u\n"
		 "# sum_raw_ns=%llu sum_busy_ns=%llu sum_pause_raw_ns=%llu sum_break_raw_ns=%llu\n"
		 "# busy_ns subtracts overhead_cycles per execution (the two timer reads).\n"
		 "# BREAK is mmb_check_break, exclusive of statements it runs.\n"
		 "kind,line,pc,count,uncached,raw_ns,busy_ns,raw_per_ns,busy_per_ns,stmt,source\n",
		 (unsigned long long)span_ns, (unsigned long long)span_ticks,
		 (unsigned long long)span_ns, (unsigned long long)span_ticks,
		 (unsigned long long)s_overhead_ticks, norder, s_overflow_count,
		 (unsigned long long)sum_raw, (unsigned long long)sum_busy,
		 (unsigned long long)sum_pause, (unsigned long long)sum_break);
	badd(&body, line);
	for (i = 0; i < norder; i++)
	{
		sp_bucket *b = order[i];
		uint64_t raw = ticks_to_ns(b->ticks, span_ticks, span_ns);
		uint64_t busy = ticks_to_ns(busy_ticks(b->ticks, b->count), span_ticks,
					    span_ns);
		uint64_t raw_per = b->count ? raw / b->count : 0;
		uint64_t busy_per = b->count ? busy / b->count : 0;
		snprintf(line, sizeof line, "%s,%d,%d,%u,%u,%llu,%llu,%llu,%llu,",
			 b->kind, b->line, b->pc, b->count, b->uncached,
			 (unsigned long long)raw, (unsigned long long)busy,
			 (unsigned long long)raw_per, (unsigned long long)busy_per);
		badd(&body, line);
		badd_quoted(&body, b->stmt);
		badd(&body, ",");
		badd_quoted(&body, b->src);
		badd(&body, "\n");
	}
	if (body.p)
		(void)write_bytes(s_path, body.p, body.n);
	free(body.p);
	free(order);

	sibling_path(s_path, "subs", subpath, (int)sizeof subpath);
	memset(&body, 0, sizeof body);
	if (s_subs && s_nsubs)
		qsort(s_subs, (size_t)s_nsubs, sizeof s_subs[0], cmp_name);
	badd(&body, "# mmcore-stmtprof sub/function entries\nname,count\n");
	for (i = 0; i < s_nsubs; i++)
	{
		snprintf(line, sizeof line, "%s,%u\n", s_subs[i].name, s_subs[i].count);
		badd(&body, line);
	}
	if (body.p)
		(void)write_bytes(subpath, body.p, body.n);
	free(body.p);

	sibling_path(s_path, "builtins", bipath, (int)sizeof bipath);
	memset(&body, 0, sizeof body);
	badd(&body, "# mmcore-stmtprof builtin function evaluations (uncached statements only)\nname,count\n");
	for (i = 1; i < 512; i++)
	{
		if (!s_builtin[i])
			continue;
		snprintf(line, sizeof line, "%s,%u\n", mmb_kw_name(i), s_builtin[i]);
		badd(&body, line);
	}
	if (body.p)
		(void)write_bytes(bipath, body.p, body.n);
	free(body.p);

	free(s_tab);
	free(s_subs);
	s_tab = 0;
	s_subs = 0;
	s_nsubs = 0;
}
