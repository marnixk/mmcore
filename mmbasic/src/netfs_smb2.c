/*
 * SMB2/3 network-drive backend (#1128) for OPTION NETWORK DRIVE.
 *
 * Mounts a Samba/Windows share at Z: and implements the mmb_vol_ops contract
 * that vfs.c expects from a physical volume. It is a *shared* file: the only
 * platform-specific part is mmb_plat_poll_fd(), so a future Circle build can
 * compile this unchanged once it supplies that hook plus libsmb2's own socket
 * layer.
 *
 * Reconnect (#1133): a transport failure (socket reset / op timeout /
 * NT_STATUS_NETWORK_NAME_DELETED) destroys the libsmb2 context and marks Z:
 * disconnected; the next op reconnects lazily. Idempotent ops (stat / isdir /
 * exists / list / read) reconnect and retry once inside the same call, while a
 * dropped write fails with ?FILE (never replayed, so a chunk is not doubled).
 *
 * Open-handle caches (#1133): one read handle and one append handle are kept
 * open between calls, mirroring the FAT backend's s_rd/s_wr caches (#984/#1000)
 * so ERROR PRINT# / INPUT# / COPY do not do a CREATE+WRITE/READ+CLOSE round
 * trip per call. A short-lived stat cache absorbs the repeated
 * CREATE+QUERY_INFO+CLOSE that FILES and COPY hammer. Every cache is dropped
 * when the context is destroyed, because smb2_destroy_context() frees the fhs
 * and dirs it owns.
 *
 * Sockets: libsmb2 opens and owns its own socket(s) for this context. SMB
 * traffic never goes through the single global mmb_net_tcp_* connection used
 * by CONNECT / TERM / OPEN "host:port", and never takes the mmb_tcp_owner
 * lock. TERM keeps draining while an SMB connect or a large read is in
 * flight (we call mmb_poll() between poll() waits), and an SMB disconnect
 * destroys only this context, leaving any TERM/CONNECT session untouched.
 *
 * Only the async libsmb2 API is used: the sync API blocks in poll(1000) with
 * no break hook. The wait loop here checks a hard budget and Ctrl-C.
 */
#include "mmb_priv.h"

#ifdef MMB_HAVE_SMB2

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "smb2.h"
#include "libsmb2.h"

/* Cap our own I/O chunks; the negotiated max may be larger. */
#define NETFS_CHUNK 65536u
/* Hard budgets for the lazy connect and each op, in milliseconds. */
#define NETFS_CONNECT_MS 5000u
#define NETFS_OP_MS 10000u
/* 5s per-PDU timeout inside libsmb2 (also drives smb2_which_events). */
#define NETFS_SMB_TIMEOUT 5
/* Paths are stored whole in the caches; matches the old char[256] buffers. */
#define NETFS_PATH_MAX 256
/* Short-lived stat cache: FILES and COPY stat the same names repeatedly. */
#define NETFS_STAT_CACHE 8
#define NETFS_STAT_TTL_MS 1000u

/*
 * The SMB context is private to this file. It is intentionally independent of
 * mmb_net_tcp_*: no shared fd, no mmb_tcp_owner, no claim/release.
 */
static struct smb2_context *s_ctx;
static int s_connected;
static char s_sub[160];       /* subpath inside the share, '/' separated */
static char s_err[128];
static char s_server[160];
static char s_share[128];

/* Set when the op in flight hit a transport failure. The retry wrappers
 * reconnect once when they see it; netfs_await() sets it for a poll/service
 * error, our own op budget expiring, or a transport NTSTATUS. */
static int s_dead;

struct netfs_req {
	int done;
	int status;
	void *data;
};

/* One op is in flight at a time, so the completion record is a file-scope
 * pointer. libsmb2 frees opendir's cb_data (free_smb2dir), so we must never
 * hand it a stack pointer: every call passes cb_data=NULL and netfs_cb reads
 * s_active instead. */
static struct netfs_req *s_active;

/* ---- open-handle caches (#1133) ------------------------------------ */

/* Single share (Z:), so one cached read handle and one cached append handle
 * suffice. Keyed by the full SMB path. */
static struct smb2fh *s_rd_fh;
static char s_rd_path[NETFS_PATH_MAX];
static struct smb2fh *s_wr_fh;
static char s_wr_path[NETFS_PATH_MAX];
static uint64_t s_wr_off;   /* next append offset for s_wr_fh */

/* Short-lived stat/isdir/exists cache. */
struct netfs_stat_ent {
	int valid;
	char path[NETFS_PATH_MAX];
	unsigned at;
	struct smb2_stat_64 st;
};
static struct netfs_stat_ent s_stat[NETFS_STAT_CACHE];
static unsigned s_stat_rr;

