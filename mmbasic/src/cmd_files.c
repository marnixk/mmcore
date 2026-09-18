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

#define DIR_LIST_MAX 4096
#define DIR_SEARCH_LIST 1024
#define DIR_PATH_MAX 160
#define DIR_DEPTH_MAX 8
#define DIR_ENT_MAX 512

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

static void dir_result_add(char *result, int resultsz, int *rlen, const char *s)
{
	int l = (int)strlen(s);
	int need = l + (*rlen > 0 ? 1 : 0);
	if (*rlen + need + 1 > resultsz)
		return;
	if (*rlen > 0)
		result[(*rlen)++] = '\n';
	memcpy(result + *rlen, s, (unsigned)l);
	*rlen += l;
	result[*rlen] = 0;
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

/* Recursive search (DIR /S): every matching entry under dirspec, with a path
 * prefix relative to the search root. */
static void dir_search(const char *dirspec, const char *prefix, const char *glob,
		       int depth, char *result, int resultsz, int *rlen)
{
	char list[DIR_SEARCH_LIST];
	char *p;
	if (depth > DIR_DEPTH_MAX)
		return;
	list[0] = 0;
	if (mmb_vfs_list(dirspec && dirspec[0] ? dirspec : 0, list, sizeof(list)) != 0)
		return;
	p = list;
	while (*p)
	{
		char name[DIR_PATH_MAX];
		char child[DIR_PATH_MAX];
		char disp[DIR_PATH_MAX];
		int l = 0, is_dir = 0;
		while (*p && *p != '\n' && l < (int)sizeof(name) - 1)
			name[l++] = *p++;
		name[l] = 0;
		while (*p == '\n')
			p++;
		if (l > 0 && name[l - 1] == '/')
		{
			name[--l] = 0;
			is_dir = 1;
		}
		if (!name[0])
			continue;
		if (prefix && prefix[0])
			dir_join(disp, sizeof(disp), prefix, "/", name);
		else
			dir_join(disp, sizeof(disp), name, 0, 0);
		if (is_dir)
		{
			size_t dl = dirspec ? strlen(dirspec) : 0;
			if (dl > 0 && dirspec[dl - 1] == '/')
				dir_join(child, sizeof(child), dirspec, "", name);
			else if (dl > 0)
				dir_join(child, sizeof(child), dirspec, "/", name);
			else
				dir_join(child, sizeof(child), name, 0, 0);
			if (mmb_glob_match(name, glob))
			{
				char d[DIR_PATH_MAX];
				dir_join(d, sizeof(d), disp, "/", 0);
				dir_result_add(result, resultsz, rlen, d);
			}
			dir_search(child, disp, glob, depth + 1, result, resultsz, rlen);
		}
		else if (mmb_glob_match(name, glob))
			dir_result_add(result, resultsz, rlen, disp);
	}
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
		char dir[128], glob[128], result[DIR_LIST_MAX];
		int rlen = 0;
		result[0] = 0;
		dir_split_spec(spec[0] ? spec : 0, dir, sizeof(dir), glob, sizeof(glob));
		dir_search(dir, "", glob, 0, result, sizeof(result), &rlen);
		if (wide)
			dir_wide(result);
		else
			mmb_out(result[0] ? result : "(empty)");
		return;
	}
	{
		char buf[DIR_LIST_MAX];
		buf[0] = 0;
		if (mmb_vfs_list(spec[0] ? spec : 0, buf, sizeof(buf)) != 0)
			mmb_error("?DRIVE");
		if (wide)
			dir_wide(buf);
		else
			mmb_out(buf[0] ? buf : "(empty)");
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

void mmb_file_close_n(int fn)
{
	if (fn < 1 || fn > MMB_MAX_FILES || !G.files[fn].open)
		return;
	if (G.files[fn].kind == MMB_FK_TCP)
		mmb_net_tcp_close();
	memset(&G.files[fn], 0, sizeof(G.files[fn]));
	G.files[fn].ungot = -1;
}

void mmb_close_tcp_files(void)
{
	int i;
	for (i = 1; i <= MMB_MAX_FILES; i++)
		if (mmb_file_is_tcp(i))
			mmb_file_close_n(i);
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
		int other;
		if (mmb_in_connect() || mmb_in_term())
			mmb_error("?FILE");
		other = mmb_tcp_any_open();
		if (other && other != fn)
			mmb_error("?FILE");
		if (G.files[fn].open)
			mmb_file_close_n(fn);
		if (mmb_net_tcp_begin(host, port) != 0)
			tcp_open_fail();
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
