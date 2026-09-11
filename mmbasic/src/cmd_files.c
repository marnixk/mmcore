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
	int n, left;
	unsigned char chunk[1024];
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
	if (n < 0)
		mmb_error("?INVALID");
	if (mmb_vfs_readonly_path(path))
		mmb_error("?READ ONLY");
	if (!G.plat || !G.plat->read_raw)
		mmb_error("?UNSUPPORTED");
	if (mmb_vfs_write(path, "", 0, 0) != 0)
		mmb_error("?FILE");
	mmb_console_write("<<XFER>>\n");
	left = n;
	while (left > 0)
	{
		int req = left > (int)sizeof(chunk) ? (int)sizeof(chunk) : left;
		if (G.plat->read_raw(chunk, (unsigned)req) != 0)
			mmb_error("?FILE");
		if (mmb_vfs_write(path, chunk, (unsigned)req, 1) != 0)
			mmb_error("?FILE");
		left -= req;
	}
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
	if (nch > MMB_MAX_STR)
		nch = MMB_MAX_STR;
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