static void netfs_stat_flush(void)
{
	int i;
	for (i = 0; i < NETFS_STAT_CACHE; i++)
		s_stat[i].valid = 0;
}

static void netfs_stat_invalidate(const char *full)
{
	int i;
	for (i = 0; i < NETFS_STAT_CACHE; i++)
		if (s_stat[i].valid && strcmp(s_stat[i].path, full) == 0)
			s_stat[i].valid = 0;
}

static void netfs_stat_store(const char *full, const struct smb2_stat_64 *st)
{
	int i;
	struct netfs_stat_ent *e = 0;

	for (i = 0; i < NETFS_STAT_CACHE; i++)
		if (s_stat[i].valid && strcmp(s_stat[i].path, full) == 0)
		{
			e = &s_stat[i];
			break;
		}
	if (!e)
		for (i = 0; i < NETFS_STAT_CACHE; i++)
			if (!s_stat[i].valid)
			{
				e = &s_stat[i];
				break;
			}
	if (!e)
		e = &s_stat[s_stat_rr++ % NETFS_STAT_CACHE];
	e->valid = 1;
	snprintf(e->path, sizeof(e->path), "%s", full);
	e->at = mmb_now_ms();
	e->st = *st;
}

/* Drop every cache. smb2_destroy_context() frees the fhs and dirs it owns, so
 * a teardown must only *forget* the pointers, never touch them. */
static void netfs_caches_forget(void)
{
	s_rd_fh = 0;
	s_rd_path[0] = 0;
	s_wr_fh = 0;
	s_wr_path[0] = 0;
	s_wr_off = 0;
	netfs_stat_flush();
}

static void netfs_teardown(void)
{
	/* Clear the callback target first: smb2_destroy_context() fires any
	 * pending PDU callbacks, and after a failed op s_active still points at
	 * that op's now-returned stack frame. */
	s_active = 0;
	netfs_caches_forget();
	if (s_ctx)
		smb2_destroy_context(s_ctx);
	s_ctx = 0;
	s_connected = 0;
	s_sub[0] = 0;
}

void mmb_netdrive_disconnect(void)
{
	netfs_teardown();
}

const char *mmb_netdrive_last_error(void)
{
	return s_err[0] ? s_err : "?NETWORK DRIVE: unavailable";
}

int mmb_netdrive_configured(void)
{
	return G.opt.netdrv_enabled && G.opt.netdrv_unc[0];
}

/* ---- transport failure classification ------------------------------ */

/* True when an op's status means the connection (not the request) failed, so
 * the context is unusable and a reconnect is the only way forward. libsmb2
 * reports transport-ish NTSTATUS values as the negative errno from
 * nterror_to_errno(); a PDU timeout is delivered raw as SMB2_STATUS_IO_TIMEOUT. */
static int netfs_status_is_transport(int st)
{
	if (st == 0)
		return 0;
	if (st == (int)SMB2_STATUS_IO_TIMEOUT)
		return 1;
	switch (st)
	{
	case -ENETRESET:
	case -ETIMEDOUT:
	case -ECONNRESET:
	case -ECONNREFUSED:
	case -EPIPE:
	case -ENOTCONN:
	case -EHOSTDOWN:
	case -EHOSTUNREACH:
	case -ENETDOWN:
	case -ENETUNREACH:
	case -ESHUTDOWN:
		return 1;
	default:
		return 0;
	}
}

static void netfs_reset(struct netfs_req *r)
{
	memset(r, 0, sizeof(*r));
	s_active = r;
}

static void netfs_cb(struct smb2_context *smb2, int status,
		     void *command_data, void *private_data)
{
	struct netfs_req *r = s_active;

	(void)smb2;
	(void)private_data;
	if (!r)
		return;
	r->status = status;
	r->data = command_data;
	r->done = 1;
}

/* Wait for `r` to complete, running libsmb2's event loop on its own fd.
 * Returns r->status, or -1 on transport error / timeout / Ctrl-C. Sets s_dead
 * when the failure is one the caller should reconnect from. */
