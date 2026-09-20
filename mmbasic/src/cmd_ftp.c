#include "mmb_priv.h"

/*
 * Minimal FTP server for the FILES browser.
 *
 * Runs on the interpreter task: mmb_ftp_poll() is called from mmb_files_poll()
 * while the FILES modal is up. Socket operations are non-blocking (the accept
 * path is polled in console/net.cpp), so a transfer is dribbled a chunk per
 * poll and the modal keeps servicing Esc. Files are accessed through the VFS,
 * so A: (ramdisk), B: (package) and the FAT drives all work, and every request
 * is sandboxed under the chosen root.
 */

#define FTP_LINE_MAX  320
#define FTP_PATH_MAX  200
#define FTP_OUT_MAX   1024
#define FTP_LIST_MAX  8192
#define FTP_CHUNK     1024
#define FTP_SEND_CAP  2048
#define FTP_RECV_CAP  2048
#define FTP_WAIT_MS   10000
#define FTP_IDLE_MS   30000

#define XF_NONE 0
#define XF_RETR 1
#define XF_STOR 2
#define XF_LIST 3
#define XF_NLST 4

typedef struct {
	int running;
	int port;
	int data_port;
	char root[FTP_PATH_MAX];   /* canonical, always ends in '/' */
	int rootlen;
	char addr[64];             /* "10.0.2.15:21" */

	int ctl_lsn;
	int ctl;
	int logged;
	int line_n;
	char line[FTP_LINE_MAX];

	char out[FTP_OUT_MAX];
	int out_n;
	int out_off;

	char cwd[FTP_PATH_MAX];    /* absolute within root, starts '/' */

	int data_lsn;
	int data;
	int xfer;
	unsigned xfer_at;
	char xpath[FTP_PATH_MAX];
	int xsize;
	unsigned xoff;
	char list_buf[FTP_LIST_MAX];
	int list_len;

	char rnfr[FTP_PATH_MAX];
	int has_rnfr;

	char status[96];
} ftp_state;

static ftp_state FT;

/* ---- small helpers ---------------------------------------------------- */

static void ftp_ser(const char *s)
{
	unsigned n;
	if (!s)
		return;
	n = (unsigned)strlen(s);
	if (n && G.plat && G.plat->write_serial)
		G.plat->write_serial(s, n);
}

static void fmt_uint(char *dst, unsigned v)
{
	char tmp[12];
	int i = 0;
	if (v == 0)
	{
		dst[0] = '0';
		dst[1] = 0;
		return;
	}
	while (v && i < 11)
	{
		tmp[i++] = (char)('0' + (v % 10));
		v /= 10;
	}
	while (i--)
		*dst++ = tmp[i];
	*dst = 0;
}

static void str_upper(char *s)
{
	for (; *s; s++)
		if (*s >= 'a' && *s <= 'z')
			*s = (char)(*s - 32);
}

static void set_status(const char *s)
{
	strncpy(FT.status, s ? s : "", sizeof(FT.status) - 1);
	FT.status[sizeof(FT.status) - 1] = 0;
}

static int parse_ip4(const char *s, unsigned char o[4])
{
	int i;
	for (i = 0; i < 4; i++)
	{
		int v = 0;
		if (*s < '0' || *s > '9')
			return -1;
		while (*s >= '0' && *s <= '9')
		{
			v = v * 10 + (*s - '0');
			if (v > 255)
				return -1;
			s++;
		}
		o[i] = (unsigned char)v;
		if (i < 3)
		{
			if (*s != '.')
				return -1;
			s++;
		}
	}
	return 0;
}

/* ---- reply queue ------------------------------------------------------ */

static void ftp_flush(void)
{
	while (FT.ctl >= 0 && FT.out_off < FT.out_n)
	{
		int rc = mmb_net_srv_send(FT.ctl, FT.out + FT.out_off,
					  (unsigned)(FT.out_n - FT.out_off));
		if (rc <= 0)
			break;
		FT.out_off += rc;
	}
	if (FT.out_off >= FT.out_n)
	{
		FT.out_n = 0;
		FT.out_off = 0;
	}
}

static void out_raw(const char *s, int n)
{
	if (n <= 0 || !s)
		return;
	if (FT.out_n + n > FTP_OUT_MAX)
		ftp_flush();
	/* A slow client can leave a partially-sent reply at the front. Compact
	 * it rather than dropping the new reply on the floor. */
	if (FT.out_n + n > FTP_OUT_MAX && FT.out_off > 0)
	{
		int left = FT.out_n - FT.out_off;
		if (left > 0)
			memmove(FT.out, FT.out + FT.out_off, (unsigned)left);
		FT.out_n = left;
		FT.out_off = 0;
	}
	if (FT.out_n + n > FTP_OUT_MAX)
		return;
	memcpy(FT.out + FT.out_n, s, (unsigned)n);
	FT.out_n += n;
}

