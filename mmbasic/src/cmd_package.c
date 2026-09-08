#include "mmb_priv.h"

static void strip_slash(char *s)
{
	int n;
	if (!s)
		return;
	n = (int)strlen(s);
	while (n > 1 && (s[n - 1] == '/' || s[n - 1] == '\\'))
	{
		if (n == 3 && s[1] == ':')
			break;
		s[--n] = 0;
	}
}

static void join_rel(char *out, int outsz, const char *dir, const char *name)
{
	int n;
	out[0] = 0;
	if (dir && dir[0])
	{
		strncpy(out, dir, (unsigned)outsz - 1);
		out[outsz - 1] = 0;
	}
	n = (int)strlen(out);
	if (n && out[n - 1] != '/' && out[n - 1] != '\\' && n + 1 < outsz)
	{
		out[n++] = '/';
		out[n] = 0;
	}
	strncat(out, name, (unsigned)outsz - strlen(out) - 1);
}

static int confirm_overwrite(void)
{
	char line[40];
	char *p;
	mmb_console_write("File exists, overwrite? [Y/n] ");
	if (!G.plat || !G.plat->read_line)
		return 0;
	if (G.plat->read_line(line, sizeof(line), 0) != 0)
		return 0;
	p = line;
	while (*p == ' ' || *p == '\t')
		p++;
	if (*p == 0 || *p == 'Y' || *p == 'y')
		return 1;
	return 0;
}

static int folder_has_main(const char *folder)
{
	char p[160];
	join_rel(p, sizeof(p), folder, "MAIN.BAS");
	return mmb_vfs_exists(p) && !mmb_vfs_isdir(p);
}

static int pack_walk(mmb_zip_w *z, const char *absdir, const char *rel,
		     const char *dest_full, int depth)
{
	char listing[2048];
	char *line, *next;
	if (depth > 8)
		return -1;
	if (mmb_vfs_list(absdir, listing, sizeof(listing)) != 0)
		return -1;
	if (!listing[0])
	{
		if (rel[0])
		{
			char d[128];
			strncpy(d, rel, sizeof(d) - 2);
			d[sizeof(d) - 2] = 0;
			if (d[strlen(d) - 1] != '/')
				strcat(d, "/");
			return mmb_zip_add(z, d, 0, 0);
		}
		return 0;
	}
	for (line = listing; line && *line; line = next)
	{
		char name[80], child_abs[160], child_rel[128], resolved[128];
		int is_dir = 0, n;
		next = strchr(line, '\n');
		if (next)
			*next++ = 0;
		n = (int)strlen(line);
		if (n && line[n - 1] == '/')
		{
			is_dir = 1;
			line[n - 1] = 0;
		}
		if (!line[0] || mmb_vfs_hidden_name(line))
			continue;
		strncpy(name, line, sizeof(name) - 1);
		name[sizeof(name) - 1] = 0;
		join_rel(child_abs, sizeof(child_abs), absdir, name);
		if (rel[0])
			join_rel(child_rel, sizeof(child_rel), rel, name);
		else
		{
			strncpy(child_rel, name, sizeof(child_rel) - 1);
			child_rel[sizeof(child_rel) - 1] = 0;
		}
		if (mmb_vfs_resolve(child_abs, resolved, sizeof(resolved)) == 0 &&
		    dest_full[0] && mmb_keyword_eq(resolved, dest_full))
			continue;
		if (is_dir)
		{
			if (pack_walk(z, child_abs, child_rel, dest_full, depth + 1) != 0)
				return -1;
			continue;
		}
		{
			int sz = mmb_vfs_size(child_abs);
			unsigned got = 0;
			unsigned char *buf;
			if (sz < 0)
				return -1;
			buf = G.plat->alloc((unsigned)sz + 1);
			if (!buf)
				return -1;
			if (mmb_vfs_read(child_abs, buf, (unsigned)sz, &got) != 0)
			{
				G.plat->free(buf);
				return -1;
			}
			if (mmb_zip_add(z, child_rel, buf, got) != 0)
			{
				G.plat->free(buf);
				return -1;
			}
			G.plat->free(buf);
		}
	}
	return 0;
}

void mmb_cmd_package(void)
{
	char pkg[128], folder[128], dest_full[128];
	mmb_zip_w z;
	unsigned char *out = 0;
	unsigned n = 0;
	mmb_val v;

	v = mmb_expr();
	if (v.type != T_STR)
		mmb_syntax();
	strncpy(pkg, v.s, sizeof(pkg) - 1);
	pkg[sizeof(pkg) - 1] = 0;
	mmb_skip_sp();
	if (*G.p == ',')
		G.p++;
	mmb_skip_sp();
	v = mmb_expr();
	if (v.type != T_STR)
		mmb_syntax();
	strncpy(folder, v.s, sizeof(folder) - 1);
	folder[sizeof(folder) - 1] = 0;
	strip_slash(folder);
	if (!strchr(pkg, '.'))
		strncat(pkg, ".APP", sizeof(pkg) - strlen(pkg) - 1);
	if (!folder[0] || !mmb_vfs_isdir(folder))
		mmb_error("?DIRECTORY");
	if (!folder_has_main(folder))
		mmb_error("?NO MAIN.BAS");
	if (mmb_vfs_exists(pkg) && !mmb_vfs_isdir(pkg))
	{
		if (G.running)
			mmb_error("?FILE EXISTS");
		if (!confirm_overwrite())
			return;
	}
	dest_full[0] = 0;
	mmb_vfs_resolve(pkg, dest_full, sizeof(dest_full));
	if (mmb_zip_begin(&z) != 0)
		mmb_error("?PACKAGE");
	if (pack_walk(&z, folder, "", dest_full, 0) != 0)
	{
		mmb_zip_abort(&z);
		mmb_error("?PACKAGE");
	}
	if (mmb_zip_finish(&z, &out, &n) != 0)
	{
		mmb_zip_abort(&z);
		mmb_error("?PACKAGE");
	}
	if (mmb_vfs_write(pkg, out, n, 0) != 0)
	{
		G.plat->free(out);
		mmb_error("?FILE");
	}
	G.plat->free(out);
}
