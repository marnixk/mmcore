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

void mmb_cmd_files(const char *kw)
{
	char buf[1024];
	char spec[128];
	mmb_val v;
	(void)kw;
	spec[0] = 0;
	mmb_skip_sp();
	if (*G.p && *G.p != ':' && *G.p != '\'')
	{
		v = mmb_expr();
		if (v.type == T_STR)
			strncpy(spec, v.s, sizeof(spec) - 1);
	}
	buf[0] = 0;
	if (mmb_vfs_list(spec[0] ? spec : 0, buf, sizeof(buf)) != 0)
		mmb_error("?DRIVE");
	mmb_out(buf[0] ? buf : "(empty)");
}

void mmb_cmd_open(void)
{
	char path[128];
	int mode = 0, fn = 1;
	mmb_val v = mmb_expr();
	if (v.type != T_STR)
		mmb_syntax();
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
	if (!mmb_match("AS"))
		mmb_syntax();
	mmb_skip_sp();
	if (*G.p == '#')
		G.p++;
	fn = (int)mmb_as_int(mmb_expr());
	if (fn < 1 || fn > MMB_MAX_FILES)
		mmb_error("?FILE NUMBER");
	if (mode == 1 || mode == 2)
	{
		if (mmb_vfs_readonly_path(path))
			mmb_error("?READ ONLY");
		if (mode == 1 && mmb_vfs_write(path, "", 0, 0) != 0)
			mmb_error("?FILE");
	}
	memset(&G.files[fn], 0, sizeof(G.files[fn]));
	G.files[fn].open = 1;
	G.files[fn].mode = mode;
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
	if (fn >= 1 && fn <= MMB_MAX_FILES)
		G.files[fn].open = 0;
}