static void out_str(const char *s)
{
	if (s)
		out_raw(s, (int)strlen(s));
}

static void ftp_put(const char *s)
{
	out_str(s);
	out_raw("\r\n", 2);
}

static void ftp_put_num(int code, const char *text)
{
	char num[8];
	fmt_uint(num, (unsigned)code);
	out_str(num);
	out_raw(" ", 1);
	ftp_put(text);
}

/* ---- path sandbox ----------------------------------------------------- */

/* Resolve an FTP path argument to a canonical "X:/..." inside the root.
 * Absolute arguments are relative to the server root; relative ones to the
 * session cwd. Returns 0 and writes canon on success, -1 on escape/error. */
static int ftp_resolve(const char *arg, char *canon, int canonsz)
{
	char rel[FTP_PATH_MAX];
	char full[FTP_PATH_MAX];
	char tmp[FTP_PATH_MAX];
	char cmp[FTP_PATH_MAX + 2];
	int rl;

	if (!arg)
		arg = "";
	if (arg[0] == '/')
	{
		strncpy(rel, arg, sizeof(rel) - 1);
		rel[sizeof(rel) - 1] = 0;
	}
	else
	{
		strncpy(rel, FT.cwd[0] ? FT.cwd : "/", sizeof(rel) - 1);
		rel[sizeof(rel) - 1] = 0;
		rl = (int)strlen(rel);
		if (rl && rel[rl - 1] != '/' && rl < (int)sizeof(rel) - 1)
		{
			rel[rl] = '/';
			rel[rl + 1] = 0;
		}
		if (strlen(rel) + strlen(arg) < sizeof(rel) - 1)
			strcat(rel, arg);
	}

	strncpy(full, FT.root, sizeof(full) - 1);
	full[sizeof(full) - 1] = 0;
	if (rel[0] == '/')
	{
		if (strlen(full) + strlen(rel + 1) < sizeof(full) - 1)
			strcat(full, rel + 1);
	}
	else if (strlen(full) + strlen(rel) < sizeof(full) - 1)
		strcat(full, rel);

	if (mmb_vfs_resolve(full, tmp, sizeof(tmp)) != 0)
		return -1;
	strncpy(cmp, tmp, sizeof(cmp) - 1);
	cmp[sizeof(cmp) - 1] = 0;
	rl = (int)strlen(cmp);
	if (rl && cmp[rl - 1] != '/' && rl < (int)sizeof(cmp) - 1)
	{
		cmp[rl] = '/';
		cmp[rl + 1] = 0;
	}
	if (strncmp(cmp, FT.root, (unsigned)FT.rootlen) != 0)
		return -1;
	strncpy(canon, tmp, (unsigned)canonsz - 1);
	canon[canonsz - 1] = 0;
	return 0;
}

/* Canonical absolute path -> cwd relative to root ("/SUB"). */
static void ftp_set_cwd(const char *canon)
{
	const char *rest = canon + (FT.rootlen > 0 ? FT.rootlen - 1 : 0);
	FT.cwd[0] = '/';
	FT.cwd[1] = 0;
	if (rest[0] == '/')
		rest++;
	if (rest[0])
	{
		strncpy(FT.cwd + 1, rest, sizeof(FT.cwd) - 2);
		FT.cwd[sizeof(FT.cwd) - 1] = 0;
	}
}

/* ---- data connection -------------------------------------------------- */

static void ftp_close_data(void)
{
	if (FT.data >= 0)
	{
		mmb_net_srv_close(FT.data);
		FT.data = -1;
	}
	if (FT.data_lsn >= 0)
	{
		mmb_net_srv_listen_close(FT.data_lsn);
		FT.data_lsn = -1;
	}
}

static int ftp_open_data(void)
{
	int port;
	ftp_close_data();
	port = mmb_net_srv_listen(FT.data_port);
	if (port < 0)
		return -1;
	FT.data_lsn = port;
	FT.data_port = mmb_net_srv_port(FT.data_lsn);
	if (FT.data_port <= 0)
		FT.data_port = FT.port + 1000;
	return 0;
}

/* ---- transfer engine -------------------------------------------------- */

