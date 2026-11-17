#include "mmb_priv.h"
#include "pkg_info.h"

/*
 * PACKAGE.INF metadata (#1523): a tiny, tolerant `key=value` parser plus the
 * dotted-version compare used by the HUB. The parser is intentionally pure
 * (no I/O) so host tests compile it on its own; the archive reader lives in
 * pkg_info_read.c.
 */

static char pi_upper(char c)
{
	return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
}

static int pi_key_eq(const char *a, const char *b)
{
	if (!a || !b)
		return 0;
	while (*a && *b)
	{
		if (pi_upper(*a) != pi_upper(*b))
			return 0;
		a++;
		b++;
	}
	return *a == 0 && *b == 0;
}

static char *pi_strdup(const char *s)
{
	unsigned n;
	char *p;

	if (!s)
		s = "";
	n = (unsigned)strlen(s);
	p = G.plat->alloc(n + 1);
	if (!p)
		return 0;
	memcpy(p, s, n + 1);
	return p;
}

static void pi_take(char **slot, char *s)
{
	if (*slot)
		G.plat->free(*slot);
	*slot = s;
}

static void pi_set_str(char **slot, const char *s)
{
	pi_take(slot, pi_strdup(s ? s : ""));
}

static void pi_set_category(struct mmb_pkg_info *pi, const char *s)
{
	char *up;
	unsigned i;

	up = pi_strdup(s ? s : "");
	if (!up)
	{
		pi_take(&pi->category, 0);
		pi->cat_index = MMB_APP_CATEGORY_OTHER;
		return;
	}
	for (i = 0; up[i]; i++)
		up[i] = pi_upper(up[i]);
	pi_take(&pi->category, up);
	if (!pi->category[0])
		pi->cat_index = MMB_APP_CATEGORY_OTHER;
	else
		pi->cat_index = mmb_app_category_index(pi->category);
}

void mmb_pkg_info_init(struct mmb_pkg_info *pi)
{
	if (!pi)
		return;
	pi->name = 0;
	pi->title = 0;
	pi->author = 0;
	pi->category = 0;
	pi->version = 0;
	pi->summary = 0;
	pi->minver = 0;
	pi->cat_index = MMB_APP_CATEGORY_OTHER;
}

void mmb_pkg_info_free(struct mmb_pkg_info *pi)
{
	if (!pi)
		return;
	pi_take(&pi->name, 0);
	pi_take(&pi->title, 0);
	pi_take(&pi->author, 0);
	pi_take(&pi->category, 0);
	pi_take(&pi->version, 0);
	pi_take(&pi->summary, 0);
	pi_take(&pi->minver, 0);
	pi->cat_index = MMB_APP_CATEGORY_OTHER;
}

int mmb_pkg_info_set(struct mmb_pkg_info *pi, const char *key, const char *value)
{
	if (!pi || !key)
		return 0;
	if (!value)
		value = "";
	if (pi_key_eq(key, "name"))
		pi_set_str(&pi->name, value);
	else if (pi_key_eq(key, "title"))
		pi_set_str(&pi->title, value);
	else if (pi_key_eq(key, "author"))
		pi_set_str(&pi->author, value);
	else if (pi_key_eq(key, "category"))
		pi_set_category(pi, value);
	else if (pi_key_eq(key, "version"))
		pi_set_str(&pi->version, value);
	else if (pi_key_eq(key, "summary"))
		pi_set_str(&pi->summary, value);
	else if (pi_key_eq(key, "minver"))
		pi_set_str(&pi->minver, value);
	return 0;
}

static void pi_rtrim(char *s)
{
	char *e = s + strlen(s);
	while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r'))
		*--e = 0;
}

int mmb_pkg_info_parse(const char *text, unsigned n, struct mmb_pkg_info *out)
{
	char *buf, *line, *next;

	if (!out)
		return -1;
	mmb_pkg_info_init(out);
	if (!text || n == 0)
		return 0;
	buf = G.plat->alloc(n + 1);
	if (!buf)
		return 0;
	memcpy(buf, text, n);
	buf[n] = 0;
	for (line = buf; line && *line; line = next)
	{
		char *eq, *key, *val;

		next = strchr(line, '\n');
		if (next)
			*next++ = 0;
		pi_rtrim(line);
		key = line;
		while (*key == ' ' || *key == '\t')
			key++;
		eq = strchr(key, '=');
		if (!eq)
			continue;
		*eq = 0;
		pi_rtrim(key);
		if (!key[0])
			continue;
		val = eq + 1;
		while (*val == ' ' || *val == '\t')
			val++;
		mmb_pkg_info_set(out, key, val);
	}
	G.plat->free(buf);
	return 0;
}

