#include "mmb_priv.h"

static char *need_path(void)
{
	static char buf[128];
	mmb_val v;
	mmb_skip_sp();
	v = mmb_expr();
	if (v.type != T_STR)
		mmb_syntax();
	strncpy(buf, v.s, sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = 0;
	return buf;
}

void mmb_cmd_chdir(void)
{
	char *p = need_path();
	if (!G.running && p[0] && (p[0] == 'B' || p[0] == 'b') && p[1] == ':')
		mmb_error("?DRIVE");
	if (mmb_vfs_chdir(p) != 0)
	{
		if (p[1] == ':' && (p[2] == 0 || p[2] == '/' || p[2] == '\\'))
			mmb_error("?DRIVE");
		mmb_error("?DIRECTORY");
	}
}

void mmb_cmd_mkdir(void)
{
	char *p = need_path();
	if (mmb_vfs_readonly_path(p))
		mmb_error("?READ ONLY");
	if (mmb_vfs_mkdir(p) != 0)
		mmb_error("?DIRECTORY");
}

void mmb_cmd_rmdir(void)
{
	char *p = need_path();
	if (mmb_vfs_readonly_path(p))
		mmb_error("?READ ONLY");
	if (mmb_vfs_rmdir(p) != 0)
		mmb_error("?DIRECTORY");
}

void mmb_cmd_kill(void)
{
	char *p = need_path();
	if (mmb_vfs_readonly_path(p))
		mmb_error("?READ ONLY");
	if (mmb_vfs_kill(p) != 0)
		mmb_error("?FILE NOT FOUND");
}

void mmb_cmd_copy(void)
{
	char src[128], dst[128];
	strncpy(src, need_path(), sizeof(src) - 1);
	mmb_skip_sp();
	if (mmb_match("TO"))
		;
	else
	{
		mmb_skip_sp();
		if (*G.p == ',')
			G.p++;
	}
	strncpy(dst, need_path(), sizeof(dst) - 1);
	if (mmb_vfs_readonly_path(dst))
		mmb_error("?READ ONLY");
	if (mmb_vfs_copy(src, dst) != 0)
		mmb_error("?FILE");
}

void mmb_cmd_xfer(void)
{
	char path[128];
	int n;
	unsigned char *buf = 0;
	mmb_val v;

	v = mmb_expr();
	if (v.type != T_STR)
		mmb_syntax();
	strncpy(path, v.s, sizeof(path) - 1);
	path[sizeof(path) - 1] = 0;
	mmb_skip_sp();
	if (*G.p == ',')
		G.p++;
	n = (int)mmb_as_int(mmb_expr());
	if (n < 0 || n > 8 * 1024 * 1024)
		mmb_error("?INVALID");
	if (mmb_vfs_readonly_path(path))
		mmb_error("?READ ONLY");
	if (!G.plat || !G.plat->read_raw)
		mmb_error("?UNSUPPORTED");
	if (n > 0)
	{
		if (!G.plat->alloc)
			mmb_error("?OUT OF MEMORY");
		buf = G.plat->alloc((unsigned)n);
		if (!buf)
			mmb_error("?OUT OF MEMORY");
	}
	mmb_console_write("<<XFER>>\n");
	if (n > 0 && G.plat->read_raw(buf, (unsigned)n) != 0)
	{
		G.plat->free(buf);
		mmb_error("?FILE");
	}
	if (mmb_vfs_write(path, buf ? (char *)buf : "", (unsigned)n, 0) != 0)
	{
		if (buf)
			G.plat->free(buf);
		mmb_error("?FILE");
	}
	if (buf)
		G.plat->free(buf);
}

void mmb_cmd_name(void)
{
	char src[128], dst[128];
	strncpy(src, need_path(), sizeof(src) - 1);
	mmb_skip_sp();
	if (!mmb_match("AS") && !mmb_match("TO"))
	{
		if (*G.p == ',')
			G.p++;
	}
	strncpy(dst, need_path(), sizeof(dst) - 1);
	if (mmb_vfs_readonly_path(dst) || mmb_vfs_readonly_path(src))
		mmb_error("?READ ONLY");
	if (mmb_vfs_rename(src, dst) != 0)
		mmb_error("?FILE");
}

/* Prompt-only CAT: write the raw contents of a file to the console. The
 * argument is a quoted or bare path; no switches are understood. */
void mmb_cmd_cat_file(const char *arg)
{
	char path[128];
	char buf[512];
	const char *p = arg;
	int n = 0, sz;
	unsigned pos = 0;

	if (*p == '"')
	{
		p++;
		while (*p && *p != '"' && n < (int)sizeof(path) - 1)
			path[n++] = *p++;
	}
	else
	{
		while (*p && *p != ' ' && *p != '\t' && n < (int)sizeof(path) - 1)
			path[n++] = *p++;
	}
	path[n] = 0;
	if (!n)
		mmb_error("?FILE NOT FOUND");

	sz = mmb_vfs_size(path);
	if (sz < 0)
		mmb_error("?FILE NOT FOUND");

	while (pos < (unsigned)sz)
	{
		unsigned want = (unsigned)sizeof(buf) - 1;
		unsigned got = 0, i;
		if (want > (unsigned)sz - pos)
			want = (unsigned)sz - pos;
		if (mmb_vfs_read_at(path, pos, buf, want, &got) != 0)
			mmb_error("?FILE");
		if (!got)
			break;
		pos += got;
		for (i = 0; i < got; i++)
		{
			if (G.outn >= MMB_OUT_LEN - 1)
				mmb_out_flush();
			G.out[G.outn++] = buf[i];
		}
		G.out[G.outn] = 0;
	}
}

#define DIR_LIST_MAX 4096
#define DIR_PATH_MAX 160
#define DIR_DEPTH_MAX 8
#define DIR_ENT_MAX 512
/* Cap on the accumulated results of a recursive search. Each result is a
 * structured mmb_dirent (name + type + size), so this bound is explicit and a
 * tree larger than it reports "... more" instead of silently dropping entries
 * the way the old fixed 4096-byte newline buffer did. */
#define DIR_RESULT_MAX 4096

static int dir_first;

static void dir_put(const char *s)
{
	if (!dir_first)
		mmb_out("\n");
	dir_first = 0;
	mmb_out(s);
	if (G.outn > MMB_OUT_LEN - 256)
		mmb_out_flush();
}

/* Consume any /W and /S switches, which may appear before or after the spec. */
static void dir_switches(int *wide, int *recurse)
{
	for (;;)
	{
		mmb_skip_sp();
		if (*G.p != '/')
			break;
		G.p++;
		while (mmb_is_ident(*G.p))
		{
			char c = *G.p++;
			if (c >= 'a' && c <= 'z')
				c -= 32;
			if (c == 'W')
				*wide = 1;
			else if (c == 'S')
				*recurse = 1;
			else
				mmb_syntax();
		}
	}
}

/* Read a quoted string literal but stop at its closing quote, so trailing
 * switches (DIR "*.BAS" /W) are not swallowed by the expression parser. */
static void dir_read_literal(char *out, int outsz)
{
	int n = 0;
	G.p++; /* opening quote */
	while (*G.p)
	{
		if (*G.p == '"')
		{
			if (G.p[1] == '"')
			{
				if (n < outsz - 1)
					out[n++] = '"';
				G.p += 2;
				continue;
			}
			break;
		}
		if (n < outsz - 1)
			out[n++] = *G.p;
		G.p++;
	}
	if (*G.p == '"')
		G.p++;
	out[n] = 0;
}

/* Wide listing: sorted names only, packed into dynamic fixed-width columns. */
static void dir_wide(char *list)
{
	char *ptrs[DIR_ENT_MAX];
	char line[DIR_LIST_MAX];
	int n = 0, i, maxlen = 0, cols, rows, r, c;
	char *p = list;
	if (!list[0])
	{
		dir_put("(empty)");
		return;
	}
	while (*p && n < DIR_ENT_MAX)
	{
		ptrs[n++] = p;
		while (*p && *p != '\n')
			p++;
		if (*p == '\n')
			*p++ = 0;
	}
	if (n == 0)
	{
		dir_put("(empty)");
		return;
	}
	for (i = 0; i < n; i++)
	{
		int l = (int)strlen(ptrs[i]);
		if (l > maxlen)
			maxlen = l;
	}
	cols = (G.plat && G.plat->video_cols ? G.plat->video_cols() : 80) / (maxlen + 2);
	if (cols < 1)
		cols = 1;
	if (cols > DIR_ENT_MAX)
		cols = DIR_ENT_MAX;
	rows = (n + cols - 1) / cols;
	for (r = 0; r < rows; r++)
	{
		int len = 0;
		for (c = 0; c < cols; c++)
		{
			int idx = r * cols + c;
			int l, pad;
			if (idx >= n)
				break;
			l = (int)strlen(ptrs[idx]);
			if (len + l + 1 >= (int)sizeof(line))
				break;
			memcpy(line + len, ptrs[idx], (unsigned)l);
			len += l;
			pad = (c + 1 < cols && idx + 1 < n) ? maxlen - l + 2 : 0;
			while (pad-- > 0 && len < (int)sizeof(line) - 1)
				line[len++] = ' ';
		}
		while (len > 0 && line[len - 1] == ' ')
			len--;
		line[len] = 0;
		dir_put(line);
	}
}

static void dir_join(char *dst, int dstsz, const char *a, const char *sep, const char *b)
{
	const char *parts[3];
	int i, n = 0;
	parts[0] = a;
	parts[1] = sep;
	parts[2] = b;
	for (i = 0; i < 3; i++)
	{
		const char *s = parts[i];
		if (!s)
			continue;
		while (*s && n < dstsz - 1)
			dst[n++] = *s++;
	}
	dst[n] = 0;
}

/* Accumulated results of a recursive search (DIR /S). Each result is a
 * structured mmb_dirent whose `name` is the path relative to the search root,
 * so the long listing prints sizes straight from the directory scan with no
 * per-result mmb_vfs_size()/f_stat(). The list grows on the heap up to
 * DIR_RESULT_MAX entries; anything beyond sets `truncated` so the caller can
 * print a "... more" note. */
typedef struct {
	mmb_dirent *ents;
	int n;
	int cap;
	int truncated;
} dir_list;

static void dir_list_add(dir_list *lst, const char *name, int is_dir, int size)
{
	mmb_dirent *e;
	if (lst->n >= DIR_RESULT_MAX)
	{
		lst->truncated = 1;
		return;
	}
	if (lst->n >= lst->cap)
	{
		int ncap = lst->cap ? lst->cap * 2 : 64;
		mmb_dirent *nb;
		if (ncap > DIR_RESULT_MAX)
			ncap = DIR_RESULT_MAX;
		nb = G.plat && G.plat->alloc
			     ? (mmb_dirent *)G.plat->alloc(
				       (unsigned)ncap * sizeof(mmb_dirent))
			     : 0;
		if (!nb)
		{
			lst->truncated = 1;
			return;
		}
		if (lst->ents)
		{
			memcpy(nb, lst->ents,
			       (unsigned)lst->n * sizeof(mmb_dirent));
			G.plat->free(lst->ents);
		}
		lst->ents = nb;
		lst->cap = ncap;
	}
	e = &lst->ents[lst->n++];
	strncpy(e->name, name, MMB_DIRENT_NAME - 1);
	e->name[MMB_DIRENT_NAME - 1] = 0;
	e->is_dir = is_dir;
	e->size = size;
}

static void dir_list_free(dir_list *lst)
{
	if (lst->ents)
		G.plat->free(lst->ents);
	lst->ents = 0;
	lst->n = 0;
	lst->cap = 0;
}

/* Long listing straight from a structured listing (#666): the entry already
 * carries the size, so no per-file mmb_vfs_size() (an f_stat() over USB on
 * Circle) is needed. A folder with more entries than the caller's cap sets
 * `truncated`, which gets a trailing note instead of the silent cut the old
 * newline listing had. */
static void dir_long_entries(const mmb_dirent *ents, int n, int truncated,
			     const char *more)
{
	int i, namecol = 4;
	const int sizecol = 10;
	if (n <= 0)
	{
		dir_put("(empty)");
		return;
	}
	for (i = 0; i < n; i++)
	{
		int l = (int)strlen(ents[i].name) + (ents[i].is_dir ? 1 : 0);
		if (l > namecol)
			namecol = l;
	}
	if (namecol > 60)
		namecol = 60;
	for (i = 0; i < n; i++)
	{
		char name[MMB_DIRENT_NAME + 2];
		char line[MMB_DIRENT_NAME + 32];
		char szs[16];
		int l, pos, pad;
		strcpy(name, ents[i].name);
		if (ents[i].is_dir)
			strcat(name, "/");
		l = (int)strlen(name);
		if (ents[i].is_dir)
			strcpy(szs, "<DIR>");
		else
		{
			int sz = ents[i].size < 0 ? 0 : ents[i].size;
			sprintf(szs, "%u", (unsigned)sz);
		}
		memcpy(line, name, (unsigned)l);
		pos = l;
		for (pad = namecol - l; pad > 0 && pos < (int)sizeof(line) - 1; pad--)
			line[pos++] = ' ';
		line[pos++] = ' ';
		for (pad = sizecol - (int)strlen(szs); pad > 0 && pos < (int)sizeof(line) - 1; pad--)
			line[pos++] = ' ';
		{
			const char *s = szs;
			while (*s && pos < (int)sizeof(line) - 1)
				line[pos++] = *s++;
		}
		line[pos] = 0;
		dir_put(line);
	}
	if (truncated)
		dir_put(more ? more : "... more");
}

/* Wide listing from structured entries: rebuild the newline list on the heap so
 * a folder is not silently cut at DIR_LIST_MAX. Truncation is reported by the
 * caller from the entries count. */
static void dir_wide_entries(const mmb_dirent *ents, int n, int truncated,
			     const char *more)
{
	char *list, *w;
	unsigned need = 1;
	int i;
	if (n <= 0)
	{
		dir_put("(empty)");
		return;
	}
	for (i = 0; i < n; i++)
		need += (unsigned)strlen(ents[i].name) +
			(ents[i].is_dir ? 1u : 0u) + 1u;
	list = G.plat && G.plat->alloc ? (char *)G.plat->alloc(need) : 0;
	if (!list)
	{
		dir_long_entries(ents, n, truncated, more);
		return;
	}
	w = list;
	for (i = 0; i < n; i++)
	{
		int l = (int)strlen(ents[i].name);
		memcpy(w, ents[i].name, (unsigned)l);
		w += l;
		if (ents[i].is_dir)
			*w++ = '/';
		*w++ = (i + 1 < n) ? '\n' : 0;
	}
	dir_wide(list);
	G.plat->free(list);
	if (truncated)
		dir_put(more ? more : "... more");
}

/* Recursive search (DIR /S): every matching entry under dirspec, with a path
 * prefix relative to the search root. Each directory is scanned once with the
 * structured entries API, so a big folder is not cut against a small newline
 * buffer and each result already carries its size. Results append to `res`
 * depth-first, folders first, matching the on-disk listing order. */
static void dir_search(const char *dirspec, const char *prefix, const char *glob,
		       int depth, dir_list *res)
{
	mmb_dirent *ents;
	int n, truncated = 0, i;
	if (depth > DIR_DEPTH_MAX)
		return;
	ents = G.plat && G.plat->alloc
		       ? (mmb_dirent *)G.plat->alloc(
				 (unsigned)(DIR_ENT_MAX * sizeof(mmb_dirent)))
		       : 0;
	if (!ents)
	{
		res->truncated = 1;
		return;
	}
	n = mmb_vfs_list_entries(dirspec && dirspec[0] ? dirspec : 0, ents,
				 DIR_ENT_MAX, &truncated);
	if (n < 0)
	{
		G.plat->free(ents);
		return;
	}
	if (truncated)
		res->truncated = 1;
	for (i = 0; i < n; i++)
	{
		char name[MMB_DIRENT_NAME];
		char child[DIR_PATH_MAX];
		char disp[DIR_PATH_MAX];
		strncpy(name, ents[i].name, sizeof(name) - 1);
		name[sizeof(name) - 1] = 0;
		if (!name[0])
			continue;
		if (prefix && prefix[0])
			dir_join(disp, sizeof(disp), prefix, "/", name);
		else
			dir_join(disp, sizeof(disp), name, 0, 0);
		if (ents[i].is_dir)
		{
			size_t dl = dirspec ? strlen(dirspec) : 0;
			if (dl > 0 && dirspec[dl - 1] == '/')
				dir_join(child, sizeof(child), dirspec, "", name);
			else if (dl > 0)
				dir_join(child, sizeof(child), dirspec, "/", name);
			else
				dir_join(child, sizeof(child), name, 0, 0);
			if (mmb_glob_match(name, glob))
				dir_list_add(res, disp, 1, -1);
			dir_search(child, disp, glob, depth + 1, res);
		}
		else if (mmb_glob_match(name, glob))
			dir_list_add(res, disp, 0, ents[i].size);
	}
	G.plat->free(ents);
}

/* Split a search spec into a directory and a glob. A path naming an existing
 * directory searches that directory with glob "*"; otherwise the final path
 * component is the glob. */
static void dir_split_spec(const char *spec, char *dir, int dirsz, char *glob, int globsz)
{
	const char *slash = 0;
	const char *q;
	dir[0] = 0;
	glob[0] = 0;
	if (!spec || !spec[0])
		return;
	if (mmb_vfs_isdir(spec))
	{
		strncpy(dir, spec, (size_t)dirsz - 1);
		dir[dirsz - 1] = 0;
		return;
	}
	for (q = spec; *q; q++)
		if (*q == '/')
			slash = q;
	if (!slash)
	{
		strncpy(glob, spec, (size_t)globsz - 1);
		glob[globsz - 1] = 0;
		return;
	}
	strncpy(glob, slash + 1, (size_t)globsz - 1);
	glob[globsz - 1] = 0;
	if (slash == spec)
	{
		strncpy(dir, "/", (size_t)dirsz - 1);
		dir[dirsz - 1] = 0;
	}
	else
	{
		int n = (int)(slash - spec);
		if (n >= dirsz)
			n = dirsz - 1;
		memcpy(dir, spec, (unsigned)n);
		dir[n] = 0;
	}
}

void mmb_cmd_files(const char *kw)
{
	char spec[128];
	int wide = 0, recurse = 0;
	(void)kw;
	spec[0] = 0;
	dir_first = 1;
	mmb_skip_sp();
	dir_switches(&wide, &recurse);
	if (*G.p == '"')
		dir_read_literal(spec, sizeof(spec));
	else if (*G.p && *G.p != ':' && *G.p != '\'')
	{
		mmb_val v = mmb_expr();
		if (v.type == T_STR)
			strncpy(spec, v.s, sizeof(spec) - 1);
	}
	spec[sizeof(spec) - 1] = 0;
	dir_switches(&wide, &recurse);
	if (recurse)
	{
		char dir[128], glob[128];
		dir_list res;
		res.ents = 0;
		res.n = 0;
		res.cap = 0;
		res.truncated = 0;
		dir_split_spec(spec[0] ? spec : 0, dir, sizeof(dir), glob, sizeof(glob));
		dir_search(dir, "", glob, 0, &res);
		if (wide)
			dir_wide_entries(res.ents, res.n, res.truncated,
					 "... more (4096 max)");
		else
			dir_long_entries(res.ents, res.n, res.truncated,
					 "... more (4096 max)");
		dir_list_free(&res);
		return;
	}
	{
		mmb_dirent *ents;
		int n, truncated = 0;
		/* One structured scan yields name, type and size (folders first),
		 * so a long listing no longer stats every file (#666). */
		ents = G.plat && G.plat->alloc
			       ? (mmb_dirent *)G.plat->alloc(
					 (unsigned)(DIR_ENT_MAX * sizeof(mmb_dirent)))
			       : 0;
		if (!ents)
			mmb_error("?OUT OF MEMORY");
		n = mmb_vfs_list_entries(spec[0] ? spec : 0, ents, DIR_ENT_MAX,
					 &truncated);
		if (n < 0)
		{
			G.plat->free(ents);
			mmb_error("?DRIVE");
		}
		if (wide)
			dir_wide_entries(ents, n, truncated, 0);
		else
			dir_long_entries(ents, n, truncated, 0);
		G.plat->free(ents);
	}
}

int mmb_file_is_tcp(int fn)
{
	return fn >= 1 && fn <= MMB_MAX_FILES && G.files[fn].open &&
	       G.files[fn].kind == MMB_FK_TCP;
}

int mmb_tcp_any_open(void)
{
	int i;
	for (i = 1; i <= MMB_MAX_FILES; i++)
		if (mmb_file_is_tcp(i))
			return i;
	return 0;
}

/* The TCP client is a single machine-wide socket shared by TERM, CONNECT and
 * OPEN ... AS #, so ownership is tracked per console rather than per console's
 * file table (#778). Without it a second console would reset the connection
 * out from under the first. -1 means no console owns the socket. */
static int s_tcp_owner = -1;

int mmb_tcp_owner(void)
{
	return s_tcp_owner;
}

int mmb_tcp_claim(void)
{
	if (s_tcp_owner >= 0 && s_tcp_owner != g_console)
		return -1;
	s_tcp_owner = g_console;
	return 0;
}

void mmb_tcp_release(void)
{
	s_tcp_owner = -1;
}

char *mmb_tcp_in_use_text(char *buf)
{
	int owner = s_tcp_owner < 0 ? g_console : s_tcp_owner;
	sprintf(buf, "?IN USE: TCP connection open on console %d", owner + 1);
	return buf;
}

void mmb_tcp_in_use(void)
{
	char e[64];
	mmb_error(mmb_tcp_in_use_text(e));
}

void mmb_file_close_n(int fn)
{
	if (fn < 1 || fn > MMB_MAX_FILES || !G.files[fn].open)
		return;
	if (G.files[fn].kind == MMB_FK_TCP)
	{
		mmb_net_tcp_close();
		mmb_tcp_release();
	}
	memset(&G.files[fn], 0, sizeof(G.files[fn]));
	G.files[fn].ungot = -1;
}

void mmb_close_tcp_files(void)
{
	int i;
	for (i = 1; i <= MMB_MAX_FILES; i++)
		if (mmb_file_is_tcp(i))
			mmb_file_close_n(i);
	mmb_tcp_release();
}

int mmb_file_read(int fn, char *buf, int nch)
{
	unsigned got = 0;
	if (fn < 1 || fn > MMB_MAX_FILES || !G.files[fn].open)
		mmb_error("?FILE");
	if (nch < 0)
		nch = 0;
	if (G.files[fn].kind == MMB_FK_TCP)
	{
		int n = 0;
		if (G.files[fn].mode == 1)
			mmb_error("?FILE");
		mmb_net_tcp_status();
		if (G.files[fn].ungot >= 0 && nch > 0)
		{
			buf[n++] = (char)G.files[fn].ungot;
			G.files[fn].ungot = -1;
		}
		if (n < nch)
		{
			int r = mmb_net_tcp_recv(buf + n, (unsigned)(nch - n));
			if (r > 0)
				n += r;
		}
		buf[n] = 0;
		return n;
	}
	if (mmb_vfs_read_at(G.files[fn].path, (unsigned)G.files[fn].pos,
			    buf, (unsigned)nch, &got) != 0)
		got = 0;
	G.files[fn].pos += (int)got;
	buf[got] = 0;
	return (int)got;
}

void mmb_file_write(int fn, const char *buf, unsigned n)
{
	if (fn < 1 || fn > MMB_MAX_FILES || !G.files[fn].open)
		return;
	if (G.files[fn].kind == MMB_FK_TCP)
	{
		if (G.files[fn].mode == 0)
			mmb_error("?FILE");
		mmb_net_tcp_status();
		if (n && mmb_net_tcp_send(buf, n) < 0)
			mmb_error("?FILE");
		return;
	}
	if (mmb_vfs_readonly_path(G.files[fn].path))
		mmb_error("?READ ONLY");
	if (mmb_vfs_write(G.files[fn].path, buf, n, 1) != 0)
		mmb_error("?FILE");
}

static int is_tcp_spec(const char *s)
{
	return s && (s[0] == 'T' || s[0] == 't') &&
	       (s[1] == 'C' || s[1] == 'c') &&
	       (s[2] == 'P' || s[2] == 'p') && s[3] == ':';
}

static int parse_tcp_host_port(const char *s, char *host, int hostsz, int *port)
{
	const char *colon = 0;
	int n, p, i;

	s += 4;
	if (!s[0])
		return -1;
	for (i = 0; s[i]; i++)
		if (s[i] == ':')
			colon = s + i;
	if (!colon || colon == s || !colon[1])
		return -1;
	n = (int)(colon - s);
	if (n >= hostsz)
		n = hostsz - 1;
	for (i = 0; i < n; i++)
		host[i] = s[i];
	host[n] = 0;
	p = 0;
	for (i = 1; colon[i]; i++)
	{
		if (colon[i] < '0' || colon[i] > '9')
			return -1;
		p = p * 10 + (colon[i] - '0');
		if (p > 65535)
			return -1;
	}
	if (p < 1)
		return -1;
	*port = p;
	return 0;
}

static void tcp_open_fail(void)
{
	char e[96];
	const char *m = mmb_net_tcp_errmsg();
	e[0] = '?';
	strncpy(e + 1, m && m[0] ? m : "FILE", sizeof(e) - 2);
	e[sizeof(e) - 1] = 0;
	mmb_error(e);
}

void mmb_cmd_open(void)
{
	char path[128];
	int mode = 0, fn = 1, tcp = 0;
	char host[80];
	int port = 0;
	mmb_val v = mmb_expr();
	if (v.type != T_STR)
		mmb_syntax();
	tcp = is_tcp_spec(v.s);
	if (tcp)
	{
		strncpy(path, v.s, sizeof(path) - 1);
		path[sizeof(path) - 1] = 0;
		if (parse_tcp_host_port(v.s, host, sizeof(host), &port) != 0)
			mmb_syntax();
	}
	else
	{
		char full[128];
		if (mmb_vfs_resolve(v.s, full, sizeof(full)) != 0)
			mmb_syntax();
		strncpy(path, full, sizeof(path) - 1);
		path[sizeof(path) - 1] = 0;
	}
	if (mmb_match("FOR"))
	{
		if (mmb_match("INPUT"))
			mode = 0;
		else if (mmb_match("OUTPUT"))
			mode = 1;
		else if (mmb_match("APPEND"))
			mode = 2;
		else
			mmb_syntax();
	}
	else if (tcp)
		mode = MMB_FM_BOTH;
	if (tcp && mode == 2)
		mmb_syntax();
	if (!mmb_match("AS"))
		mmb_syntax();
	mmb_skip_sp();
	if (*G.p == '#')
		G.p++;
	fn = (int)mmb_as_int(mmb_expr());
	if (fn < 1 || fn > MMB_MAX_FILES)
		mmb_error("?FILE NUMBER");
	if (tcp)
	{
		int other, owner;
		if (mmb_in_connect() || mmb_in_term())
			mmb_error("?FILE");
		owner = mmb_tcp_owner();
		if (owner >= 0 && owner != g_console)
			mmb_tcp_in_use();
		other = mmb_tcp_any_open();
		if (other && other != fn)
			mmb_error("?FILE");
		if (G.files[fn].open)
			mmb_file_close_n(fn);
		if (mmb_net_tcp_begin(host, port) != 0)
			tcp_open_fail();
		mmb_tcp_claim();
		memset(&G.files[fn], 0, sizeof(G.files[fn]));
		G.files[fn].open = 1;
		G.files[fn].mode = mode;
		G.files[fn].kind = MMB_FK_TCP;
		G.files[fn].ungot = -1;
		strncpy(G.files[fn].path, path, sizeof(G.files[fn].path) - 1);
		return;
	}
	if (mode == 1 || mode == 2)
	{
		if (mmb_vfs_readonly_path(path))
			mmb_error("?READ ONLY");
		if (mode == 1 && mmb_vfs_write(path, "", 0, 0) != 0)
			mmb_error("?FILE");
	}
	if (G.files[fn].open)
		mmb_file_close_n(fn);
	memset(&G.files[fn], 0, sizeof(G.files[fn]));
	G.files[fn].open = 1;
	G.files[fn].mode = mode;
	G.files[fn].kind = MMB_FK_FILE;
	G.files[fn].ungot = -1;
	G.files[fn].pos = mode == 2 ? mmb_vfs_size(path) : 0;
	strncpy(G.files[fn].path, path, sizeof(G.files[fn].path) - 1);
}

void mmb_cmd_close(void)
{
	int fn;
	mmb_skip_sp();
	if (*G.p == '#')
		G.p++;
	fn = (int)mmb_as_int(mmb_expr());
	mmb_file_close_n(fn);
}