static void ftp_xfer_clear(void)
{
	FT.xfer = XF_NONE;
	FT.list_len = 0;
	FT.xsize = 0;
	FT.xoff = 0;
	FT.xpath[0] = 0;
}

static void ftp_xfer_finish(void)
{
	ftp_close_data();
	ftp_put_num(226, "Transfer complete");
	ftp_xfer_clear();
	set_status("Client connected");
}

static void ftp_xfer_fail(const char *msg)
{
	ftp_close_data();
	ftp_put(msg);
	ftp_xfer_clear();
	set_status("Client connected");
}

static int ftp_xfer_begin(int kind, const char *canon, int size)
{
	if (FT.data_lsn < 0 && FT.data < 0)
	{
		ftp_put_num(425, "Use PASV first");
		return -1;
	}
	FT.xfer = kind;
	FT.xfer_at = mmb_now_ms();
	strncpy(FT.xpath, canon ? canon : "", sizeof(FT.xpath) - 1);
	FT.xpath[sizeof(FT.xpath) - 1] = 0;
	FT.xsize = size;
	FT.xoff = 0;
	ftp_put_num(150, "Opening data connection");
	return 0;
}

static void ftp_xfer_poll(void)
{
	unsigned sent = 0;

	if (FT.xfer == XF_NONE)
		return;
	if (FT.data < 0)
	{
		int d;
		if (FT.data_lsn < 0)
		{
			ftp_xfer_fail("425 No data connection");
			return;
		}
		d = mmb_net_srv_accept(FT.data_lsn);
		if (d < 0)
		{
			if (mmb_now_ms() - FT.xfer_at > FTP_WAIT_MS)
				ftp_xfer_fail("425 Data connection timed out");
			return;
		}
		FT.data = d;
		mmb_net_srv_listen_close(FT.data_lsn);
		FT.data_lsn = -1;
	}

	switch (FT.xfer)
	{
	case XF_RETR:
		while (sent < FTP_SEND_CAP)
		{
			unsigned want, got = 0;
			int rc;

			if (FT.xoff >= (unsigned)FT.xsize)
			{
				ftp_xfer_finish();
				return;
			}
			want = (unsigned)FT.xsize - FT.xoff;
			if (want > FTP_CHUNK)
				want = FTP_CHUNK;
			if (mmb_vfs_read_at(FT.xpath, FT.xoff, FT.list_buf, want, &got) != 0 ||
			    got == 0)
			{
				ftp_xfer_fail("426 Read failed");
				return;
			}
			rc = mmb_net_srv_send(FT.data, FT.list_buf, got);
			if (rc < 0)
			{
				ftp_xfer_fail("426 Connection closed");
				return;
			}
			if (rc == 0)
				return;	/* peer window full; resume next poll */
			FT.xoff += (unsigned)rc;
			sent += (unsigned)rc;
			FT.xfer_at = mmb_now_ms();
			if ((unsigned)rc < got)
				return;
		}
		set_status("Sending file");
		return;

	case XF_STOR:
		while (sent < FTP_RECV_CAP)
		{
			int n = mmb_net_srv_recv(FT.data, FT.list_buf, FTP_CHUNK);
			if (n < 0)
			{
				ftp_xfer_finish();
				return;
			}
			if (n == 0)
			{
				if (mmb_net_srv_closed(FT.data))
				{
					ftp_xfer_finish();
					return;
				}
				if (mmb_now_ms() - FT.xfer_at > FTP_IDLE_MS)
				{
					ftp_xfer_fail("426 Data connection timed out");
					return;
				}
				break;
			}
			if (mmb_vfs_write(FT.xpath, FT.list_buf, (unsigned)n, 1) != 0)
			{
				ftp_xfer_fail("552 Write failed");
				return;
			}
			FT.xoff += (unsigned)n;
			sent += (unsigned)n;
			FT.xfer_at = mmb_now_ms();
		}
		if (FT.xfer != XF_NONE)
			set_status("Receiving file");
		return;

	case XF_LIST:
	case XF_NLST:
		while (sent < FTP_SEND_CAP && FT.xoff < (unsigned)FT.list_len)
		{
			unsigned want = (unsigned)FT.list_len - FT.xoff;
			int rc;
			if (want > FTP_CHUNK)
				want = FTP_CHUNK;
			rc = mmb_net_srv_send(FT.data, FT.list_buf + FT.xoff, want);
			if (rc < 0)
			{
				ftp_xfer_fail("426 Connection closed");
				return;
			}
			if (rc == 0)
				return;	/* peer window full; resume next poll */
			FT.xoff += (unsigned)rc;
			sent += (unsigned)rc;
			FT.xfer_at = mmb_now_ms();
		}
		if (FT.xoff >= (unsigned)FT.list_len)
		{
			ftp_xfer_finish();
			return;
		}
		set_status("Listing");
		return;

	default:
		ftp_xfer_fail("451 Transfer error");
		return;
	}
}