static int netfs_await(struct netfs_req *r, unsigned budget_ms)
{
	unsigned start = mmb_now_ms();

	while (!r->done)
	{
		/* intptr_t so a 64-bit Winsock SOCKET is not truncated (#1136). */
		intptr_t fd = (intptr_t)smb2_get_fd(s_ctx);
		int events = smb2_which_events(s_ctx);
		int revents;

		revents = mmb_plat_poll_fd(fd, events, 50);
		if (revents < 0)
		{
			s_dead = 1;
			return -1;
		}
		if (revents > 0)
		{
			if (smb2_service(s_ctx, revents) < 0)
			{
				s_dead = 1;
				return -1;
			}
		}
		if ((mmb_now_ms() - start) > budget_ms)
		{
			s_dead = 1;
			return -1;
		}
		/* Keep TERM/CONNECT/audio/etc. running while we wait. */
		mmb_poll();
		/* Ctrl-C aborts cleanly instead of leaving a half-open context. */
		if (G.running && G.plat && G.plat->take_break && G.plat->take_break())
		{
			netfs_teardown();
			mmb_error("?BREAK");
		}
	}
	if (netfs_status_is_transport(r->status))
		s_dead = 1;
	return r->status;
}

/* ---- config parsing / connection ----------------------------------- */

/* Split the normalised UNC "\host\share[\sub]" into its parts. */
static int netfs_parse_unc(void)
{
	const char *p = G.opt.netdrv_unc;
	int o;

	if (p[0] != '\\' || p[1] != '\\')
		return -1;
	p += 2;
	o = 0;
	while (*p && *p != '\\' && o < (int)sizeof(s_server) - 1)
		s_server[o++] = *p++;
	s_server[o] = 0;
	if (!o || *p != '\\')
		return -1;
	p++;
	o = 0;
	while (*p && *p != '\\' && o < (int)sizeof(s_share) - 1)
		s_share[o++] = *p++;
	s_share[o] = 0;
	if (!o)
		return -1;
	if (*p == '\\')
		p++;
	o = 0;
	while (*p && o < (int)sizeof(s_sub) - 1)
	{
		s_sub[o++] = (*p == '\\') ? '/' : *p;
		p++;
	}
	s_sub[o] = 0;
	return 0;
}

/* Split "DOMAIN\user" (or bare "user") into domain and user. */
static void netfs_split_user(const char *in, char *dom, int dsz, char *usr, int usz)
{
	const char *slash = 0, *p;
	dom[0] = 0;
	for (p = in; *p; p++)
		if (*p == '\\' || *p == '/')
			slash = p;
	if (slash)
	{
		int n = (int)(slash - in);
		if (n > dsz - 1)
			n = dsz - 1;
		memcpy(dom, in, (size_t)n);
		dom[n] = 0;
		strncpy(usr, slash + 1, (size_t)usz - 1);
		usr[usz - 1] = 0;
	}
	else
	{
		strncpy(usr, in, (size_t)usz - 1);
		usr[usz - 1] = 0;
	}
}

static void netfs_map_connect_error(void)
{
	uint32_t nt = s_ctx ? (uint32_t)smb2_get_nterror(s_ctx) : 0;

	if (nt == SMB2_STATUS_LOGON_FAILURE || nt == SMB2_STATUS_ACCOUNT_DISABLED)
		snprintf(s_err, sizeof(s_err), "?NETWORK DRIVE: logon failed");
	else if (nt == SMB2_STATUS_BAD_NETWORK_NAME ||
		 nt == SMB2_STATUS_OBJECT_NAME_NOT_FOUND ||
		 nt == SMB2_STATUS_ACCESS_DENIED ||
		 nt == SMB2_STATUS_NETWORK_ACCESS_DENIED)
		snprintf(s_err, sizeof(s_err), "?NETWORK DRIVE: share not found");
	else if (nt)
		snprintf(s_err, sizeof(s_err), "?NETWORK DRIVE: %s",
			 nterror_to_str(nt));
	else
		snprintf(s_err, sizeof(s_err), "?NETWORK DRIVE: host unreachable");
}

