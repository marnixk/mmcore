/* Host test for FTP STOR/APPE path handling in mmbasic/src/cmd_ftp.c.
 *
 * A client like tnftp sends the full local path as the remote name when no
 * remote name is given ("put /home/me/pic.png"). The server maps that under
 * its sandbox root, so the parent folders usually do not exist. This test
 * drives a STOR of a multi-kilobyte binary payload to such a path and checks
 * that the server creates the missing folders, writes the exact bytes, and
 * completes with 226 rather than failing with 552.
 */
#include <stdio.h>
#include <string.h>

#include "mmb_priv.h"

mmb_test_globals G;

/* ---- fake VFS --------------------------------------------------------- */

static char g_mkdirs[16][160];
static int g_nmkdirs;

static char g_dirs[16][160];
static int g_ndirs = 1;	/* "A:/" exists from boot */

static char g_wpath[160];
static unsigned char g_written[65536];
static int g_written_len;

static int same_path(const char *a, const char *b)
{
	size_t la = strlen(a), lb = strlen(b);
	while (la > 1 && a[la - 1] == '/')
		la--;
	while (lb > 1 && b[lb - 1] == '/')
		lb--;
	return la == lb && strncmp(a, b, la) == 0;
}

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
	return "A:/";
}

int mmb_vfs_isdir(const char *path)
{
	int i;
	for (i = 0; i < g_ndirs; i++)
		if (same_path(g_dirs[i], path))
			return 1;
	return 0;
}

int mmb_vfs_exists(const char *path)
{
	(void)path;
	return 0;
}

int mmb_vfs_size(const char *path)
{
	(void)path;
	return -1;
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
	const char *s, *slash = 0;
	char parent[160];
	/* Model FatFs: opening a file whose parent folder is missing fails. */
	for (s = path; *s; s++)
		if (*s == '/')
			slash = s;
	if (slash)
	{
		int plen = (int)(slash - path);
		if (plen == 0)
			plen = 1;
		memcpy(parent, path, (unsigned)plen);
		parent[plen] = 0;
		if (strcmp(parent, "A:") != 0 && !mmb_vfs_isdir(parent))
			return -1;
	}
	if (!append)
	{
		strncpy(g_wpath, path, sizeof(g_wpath) - 1);
		g_wpath[sizeof(g_wpath) - 1] = 0;
		g_written_len = 0;
	}
	if (n)
	{
		if (g_written_len + (int)n > (int)sizeof(g_written))
			return -1;
		memcpy(g_written + g_written_len, data, n);
		g_written_len += (int)n;
	}
	return 0;
}

/* Streaming API used by FTP STOR (open once, append chunks, close). */
static int g_writer_open;

int mmb_vfs_wopen(const char *path, int append)
{
	const char *s, *slash = 0;
	char parent[160];
	/* Model FatFs: opening a file whose parent folder is missing fails. */
	for (s = path; *s; s++)
		if (*s == '/')
			slash = s;
	if (slash)
	{
		int plen = (int)(slash - path);
		if (plen == 0)
			plen = 1;
		memcpy(parent, path, (unsigned)plen);
		parent[plen] = 0;
		if (strcmp(parent, "A:") != 0 && !mmb_vfs_isdir(parent))
			return -1;
	}
	if (!append || g_written_len == 0)
	{
		strncpy(g_wpath, path, sizeof(g_wpath) - 1);
		g_wpath[sizeof(g_wpath) - 1] = 0;
		if (!append)
			g_written_len = 0;
	}
	g_writer_open = 1;
	return 0;
}

int mmb_vfs_wwrite(int handle, const void *data, unsigned n)
{
	if (handle < 0 || !g_writer_open)
		return -1;
	if (n)
	{
		if (g_written_len + (int)n > (int)sizeof(g_written))
			return -1;
		memcpy(g_written + g_written_len, data, n);
		g_written_len += (int)n;
	}
	return 0;
}

int mmb_vfs_wclose(int handle)
{
	if (handle < 0 || !g_writer_open)
		return -1;
	g_writer_open = 0;
	return 0;
}

int mmb_vfs_kill(const char *path)
{
	(void)path;
	return 0;
}