/* ---- LIST/NLST building ----------------------------------------------- */

static void list_append(const char *s)
{
	int n = (int)strlen(s);
	if (FT.list_len + n + 2 > FTP_LIST_MAX)
		return;
	memcpy(FT.list_buf + FT.list_len, s, (unsigned)n);
	FT.list_len += n;
	FT.list_buf[FT.list_len++] = '\r';
	FT.list_buf[FT.list_len++] = '\n';
}

static void list_line(int is_dir, unsigned size, const char *name)
{
	char line[220];
	char num[16];
	int pad, i;

	strncpy(line, is_dir ? "drwxr-xr-x   1 ftp      ftp  "
			    : "-rw-r--r--   1 ftp      ftp  ",
		sizeof(line) - 1);
	line[sizeof(line) - 1] = 0;
	fmt_uint(num, size);
	pad = 8 - (int)strlen(num);
	if (pad < 0)
		pad = 0;
	for (i = 0; i < pad; i++)
		strncat(line, " ", sizeof(line) - strlen(line) - 1);
	strncat(line, num, sizeof(line) - strlen(line) - 1);
	strncat(line, " Jan 01 00:00 ", sizeof(line) - strlen(line) - 1);
	strncat(line, name, sizeof(line) - strlen(line) - 1);
	list_append(line);
}

static const char *base_name(const char *path)
{
	const char *slash = 0;
	while (*path)
	{
		if (*path == '/')
			slash = path;
		path++;
	}
	return slash ? slash + 1 : path;
}

static int ftp_start_list(const char *arg, int names_only)
{
	char canon[FTP_PATH_MAX];
	char names[2048];
	char *s, *nl;

	if (arg[0] == 0)
		arg = FT.cwd;
	if (ftp_resolve(arg, canon, sizeof(canon)) != 0 ||
	    (!mmb_vfs_isdir(canon) && !mmb_vfs_exists(canon)))
	{
		ftp_put_num(550, "Not found");
		return -1;
	}
	FT.list_len = 0;
	if (!mmb_vfs_isdir(canon))
	{
		const char *name = base_name(canon);
		if (names_only)
			list_append(name);
		else
		{
			int sz = mmb_vfs_size(canon);
			list_line(0, (unsigned)(sz < 0 ? 0 : sz), name);
		}
	}
	else
	{
		names[0] = 0;
		if (mmb_vfs_list(canon, names, sizeof(names)) != 0)
			names[0] = 0;
		s = names;
		while (*s)
		{
			int d = 0, n;
			char *next;
			nl = s;
			while (*nl && *nl != '\n' && *nl != '\r')
				nl++;
			n = (int)(nl - s);
			next = nl;
			if (*next == '\r')
				next++;
			if (*next == '\n')
				next++;
			if (n > 0 && s[n - 1] == '/')
			{
				s[n - 1] = 0;
				d = 1;
			}
			else if (*nl)
				*nl = 0;	/* terminate the name at the separator */
			if (s[0] && !(s[0] == '.' && s[1] == 0))
			{
				if (names_only)
					list_append(s);
				else if (d)
					list_line(1, 0, s);
				else
				{
					char full[FTP_PATH_MAX];
					int sz;
					strncpy(full, canon, sizeof(full) - 1);
					full[sizeof(full) - 1] = 0;
					if (strlen(full) && full[strlen(full) - 1] != '/')
						strncat(full, "/", sizeof(full) - strlen(full) - 1);
					strncat(full, s, sizeof(full) - strlen(full) - 1);
					sz = mmb_vfs_size(full);
					list_line(0, (unsigned)(sz < 0 ? 0 : sz), s);
				}
			}
			s = next;
		}
	}
	if (ftp_xfer_begin(names_only ? XF_NLST : XF_LIST, canon, FT.list_len) != 0)
		return -1;
	set_status(names_only ? "Listing names" : "Listing");
	return 0;
}

/* ---- commands --------------------------------------------------------- */

static const char *path_arg(const char *s)
{
	while (*s == ' ')
		s++;
	return s;
}