static int netfs_connect_internal(void)
{
	struct netfs_req req;
	char dom[64], usr[64];
	int rc;

	if (s_connected)
		return 0;
	if (!mmb_netdrive_configured())
	{
		snprintf(s_err, sizeof(s_err), "?NETWORK DRIVE not configured");
		return -1;
	}
	netfs_teardown();
	if (netfs_parse_unc() != 0)
	{
		snprintf(s_err, sizeof(s_err), "?NETWORK DRIVE: bad UNC");
		return -1;
	}
	s_ctx = smb2_init_context();
	if (!s_ctx)
	{
		snprintf(s_err, sizeof(s_err), "?NETWORK DRIVE: out of memory");
		return -1;
	}
	smb2_set_timeout(s_ctx, NETFS_SMB_TIMEOUT);
	/* Sign when the server requires it; no SMB3 encryption for now. */
	smb2_set_security_mode(s_ctx, SMB2_NEGOTIATE_SIGNING_ENABLED);
	if (G.opt.netdrv_user[0])
	{
		netfs_split_user(G.opt.netdrv_user, dom, sizeof(dom), usr,
				 sizeof(usr));
		if (dom[0])
			smb2_set_domain(s_ctx, dom);
		smb2_set_user(s_ctx, usr);
		if (G.opt.netdrv_pass[0])
			smb2_set_password(s_ctx, G.opt.netdrv_pass);
	}
	else
	{
		/* Guest: anonymous session (NULL password). */
		smb2_set_user(s_ctx, "Guest");
	}
	netfs_reset(&req);
	rc = smb2_connect_share_async(s_ctx, s_server, s_share, NULL, netfs_cb, NULL);
	if (rc < 0)
	{
		netfs_map_connect_error();
		netfs_teardown();
		return -1;
	}
	rc = netfs_await(&req, NETFS_CONNECT_MS);
	if (rc != 0 || req.status != 0)
	{
		netfs_map_connect_error();
		netfs_teardown();
		return -1;
	}
	s_connected = 1;
	return 0;
}

int mmb_netdrive_connect(void)
{
	return netfs_connect_internal();
}

/* Lazily connect; on failure raise the short network-drive error. */
static int netfs_ensure(void)
{
	if (s_connected)
		return 0;
	if (netfs_connect_internal() != 0)
		mmb_error(mmb_netdrive_last_error());
	return 0;
}

/* ---- path mapping -------------------------------------------------- */

/* Build the path libsmb2 wants: the configured sub-folder followed by the
 * drive-absolute path, e.g. "sub/dir/file". Empty means the share root. */
static void netfs_full(const char *path, char *out, int outsz)
{
	int n = 0;

	if (s_sub[0] && strcmp(s_sub, "/") != 0)
	{
		n = snprintf(out, (size_t)outsz, "%s", s_sub);
		if (n > 0 && out[n - 1] != '/' && n < outsz - 1)
			out[n++] = '/';
	}
	while (*path == '/')
		path++;
	snprintf(out + n, (size_t)outsz - (size_t)n, "%s", path);
}

/* ---- low-level SMB calls ------------------------------------------- */

static struct smb2fh *netfs_open_path(const char *path, int flags,
				      unsigned budget)
{
	struct netfs_req req;
	char full[NETFS_PATH_MAX];

	if (netfs_ensure() != 0)
		return 0;
	netfs_full(path, full, sizeof(full));
	netfs_reset(&req);
	if (smb2_open_async(s_ctx, full, flags, netfs_cb, NULL) < 0)
		return 0;
	if (netfs_await(&req, budget) != 0 || req.status != 0)
		return 0;
	return (struct smb2fh *)req.data;
}

static int netfs_close_path(struct smb2fh *fh)
{
	struct netfs_req req;

	if (!fh || !s_ctx)
		return -1;
	netfs_reset(&req);
	if (smb2_close_async(s_ctx, fh, netfs_cb, NULL) < 0)
		return -1;
	if (netfs_await(&req, NETFS_OP_MS) != 0)
		return -1;
	return 0;
}

/* Close a cached handle if it is the one named (or any, when full==NULL), then
 * forget it. Only touch the server while the context is still usable. */
static void netfs_read_cache_drop(const char *full)
{
	if (s_rd_fh && (!full || strcmp(s_rd_path, full) == 0))
	{
		if (!s_dead && s_ctx)
			netfs_close_path(s_rd_fh);
		s_rd_fh = 0;
		s_rd_path[0] = 0;
	}
}

static void netfs_write_cache_drop(const char *full)
{
	if (s_wr_fh && (!full || strcmp(s_wr_path, full) == 0))
	{
		if (!s_dead && s_ctx)
			netfs_close_path(s_wr_fh);
		s_wr_fh = 0;
		s_wr_path[0] = 0;
		s_wr_off = 0;
	}
}

/* ---- stat (cached + retry) ----------------------------------------- */

/* Always asks the server; does not populate the cache. Used where a stale
 * size would misplace an append. */
static int netfs_stat_query(const char *path, struct smb2_stat_64 *st)
{
	struct netfs_req req;
	char full[NETFS_PATH_MAX];

	if (netfs_ensure() != 0)
		return -1;
	netfs_full(path, full, sizeof(full));
	memset(st, 0, sizeof(*st));
	netfs_reset(&req);
	if (smb2_stat_async(s_ctx, full, st, netfs_cb, NULL) < 0)
		return -1;
	if (netfs_await(&req, NETFS_OP_MS) != 0 || req.status != 0)
		return -1;
	return 0;
}

