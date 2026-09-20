/* Host test for the FTP LIST/NLST line builder in mmbasic/src/cmd_ftp.c.
 *
 * Compiles the real cmd_ftp.c against the shims declared in
 * tests/ftp_shim/mmb_priv.h. Feeds a fake control session and captures the
 * bytes the server writes to the data connection, asserting every line is
 * CRLF-terminated (a bare LF makes tnftp warn) and that each directory entry
 * is emitted exactly once.
 */
#include <stdio.h>
#include <string.h>

#include "mmb_priv.h"

mmb_test_globals G;

/* ---- fake VFS --------------------------------------------------------- */

static const char *g_listing = "";
static const char *g_cwd = "A:/";

int mmb_vfs_resolve(const char *path, char *out, int outsz)
{
	if (!path || !out || outsz <= 0)
		return -1;
	strncpy(out, path, (size_t)outsz - 1);
	out[outsz - 1] = 0;
	return 0;
}

const char *mmb_vfs_cwd(void)
{
	return g_cwd;
}

int mmb_vfs_isdir(const char *path)
{
	return strcmp(path, "A:/") == 0;
}

int mmb_vfs_exists(const char *path)
{
	(void)path;
	return 1;
}

int mmb_vfs_size(const char *path)
{
	if (strstr(path, "ai-independence.md"))
		return 665;
	if (strstr(path, "locate.bas"))
		return 1;
	return 0;
}

int mmb_vfs_read_at(const char *path, unsigned pos, void *data, unsigned n, unsigned *got)
{
	(void)path;
	(void)pos;
	(void)data;
	(void)n;
	if (got)
		*got = 0;
	return -1;
}

int mmb_vfs_write(const char *path, const void *data, unsigned n, int append)
{
	(void)path;
	(void)data;
	(void)n;
	(void)append;
	return 0;
}

int mmb_vfs_kill(const char *path)
{
	(void)path;
	return 0;
}

int mmb_vfs_mkdir(const char *path)
{
	(void)path;
	return 0;
}

int mmb_vfs_rmdir(const char *path)
{
	(void)path;
	return 0;
}

int mmb_vfs_rename(const char *src, const char *dst)
{
	(void)src;
	(void)dst;
	return 0;
}

int mmb_vfs_list(const char *spec, char *out, int outsz)
{
	(void)spec;
	strncpy(out, g_listing, (size_t)outsz - 1);
	out[outsz - 1] = 0;
	return 0;
}

/* ---- fake network ----------------------------------------------------- */

#define CONN_CTL   100
#define CONN_DATA  101

static char g_ctl_in[1024];
static int g_ctl_in_len;
static int g_ctl_in_off;

static char g_ctl_out[4096];
static int g_ctl_out_len;

static char g_data_out[8192];
static int g_data_out_len;

static int g_lsn_port[4];
static int g_data_accepted;
static int g_next_lsn;

int mmb_eth_start(void)
{
	return 0;
}

int mmb_net_available(void)
{
	return 1;
}

void mmb_net_yield(void)
{
}

static unsigned g_now;

unsigned mmb_now_ms(void)
{
	return ++g_now;
}

int mmb_net_srv_listen(int port)
{
	int lsn = g_next_lsn++;
	g_lsn_port[lsn] = port;
	return lsn;
}

int mmb_net_srv_port(int lsn)
{
	return g_lsn_port[lsn];
}

void mmb_net_srv_listen_close(int lsn)
{
	(void)lsn;
}

int mmb_net_srv_accept(int lsn)
{
	if (lsn == 0)
		return CONN_CTL;
	g_data_accepted = 1;
	return CONN_DATA;
}

int mmb_net_srv_recv(int conn, void *data, unsigned maxn)
{
	int left, n;
	if (conn != CONN_CTL)
		return 0;
	left = g_ctl_in_len - g_ctl_in_off;
	if (left <= 0)
		return 0;
	n = left;
	if ((unsigned)n > maxn)
		n = (int)maxn;
	memcpy(data, g_ctl_in + g_ctl_in_off, (size_t)n);
	g_ctl_in_off += n;
	return n;
}