static void ftp_cmd_cwd(const char *arg)
{
	char canon[FTP_PATH_MAX];
	if (arg[0] == 0)
	{
		ftp_put_num(250, "Directory changed");
		return;
	}
	if (ftp_resolve(arg, canon, sizeof(canon)) != 0 || !mmb_vfs_isdir(canon))
	{
		ftp_put_num(550, "Not a directory");
		return;
	}
	ftp_set_cwd(canon);
	ftp_put_num(250, "Directory changed");
}

static void ftp_cmd_retr(const char *arg)
{
	char canon[FTP_PATH_MAX];
	int sz;
	if (arg[0] == 0)
	{
		ftp_put_num(501, "No file name");
		return;
	}
	if (ftp_resolve(arg, canon, sizeof(canon)) != 0 || !mmb_vfs_exists(canon) ||
	    mmb_vfs_isdir(canon))
	{
		ftp_put_num(550, "File not found");
		return;
	}
	sz = mmb_vfs_size(canon);
	if (sz < 0)
	{
		ftp_put_num(550, "File not found");
		return;
	}
	if (ftp_xfer_begin(XF_RETR, canon, sz) != 0)
		return;
	set_status("Sending file");
	ftp_ser("[FTP] RETR ");
	ftp_ser(base_name(canon));
	ftp_ser("\r\n");
}

static void ftp_cmd_stor(const char *arg, int append)
{
	char canon[FTP_PATH_MAX];
	if (arg[0] == 0)
	{
		ftp_put_num(501, "No file name");
		return;
	}
	if (ftp_resolve(arg, canon, sizeof(canon)) != 0 || mmb_vfs_isdir(canon))
	{
		ftp_put_num(550, "Invalid path");
		return;
	}
	if (!append && mmb_vfs_write(canon, "", 0, 0) != 0)
	{
		ftp_put_num(552, "Write failed");
		return;
	}
	if (ftp_xfer_begin(XF_STOR, canon, 0) != 0)
		return;
	set_status("Receiving file");
	ftp_ser("[FTP] STOR ");
	ftp_ser(base_name(canon));
	ftp_ser("\r\n");
}

static void ftp_cmd_size(const char *arg)
{
	char canon[FTP_PATH_MAX];
	char num[16];
	int sz;
	if (arg[0] == 0 || ftp_resolve(arg, canon, sizeof(canon)) != 0 ||
	    mmb_vfs_isdir(canon))
	{
		ftp_put_num(550, "Not found");
		return;
	}
	sz = mmb_vfs_size(canon);
	if (sz < 0)
	{
		ftp_put_num(550, "Not found");
		return;
	}
	fmt_uint(num, (unsigned)sz);
	ftp_put_num(213, num);
}

static void ftp_cmd_dele(const char *arg)
{
	char canon[FTP_PATH_MAX];
	if (ftp_resolve(arg, canon, sizeof(canon)) != 0 || mmb_vfs_isdir(canon) ||
	    mmb_vfs_kill(canon) != 0)
	{
		ftp_put_num(550, "Delete failed");
		return;
	}
	ftp_put_num(250, "Deleted");
}

static void ftp_cmd_mkd(const char *arg)
{
	char canon[FTP_PATH_MAX];
	char line[FTP_PATH_MAX + 32];
	if (arg[0] == 0 || ftp_resolve(arg, canon, sizeof(canon)) != 0 ||
	    mmb_vfs_mkdir(canon) != 0)
	{
		ftp_put_num(550, "Create failed");
		return;
	}
	strcpy(line, "257 \"");
	strncat(line, canon, sizeof(line) - strlen(line) - 1);
	strncat(line, "\" created", sizeof(line) - strlen(line) - 1);
	ftp_put(line);
}

static void ftp_cmd_rmd(const char *arg)
{
	char canon[FTP_PATH_MAX];
	if (ftp_resolve(arg, canon, sizeof(canon)) != 0 || mmb_vfs_rmdir(canon) != 0)
	{
		ftp_put_num(550, "Remove failed");
		return;
	}
	ftp_put_num(250, "Removed");
}

static void ftp_cmd_rnfr(const char *arg)
{
	char canon[FTP_PATH_MAX];
	if (ftp_resolve(arg, canon, sizeof(canon)) != 0 || !mmb_vfs_exists(canon))
	{
		ftp_put_num(550, "Not found");
		return;
	}
	strncpy(FT.rnfr, canon, sizeof(FT.rnfr) - 1);
	FT.rnfr[sizeof(FT.rnfr) - 1] = 0;
	FT.has_rnfr = 1;
	ftp_put_num(350, "Ready for RNTO");
}