static int netfs_stat_cached(const char *path, struct smb2_stat_64 *st)
{
	struct netfs_stat_ent *e;
	char full[NETFS_PATH_MAX];
	int i;

	if (netfs_ensure() != 0)
		return -1;
	netfs_full(path, full, sizeof(full));
	for (i = 0; i < NETFS_STAT_CACHE; i++)
	{
		e = &s_stat[i];
		if (e->valid && strcmp(e->path, full) == 0 &&
		    (mmb_now_ms() - e->at) < NETFS_STAT_TTL_MS)
		{
			*st = e->st;
			return 0;
		}
	}
	if (netfs_stat_query(path, st) != 0)
		return -1;
	netfs_stat_store(full, st);
	return 0;
}

/* Idempotent: reconnect once and retry. */
static int netfs_stat_retry(const char *path, struct smb2_stat_64 *st)
{
	int rc;

	s_dead = 0;
	rc = netfs_stat_cached(path, st);
	if (s_dead)
	{
		netfs_teardown();
		s_dead = 0;
		rc = netfs_stat_cached(path, st);
	}
	if (s_dead)
		netfs_teardown();
	return rc;
}

/* ---- directory listing (idempotent, retried) ------------------------ */

static struct smb2dir *netfs_opendir_do(const char *path)
{
	struct netfs_req req;
	char full[NETFS_PATH_MAX];

	if (netfs_ensure() != 0)
		return 0;
	netfs_full(path, full, sizeof(full));
	netfs_reset(&req);
	if (smb2_opendir_async(s_ctx, full, netfs_cb, NULL) < 0)
		return 0;
	if (netfs_await(&req, NETFS_OP_MS) != 0 || req.status != 0)
		return 0;
	return (struct smb2dir *)req.data;
}

static struct smb2dir *netfs_opendir(const char *path)
{
	struct smb2dir *d;

	s_dead = 0;
	d = netfs_opendir_do(path);
	if (s_dead)
	{
		netfs_teardown();
		s_dead = 0;
		d = netfs_opendir_do(path);
	}
	if (s_dead)
		netfs_teardown();
	return d;
}

/* ---- reads (idempotent, retried; cached read handle) --------------- */

static int netfs_read_do(const char *path, unsigned pos, void *data,
			 unsigned n, unsigned *got)
{
	struct netfs_req req;
	char full[NETFS_PATH_MAX];
	unsigned done = 0;
	int rc = 0;

	if (got)
		*got = 0;
	if (netfs_ensure() != 0)
		return -1;
	netfs_full(path, full, sizeof(full));
	if (!(s_rd_fh && strcmp(s_rd_path, full) == 0))
	{
		netfs_read_cache_drop(0);
		if (s_dead)
			return -1;
		s_rd_fh = netfs_open_path(path, O_RDONLY, NETFS_OP_MS);
		if (!s_rd_fh)
			return -1;
		snprintf(s_rd_path, sizeof(s_rd_path), "%s", full);
	}
	while (done < n)
	{
		unsigned chunk = n - done;
		if (chunk > NETFS_CHUNK)
			chunk = NETFS_CHUNK;
		netfs_reset(&req);
		if (smb2_pread_async(s_ctx, s_rd_fh, (uint8_t *)data + done,
				     chunk, (uint64_t)pos + done, netfs_cb,
				     NULL) < 0)
		{
			rc = -1;
			break;
		}
		if (netfs_await(&req, NETFS_OP_MS) < 0 || req.status < 0)
		{
			rc = -1;
			break;
		}
		if (req.status == 0)
			break;
		done += (unsigned)req.status;
		if ((unsigned)req.status < chunk)
			break; /* short read: EOF */
	}
	if (got)
		*got = done;
	return rc;
}

static int netfs_read_retry(const char *path, unsigned pos, void *data,
			    unsigned n, unsigned *got)
{
	int rc;

	s_dead = 0;
	rc = netfs_read_do(path, pos, data, n, got);
	if (s_dead)
	{
		netfs_teardown();
		s_dead = 0;
		rc = netfs_read_do(path, pos, data, n, got);
	}
	if (s_dead)
		netfs_teardown();
	return rc;
}

/* ---- writes (never replayed; cached append handle) ----------------- */