int mmb_vfs_mkdir(const char *path)
{
	/* Model mkdir_parents(): every ancestor of the requested folder exists
	 * afterwards. */
	char full[160];
	int i;
	strncpy(full, path, sizeof(full) - 1);
	full[sizeof(full) - 1] = 0;
	if (g_nmkdirs < 16)
	{
		strncpy(g_mkdirs[g_nmkdirs], path, sizeof(g_mkdirs[0]) - 1);
		g_mkdirs[g_nmkdirs][sizeof(g_mkdirs[0]) - 1] = 0;
		g_nmkdirs++;
	}
	for (i = 0; full[i]; i++)
	{
		if (full[i] != '/' || i < 2)
			continue;
		full[i] = 0;
		if (!mmb_vfs_isdir(full) && g_ndirs < 16)
		{
			strncpy(g_dirs[g_ndirs], full, sizeof(g_dirs[0]) - 1);
			g_dirs[g_ndirs][sizeof(g_dirs[0]) - 1] = 0;
			g_ndirs++;
		}
		full[i] = '/';
	}
	if (!mmb_vfs_isdir(full) && g_ndirs < 16)
	{
		strncpy(g_dirs[g_ndirs], full, sizeof(g_dirs[0]) - 1);
		g_dirs[g_ndirs][sizeof(g_dirs[0]) - 1] = 0;
		g_ndirs++;
	}
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

int mmb_vfs_list(const char *spec, char *out, int outsz, int *truncated)
{
	(void)spec;
	if (truncated)
		*truncated = 0;
	if (outsz > 0)
		out[0] = 0;
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

static unsigned char g_data_in[16384];
static int g_data_in_len;
static int g_data_in_off;

/* Fault injection for the STOR failure paths. g_data_error_at >= 0 makes the
 * next recv return g_data_error_code once that many bytes are consumed;
 * g_data_eof_code replaces the drained-queue 0 (an orderly FIN is negative). */
static int g_data_error_at = -1;
static int g_data_error_code;
static int g_data_eof_code;

static int g_data_accepted;
static int g_lsn_port[4];
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
	if (conn == CONN_DATA)
	{
		if (g_data_error_code && g_data_error_at >= 0 &&
		    g_data_in_off >= g_data_error_at)
			return g_data_error_code;
		left = g_data_in_len - g_data_in_off;
		if (left <= 0)
			return g_data_eof_code;
		n = left;
		if ((unsigned)n > maxn)
			n = (int)maxn;
		memcpy(data, g_data_in + g_data_in_off, (size_t)n);
		g_data_in_off += n;
		return n;
	}
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

/* Circle's -NET_ERROR_NOT_CONNECTED / -NET_ERROR_CONNECTION_RESET. */
int mmb_net_srv_eof(int err)
{
	return err == -56;
}

const char *mmb_net_srv_reason(int err)
{
	switch (err)
	{
	case -54:
		return "reset by peer";
	case -56:
		return "closed by remote host";
	case -57:
		return "timed out (no ACK from peer)";
	default:
		return "network error";
	}
}

int mmb_net_srv_send(int conn, const void *data, unsigned n)
{
	if (conn != CONN_CTL)
		return (int)n;
	if (g_ctl_out_len + (int)n <= (int)sizeof(g_ctl_out))
	{
		memcpy(g_ctl_out + g_ctl_out_len, data, n);
		g_ctl_out_len += (int)n;
	}
	return (int)n;
}

int mmb_net_srv_closed(int conn)
{
	/* The data connection is done once the payload has been drained. */
	return conn == CONN_DATA && g_data_in_off >= g_data_in_len;
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

static char g_out[4096];
static int g_out_len;

static void feed(const char *s)
{
	int n = (int)strlen(s);
	if (g_out_len + n >= (int)sizeof(g_out))
		n = (int)sizeof(g_out) - g_out_len - 1;
	memcpy(g_out + g_out_len, s, (size_t)n);
	g_out_len += n;
	g_out[g_out_len] = 0;
}

static int out_has(const char *needle)
{
	return strstr(g_out, needle) != 0;
}

/* Capture the server's serial status markers ([FTP] STOR/RX). */
static char g_ser[4096];
static int g_ser_len;

static void test_write_serial(const char *s, unsigned n)
{
	if (g_ser_len + (int)n >= (int)sizeof(g_ser))
		return;
	memcpy(g_ser + g_ser_len, s, n);
	g_ser_len += (int)n;
	g_ser[g_ser_len] = 0;
}

static mmb_test_plat g_plat = { test_write_serial };

static void reset(void)
{
	mmb_ftp_stop();
	g_ctl_in_len = g_ctl_in_off = 0;
	g_ctl_out_len = 0;
	g_ctl_out[0] = 0;
	g_out_len = 0;
	g_out[0] = 0;
	g_data_in_len = g_data_in_off = 0;
	g_data_error_at = -1;
	g_data_error_code = 0;
	g_data_eof_code = 0;
	g_data_accepted = 0;
	g_nmkdirs = 0;
	g_ndirs = 1;
	strcpy(g_dirs[0], "A:/");
	g_written_len = 0;
	g_wpath[0] = 0;
	g_ser_len = 0;
	g_ser[0] = 0;
	g_writer_open = 0;
	g_now = 0;
	g_next_lsn = 0;
}

static void run_stor(const char *stor_arg, const char *expect_path,
		     const unsigned char *payload, int payload_len,
		     const char *expect_parent)
{
	int i;
	char ctl[256];

	reset();
	check(mmb_ftp_start("A:/", 21) == 0, "start");
	g_data_in_len = payload_len;
	if (payload_len > 0)
		memcpy(g_data_in, payload, (size_t)payload_len);

	strcpy(ctl, "USER anonymous\r\nPASS x\r\nEPSV\r\nSTOR ");
	strncat(ctl, stor_arg, sizeof(ctl) - strlen(ctl) - 1);
	strncat(ctl, "\r\n", sizeof(ctl) - strlen(ctl) - 1);
	strcpy(g_ctl_in, ctl);
	g_ctl_in_len = (int)strlen(g_ctl_in);

	for (i = 0; i < 2000 && !out_has("226 ") && !out_has("552 "); i++)
	{
		mmb_ftp_poll();
		/* Drain control replies into the assertion buffer. */
		if (g_ctl_out_len)
		{
			feed(g_ctl_out);
			g_ctl_out_len = 0;
		}
	}

	check(out_has("226 "), "transfer completes with 226");
	check(!out_has("552 "), "no 552 write failure");
	check(g_data_accepted == 1, "data connection accepted");
	check(strcmp(g_wpath, expect_path) == 0, "file written to the requested path");
	check(g_written_len == payload_len, "all payload bytes written");
	if (g_written_len == payload_len)
		check(memcmp(g_written, payload, (size_t)payload_len) == 0,
		      "payload bytes preserved (binary safe)");
	check(strstr(g_ser, "[FTP] STOR ") != 0, "STOR marker emitted");
	check(strstr(g_ser, "[FTP] STOR DONE ") != 0, "STOR completion marker emitted");
	if (payload_len >= 8192)
		check(strstr(g_ser, "[FTP] RX ") != 0, "STOR progress marker emitted");

	if (expect_parent)
	{
		int found = 0;
		for (i = 0; i < g_nmkdirs; i++)
			if (strcmp(g_mkdirs[i], expect_parent) == 0)
				found = 1;
		check(found, "missing parent folder was created");
	}
	else
		check(g_nmkdirs == 0, "no folder created for a root-level file");

	if (g_failures)
	{
		printf("  stor arg : %s\n", stor_arg);
		printf("  replies  : %s\n", g_out);
		printf("  serial   : %s\n", g_ser);
		printf("  wpath    : %s\n", g_wpath);
		printf("  mkdirs   : %d\n", g_nmkdirs);
		for (i = 0; i < g_nmkdirs; i++)
			printf("    %s\n", g_mkdirs[i]);
	}
	mmb_ftp_stop();
}

/* Inject a mid-transfer data-connection failure: the server must tell the
 * client (426) and log the byte count and Circle error, not send a 226. */
static void run_stor_fail(const unsigned char *payload, int payload_len,
			  int error_at, int error_code, const char *expect_ser)
{
	int i;
	char ctl[128];

	reset();
	check(mmb_ftp_start("A:/", 21) == 0, "start");
	g_data_in_len = payload_len;
	if (payload_len > 0)
		memcpy(g_data_in, payload, (size_t)payload_len);
	g_data_error_at = error_at;
	g_data_error_code = error_code;

	strcpy(ctl, "USER anonymous\r\nPASS x\r\nEPSV\r\nSTOR DROP.BIN\r\n");
	strcpy(g_ctl_in, ctl);
	g_ctl_in_len = (int)strlen(g_ctl_in);

	for (i = 0; i < 2000 && !out_has("226 ") && !out_has("426 "); i++)
	{
		mmb_ftp_poll();
		if (g_ctl_out_len)
		{
			feed(g_ctl_out);
			g_ctl_out_len = 0;
		}
	}

	check(out_has("426 "), "dropped connection reports 426");
	check(!out_has("226 "), "no 226 for a dropped connection");
	check(strstr(g_ser, "[FTP] STOR FAIL ") != 0, "STOR FAIL marker emitted");
	check(strstr(g_ser, "[FTP] STOR DONE ") == 0, "no STOR DONE marker");
	if (expect_ser)
		check(strstr(g_ser, expect_ser) != 0, "STOR FAIL names bytes and code");

	if (g_failures)
	{
		printf("  replies  : %s\n", g_out);
		printf("  serial   : %s\n", g_ser);
	}
	mmb_ftp_stop();
}

/* A peer FIN is reported as -NET_ERROR_NOT_CONNECTED, which still means the
 * STOR completed normally. */
static void run_stor_eof(const unsigned char *payload, int payload_len)
{
	int i;
	char ctl[128];

	reset();
	check(mmb_ftp_start("A:/", 21) == 0, "start");
	g_data_in_len = payload_len;
	if (payload_len > 0)
		memcpy(g_data_in, payload, (size_t)payload_len);
	g_data_eof_code = -56;

	strcpy(ctl, "USER anonymous\r\nPASS x\r\nEPSV\r\nSTOR EOF.BIN\r\n");
	strcpy(g_ctl_in, ctl);
	g_ctl_in_len = (int)strlen(g_ctl_in);

	for (i = 0; i < 2000 && !out_has("226 ") && !out_has("426 "); i++)
	{
		mmb_ftp_poll();
		if (g_ctl_out_len)
		{
			feed(g_ctl_out);
			g_ctl_out_len = 0;
		}
	}

	check(out_has("226 "), "orderly FIN completes with 226");
	check(!out_has("426 "), "orderly FIN is not an error");
	check(strstr(g_ser, "[FTP] STOR DONE ") != 0, "STOR DONE marker emitted");
	check(strstr(g_ser, "[FTP] STOR FAIL ") == 0, "no STOR FAIL marker");

	if (g_failures)
	{
		printf("  replies  : %s\n", g_out);
		printf("  serial   : %s\n", g_ser);
	}
	mmb_ftp_stop();
}

int main(void)
{
	static unsigned char payload[13312];
	int i;
	G.plat = &g_plat;
	for (i = 0; i < (int)sizeof(payload); i++)
		payload[i] = (unsigned char)((i * 37 + 11) & 0xFF);
	/* Make sure NUL/CR/LF bytes are present, to prove binary safety. */
	payload[0] = 0;
	payload[1] = '\r';
	payload[2] = '\n';

	/* tnftp-style full local path: parents are created under the root. */
	run_stor("/home/marnix/Downloads/nibblets/playfield_bg_wide.png",
		 "A:/home/marnix/Downloads/nibblets/playfield_bg_wide.png",
		 payload, (int)sizeof(payload), "A:/home/marnix/Downloads/nibblets");

	/* A bare file name must not create any folder. */
	run_stor("PLAIN.PNG", "A:/PLAIN.PNG", payload, 100, 0);

	/* A reset mid-stream is an error, not a completed transfer. */
	run_stor_fail(payload, (int)sizeof(payload), 2048, -54,
		      "STOR FAIL 2048 code=-54 reset by peer");

	/* An orderly FIN is negative too, but completes the STOR. */
	run_stor_eof(payload, 1024);

	if (g_failures)
	{
		printf("%d check(s) failed\n", g_failures);
		return 1;
	}
	printf("all checks passed\n");
	return 0;
}
