/*
 * HUB engine (#1525).
 *
 * Non-UI package-hub engine: fetch and parse `index.json` (schema 1, server
 * contract marnixk/mmcore-apps#17), detect what is installed on OPTION PATH,
 * download icons/thumbs/shots through a small LRU cache, and install, update
 * or remove `.APP` packages with a SHA-256 check.
 *
 * The HTTP work reuses the one client in cmd_web.c through the non-throwing
 * mmb_url_fetch_ex() path, so a missing network, TLS or server never crashes
 * the interpreter: errors come back as friendly strings instead. All strings
 * are dynamically allocated with the interpreter allocator.
 */
#include "mmb_priv.h"
#include "hub.h"
#include "pkg_info.h"
#include "sha256.h"
#include "cJSON.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define HUB_CACHE_MAX_BYTES (4u * 1024u * 1024u)

static hub_app *s_apps;
static int s_napps;

typedef struct hub_cache_entry {
	char *key;
	unsigned char *data;
	unsigned n;
	struct hub_cache_entry *next;
} hub_cache_entry;

static hub_cache_entry *s_cache;
static unsigned s_cache_bytes;

/* ---- small helpers --------------------------------------------------- */

static char *hub_strdup(const char *s)
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

static void hub_set_str(char **slot, const char *s)
{
	char *p = hub_strdup(s ? s : "");

	if (*slot)
		G.plat->free(*slot);
	*slot = p;
}