static int netfs_write_do(const char *path, const void *data, unsigned n,
			  int append)
{
	char full[NETFS_PATH_MAX];
	struct smb2fh *fh;
	uint64_t off = 0;
	unsigned done = 0;

	if (netfs_ensure() != 0)
		return -1;
	netfs_full(path, full, sizeof(full));
	/* A write makes any cached metadata stale. */
	netfs_stat_invalidate(full);
	netfs_read_cache_drop(full);
	if (s_dead)
		return -1;

	if (append)
	{
		if (!(s_wr_fh && strcmp(s_wr_path, full) == 0))
		{
			struct smb2_stat_64 st;

			netfs_write_cache_drop(0);
			if (s_dead)
				return -1;
			if (netfs_stat_query(path, &st) == 0 &&
			    st.smb2_type != SMB2_TYPE_DIRECTORY)
				off = st.smb2_size;
			if (s_dead)
				return -1;
			s_wr_fh = netfs_open_path(path, O_WRONLY | O_CREAT,
						  NETFS_OP_MS);
			if (!s_wr_fh)
				return -1;
			snprintf(s_wr_path, sizeof(s_wr_path), "%s", full);
			s_wr_off = off;
		}
		fh = s_wr_fh;
		off = s_wr_off;
		while (done < n)
		{
			struct netfs_req req;
			unsigned chunk = n - done;
			if (chunk > NETFS_CHUNK)
				chunk = NETFS_CHUNK;
			netfs_reset(&req);
			if (smb2_pwrite_async(s_ctx, fh,
					      (const uint8_t *)data + done,
					      chunk, off + done, netfs_cb,
					      NULL) < 0)
				return -1;
			if (netfs_await(&req, NETFS_OP_MS) < 0 ||
			    req.status != (int)chunk)
				return -1;
			done += chunk;
		}
		s_wr_off += n;
		return 0;
	}

	/* Overwrite: a one-shot handle. If it dies it is not replayed. */
	netfs_write_cache_drop(full);
	if (s_dead)
		return -1;
	fh = netfs_open_path(path, O_WRONLY | O_CREAT | O_TRUNC, NETFS_OP_MS);
	if (!fh)
		return -1;
	while (done < n)
	{
		struct netfs_req req;
		unsigned chunk = n - done;
		if (chunk > NETFS_CHUNK)
			chunk = NETFS_CHUNK;
		netfs_reset(&req);
		if (smb2_pwrite_async(s_ctx, fh, (const uint8_t *)data + done,
				      chunk, done, netfs_cb, NULL) < 0)
		{
			done = n + 1; /* fall out with failure */
			break;
		}
		if (netfs_await(&req, NETFS_OP_MS) < 0 || req.status != (int)chunk)
		{
			done = n + 1;
			break;
		}
		done += chunk;
	}
	if (done != n)
	{
		/* Do not try to close on a dead socket; the teardown frees it. */
		if (!s_dead)
			netfs_close_path(fh);
		return -1;
	}
	return netfs_close_path(fh);
}

static int mmb_net_write(int letter, const char *path, const void *data,
			 unsigned n, int append)
{
	int rc;

	(void)letter;
	s_dead = 0;
	rc = netfs_write_do(path, data, n, append);
	/* A dropped write is not replayed: the caller sees ?FILE and the next
	 * op reconnects lazily. */
	if (s_dead)
		netfs_teardown();
	return rc;
}

/* ---- streaming writers (FTP upload etc.) --------------------------- */

typedef struct netfs_file {
	struct smb2fh *fh;
	uint64_t off;
} netfs_file;

static void *mmb_net_wopen(int letter, const char *path, int append)
{
	netfs_file *f;
	char full[NETFS_PATH_MAX];
	uint64_t off = 0;

	(void)letter;
	s_dead = 0;
	if (netfs_ensure() != 0)
		return 0;
	netfs_full(path, full, sizeof(full));
	netfs_stat_invalidate(full);
	/* A streaming writer takes over the file; drop any cached handle so the
	 * two do not fight over the same directory entry. */
	netfs_read_cache_drop(full);
	netfs_write_cache_drop(full);
	if (append)
	{
		struct smb2_stat_64 st;
		if (netfs_stat_query(path, &st) == 0 &&
		    st.smb2_type != SMB2_TYPE_DIRECTORY)
			off = st.smb2_size;
	}
	f = calloc(1, sizeof(*f));
	if (!f)
		return 0;
	f->fh = netfs_open_path(path, O_WRONLY | O_CREAT | (append ? 0 : O_TRUNC),
				NETFS_OP_MS);
	if (!f->fh)
	{
		free(f);
		if (s_dead)
			netfs_teardown();
		return 0;
	}
	f->off = off;
	return f;
}