static void ftp_cmd_rnto(const char *arg)
{
	char canon[FTP_PATH_MAX];
	if (!FT.has_rnfr)
	{
		ftp_put_num(503, "RNFR required first");
		return;
	}
	FT.has_rnfr = 0;
	if (ftp_resolve(arg, canon, sizeof(canon)) != 0 ||
	    mmb_vfs_rename(FT.rnfr, canon) != 0)
	{
		ftp_put_num(550, "Rename failed");
		return;
	}
	ftp_put_num(250, "Renamed");
}

static void ftp_cmd_pasv(int extended)
{
	char ip[32];
	char line[96];
	int hi, lo;

	if (ftp_open_data() != 0)
	{
		ftp_put_num(425, "Cannot open data port");
		return;
	}
	if (extended)
	{
		char num[8];
		fmt_uint(num, (unsigned)FT.data_port);
		strcpy(line, "229 Entering Extended Passive Mode (|||");
		strncat(line, num, sizeof(line) - strlen(line) - 1);
		strncat(line, "|)", sizeof(line) - strlen(line) - 1);
		ftp_put(line);
		return;
	}
	{
		unsigned char a = 127, b = 0, c = 0, d = 1;
		char h1[4], h2[4], h3[4], h4[4], p1[4], p2[4];
		if (mmb_net_srv_ip(ip, sizeof(ip)) == 0)
		{
			unsigned char o[4];
			if (parse_ip4(ip, o) == 0)
			{
				a = o[0];
				b = o[1];
				c = o[2];
				d = o[3];
			}
		}
		hi = (FT.data_port >> 8) & 0xFF;
		lo = FT.data_port & 0xFF;
		fmt_uint(h1, a);
		fmt_uint(h2, b);
		fmt_uint(h3, c);
		fmt_uint(h4, d);
		fmt_uint(p1, (unsigned)hi);
		fmt_uint(p2, (unsigned)lo);
		strcpy(line, "227 Entering Passive Mode (");
		strncat(line, h1, sizeof(line) - strlen(line) - 1);
		strncat(line, ",", sizeof(line) - strlen(line) - 1);
		strncat(line, h2, sizeof(line) - strlen(line) - 1);
		strncat(line, ",", sizeof(line) - strlen(line) - 1);
		strncat(line, h3, sizeof(line) - strlen(line) - 1);
		strncat(line, ",", sizeof(line) - strlen(line) - 1);
		strncat(line, h4, sizeof(line) - strlen(line) - 1);
		strncat(line, ",", sizeof(line) - strlen(line) - 1);
		strncat(line, p1, sizeof(line) - strlen(line) - 1);
		strncat(line, ",", sizeof(line) - strlen(line) - 1);
		strncat(line, p2, sizeof(line) - strlen(line) - 1);
		strncat(line, ").", sizeof(line) - strlen(line) - 1);
	}
	ftp_put(line);
}