static int hub_ci_eq(const char *a, const char *b)
{
	if (!a || !b)
		return 0;
	while (*a && *b)
	{
		char ca = *a, cb = *b;

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

static void hub_set_err(char *err, int errsz, const char *fmt, ...)
{
	va_list ap;

	if (!err || errsz <= 0)
		return;
	va_start(ap, fmt);
	vsnprintf(err, (size_t)errsz, fmt, ap);
	va_end(ap);
}

static void hub_err_out_of_memory(char *err, int errsz)
{
	hub_set_err(err, errsz, "OUT OF MEMORY");
}

static void hub_trim(char *s)
{
	char *e;

	while (*s == ' ' || *s == '\t')
		memmove(s, s + 1, strlen(s));
	e = s + strlen(s);
	while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r'))
		*--e = 0;
}

static char *hub_url_join(const char *rel)
{
	const char *base = G.opt.hub_url;
	unsigned bn, rn;
	char *out;

	if (!rel)
		rel = "";
	bn = (unsigned)strlen(base);
	rn = (unsigned)strlen(rel);
	out = G.plat->alloc(bn + rn + 2);
	if (!out)
		return 0;
	memcpy(out, base, bn);
	if (bn && out[bn - 1] != '/')
		out[bn++] = '/';
	memcpy(out + bn, rel, rn + 1);
	return out;
}

/* Join a directory and a file name with exactly one slash. */
static char *hub_join(const char *dir, const char *name)
{
	unsigned dn, nn;
	int slash;
	char *out;

	if (!dir)
		dir = "";
	if (!name)
		name = "";
	dn = (unsigned)strlen(dir);
	nn = (unsigned)strlen(name);
	slash = dn > 0 && dir[dn - 1] != '/' && dir[dn - 1] != '\\';
	out = G.plat->alloc(dn + nn + (unsigned)slash + 1);
	if (!out)
		return 0;
	memcpy(out, dir, dn);
	if (slash)
		out[dn++] = '/';
	memcpy(out + dn, name, nn + 1);
	return out;
}

/* ---- app records ----------------------------------------------------- */

static void hub_app_clear(hub_app *a)
{
	int i;

	if (!a)
		return;
#define HUB_FREE_FIELD(f) do { if (a->f) G.plat->free(a->f); a->f = 0; } while (0)
	HUB_FREE_FIELD(id);
	HUB_FREE_FIELD(file);
	HUB_FREE_FIELD(title);
	HUB_FREE_FIELD(author);
	HUB_FREE_FIELD(category);
	HUB_FREE_FIELD(version);
	HUB_FREE_FIELD(summary);
	HUB_FREE_FIELD(minver);
	HUB_FREE_FIELD(sha256);
	HUB_FREE_FIELD(updated);
	HUB_FREE_FIELD(icon);
	HUB_FREE_FIELD(thumb);
	HUB_FREE_FIELD(about);
	HUB_FREE_FIELD(installed_path);
	HUB_FREE_FIELD(installed_version);
#undef HUB_FREE_FIELD
	for (i = 0; i < a->nshots; i++)
		if (a->shots[i])
			G.plat->free(a->shots[i]);
	if (a->shots)
		G.plat->free(a->shots);
	a->shots = 0;
	a->nshots = 0;
	a->size = 0;
	a->cat_index = MMB_APP_CATEGORY_OTHER;
	a->state = HUB_NOT_INSTALLED;
}

void hub_free(void)
{
	int i;

	for (i = 0; i < s_napps; i++)
		hub_app_clear(&s_apps[i]);
	if (s_apps)
		G.plat->free(s_apps);
	s_apps = 0;
	s_napps = 0;
}

int hub_count(void)
{
	return s_napps;
}

hub_app *hub_get(int i)
{
	if (i < 0 || i >= s_napps)
		return 0;
	return &s_apps[i];
}

int hub_filter(int cat_index, int *out, int max)
{
	int i, n = 0, total = 0;

	for (i = 0; i < s_napps; i++)
	{
		if (cat_index >= 0 && s_apps[i].cat_index != cat_index)
			continue;
		if (out && n < max)
			out[n] = i;
		n++;
		total++;
	}
	return total;
}

/* ---- installed detection -------------------------------------------- */

typedef struct hub_inst_entry {
	char *path;
	char *stem;
	char *infname;
	char *version;
} hub_inst_entry;

typedef struct hub_inst_list {
	hub_inst_entry *v;
	int n;
	int cap;
} hub_inst_list;

static void hub_inst_free(hub_inst_list *l)
{
	int i;

	for (i = 0; i < l->n; i++)
	{
		if (l->v[i].path)
			G.plat->free(l->v[i].path);
		if (l->v[i].stem)
			G.plat->free(l->v[i].stem);
		if (l->v[i].infname)
			G.plat->free(l->v[i].infname);
		if (l->v[i].version)
			G.plat->free(l->v[i].version);
	}
	if (l->v)
		G.plat->free(l->v);
	l->v = 0;
	l->n = 0;
	l->cap = 0;
}

static int hub_inst_push(hub_inst_list *l, const char *path, const char *stem,
			 const char *infname, const char *version)
{
	hub_inst_entry *e;

	if (l->n >= l->cap)
	{
		int cap = l->cap ? l->cap * 2 : 16;
		hub_inst_entry *nv = G.plat->alloc(sizeof(*nv) * (unsigned)cap);

		if (!nv)
			return -1;
		if (l->v)
		{
			memcpy(nv, l->v, sizeof(*nv) * (unsigned)l->n);
			G.plat->free(l->v);
		}
		l->v = nv;
		l->cap = cap;
	}
	e = &l->v[l->n];
	memset(e, 0, sizeof(*e));
	e->path = hub_strdup(path);
	e->stem = hub_strdup(stem ? stem : "");
	e->infname = hub_strdup(infname ? infname : "");
	e->version = hub_strdup(version ? version : "");
	if (!e->path || !e->stem || !e->infname || !e->version)
	{
		if (e->path)
			G.plat->free(e->path);
		if (e->stem)
			G.plat->free(e->stem);
		if (e->infname)
			G.plat->free(e->infname);
		if (e->version)
			G.plat->free(e->version);
		memset(e, 0, sizeof(*e));
		return -1;
	}
	l->n++;
	return 0;
}

/* One directory listing, grown until it fits. Returns NULL on error. */
static mmb_dirent *hub_list_entries(const char *dir, int *count)
{
	int cap = 64;

	for (;;)
	{
		int n, trunc = 0;
		mmb_dirent *v = G.plat->alloc(sizeof(*v) * (unsigned)cap);

		if (!v)
			return 0;
		n = mmb_vfs_list_entries(dir, v, cap, &trunc);
		if (n < 0)
		{
			G.plat->free(v);
			return 0;
		}
		if (!trunc)
		{
			*count = n;
			return v;
		}
		G.plat->free(v);
		if (cap > (1 << 20))
			return 0;
		cap *= 2;
	}
}

/* Build a first-match-ordered list of installed .APP packages across every
 * OPTION PATH directory. */
static void hub_scan_installed(hub_inst_list *l)
{
	const char *p = G.opt.app_path;

	memset(l, 0, sizeof(*l));
	while (p && *p)
	{
		const char *e = p;
		char dir[160];
		int dn, count = 0, i;
		mmb_dirent *ents;

		while (*e && *e != ';')
			e++;
		dn = (int)(e - p);
		if (dn > (int)sizeof(dir) - 1)
			dn = (int)sizeof(dir) - 1;
		memcpy(dir, p, (unsigned)dn);
		dir[dn] = 0;
		hub_trim(dir);
		p = *e ? e + 1 : e;
		if (!dir[0])
			continue;
		ents = hub_list_entries(dir, &count);
		if (!ents)
			continue;
		for (i = 0; i < count; i++)
		{
			const char *name = ents[i].name;
			int nl;
			char stem[128];
			char *full;
			struct mmb_pkg_info pi;

			if (ents[i].is_dir || mmb_vfs_hidden_name(name))
				continue;
			nl = (int)strlen(name);
			if (nl < 5 || !hub_ci_eq(name + nl - 4, ".APP"))
				continue;
			if (nl - 4 >= (int)sizeof(stem))
				continue;
			memcpy(stem, name, (unsigned)(nl - 4));
			stem[nl - 4] = 0;
			full = hub_join(dir, name);
			if (!full)
				continue;
			mmb_pkg_info_read(full, &pi);
			hub_inst_push(l, full, stem, pi.name ? pi.name : "",
				      pi.version ? pi.version : "");
			mmb_pkg_info_free(&pi);
			G.plat->free(full);
		}
		G.plat->free(ents);
	}
}

/* Case-insensitive match against "id", "id.app" and an INF name that omits
 * the extension. */
static int hub_name_matches(const char *candidate, const char *id)
{
	int n;

	if (!candidate || !id)
		return 0;
	if (hub_ci_eq(candidate, id))
		return 1;
	n = (int)strlen(candidate);
	if (n > 4 && hub_ci_eq(candidate + n - 4, ".APP") &&
	    (int)strlen(id) == n - 4)
	{
		char tmp[128];

		if (n - 4 < (int)sizeof(tmp))
		{
			memcpy(tmp, candidate, (unsigned)(n - 4));
			tmp[n - 4] = 0;
			return hub_ci_eq(tmp, id);
		}
	}
	return 0;
}

static void hub_compute_states(void)
{
	hub_inst_list inst;
	const char *release = mmb_release_version();
	int i;

	memset(&inst, 0, sizeof(inst));
	hub_scan_installed(&inst);
	for (i = 0; i < s_napps; i++)
	{
		hub_app *a = &s_apps[i];
		int j, found = 0;
		int needs_newer;

		if (a->installed_path)
		{
			G.plat->free(a->installed_path);
			a->installed_path = 0;
		}
		if (a->installed_version)
		{
			G.plat->free(a->installed_version);
			a->installed_version = 0;
		}
		for (j = 0; j < inst.n; j++)
		{
			hub_inst_entry *e = &inst.v[j];

			if (hub_name_matches(e->stem, a->id) ||
			    hub_name_matches(e->infname, a->id))
			{
				a->installed_path = hub_strdup(e->path);
				a->installed_version = hub_strdup(e->version);
				if (a->installed_path && a->installed_version)
					found = 1;
				else
				{
					if (a->installed_path)
						G.plat->free(a->installed_path);
					if (a->installed_version)
						G.plat->free(a->installed_version);
					a->installed_path = 0;
					a->installed_version = 0;
				}
				break;
			}
		}

		needs_newer = a->minver && a->minver[0] &&
			      mmb_version_cmp(release, a->minver) < 0;
		if (!found)
			a->state = HUB_NOT_INSTALLED;
		else if (a->version && a->version[0] &&
			 (!a->installed_version[0] ||
			  mmb_version_cmp(a->installed_version, a->version) < 0))
			a->state = HUB_UPDATE;
		else
			a->state = HUB_INSTALLED;
		if (needs_newer)
			a->state = HUB_NEEDS_NEWER;
	}
	hub_inst_free(&inst);
}

/* ---- index loading --------------------------------------------------- */

static const char *hub_json_str(const cJSON *obj, const char *key)
{
	const cJSON *it = cJSON_GetObjectItem(obj, key);

	if (it && cJSON_IsString(it) && it->valuestring)
		return it->valuestring;
	return "";
}

static long hub_json_long(const cJSON *obj, const char *key)
{
	const cJSON *it = cJSON_GetObjectItem(obj, key);

	if (it && cJSON_IsNumber(it))
		return (long)it->valuedouble;
	return 0;
}

static int hub_parse_apps(const cJSON *root, char *err, int errsz)
{
	const cJSON *apps = cJSON_GetObjectItem(root, "apps");
	int n, i;

	if (!apps || !cJSON_IsArray(apps))
		return 0;
	n = cJSON_GetArraySize(apps);
	if (n <= 0)
		return 0;
	s_apps = G.plat->alloc(sizeof(*s_apps) * (unsigned)n);
	if (!s_apps)
	{
		hub_err_out_of_memory(err, errsz);
		return -1;
	}
	memset(s_apps, 0, sizeof(*s_apps) * (unsigned)n);
	s_napps = n;
	for (i = 0; i < n; i++)
	{
		const cJSON *obj = cJSON_GetArrayItem(apps, i);
		hub_app *a = &s_apps[i];
		const cJSON *shots;
		int ns, k;

		a->cat_index = MMB_APP_CATEGORY_OTHER;
		a->state = HUB_NOT_INSTALLED;
		if (!obj || !cJSON_IsObject(obj))
			continue;
		a->id = hub_strdup(hub_json_str(obj, "id"));
		a->file = hub_strdup(hub_json_str(obj, "file"));
		a->title = hub_strdup(hub_json_str(obj, "title"));
		a->author = hub_strdup(hub_json_str(obj, "author"));
		a->category = hub_strdup(hub_json_str(obj, "category"));
		a->version = hub_strdup(hub_json_str(obj, "version"));
		a->summary = hub_strdup(hub_json_str(obj, "summary"));
		a->minver = hub_strdup(hub_json_str(obj, "minver"));
		a->sha256 = hub_strdup(hub_json_str(obj, "sha256"));
		a->updated = hub_strdup(hub_json_str(obj, "updated"));
		a->icon = hub_strdup(hub_json_str(obj, "icon"));
		a->thumb = hub_strdup(hub_json_str(obj, "thumb"));
		a->about = hub_strdup(hub_json_str(obj, "about"));
		if (!a->id || !a->file || !a->title || !a->author ||
		    !a->category || !a->version || !a->summary || !a->minver ||
		    !a->sha256 || !a->updated || !a->icon || !a->thumb ||
		    !a->about)
		{
			hub_err_out_of_memory(err, errsz);
			return -1;
		}
		if (!a->title[0])
			hub_set_str(&a->title, a->id);
		if (a->category[0])
			a->cat_index = mmb_app_category_index(a->category);
		a->size = hub_json_long(obj, "size");
		shots = cJSON_GetObjectItem(obj, "shots");
		ns = (shots && cJSON_IsArray(shots)) ? cJSON_GetArraySize(shots) : 0;
		if (ns > 0)
		{
			a->shots = G.plat->alloc(sizeof(*a->shots) * (unsigned)ns);
			if (!a->shots)
			{
				hub_err_out_of_memory(err, errsz);
				return -1;
			}
			memset(a->shots, 0, sizeof(*a->shots) * (unsigned)ns);
			a->nshots = ns;
			for (k = 0; k < ns; k++)
			{
				const cJSON *sh = cJSON_GetArrayItem(shots, k);

				if (sh && cJSON_IsString(sh))
					a->shots[k] = hub_strdup(sh->valuestring);
				else
					a->shots[k] = hub_strdup("");
				if (!a->shots[k])
				{
					hub_err_out_of_memory(err, errsz);
					return -1;
				}
			}
		}
	}
	return 0;
}

int hub_load_index(char *err, int errsz)
{
	unsigned char *buf = 0;
	unsigned n = 0;
	void *token = 0;
	int status = 0, rc;
	char *url;
	cJSON *root;
	const cJSON *schema;
	int schema_n;

	if (err && errsz > 0)
		err[0] = 0;
	hub_free();
	url = hub_url_join("index.json");
	if (!url)
	{
		hub_err_out_of_memory(err, errsz);
		return -1;
	}
	rc = mmb_url_fetch_ex(url, 0, 0, &buf, &n, &token, &status, err, errsz,
			      0, 0, 0);
	G.plat->free(url);
	if (rc <= 0)
	{
		if (rc == 0)
			hub_set_err(err, errsz, "UNSUPPORTED INDEX: not an http(s) URL");
		else if (err && errsz > 0 && strstr(err, "CANCELLED"))
			hub_set_err(err, errsz, "CANCELLED");
		else if (status > 0)
			hub_set_err(err, errsz, "SERVER ERROR %d", status);
		else
			hub_set_err(err, errsz, "OFFLINE");
		if (token)
			mmb_url_release(token);
		return -1;
	}

	root = (cJSON *)mmb_json_parse_tree((const char *)buf);
	if (!root)
	{
		if (token)
			mmb_url_release(token);
		hub_set_err(err, errsz, "UNSUPPORTED INDEX");
		return -1;
	}
	schema = cJSON_GetObjectItem(root, "schema");
	schema_n = (schema && cJSON_IsNumber(schema)) ? (int)schema->valuedouble : 0;
	if (schema_n != 1)
	{
		if (schema_n > 1)
			hub_set_err(err, errsz,
				    "UNSUPPORTED INDEX: schema %d needs a newer mmcore",
				    schema_n);
		else
			hub_set_err(err, errsz, "UNSUPPORTED INDEX");
		cJSON_Delete(root);
		if (token)
			mmb_url_release(token);
		return -1;
	}
	if (hub_parse_apps(root, err, errsz) != 0)
	{
		cJSON_Delete(root);
		hub_free();
		if (token)
			mmb_url_release(token);
		return -1;
	}
	cJSON_Delete(root);
	if (token)
		mmb_url_release(token);
	hub_compute_states();
	return 0;
}

/* ---- asset LRU cache ------------------------------------------------- */

static void hub_cache_free_entry(hub_cache_entry *e)
{
	if (!e)
		return;
	if (e->key)
		G.plat->free(e->key);
	if (e->data)
		G.plat->free(e->data);
	G.plat->free(e);
}

static unsigned char *hub_copy_bytes(const unsigned char *p, unsigned n)
{
	unsigned char *c = G.plat->alloc(n ? n : 1);

	if (!c)
		return 0;
	if (n)
		memcpy(c, p, n);
	return c;
}

static void hub_cache_evict(void)
{
	while (s_cache_bytes > HUB_CACHE_MAX_BYTES && s_cache)
	{
		hub_cache_entry *prev = 0, *e = s_cache;

		while (e->next)
		{
			prev = e;
			e = e->next;
		}
		if (prev)
			prev->next = 0;
		else
			s_cache = 0;
		if (s_cache_bytes >= e->n)
			s_cache_bytes -= e->n;
		else
			s_cache_bytes = 0;
		hub_cache_free_entry(e);
	}
}

int hub_fetch(const char *relpath, unsigned char **buf, unsigned *n,
	      char *err, int errsz)
{
	hub_cache_entry *e, *prev;
	char *url;
	unsigned char *data = 0;
	unsigned dn = 0;
	void *token = 0;
	int status = 0, rc;

	if (buf)
		*buf = 0;
	if (n)
		*n = 0;
	if (err && errsz > 0)
		err[0] = 0;
	if (!relpath || !relpath[0])
		return 0;

	prev = 0;
	for (e = s_cache; e; prev = e, e = e->next)
	{
		if (strcmp(e->key, relpath) != 0)
			continue;
		if (prev)
		{
			prev->next = e->next;
			e->next = s_cache;
			s_cache = e;
		}
		if (buf)
			*buf = hub_copy_bytes(e->data, e->n);
		if (n)
			*n = e->n;
		if (buf && !*buf)
		{
			hub_err_out_of_memory(err, errsz);
			return -1;
		}
		return 0;
	}

	url = hub_url_join(relpath);
	if (!url)
	{
		hub_err_out_of_memory(err, errsz);
		return -1;
	}
	rc = mmb_url_fetch_ex(url, 0, 0, &data, &dn, &token, &status, err, errsz,
			      0, 0, 0);
	G.plat->free(url);
	if (rc <= 0)
	{
		if (rc == 0)
			hub_set_err(err, errsz, "UNSUPPORTED URL");
		else if (err && errsz > 0 && strstr(err, "CANCELLED"))
			hub_set_err(err, errsz, "CANCELLED");
		else if (status > 0)
			hub_set_err(err, errsz, "SERVER ERROR %d", status);
		else
			hub_set_err(err, errsz, "OFFLINE");
		if (token)
			mmb_url_release(token);
		return -1;
	}

	e = G.plat->alloc(sizeof(*e));
	if (e)
	{
		memset(e, 0, sizeof(*e));
		e->key = hub_strdup(relpath);
		e->data = data;
		e->n = dn;
		if (!e->key)
		{
			if (e->data)
				G.plat->free(e->data);
			G.plat->free(e);
			e = 0;
		}
	}
	if (e)
	{
		e->next = s_cache;
		s_cache = e;
		s_cache_bytes += dn;
		hub_cache_evict();
	}
	if (buf)
		*buf = hub_copy_bytes(data, dn);
	if (n)
		*n = dn;
	if (buf && !*buf)
	{
		hub_err_out_of_memory(err, errsz);
		return -1;
	}
	return 0;
}

/* ---- install / remove ------------------------------------------------ */

static char *hub_install_target(const char *id, char *err, int errsz)
{
	const char *p = G.opt.app_path;

	while (p && *p)
	{
		const char *e = p;
		char dir[160];
		int dn;
		char *target;

		while (*e && *e != ';')
			e++;
		dn = (int)(e - p);
		if (dn > (int)sizeof(dir) - 1)
			dn = (int)sizeof(dir) - 1;
		memcpy(dir, p, (unsigned)dn);
		dir[dn] = 0;
		hub_trim(dir);
		p = *e ? e + 1 : e;
		if (!dir[0])
			continue;
		if (mmb_vfs_exists(dir))
		{
			if (!mmb_vfs_isdir(dir) || mmb_vfs_readonly_path(dir))
				continue;
		}
		else if (!hub_ci_eq(dir, MMB_APP_PATH_DEFAULT))
			continue;
		else if (mmb_vfs_mkdir(dir) != 0)
			continue;
		target = hub_join(dir, id);
		if (target)
		{
			char *with_ext = G.plat->alloc((unsigned)strlen(target) + 5);

			if (with_ext)
			{
				strcpy(with_ext, target);
				strcat(with_ext, ".APP");
			}
			G.plat->free(target);
			target = with_ext;
		}
		if (target)
			return target;
	}
	hub_set_err(err, errsz, "NO WRITABLE APP FOLDER");
	return 0;
}

int hub_install(int i, hub_progress_fn cb, void *ctx, char *err, int errsz)
{
	hub_app *a;
	char *url = 0, *target = 0, *tmp = 0;
	unsigned char *data = 0;
	unsigned n = 0;
	void *token = 0;
	int status = 0, rc;
	unsigned char digest[32];
	char hex[65];

	if (err && errsz > 0)
		err[0] = 0;
	if (i < 0 || i >= s_napps)
	{
		hub_set_err(err, errsz, "NOT FOUND");
		return -1;
	}
	a = &s_apps[i];
	if (a->state == HUB_NEEDS_NEWER)
	{
		hub_set_err(err, errsz, "NEEDS MMCORE %s",
			    a->minver ? a->minver : "");
		return -1;
	}
	if (a->installed_path)
		target = hub_strdup(a->installed_path);
	else
		target = hub_install_target(a->id ? a->id : "", err, errsz);
	if (!target)
		return -1;
	url = hub_url_join(a->file ? a->file : "");
	if (!url)
	{
		G.plat->free(target);
		hub_err_out_of_memory(err, errsz);
		return -1;
	}
	rc = mmb_url_fetch_ex(url, 0, 0, &data, &n, &token, &status, err, errsz,
			      cb, ctx, (unsigned)(a->size > 0 ? a->size : 0));
	G.plat->free(url);
	if (rc <= 0)
	{
		if (rc == 0)
			hub_set_err(err, errsz, "UNSUPPORTED URL");
		else if (err && errsz > 0 && strstr(err, "CANCELLED"))
			hub_set_err(err, errsz, "CANCELLED");
		else if (status > 0)
			hub_set_err(err, errsz, "SERVER ERROR %d", status);
		else
			hub_set_err(err, errsz, "OFFLINE");
		if (token)
			mmb_url_release(token);
		G.plat->free(target);
		return -1;
	}

	if (a->size > 0 && (long)n != a->size)
	{
		hub_set_err(err, errsz, "CHECKSUM FAILED");
		mmb_url_release(token);
		G.plat->free(target);
		return -1;
	}
	if (a->sha256 && a->sha256[0])
	{
		mmb_sha256(data, n, digest);
		mmb_sha256_hex(digest, hex);
		if (!hub_ci_eq(hex, a->sha256))
		{
			hub_set_err(err, errsz, "CHECKSUM FAILED");
			mmb_url_release(token);
			G.plat->free(target);
			return -1;
		}
	}

	tmp = G.plat->alloc((unsigned)strlen(target) + 5);
	if (!tmp)
	{
		mmb_url_release(token);
		G.plat->free(target);
		hub_err_out_of_memory(err, errsz);
		return -1;
	}
	strcpy(tmp, target);
	strcat(tmp, ".tmp");
	if (mmb_vfs_write(tmp, data, n, 0) != 0)
	{
		mmb_vfs_kill(tmp);
		mmb_url_release(token);
		G.plat->free(tmp);
		G.plat->free(target);
		hub_set_err(err, errsz, "WRITE FAILED");
		return -1;
	}
	if (mmb_vfs_exists(target))
		mmb_vfs_kill(target);
	if (mmb_vfs_rename(tmp, target) != 0)
	{
		mmb_vfs_kill(tmp);
		mmb_url_release(token);
		G.plat->free(tmp);
		G.plat->free(target);
		hub_set_err(err, errsz, "WRITE FAILED");
		return -1;
	}
	mmb_url_release(token);
	G.plat->free(tmp);
	G.plat->free(target);
	hub_compute_states();
	return 0;
}

int hub_remove(int i, char *err, int errsz)
{
	hub_app *a;

	if (err && errsz > 0)
		err[0] = 0;
	if (i < 0 || i >= s_napps)
	{
		hub_set_err(err, errsz, "NOT FOUND");
		return -1;
	}
	a = &s_apps[i];
	if (!a->installed_path)
	{
		hub_set_err(err, errsz, "NOT INSTALLED");
		return -1;
	}
	if (mmb_vfs_kill(a->installed_path) != 0)
	{
		hub_set_err(err, errsz, "REMOVE FAILED");
		return -1;
	}
	hub_compute_states();
	return 0;
}

/* ---- commands -------------------------------------------------------- */

static const char *hub_state_name(int state)
{
	switch (state)
	{
	case HUB_INSTALLED: return "INSTALLED";
	case HUB_UPDATE: return "UPDATE";
	case HUB_NEEDS_NEWER: return "NEEDS NEWER";
	default: return "NOT INSTALLED";
	}
}

static void hub_fail(const char *err)
{
	char msg[192];

	snprintf(msg, sizeof(msg), "?HUB: %s", err && err[0] ? err : "failed");
	mmb_error(msg);
}

static void hub_print_line(const char *s)
{
	mmb_out(s);
	mmb_out("\n");
}

static void hub_print_apps(void)
{
	int i;

	for (i = 0; i < s_napps; i++)
	{
		hub_app *a = &s_apps[i];
		char line[256];

		snprintf(line, sizeof(line), "%-16s  %-8s  %s",
			 a->id ? a->id : "",
			 a->version && a->version[0] ? a->version : "-",
			 hub_state_name(a->state));
		hub_print_line(line);
	}
	mmb_out_flush();
}

static int hub_find_id(const char *id)
{
	int i;

	for (i = 0; i < s_napps; i++)
		if (hub_ci_eq(s_apps[i].id, id))
			return i;
	return -1;
}

static int hub_load_or_fail(void)
{
	char err[192];

	if (hub_load_index(err, sizeof(err)) != 0)
	{
		hub_fail(err);
		return -1;
	}
	return 0;
}

#if defined(MMB_PLATFORM_POSIX)
static unsigned hub_test_cancel_after(void)
{
	const char *s = getenv("MMCORE_HUB_CANCEL_AFTER");

	if (!s || !*s)
		return 0;
	return (unsigned)strtoul(s, 0, 10);
}
#else
static unsigned hub_test_cancel_after(void)
{
	return 0;
}
#endif

typedef struct hub_cmd_progress {
	const char *id;
	int pct;
	int printed_start;
} hub_cmd_progress;

static int hub_cmd_progress_cb(unsigned done, unsigned total, void *ctx)
{
	hub_cmd_progress *p = (hub_cmd_progress *)ctx;
	char line[128];
	int pct = total ? (int)((uint64_t)done * 100u / total) : 0;
	unsigned cancel_after = hub_test_cancel_after();

	if (cancel_after && done >= cancel_after)
		return 1;
	if (!p->printed_start)
	{
		snprintf(line, sizeof(line), "HUB: %s 0/%u", p->id, total);
		hub_print_line(line);
		mmb_out_flush();
		p->printed_start = 1;
	}
	if (total && pct >= p->pct + 25 && done != total)
	{
		p->pct = pct - (pct % 25);
		snprintf(line, sizeof(line), "HUB: %s %u/%u", p->id, done, total);
		hub_print_line(line);
		mmb_out_flush();
	}
	return 0;
}

static void hub_install_one(int i)
{
	hub_app *a = &s_apps[i];
	char err[192];
	hub_cmd_progress p;

	memset(&p, 0, sizeof(p));
	p.id = a->id ? a->id : "";
	p.pct = 0;
	if (hub_install(i, hub_cmd_progress_cb, &p, err, sizeof(err)) != 0)
		hub_fail(err);
	{
		char line[160];

		snprintf(line, sizeof(line), "HUB: %s %s",
			 a->id ? a->id : "", hub_state_name(a->state));
		hub_print_line(line);
		mmb_out_flush();
	}
}

static void hub_cmd_list(void)
{
	if (hub_load_or_fail() != 0)
		return;
	hub_print_apps();
}

static void hub_cmd_install(void)
{
	mmb_val v = mmb_expr();
	int i;

	if (v.type != T_STR)
		mmb_error("?TYPE MISMATCH");
	if (hub_load_or_fail() != 0)
		return;
	i = hub_find_id(v.s ? v.s : "");
	if (i < 0)
		hub_fail("NOT FOUND");
	hub_install_one(i);
}

static void hub_cmd_update(void)
{
	int i, any = 0;

	if (hub_load_or_fail() != 0)
		return;
	for (i = 0; i < s_napps; i++)
	{
		if (s_apps[i].state != HUB_UPDATE)
			continue;
		any = 1;
		hub_install_one(i);
	}
	if (!any)
	{
		hub_print_line("HUB: nothing to update");
		mmb_out_flush();
	}
}

static void hub_cmd_remove(void)
{
	mmb_val v = mmb_expr();
	char err[128];
	int i;

	if (v.type != T_STR)
		mmb_error("?TYPE MISMATCH");
	if (hub_load_or_fail() != 0)
		return;
	i = hub_find_id(v.s ? v.s : "");
	if (i < 0)
		hub_fail("NOT FOUND");
	if (hub_remove(i, err, sizeof(err)) != 0)
		hub_fail(err);
	{
		char line[160];

		snprintf(line, sizeof(line), "HUB: %s removed", v.s);
		hub_print_line(line);
		mmb_out_flush();
	}
}

static void hub_cmd_fetch(void)
{
	mmb_val v = mmb_expr();
	unsigned char *buf = 0;
	unsigned n = 0;
	char err[160];
	char line[64];

	if (v.type != T_STR)
		mmb_error("?TYPE MISMATCH");
	if (hub_load_or_fail() != 0)
		return;
	if (hub_fetch(v.s ? v.s : "", &buf, &n, err, sizeof(err)) != 0)
		hub_fail(err);
	if (buf)
		G.plat->free(buf);
	snprintf(line, sizeof(line), "FETCH %u", n);
	hub_print_line(line);
	mmb_out_flush();
}

void mmb_cmd_hub(void)
{
	mmb_skip_sp();
	if (*G.p == 0 || *G.p == ':' || *G.p == '\'')
	{
		hub_cmd_list();
		return;
	}
	if (mmb_match("LIST"))
		hub_cmd_list();
	else if (mmb_match("INSTALL"))
		hub_cmd_install();
	else if (mmb_match("UPDATE"))
		hub_cmd_update();
	else if (mmb_match("REMOVE"))
		hub_cmd_remove();
	else if (mmb_match("FETCH"))
		hub_cmd_fetch();
	else
		mmb_syntax();
}