static int mmb_net_wwrite(void *handle, const void *data, unsigned n)
{
	netfs_file *f = handle;
	unsigned done = 0;

	if (!f || !f->fh)
		return -1;
	if (!n)
		return 0;
	s_dead = 0;
	if (netfs_ensure() != 0)
		return -1;
	while (done < n)
	{
		struct netfs_req req;
		unsigned chunk = n - done;
		if (chunk > NETFS_CHUNK)
			chunk = NETFS_CHUNK;
		netfs_reset(&req);
		if (smb2_pwrite_async(s_ctx, f->fh,
				      (const uint8_t *)data + done, chunk,
				      f->off + done, netfs_cb, NULL) < 0)
			return -1;
		if (netfs_await(&req, NETFS_OP_MS) < 0 ||
		    req.status != (int)chunk)
		{
			if (s_dead)
			{
				f->fh = 0; /* context frees it on teardown */
				netfs_teardown();
			}
			return -1;
		}
		done += chunk;
	}
	f->off += n;
	return 0;
}

static int mmb_net_wclose(void *handle)
{
	netfs_file *f = handle;
	int rc = 0;

	if (!f)
		return -1;
	if (f->fh)
		rc = netfs_close_path(f->fh);
	free(f);
	if (s_dead)
		netfs_teardown();
	return rc;
}

/* ---- mmb_vol_ops implementation ------------------------------------ */

static int mmb_net_ready(int letter)
{
	(void)letter;
	/* Must not connect: only the first real Z: use does. */
	return mmb_netdrive_configured() ? 1 : 0;
}

static int mmb_net_chdir(int letter, const char *path)
{
	struct smb2_stat_64 st;
	(void)letter;
	if (!path || !path[0] || strcmp(path, "/") == 0)
		return netfs_ensure();
	if (netfs_stat_retry(path, &st) != 0)
		return -1;
	return st.smb2_type == SMB2_TYPE_DIRECTORY ? 0 : -1;
}

static int netfs_simple(int letter, const char *path, int which)
{
	struct netfs_req req;
	char full[NETFS_PATH_MAX];
	int rc = 0;

	(void)letter;
	s_dead = 0;
	if (netfs_ensure() != 0)
		return -1;
	netfs_full(path, full, sizeof(full));
	netfs_stat_invalidate(full);
	netfs_read_cache_drop(full);
	netfs_write_cache_drop(full);
	netfs_reset(&req);
	switch (which)
	{
	case 0:
		if (smb2_mkdir_async(s_ctx, full, netfs_cb, NULL) < 0)
			rc = -1;
		break;
	case 1:
		if (smb2_rmdir_async(s_ctx, full, netfs_cb, NULL) < 0)
			rc = -1;
		break;
	default:
		if (smb2_unlink_async(s_ctx, full, netfs_cb, NULL) < 0)
			rc = -1;
		break;
	}
	if (rc == 0 && (netfs_await(&req, NETFS_OP_MS) != 0 || req.status != 0))
		rc = -1;
	if (s_dead)
		netfs_teardown();
	return rc;
}

static int mmb_net_mkdir(int letter, const char *path)
{
	return netfs_simple(letter, path, 0);
}

static int mmb_net_rmdir(int letter, const char *path)
{
	return netfs_simple(letter, path, 1);
}

static int mmb_net_unlink(int letter, const char *path)
{
	return netfs_simple(letter, path, 2);
}

static int mmb_net_rename(int letter, const char *from, const char *to)
{
	struct netfs_req req;
	char a[NETFS_PATH_MAX], b[NETFS_PATH_MAX];
	int rc;

	(void)letter;
	s_dead = 0;
	if (netfs_ensure() != 0)
		return -1;
	netfs_full(from, a, sizeof(a));
	netfs_full(to, b, sizeof(b));
	netfs_stat_invalidate(a);
	netfs_stat_invalidate(b);
	netfs_read_cache_drop(a);
	netfs_read_cache_drop(b);
	netfs_write_cache_drop(a);
	netfs_write_cache_drop(b);
	netfs_reset(&req);
	if (smb2_rename_async(s_ctx, a, b, netfs_cb, NULL) < 0)
		return -1;
	rc = (netfs_await(&req, NETFS_OP_MS) != 0 || req.status != 0) ? -1 : 0;
	if (s_dead)
		netfs_teardown();
	return rc;
}

static int mmb_net_exists(int letter, const char *path)
{
	struct smb2_stat_64 st;
	(void)letter;
	return netfs_stat_retry(path, &st) == 0;
}