typedef struct pi_sb {
	char *p;
	unsigned n, cap;
} pi_sb;

static int pi_sb_add(pi_sb *b, const char *s, unsigned n)
{
	unsigned need = b->n + n + 1;
	if (need > b->cap)
	{
		unsigned cap = b->cap ? b->cap : 128;
		char *p;
		while (cap < need)
			cap *= 2;
		p = G.plat->alloc(cap);
		if (!p)
			return -1;
		if (b->p)
		{
			memcpy(p, b->p, b->n);
			G.plat->free(b->p);
		}
		b->p = p;
		b->cap = cap;
	}
	memcpy(b->p + b->n, s, n);
	b->n += n;
	b->p[b->n] = 0;
	return 0;
}

static int pi_sb_kv(pi_sb *b, const char *key, const char *value, unsigned max)
{
	unsigned n;
	if (!value || !value[0])
		return 0;
	n = (unsigned)strlen(value);
	if (max && n > max)
		n = max;
	if (pi_sb_add(b, key, (unsigned)strlen(key)) != 0 ||
	    pi_sb_add(b, value, n) != 0 ||
	    pi_sb_add(b, "\n", 1) != 0)
		return -1;
	return 0;
}

int mmb_pkg_info_build(const struct mmb_pkg_info *pi, char **out, unsigned *n)
{
	pi_sb b;

	if (!out)
		return -1;
	*out = 0;
	if (n)
		*n = 0;
	b.p = 0;
	b.n = b.cap = 0;
	if (pi_sb_kv(&b, "name=", pi ? pi->name : "", 0) != 0 ||
	    pi_sb_add(&b, "main=MAIN.BAS\n", 14) != 0 ||
	    pi_sb_kv(&b, "title=", pi ? pi->title : "", 0) != 0 ||
	    pi_sb_kv(&b, "author=", pi ? pi->author : "", 0) != 0 ||
	    pi_sb_kv(&b, "category=", pi ? pi->category : "", 0) != 0 ||
	    pi_sb_kv(&b, "version=", pi ? pi->version : "", 0) != 0 ||
	    pi_sb_kv(&b, "summary=", pi ? pi->summary : "", 80) != 0 ||
	    pi_sb_kv(&b, "minver=", pi ? pi->minver : "", 0) != 0)
	{
		if (b.p)
			G.plat->free(b.p);
		return -1;
	}
	if (!b.p)
	{
		b.p = G.plat->alloc(1);
		if (!b.p)
			return -1;
		b.p[0] = 0;
	}
	*out = b.p;
	if (n)
		*n = b.n;
	return 0;
}

typedef struct pi_ver {
	const char *p;
	int done;
} pi_ver;

static unsigned long long pi_ver_next(pi_ver *v)
{
	unsigned long long x = 0;

	if (v->done)
		return 0;
	if (*v->p < '0' || *v->p > '9')
	{
		v->done = 1;
		return 0;
	}
	while (*v->p >= '0' && *v->p <= '9')
	{
		if (x <= (~0ULL - 9ULL) / 10ULL)
			x = x * 10ULL + (unsigned long long)(*v->p - '0');
		else
			x = ~0ULL;
		v->p++;
	}
	if (*v->p == '.')
		v->p++;
	else
		v->done = 1;
	return x;
}

int mmb_version_cmp(const char *a, const char *b)
{
	pi_ver va, vb;

	if (!a)
		a = "";
	if (!b)
		b = "";
	if (*a == 'v' || *a == 'V')
		a++;
	if (*b == 'v' || *b == 'V')
		b++;
	va.p = a;
	va.done = 0;
	vb.p = b;
	vb.done = 0;
	for (;;)
	{
		unsigned long long x, y;
		if (va.done && vb.done)
			return 0;
		x = pi_ver_next(&va);
		y = pi_ver_next(&vb);
		if (x < y)
			return -1;
		if (x > y)
			return 1;
	}
}