/* Returns 1 when the client asked to quit. */
static int ftp_command(char *line)
{
	char cmd[8];
	char *arg;
	int i = 0;

	while (line[i] && line[i] != ' ' && i < 7)
	{
		cmd[i] = line[i];
		i++;
	}
	cmd[i] = 0;
	arg = line + i;
	while (*arg == ' ')
		arg++;
	str_upper(cmd);

	if (!FT.logged && strcmp(cmd, "USER") != 0 && strcmp(cmd, "PASS") != 0 &&
	    strcmp(cmd, "QUIT") != 0 && strcmp(cmd, "FEAT") != 0 &&
	    strcmp(cmd, "SYST") != 0 && strcmp(cmd, "NOOP") != 0 &&
	    strcmp(cmd, "OPTS") != 0)
	{
		ftp_put_num(530, "Not logged in");
		return 0;
	}

	if (strcmp(cmd, "USER") == 0)
		ftp_put_num(331, "User name ok, need password");
	else if (strcmp(cmd, "PASS") == 0)
	{
		FT.logged = 1;
		ftp_put_num(230, "Login successful");
	}
	else if (strcmp(cmd, "SYST") == 0)
		ftp_put_num(215, "UNIX Type: L8");
	else if (strcmp(cmd, "FEAT") == 0)
	{
		ftp_put("211-Features:");
		ftp_put(" UTF8");
		ftp_put(" SIZE");
		ftp_put(" PASV");
		ftp_put(" EPSV");
		ftp_put_num(211, "End");
	}
	else if (strcmp(cmd, "PWD") == 0 || strcmp(cmd, "XPWD") == 0)
	{
		char line2[FTP_PATH_MAX + 8];
		strcpy(line2, "257 \"");
		strncat(line2, FT.cwd, sizeof(line2) - strlen(line2) - 1);
		strncat(line2, "\"", sizeof(line2) - strlen(line2) - 1);
		ftp_put(line2);
	}
	else if (strcmp(cmd, "CWD") == 0 || strcmp(cmd, "XCWD") == 0)
		ftp_cmd_cwd(path_arg(arg));
	else if (strcmp(cmd, "CDUP") == 0 || strcmp(cmd, "XCUP") == 0)
		ftp_cmd_cwd("..");
	else if (strcmp(cmd, "TYPE") == 0 || strcmp(cmd, "MODE") == 0 ||
		 strcmp(cmd, "STRU") == 0 || strcmp(cmd, "OPTS") == 0 ||
		 strcmp(cmd, "NOOP") == 0 || strcmp(cmd, "ACCT") == 0)
		ftp_put_num(200, "Command okay");
	else if (strcmp(cmd, "PASV") == 0)
		ftp_cmd_pasv(0);
	else if (strcmp(cmd, "EPSV") == 0)
		ftp_cmd_pasv(1);
	else if (strcmp(cmd, "PORT") == 0 || strcmp(cmd, "EPRT") == 0)
		ftp_put_num(502, "Active mode not supported");
	else if (strcmp(cmd, "LIST") == 0)
		ftp_start_list(path_arg(arg), 0);
	else if (strcmp(cmd, "NLST") == 0)
		ftp_start_list(path_arg(arg), 1);
	else if (strcmp(cmd, "SIZE") == 0)
		ftp_cmd_size(path_arg(arg));
	else if (strcmp(cmd, "MDTM") == 0)
		ftp_put_num(213, "20260101000000");
	else if (strcmp(cmd, "RETR") == 0)
		ftp_cmd_retr(path_arg(arg));
	else if (strcmp(cmd, "STOR") == 0 || strcmp(cmd, "APPE") == 0)
		ftp_cmd_stor(path_arg(arg), strcmp(cmd, "APPE") == 0);
	else if (strcmp(cmd, "DELE") == 0)
		ftp_cmd_dele(path_arg(arg));
	else if (strcmp(cmd, "MKD") == 0 || strcmp(cmd, "XMKD") == 0)
		ftp_cmd_mkd(path_arg(arg));
	else if (strcmp(cmd, "RMD") == 0 || strcmp(cmd, "XRMD") == 0)
		ftp_cmd_rmd(path_arg(arg));
	else if (strcmp(cmd, "RNFR") == 0)
		ftp_cmd_rnfr(path_arg(arg));
	else if (strcmp(cmd, "RNTO") == 0)
		ftp_cmd_rnto(path_arg(arg));
	else if (strcmp(cmd, "ABOR") == 0)
	{
		if (FT.xfer != XF_NONE)
			ftp_xfer_fail("426 Aborted");
		ftp_put_num(226, "Abort successful");
	}
	else if (strcmp(cmd, "QUIT") == 0)
	{
		ftp_put_num(221, "Goodbye");
		return 1;
	}
	else
		ftp_put_num(502, "Command not implemented");
	return 0;
}

/* ---- control connection ---------------------------------------------- */

static void ftp_ctl_drop(void)
{
	if (FT.ctl >= 0)
	{
		mmb_net_srv_close(FT.ctl);
		FT.ctl = -1;
	}
	ftp_close_data();
	FT.logged = 0;
	FT.line_n = 0;
	FT.out_n = 0;
	FT.out_off = 0;
	FT.has_rnfr = 0;
	ftp_xfer_clear();
	set_status(FT.running ? "Waiting for client" : "");
	if (FT.running)
		ftp_ser("[FTP] DISCONNECTED\r\n");
}

static void ftp_ctl_read(void)
{
	unsigned char buf[256];
	int quit = 0;

	for (;;)
	{
		int n, i;
		if (FT.ctl < 0)
			return;
		n = mmb_net_srv_recv(FT.ctl, buf, sizeof(buf));
		if (n < 0)
		{
			ftp_ctl_drop();
			return;
		}
		if (n == 0)
			return;
		for (i = 0; i < n; i++)
		{
			char c = (char)buf[i];
			if (c == '\r')
				continue;
			if (c == '\n')
			{
				FT.line[FT.line_n] = 0;
				quit = ftp_command(FT.line);
				FT.line_n = 0;
				if (FT.xfer != XF_NONE || quit)
					break;
				continue;
			}
			if (FT.line_n < FTP_LINE_MAX - 1)
				FT.line[FT.line_n++] = c;
		}
		ftp_flush();
		if (FT.xfer != XF_NONE || quit)
			break;
	}
	if (quit)
	{
		ftp_flush();
		ftp_ctl_drop();
	}
}