static int mmb_net_size(int letter, const char *path)
{
	struct smb2_stat_64 st;
	(void)letter;
	if (netfs_stat_retry(path, &st) != 0)
		return -1;
	if (st.smb2_type == SMB2_TYPE_DIRECTORY)
		return -1;
	if (st.smb2_size > 0x7fffffffULL)
		return -1;
	return (int)st.smb2_size;
}

static int mmb_net_isdir(int letter, const char *path)
{
	struct smb2_stat_64 st;
	(void)letter;
	if (!path || !path[0] || strcmp(path, "/") == 0)
		return netfs_ensure() == 0;
	if (netfs_stat_retry(path, &st) != 0)
		return 0;
	return st.smb2_type == SMB2_TYPE_DIRECTORY;
}

static int mmb_net_list(int letter, const char *dir, const char *pat,
			char *out, int outsz, int *truncated)
{
	struct smb2dir *d;
	struct smb2dirent *e;

	(void)letter;
	if (out && outsz > 0)
		out[0] = 0;
	if (truncated)
		*truncated = 0;
	d = netfs_opendir(dir);
	if (!d)
		return -1;
	while ((e = smb2_readdir(s_ctx, d)))
	{
		int isd, used, need;
		if (strcmp(e->name, ".") == 0 || strcmp(e->name, "..") == 0)
			continue;
		if (pat && pat[0] && !mmb_glob_match(e->name, pat))
			continue;
		isd = e->st.smb2_type == SMB2_TYPE_DIRECTORY;
		used = (int)strlen(out);
		need = (int)strlen(e->name) + (isd ? 1 : 0) + 1;
		if (used + need < outsz)
		{
			strcat(out, e->name);
			if (isd)
				strcat(out, "/");
			strcat(out, "\n");
		}
		else if (truncated)
			*truncated = 1;
	}
	smb2_closedir(s_ctx, d);
	return 0;
}

static int mmb_net_list_entries(int letter, const char *dir, const char *pat,
				mmb_dirent *out, int max, int *truncated)
{
	struct smb2dir *d;
	struct smb2dirent *e;
	int n = 0, total = 0;

	(void)letter;
	if (!out || max <= 0)
		return -1;
	if (truncated)
		*truncated = 0;
	d = netfs_opendir(dir);
	if (!d)
		return -1;
	while ((e = smb2_readdir(s_ctx, d)))
	{
		mmb_dirent ent;
		int isd;
		if (strcmp(e->name, ".") == 0 || strcmp(e->name, "..") == 0)
			continue;
		if (pat && pat[0] && !mmb_glob_match(e->name, pat))
			continue;
		isd = e->st.smb2_type == SMB2_TYPE_DIRECTORY;
		memset(&ent, 0, sizeof(ent));
		strncpy(ent.name, e->name, sizeof(ent.name) - 1);
		ent.is_dir = isd;
		ent.size = isd ? -1 :
			(e->st.smb2_size > 0x7fffffffULL ? -1 :
			 (int)e->st.smb2_size);
		total++;
		mmb_dirent_offer(out, &n, max, &ent);
	}
	smb2_closedir(s_ctx, d);
	if (truncated)
		*truncated = total > max;
	return n;
}

static int mmb_net_read_at(int letter, const char *path, unsigned pos,
			   void *data, unsigned n, unsigned *got)
{
	(void)letter;
	return netfs_read_retry(path, pos, data, n, got);
}

void mmb_net_drive_line(int letter, char *out, int outsz)
{
	if (out && outsz > 0)
		out[0] = 0;
	if (!mmb_netdrive_configured())
		return;
	snprintf(out, (size_t)outsz, "%c: SMB %s (%s)", (char)letter,
		 G.opt.netdrv_unc, s_connected ? "connected" : "not connected");
}

static int mmb_net_label(int letter, char *out, int outsz)
{
	(void)letter;
	if (out && outsz > 0)
		out[0] = 0;
	return 0;
}

static int mmb_net_eject(int letter)
{
	(void)letter;
	/* Drop the SMB context only; a TERM/CONNECT session is a different
	 * socket and stays untouched. */
	mmb_netdrive_disconnect();
	return 0;
}

const mmb_vol_ops mmb_net_ops = {
	mmb_net_ready, mmb_net_chdir, mmb_net_mkdir, mmb_net_rmdir,
	mmb_net_unlink, mmb_net_rename, mmb_net_list, mmb_net_list_entries,
	mmb_net_write, mmb_net_wopen, mmb_net_wwrite, mmb_net_wclose,
	mmb_net_read_at, mmb_net_size, mmb_net_exists, mmb_net_isdir,
	mmb_net_drive_line, mmb_net_label, mmb_net_eject
};

#endif /* MMB_HAVE_SMB2 */