int mmb_net_srv_send(int conn, const void *data, unsigned n)
{
	if (conn == CONN_DATA)
	{
		if (g_data_out_len + (int)n <= (int)sizeof(g_data_out))
		{
			memcpy(g_data_out + g_data_out_len, data, n);
			g_data_out_len += (int)n;
		}
		return (int)n;
	}
	if (g_ctl_out_len + (int)n <= (int)sizeof(g_ctl_out))
	{
		memcpy(g_ctl_out + g_ctl_out_len, data, n);
		g_ctl_out_len += (int)n;
	}
	return (int)n;
}

int mmb_net_srv_closed(int conn)
{
	(void)conn;
	return 0;
}

void mmb_net_srv_close(int conn)
{
	(void)conn;
}

int mmb_net_srv_ip(char *buf, int bufsize)
{
	if (bufsize < 10)
		return -1;
	strcpy(buf, "10.0.2.15");
	return 0;
}

/* ---- harness ---------------------------------------------------------- */

static int g_failures;

static void check(int cond, const char *what)
{
	if (!cond)
	{
		printf("FAIL: %s\n", what);
		g_failures++;
	}
}

static void check_crlf(const char *label)
{
	int i;
	check(g_data_out_len >= 2, label);
	for (i = 0; i < g_data_out_len; i++)
	{
		if (g_data_out[i] == '\n')
			check(i > 0 && g_data_out[i - 1] == '\r',
			      "every LF must be preceded by CR (bare LF warning)");
	}
	/* No NUL and no stray CR before a non-LF. */
	for (i = 0; i < g_data_out_len; i++)
	{
		if (g_data_out[i] == '\r')
			check(i + 1 < g_data_out_len && g_data_out[i + 1] == '\n',
			      "every CR must be followed by LF");
	}
}

static void reset_session(void)
{
	mmb_ftp_stop();
	g_ctl_in_len = g_ctl_in_off = 0;
	g_ctl_out_len = 0;
	g_data_out_len = 0;
	g_data_accepted = 0;
	g_now = 0;
	g_next_lsn = 0;
}

static void run_session(const char *names, const char *expected)
{
	int i;
	reset_session();
	g_listing = names;
	check(mmb_ftp_start("A:/", 21) == 0, "start");
	/* USER + PASS, then a passive data connection and a LIST. */
	strcpy(g_ctl_in, "USER anonymous\r\nPASS x\r\nEPSV\r\nLIST\r\n");
	g_ctl_in_len = (int)strlen(g_ctl_in);
	for (i = 0; i < 40 && g_data_out_len == 0 && g_failures == 0; i++)
		mmb_ftp_poll();
	check(g_data_accepted == 1, "data connection was accepted");
	check(g_data_out_len == (int)strlen(expected), "listing length");
	if (g_data_out_len == (int)strlen(expected))
		check(memcmp(g_data_out, expected, (size_t)g_data_out_len) == 0,
		      "listing bytes");
	if (g_data_out_len != (int)strlen(expected) ||
	    memcmp(g_data_out, expected, (size_t)g_data_out_len) != 0)
	{
		printf("expected: %s\n", expected);
		printf("actual  : %.*s\n", g_data_out_len, g_data_out);
	}
	check_crlf("data is non-empty");
	reset_session();
}

int main(void)
{
	run_session(
	    "ai-independence.md\nlocate.bas",
	    "-rw-r--r--   1 ftp      ftp       665 Jan 01 00:00 ai-independence.md\r\n"
	    "-rw-r--r--   1 ftp      ftp         1 Jan 01 00:00 locate.bas\r\n");

	run_session(
	    "subdir/\nhello.bas",
	    "drwxr-xr-x   1 ftp      ftp         0 Jan 01 00:00 subdir\r\n"
	    "-rw-r--r--   1 ftp      ftp         0 Jan 01 00:00 hello.bas\r\n");

	if (g_failures)
	{
		printf("%d check(s) failed\n", g_failures);
		return 1;
	}
	printf("all checks passed\n");
	return 0;
}