/* ---- public API ------------------------------------------------------- */

int mmb_ftp_start(const char *root, int port)
{
	char canon[FTP_PATH_MAX];
	unsigned t0;

	if (FT.running)
		return 0;
	if (port < 1 || port > 64535)
		return -1;
	if (!mmb_net_available())
	{
		if (mmb_eth_start() != 0)
			return -1;
		t0 = mmb_now_ms();
		while (!mmb_net_available() && mmb_now_ms() - t0 < 5000)
			mmb_net_yield();
	}
	if (!mmb_net_available())
		return -1;
	if (!root || !root[0])
		root = mmb_vfs_cwd();
	if (mmb_vfs_resolve(root, canon, sizeof(canon)) != 0)
		return -1;

	memset(&FT, 0, sizeof(FT));
	FT.ctl = -1;
	FT.data = -1;
	FT.ctl_lsn = -1;
	FT.data_lsn = -1;
	strncpy(FT.root, canon, sizeof(FT.root) - 1);
	FT.root[sizeof(FT.root) - 1] = 0;
	if (FT.root[0] && FT.root[strlen(FT.root) - 1] != '/')
		strncat(FT.root, "/", sizeof(FT.root) - strlen(FT.root) - 1);
	FT.rootlen = (int)strlen(FT.root);
	FT.port = port;
	FT.data_port = port + 1000;
	FT.cwd[0] = '/';
	FT.cwd[1] = 0;

	FT.ctl_lsn = mmb_net_srv_listen(port);
	if (FT.ctl_lsn < 0)
	{
		memset(&FT, 0, sizeof(FT));
		FT.ctl = FT.data = FT.ctl_lsn = FT.data_lsn = -1;
		return -1;
	}
	FT.running = 1;
	FT.addr[0] = 0;
	{
		char ip[32];
		char num[8];
		if (mmb_net_srv_ip(ip, sizeof(ip)) == 0)
		{
			strncpy(FT.addr, ip, sizeof(FT.addr) - 1);
			strncat(FT.addr, ":", sizeof(FT.addr) - strlen(FT.addr) - 1);
			fmt_uint(num, (unsigned)port);
			strncat(FT.addr, num, sizeof(FT.addr) - strlen(FT.addr) - 1);
		}
	}
	set_status("Waiting for client");
	ftp_ser("[FTP] LISTEN ROOT ");
	ftp_ser(FT.root);
	ftp_ser(" PORT ");
	{
		char num[8];
		fmt_uint(num, (unsigned)port);
		ftp_ser(num);
	}
	ftp_ser(" DATA ");
	{
		char num[8];
		fmt_uint(num, (unsigned)FT.data_port);
		ftp_ser(num);
	}
	ftp_ser(" ADDR ");
	ftp_ser(FT.addr);
	ftp_ser("\r\n");
	return 0;
}

void mmb_ftp_stop(void)
{
	if (!FT.running)
		return;
	ftp_close_data();
	if (FT.ctl >= 0)
		mmb_net_srv_close(FT.ctl);
	if (FT.ctl_lsn >= 0)
		mmb_net_srv_listen_close(FT.ctl_lsn);
	memset(&FT, 0, sizeof(FT));
	FT.ctl = FT.data = FT.ctl_lsn = FT.data_lsn = -1;
	FT.running = 0;
	ftp_ser("[FTP] STOP\r\n");
}

void mmb_ftp_poll(void)
{
	if (!FT.running)
		return;
	ftp_flush();
	if (FT.ctl < 0)
	{
		int c = mmb_net_srv_accept(FT.ctl_lsn);
		if (c >= 0)
		{
			FT.ctl = c;
			FT.logged = 0;
			FT.line_n = 0;
			FT.out_n = 0;
			FT.out_off = 0;
			FT.has_rnfr = 0;
			ftp_put("220 MMBasic FTP server ready");
			ftp_flush();
			ftp_ser("[FTP] CONNECTED\r\n");
			set_status("Client connected");
		}
		return;
	}
	if (FT.xfer != XF_NONE)
	{
		ftp_xfer_poll();
		ftp_flush();
		return;
	}
	ftp_ctl_read();
	ftp_flush();
}

int mmb_ftp_running(void)
{
	return FT.running;
}

int mmb_ftp_port(void)
{
	return FT.port;
}

const char *mmb_ftp_status(void)
{
	return FT.status;
}
