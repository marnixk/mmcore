/*
 * POSIX storage backend for the native build (LN-08).
 *
 * Physical drive C: always maps to <root>/C under a configurable root
 * (MMB_DRIVE_ROOT, default $HOME/.mmbasic) and is the persistent home for
 * settings and user files. D: (and E:-H:) map to <root>/<letter> too, but are
 * only created when explicitly mounted (--drive, app mode), so an unused drive
 * never litters the host directory. A:/B: are the in-RAM volumes in
 * mmbasic/src/vfs.c and never reach this file.
 *
 * MMBasic/FatFs is case-insensitive and treats '/' as the separator; the host
 * filesystem is case-sensitive, so every lookup resolves path components by
 * scanning the parent directory (case-insensitively) before use.
 */
#include "win_compat.h"

#include "mmb_priv.h"
#include "storage_posix.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#if defined(__linux__)
#include <sys/wait.h>
#endif
#include <time.h>
#include <unistd.h>

#define DRIVE_LO 'C'
#define DRIVE_HI 'H'
#define DRIVE_N (DRIVE_HI - DRIVE_LO + 1)
#define P_BUF 1024

static int s_init;
static char s_override[26][P_BUF];
static int s_has_override[26];
static int s_ejected[26];
static int s_present[26];
static int s_notify_ready;

/* Open read handle cache, one per volume slot (#1151), mirroring the Circle
 * backend's (#984, console/storage.cpp). A chunked read_at loop over the same
 * file otherwise pays an fopen/fseek/fread/fclose per chunk; keeping one
 * FILE* positioned at the last read offset lets an in-order chunk reuse it
 * with no reopen or seek. The handle is keyed by the resolved host path and
 * invalidated whenever that path (or its volume) is modified, so a stale
 * handle can never serve old bytes. */
static FILE *s_rd[26];
static char s_rd_path[26][P_BUF];
static unsigned s_rd_pos[26];

static int drive_of(int letter);
static void drive_dir_raw(int letter, char *out, int outsz);
static void rd_cache_close(int letter);
static void rd_cache_close_path(int letter, const char *full);
static void storage_check_resume(void);
static void storage_guard(void);
static int storage_io_retry(int letter, int err);
static int is_mountpoint(const char *path);

int storage_posix_mount(int letter, const char *path)
{
	if (!drive_of(letter) || !path || !path[0] || strlen(path) >= P_BUF)
		return -1;
	snprintf(s_override[letter - 'A'], sizeof s_override[0], "%s", path);
	s_has_override[letter - 'A'] = 1;
	return 0;
}

static const char *root_dir(void)
{
	static char home_root[P_BUF];
	const char *env = getenv("MMB_DRIVE_ROOT");
	const char *home;

	if (env && *env)
		return env;
	home = getenv("HOME");
	if (home && *home)
	{
		snprintf(home_root, sizeof home_root, "%s/.mmbasic", home);
		return home_root;
	}
	return ".mmbasic";
}

static int drive_of(int letter)
{
	return letter >= DRIVE_LO && letter <= DRIVE_HI;
}

static void ensure_root(void)
{
	const char *root = root_dir();
	char db[P_BUF];

	if (s_init)
		return;
	s_init = 1;
	mkdir(root, 0777);
	/* C: is the persistent drive (settings, user files) and always exists.
	 * D:-H: are created lazily, only when explicitly mounted, so an unused
	 * drive never leaves an empty directory behind. */
	drive_dir_raw('C', db, sizeof db);
	mkdir(db, 0777);
}

static void drive_dir_raw(int letter, char *out, int outsz)
{
	if (s_has_override[letter - 'A'])
	{
		snprintf(out, (size_t)outsz, "%s",
			 s_override[letter - 'A']);
		return;
	}
	snprintf(out, (size_t)outsz, "%s/%c", root_dir(), (char)letter);
}

static int is_dir(const char *path)
{
	struct stat st;

	return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* Directory test that trusts dirent.d_type when the host fills it in, so a
 * large listing does not stat every name (#621). Symlinks and DT_UNKNOWN fall
 * back to stat() to keep following links the way the old code did. */
static int dirent_is_dir(const char *dirpath, const struct dirent *e)
{
#if defined(DT_DIR) && defined(DT_REG) && defined(DT_UNKNOWN) && \
	!defined(MMB_NO_DTYPE)
	if (e->d_type == DT_DIR)
		return 1;
	if (e->d_type == DT_REG)
		return 0;
#endif
	{
		char sub[P_BUF];

		snprintf(sub, sizeof sub, "%s/%s", dirpath, e->d_name);
		return is_dir(sub);
	}
}

static int ci_lookup(const char *dir, const char *name, char *out, int outsz)
{
	DIR *d = opendir(dir);
	struct dirent *e;

	if (!d)
		return 0;
	while ((e = readdir(d)))
	{
		if (strcasecmp(e->d_name, name) == 0)
		{
			snprintf(out, (size_t)outsz, "%s", e->d_name);
			closedir(d);
			return 1;
		}
	}
	closedir(d);
	return 0;
}

static void path_append(char *cur, const char *name)
{
	size_t n = strlen(cur);

	if (n && cur[n - 1] != '/' && n + 1 < P_BUF)
		strncat(cur, "/", P_BUF - n - 1);
	strncat(cur, name, P_BUF - strlen(cur) - 1);
}

/* Resolve a drive-absolute path to a host path. must_exist=1 fails if any
 * component is missing; must_exist=0 keeps the literal name for the missing
 * tail (used for create). Returns 1 on success. */
static int build_path(int letter, const char *path, char *out, int outsz,
		      int must_exist)
{
	char cur[P_BUF];
	char work[P_BUF];
	char *tok, *save;
	int rootlen;

	if (!drive_of(letter))
		return 0;
	drive_dir_raw(letter, cur, sizeof cur);
	rootlen = (int)strlen(cur);
	if (!path)
		path = "";
	snprintf(work, sizeof work, "%s", path);

	for (tok = strtok_r(work, "/", &save); tok;
	     tok = strtok_r(0, "/", &save))
	{
		char actual[512];

		if (!tok[0] || strcmp(tok, ".") == 0)
			continue;
		if (strcmp(tok, "..") == 0)
		{
			char *slash;

			if ((int)strlen(cur) <= (int)rootlen)
				continue; /* never above the drive root */
			slash = strrchr(cur, '/');
			if (slash && (int)(slash - cur) >= (int)rootlen)
				*slash = 0;
			continue;
		}
		if (ci_lookup(cur, tok, actual, sizeof actual))
			path_append(cur, actual);
		else if (!must_exist)
			path_append(cur, tok);
		else
			return 0;
	}
	snprintf(out, (size_t)outsz, "%s", cur);
	return 1;
}

static void mkdirs(const char *full)
{
	char tmp[P_BUF];
	char *p;

	snprintf(tmp, sizeof tmp, "%s", full);
	for (p = tmp + 1; *p; p++)
	{
		if (*p == '/')
		{
			*p = 0;
			mkdir(tmp, 0777);
			*p = '/';
		}
	}
	mkdir(tmp, 0777);
}

/* Create every parent directory of a file path, but not the final name. */
static void mkdirs_parent(const char *full)
{
	char tmp[P_BUF];
	char *slash;

	snprintf(tmp, sizeof tmp, "%s", full);
	slash = strrchr(tmp, '/');
	if (!slash)
		return;
	*slash = 0;
	if (tmp[0])
		mkdirs(tmp);
}

static int glob_ci(const char *pat, const char *s)
{
	while (*pat)
	{
		if (*pat == '*')
		{
			pat++;
			if (!*pat)
				return 1;
			for (; *s; s++)
				if (glob_ci(pat, s))
					return 1;
			return 0;
		}
		else if (*pat == '?')
		{
			if (!*s)
				return 0;
			pat++;
			s++;
		}
		else
		{
			if (tolower((unsigned char)*pat) !=
			    tolower((unsigned char)*s))
				return 0;
			pat++;
			s++;
		}
	}
	return *s == 0;
}

int mmb_fat_ready(int letter)
{
	char db[P_BUF];

	for (;;)
	{
		int present, err;

		storage_guard();
		ensure_root();
		if (!drive_of(letter) || s_ejected[letter - 'A'])
			return 0;
		drive_dir_raw(letter, db, sizeof db);
		errno = 0;
		present = is_dir(db);
		if (present)
			return 1;
		err = errno;
		if (!storage_io_retry(letter, err))
			return 0;
	}
}

/* Validate that a directory-absolute path names a directory. The current
 * directory is session state and lives in the per-console mmb; only the drive
 * mount table is global. */
int mmb_fat_chdir(int letter, const char *path)
{
	char full[P_BUF];

	for (;;)
	{
		int err;

		storage_guard();
		ensure_root();
		if (!path || !path[0] || strcmp(path, "/") == 0)
		{
			char db[P_BUF];

			if (!drive_of(letter))
				return -1;
			drive_dir_raw(letter, db, sizeof db);
			errno = 0;
			if (is_dir(db))
				return 0;
			err = errno;
		}
		else
		{
			errno = 0;
			if (build_path(letter, path, full, sizeof full, 1) &&
			    is_dir(full))
				return 0;
			err = errno;
		}
		if (!storage_io_retry(letter, err))
			return -1;
	}
}

int mmb_fat_mkdir(int letter, const char *path)
{
	char full[P_BUF];

	for (;;)
	{
		int err;

		storage_guard();
		ensure_root();
		errno = 0;
		if (!build_path(letter, path, full, sizeof full, 0))
		{
			err = errno;
		}
		else
		{
			/* A directory change can introduce or remove a name the
			 * cached path resolution depended on; drop the volume's
			 * handle to be safe. */
			rd_cache_close(letter);
			mkdirs(full);
			errno = 0;
			if (is_dir(full))
				return 0;
			err = errno;
		}
		if (!storage_io_retry(letter, err))
			return -1;
	}
}

int mmb_fat_rmdir(int letter, const char *path)
{
	char full[P_BUF];

	for (;;)
	{
		int err;

		storage_guard();
		ensure_root();
		errno = 0;
		if (!build_path(letter, path, full, sizeof full, 1))
		{
			err = errno;
		}
		else
		{
			rd_cache_close(letter);
			if (rmdir(full) == 0)
				return 0;
			err = errno;
		}
		if (!storage_io_retry(letter, err))
			return -1;
	}
}

int mmb_fat_unlink(int letter, const char *path)
{
	char full[P_BUF];

	for (;;)
	{
		int err;

		storage_guard();
		ensure_root();
		errno = 0;
		if (!build_path(letter, path, full, sizeof full, 1))
		{
			err = errno;
		}
		else
		{
			rd_cache_close(letter);
			if (unlink(full) == 0)
				return 0;
			err = errno;
		}
		if (!storage_io_retry(letter, err))
			return -1;
	}
}

int mmb_fat_rename(int letter, const char *from, const char *to)
{
	char a[P_BUF], b[P_BUF];

	for (;;)
	{
		int err;

		storage_guard();
		ensure_root();
		errno = 0;
		if (!build_path(letter, from, a, sizeof a, 1) ||
		    !build_path(letter, to, b, sizeof b, 0))
		{
			err = errno;
		}
		else
		{
			/* The cached handle can name either end of the rename. */
			rd_cache_close(letter);
			if (rename(a, b) == 0)
				return 0;
			err = errno;
		}
		if (!storage_io_retry(letter, err))
			return -1;
	}
}

int mmb_fat_exists(int letter, const char *path)
{
	char full[P_BUF];
	struct stat st;

	for (;;)
	{
		int err;

		storage_guard();
		ensure_root();
		errno = 0;
		if (!build_path(letter, path, full, sizeof full, 1))
		{
			err = errno;
		}
		else
		{
			if (stat(full, &st) == 0)
				return 1;
			err = errno;
		}
		if (!storage_io_retry(letter, err))
			return 0;
	}
}

int mmb_fat_size(int letter, const char *path)
{
	char full[P_BUF];
	struct stat st;

	for (;;)
	{
		int err;

		storage_guard();
		ensure_root();
		errno = 0;
		if (!build_path(letter, path, full, sizeof full, 1))
		{
			err = errno;
		}
		else
		{
			if (stat(full, &st) == 0)
				return S_ISDIR(st.st_mode) ? -1 : (int)st.st_size;
			err = errno;
		}
		if (!storage_io_retry(letter, err))
			return -1;
	}
}

int mmb_fat_isdir(int letter, const char *path)
{
	char full[P_BUF];
	struct stat st;

	for (;;)
	{
		int err;

		storage_guard();
		ensure_root();
		if (!path || !path[0] || strcmp(path, "/") == 0)
			return 1; /* drive root is always a directory */
		errno = 0;
		if (!build_path(letter, path, full, sizeof full, 1))
		{
			err = errno;
		}
		else
		{
			if (stat(full, &st) == 0)
				return S_ISDIR(st.st_mode) ? 1 : 0;
			err = errno;
		}
		if (!storage_io_retry(letter, err))
			return 0;
	}
}

int mmb_fat_list(int letter, const char *dir, const char *pat, char *out,
		 int outsz, int *truncated)
{
	char full[P_BUF];
	DIR *d;
	struct dirent *e;

	for (;;)
	{
		int err;

		storage_guard();
		ensure_root();
		if (out && outsz > 0)
			out[0] = 0;
		if (truncated)
			*truncated = 0;
		errno = 0;
		if (!dir || !dir[0] || strcmp(dir, "/") == 0)
			drive_dir_raw(letter, full, sizeof full);
		else if (!build_path(letter, dir, full, sizeof full, 1))
		{
			err = errno;
			goto list_retry;
		}
		d = opendir(full);
		if (!d)
		{
			err = errno;
			goto list_retry;
		}
		if (!pat || !pat[0])
			pat = "*"; /* a bare directory (e.g. DIR "C:/") lists everything */
		errno = 0;
		while ((e = readdir(d)))
		{
			int isd;
			size_t used;

			if (strcmp(e->d_name, ".") == 0 ||
			    strcmp(e->d_name, "..") == 0)
				continue;
			if (!glob_ci(pat, e->d_name))
				continue;
			isd = dirent_is_dir(full, e);
			used = out ? strlen(out) : 0;
			if (out && used + strlen(e->d_name) + 2 < (size_t)outsz)
			{
				strcat(out, e->d_name);
				if (isd)
					strcat(out, "/");
				strcat(out, "\n");
			}
			else
			{
				if (truncated)
					*truncated = 1;
				break;
			}
		}
		err = errno;
		closedir(d);
		if (err == 0)
			return 0;
	list_retry:
		if (!storage_io_retry(letter, err))
			return -1;
	}
}

int mmb_fat_list_entries(int letter, const char *dir, const char *pat,
			 mmb_dirent *out, int max, int *truncated)
{
	char full[P_BUF];
	char sub[P_BUF];
	DIR *d;
	struct dirent *e;
	int n, total, i;
	struct stat st;

	for (;;)
	{
		int err;

		storage_guard();
		ensure_root();
		n = 0;
		total = 0;
		if (truncated)
			*truncated = 0;
		if (!out || max <= 0)
			return -1;
		errno = 0;
		if (!dir || !dir[0] || strcmp(dir, "/") == 0)
			drive_dir_raw(letter, full, sizeof full);
		else if (!build_path(letter, dir, full, sizeof full, 1))
		{
			err = errno;
			goto entries_retry;
		}
		d = opendir(full);
		if (!d)
		{
			err = errno;
			goto entries_retry;
		}
		if (!pat || !pat[0])
			pat = "*";
		errno = 0;
		while ((e = readdir(d)))
		{
			int isd;
			mmb_dirent ent;

			if (strcmp(e->d_name, ".") == 0 ||
			    strcmp(e->d_name, "..") == 0)
				continue;
			if (!glob_ci(pat, e->d_name))
				continue;
			isd = dirent_is_dir(full, e);
			memset(&ent, 0, sizeof(ent));
			snprintf(ent.name, sizeof(ent.name), "%s", e->d_name);
			ent.is_dir = isd;
			ent.size = -1; /* stat()ed lazily below */
			total++;
			/* Keep the sorted-first `max` regardless of readdir order
			 * (#676). */
			mmb_dirent_offer(out, &n, max, &ent);
		}
		err = errno;
		closedir(d);
		if (err == 0)
			break;
	entries_retry:
		if (!storage_io_retry(letter, err))
			return -1;
	}
	/* #694: fill sizes without stat()ing every name the filesystem enumerated.
	 * The d_type fast path in dirent_is_dir already gives the directory bit, so
	 * only the regular files that survived the cap need a size lookup. */
	for (i = 0; i < n; i++)
	{
		if (out[i].is_dir)
			continue;
		snprintf(sub, sizeof sub, "%s/%s", full, out[i].name);
		if (stat(sub, &st) == 0)
			out[i].size = (int)st.st_size;
	}
	if (truncated)
		*truncated = total > max;
	return n;
}

/* Drop the cached read handle for one volume. */
static void rd_cache_close(int letter)
{
	int idx;

	if (!drive_of(letter))
		return;
	idx = letter - 'A';
	if (s_rd[idx])
	{
		fclose(s_rd[idx]);
		s_rd[idx] = 0;
	}
	s_rd_path[idx][0] = 0;
	s_rd_pos[idx] = 0;
}

static void rd_cache_close_all(void)
{
	int letter;

	for (letter = DRIVE_LO; letter <= DRIVE_HI; letter++)
		rd_cache_close(letter);
}

/* ---- persistence recovery after standby ------------------------------- *
 * mmcore's persistent volumes can disappear or go stale when suspend-to-RAM
 * resets the SD/USB controller behind them. That is true for a live boot
 * (MMCORE, optionally the read-write overlay) and for a disk install, whose
 * GRUB line names MMCORE-SYS as C: and MMCORE-DATA as D:.
 *
 * Recovery is enabled by either boot shape: mmcore.live=1, or mmcore.sys= on
 * an installed command line. Which label backs which drive is an explicit map
 * built from that command line and the drive mounts, never from a mount-point
 * basename: after one recovery the path may be a fallback, and deriving the
 * label from it would look for a volume that cannot exist. Each volume keeps
 * its ORIGINAL mountpoint so later sleeps and settings paths stay stable.
 *
 * A resume is detected from the growing gap between CLOCK_BOOTTIME and
 * CLOCK_MONOTONIC. The same recovery runs when an I/O call fails with
 * EIO/ENODEV/EROFS/ESTALE at any other time, because the device may only
 * re-enumerate seconds after userspace resumed. The mount is retried with
 * backoff for up to ~30 s: remount in place first, then lazy-umount and mount
 * by label at the original mountpoint.
 *
 * The recovery itself is POSIX, not Linux-only: the clock check is the only
 * Linux piece, and the host test drives the same recovery through
 * storage_mount_ops so the remount/re-point path is covered without a live
 * ISO. */
#define STORAGE_VOL_MAX 2
#define STORAGE_RECOVER_ATTEMPTS 60
#define STORAGE_RECOVER_DELAY_MS 500

typedef struct {
	char letter;
	char label[64];
	char path[P_BUF]; /* original mountpoint; never rewritten */
	int lost_notified;
} storage_volume;

#if defined(__linux__)
static long long s_suspend_offset_ms;
static int s_suspend_tracked;
#endif
static int s_live_boot = -1;
static int s_installed_boot;
static int s_recovery_enabled = -1;
static int s_boot_scanned;
static char s_sys_spec[128];
static char s_data_spec[128];
static int s_overlay_backed = -1;
static char s_overlay_root[P_BUF];
static int s_overlay_lost_notified;
static int s_overlay_broken;
static int s_in_recovery;
static unsigned s_storage_generation;
static storage_volume s_vols[STORAGE_VOL_MAX];
static int s_vol_count;
static int s_vol_map_valid;

/* Bumped whenever the backing store is remounted/re-pointed. The VFS uses it
 * to drop per-file read-ahead buffers that were filled before a suspend, so a
 * file open across it is reopened against the recovered volume. */
unsigned mmb_storage_generation(void)
{
	return s_storage_generation;
}

#ifdef MMB_STORAGE_TEST
static int s_test_live = -1;
static int s_test_overlay = -1;
static const storage_mount_ops *s_test_ops;
static char s_test_cmdline[512];
static int s_test_recover_attempts = -1;
static int s_test_recover_delay = -1;

void mmb_storage_test_set_live(int live)
{
	s_test_live = live;
	s_boot_scanned = 0;
	s_recovery_enabled = -1;
	s_vol_map_valid = 0;
}

void mmb_storage_test_set_overlay(int overlay)
{
	s_test_overlay = overlay;
}

void mmb_storage_test_set_ops(const storage_mount_ops *ops)
{
	s_test_ops = ops;
}

void mmb_storage_test_set_cmdline(const char *cmdline)
{
	snprintf(s_test_cmdline, sizeof s_test_cmdline, "%s",
		 cmdline ? cmdline : "");
	s_boot_scanned = 0;
	s_recovery_enabled = -1;
	s_vol_map_valid = 0;
}

void mmb_storage_test_set_recovery(int attempts, int delay_ms)
{
	s_test_recover_attempts = attempts;
	s_test_recover_delay = delay_ms;
}

void mmb_storage_test_reset(void)
{
	s_test_live = -1;
	s_test_overlay = -1;
	s_test_ops = 0;
	s_test_cmdline[0] = 0;
	s_test_recover_attempts = -1;
	s_test_recover_delay = -1;
	s_boot_scanned = 0;
	s_recovery_enabled = -1;
	s_vol_map_valid = 0;
	s_overlay_backed = -1;
	s_overlay_root[0] = 0;
	s_overlay_broken = 0;
	s_overlay_lost_notified = 0;
	s_vol_count = 0;
}
#endif

static int root_has(const char *root, const char *rel)
{
	char path[P_BUF];

	snprintf(path, sizeof path, "%s/%s", root, rel);
	return is_dir(path);
}

static int root_has_file(const char *root, const char *rel)
{
	char path[P_BUF];

	snprintf(path, sizeof path, "%s/%s", root, rel);
	return access(path, F_OK) == 0;
}

/* --read-write seeds /.mmcore-rw and rw/upper + rw/work; live-init then uses
 * that volume as the root overlay's upper. Detect it at boot, before a resume
 * can drop the mount and make the paths unreadable. */
static int live_overlay_backed(const char *root)
{
#ifdef MMB_STORAGE_TEST
	if (s_test_overlay >= 0)
	{
		s_overlay_backed = s_test_overlay;
		return s_test_overlay;
	}
#endif
	if (s_overlay_backed >= 0 && s_overlay_root[0] &&
	    strcmp(s_overlay_root, root) == 0)
		return s_overlay_backed;
	snprintf(s_overlay_root, sizeof s_overlay_root, "%s", root);
	s_overlay_backed =
		(root_has(root, "rw/upper") && root_has(root, "rw/work") &&
		 root_has_file(root, ".mmcore-rw")) ? 1 : 0;
	return s_overlay_backed;
}

static void cmdline_spec(const char *cmdline, const char *prefix, char *out,
			 size_t cap)
{
	char work[512];
	char *tok, *save;

	out[0] = 0;
	snprintf(work, sizeof work, "%s", cmdline);
	for (tok = strtok_r(work, " \t\r\n", &save); tok;
	     tok = strtok_r(0, " \t\r\n", &save))
	{
		size_t n = strlen(prefix);

		if (strncmp(tok, prefix, n) == 0)
		{
			snprintf(out, cap, "%s", tok + n);
			return;
		}
	}
}

static void spec_label(const char *spec, const char *fallback, char *out,
		       size_t cap)
{
	if (strncmp(spec, "LABEL=", 6) == 0 && spec[6])
		snprintf(out, cap, "%s", spec + 6);
	else
		snprintf(out, cap, "%s", fallback);
}

static void storage_boot_scan(void)
{
	static char proc_cmdline[512];
	const char *cmd = 0;
	FILE *f;
	size_t n;

	if (s_boot_scanned)
		return;
#ifdef MMB_STORAGE_TEST
	if (s_test_cmdline[0])
		cmd = s_test_cmdline;
#endif
	if (!cmd)
	{
		f = fopen("/proc/cmdline", "rb");
		if (f)
		{
			n = fread(proc_cmdline, 1, sizeof(proc_cmdline) - 1, f);
			fclose(f);
			proc_cmdline[n] = 0;
			cmd = proc_cmdline;
		}
		else
			cmd = "";
	}
	s_sys_spec[0] = 0;
	s_data_spec[0] = 0;
	s_live_boot = strstr(cmd, "mmcore.live=1") ? 1 : 0;
	cmdline_spec(cmd, "mmcore.sys=", s_sys_spec, sizeof s_sys_spec);
	cmdline_spec(cmd, "mmcore.data=", s_data_spec, sizeof s_data_spec);
	s_installed_boot = s_sys_spec[0] != 0;
#ifdef MMB_STORAGE_TEST
	if (s_test_live >= 0)
	{
		s_live_boot = s_test_live;
		if (s_live_boot)
			s_installed_boot = 0;
	}
#endif
	s_recovery_enabled = s_live_boot || s_installed_boot;
	s_boot_scanned = 1;
}

static int storage_recovery_enabled(void)
{
	if (s_recovery_enabled < 0)
		storage_boot_scan();
	return s_recovery_enabled;
}

static storage_volume *storage_volume_for(int letter)
{
	int i;

	if (!s_vol_map_valid)
		return 0;
	for (i = 0; i < s_vol_count; i++)
		if (s_vols[i].letter == letter)
			return &s_vols[i];
	return 0;
}

/* Build the explicit drive -> label map. C: is MMCORE-SYS on an installed
 * disk and MMCORE on a live boot; D: is MMCORE-DATA when --drive mounted it.
 * The original mountpoint comes from the environment/option that set the
 * drive, so recovery never has to guess it from a path. */
static void storage_volume_map(void)
{
	const char *env;
	storage_volume *v;

	if (s_vol_map_valid)
		return;
	storage_boot_scan();
	memset(s_vols, 0, sizeof s_vols);
	s_vol_count = 0;
	if (!s_recovery_enabled)
	{
		s_vol_map_valid = 1;
		return;
	}

	env = getenv("MMB_DRIVE_ROOT");
	/* A read-only live session without a persistent MMCORE partition has no
	 * volume to recover; do not spend the retry budget on it after every
	 * resume. A partition that was mounted (or is still present) is mapped. */
	if (s_live_boot && !(env && env[0]) && !is_mountpoint("/media/mmcore") &&
	    !is_dir("/media/mmcore"))
	{
		s_vol_map_valid = 1;
		return;
	}

	v = &s_vols[s_vol_count++];
	v->letter = 'C';
	if (s_live_boot)
		snprintf(v->label, sizeof v->label, "MMCORE");
	else
		spec_label(s_sys_spec, "MMCORE-SYS", v->label, sizeof v->label);
	if (env && env[0])
		snprintf(v->path, sizeof v->path, "%s", env);
	else if (s_live_boot)
		snprintf(v->path, sizeof v->path, "/media/mmcore");
	else
		snprintf(v->path, sizeof v->path, "/media/mmcore-sys");

	if (s_installed_boot && s_has_override['D' - 'A'])
	{
		v = &s_vols[s_vol_count++];
		v->letter = 'D';
		spec_label(s_data_spec, "MMCORE-DATA", v->label,
			   sizeof v->label);
		snprintf(v->path, sizeof v->path, "%s",
			 s_override['D' - 'A']);
	}

	/* Cache the overlay bit while the original C: mount is still readable;
	 * after a resume the volume backing it may be gone. */
	(void)live_overlay_backed(s_vols[0].path);
	s_vol_map_valid = 1;
}

/* A mount point has a different device id from its parent directory. */
static int is_mountpoint(const char *path)
{
	char parent[P_BUF];
	struct stat a, b;
	size_t n;

	if (!path || stat(path, &a) != 0 || !S_ISDIR(a.st_mode))
		return 0;
	snprintf(parent, sizeof parent, "%s", path);
	n = strlen(parent);
	while (n > 1 && parent[n - 1] == '/')
		parent[--n] = 0;
	while (n > 1 && parent[n - 1] != '/')
		n--;
	if (n > 1)
		parent[n - 1] = 0;
	else
		strcpy(parent, "/");
	if (stat(parent, &b) != 0)
		return 0;
	return a.st_dev != b.st_dev;
}

/* Run a mount helper without letting its diagnostics paint over the UI. */
#if defined(_WIN32)
/* The Windows desktop build has no live ISO or mount helpers. */
static int run_mount(char *const argv[])
{
	(void)argv;
	return -1;
}
#else
static int run_mount(char *const argv[])
{
	pid_t pid;
	int st;

	pid = fork();
	if (pid < 0)
		return -1;
	if (pid == 0)
	{
		int devnull = open("/dev/null", O_WRONLY);

		if (devnull >= 0)
		{
			dup2(devnull, STDOUT_FILENO);
			dup2(devnull, STDERR_FILENO);
			if (devnull > STDERR_FILENO)
				close(devnull);
		}
		execv(argv[0], argv);
		_exit(127);
	}
	if (waitpid(pid, &st, 0) != pid)
		return -1;
	return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : -1;
}
#endif

/* Mount the volume carrying `label` at `path`, writable. */
static int mount_by_label(const char *path, const char *label)
{
	static const char *const helpers[] = {
		"/bin/mount", "/sbin/mount", "/usr/bin/mount"
	};
	size_t i;

	for (i = 0; i < sizeof(helpers) / sizeof(helpers[0]); i++)
	{
		char *argv[] = { (char *)helpers[i], "-o", "rw", "-L",
				 (char *)label, (char *)path, 0 };

		if (run_mount(argv) == 0)
			return 0;
	}
	return -1;
}

static int remount_root_rw(const char *root)
{
	static const char *const helpers[] = {
		"/bin/mount", "/sbin/mount", "/usr/bin/mount"
	};
	size_t i;

	for (i = 0; i < sizeof(helpers) / sizeof(helpers[0]); i++)
	{
		char *argv[] = { (char *)helpers[i], "-o", "remount,rw",
				 (char *)root, 0 };

		if (run_mount(argv) == 0)
			return 0;
	}
	return -1;
}

/* Drop a dead mount entry so the volume can be mounted again at the same
 * original path. Lazy (`-l`) never blocks on busy files. */
static int unmount_lazy_path(const char *path)
{
	static const char *const helpers[] = {
		"/bin/umount", "/sbin/umount", "/usr/bin/umount"
	};
	size_t i;

	for (i = 0; i < sizeof(helpers) / sizeof(helpers[0]); i++)
	{
		char *argv[] = { (char *)helpers[i], "-l", (char *)path, 0 };

		if (run_mount(argv) == 0)
			return 0;
	}
	return -1;
}

/* A write through the root overlay proves its upper is still usable. Used
 * only after a resume when MMCORE backs the upper (--read-write). */
static int root_overlay_writable(void)
{
	int fd = open("/tmp/.mmcore-persist-probe", O_WRONLY | O_CREAT | O_TRUNC,
		      0600);

	if (fd < 0)
		return 0;
	close(fd);
	unlink("/tmp/.mmcore-persist-probe");
	return 1;
}

/* A write into the drive root proves the (re)mounted filesystem is alive and
 * accepts writes, rather than being a mount entry backed by a dead device. */
static int drive_root_writable(const char *root)
{
	char path[P_BUF];
	int fd;

	snprintf(path, sizeof path, "%s/.mmcore-persist-probe", root);
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0)
		return 0;
	close(fd);
	unlink(path);
	return 1;
}

static const storage_mount_ops s_default_ops = {
	is_mountpoint,
	mount_by_label,
	remount_root_rw,
	root_overlay_writable,
	drive_root_writable,
	0,
	unmount_lazy_path
};

static const storage_mount_ops *storage_ops(void)
{
#ifdef MMB_STORAGE_TEST
	if (s_test_ops)
		return s_test_ops;
#endif
	return &s_default_ops;
}

static void storage_sleep_ms(int ms)
{
#if defined(_WIN32)
	Sleep((DWORD)ms);
#else
	usleep((useconds_t)ms * 1000);
#endif
}

static int volume_overlay(const storage_volume *v)
{
	return v->letter == 'C' && s_overlay_backed == 1;
}

/* Return 0 when the volume could not be recovered, 1 when it is fully usable
 * at `used`, and 2 when C: is usable but a --read-write overlay still needs a
 * reboot. `fresh` is set when the volume had to be mounted (or retried), so
 * the caller can report a recovery without spam on a normal resume. */
static int recover_volume(const storage_volume *v, char *used, size_t usedsz,
			  int *fresh)
{
	const storage_mount_ops *ops = storage_ops();
	const char *candidates[4];
	int n = 0, i, attempt;
	int attempts = STORAGE_RECOVER_ATTEMPTS;
	int delay = STORAGE_RECOVER_DELAY_MS;
	int overlay = volume_overlay(v);

#ifdef MMB_STORAGE_TEST
	if (s_test_recover_attempts >= 0)
		attempts = s_test_recover_attempts;
	if (s_test_recover_delay >= 0)
		delay = s_test_recover_delay;
#endif
	*fresh = 0;
	candidates[n++] = v->path;
	/* A dead mount entry can keep the original pathname occupied even after
	 * a lazy umount. C: may then use a fresh mountpoint; the label is still
	 * the explicit one, so a later sleep finds the volume again. */
	if (v->letter == 'C')
	{
		if (ops->recovery_path && ops->recovery_path[0] &&
		    strcmp(ops->recovery_path, v->path) != 0)
			candidates[n++] = ops->recovery_path;
		candidates[n++] = "/run/mmcore-recovered";
		if (strcmp(v->path, "/media/mmcore-recovered") != 0)
			candidates[n++] = "/media/mmcore-recovered";
	}

	for (attempt = 0; attempt < attempts; attempt++)
	{
		for (i = 0; i < n; i++)
		{
			const char *p = candidates[i];

			if (i == 0 && ops->is_mountpoint(p))
			{
				(void)ops->remount_rw(p);
				if (ops->root_writable(p))
				{
					snprintf(used, usedsz, "%s", p);
					return overlay &&
							       !ops->overlay_writable()
						       ? 2
						       : 1;
				}
				if (ops->unmount_lazy)
					(void)ops->unmount_lazy(p);
			}
			mkdir(p, 0777);
			if (ops->mount_label(p, v->label) == 0 &&
			    ops->root_writable(p))
			{
				snprintf(used, usedsz, "%s", p);
				*fresh = 1;
				return overlay && !ops->overlay_writable() ? 2 : 1;
			}
		}
		if (attempt + 1 < attempts && delay > 0)
			storage_sleep_ms(delay);
	}
	return 0;
}

static void storage_apply_recovered(storage_volume *v, const char *used,
				    int result, int fresh)
{
	if (v->letter == 'C')
		setenv("MMB_DRIVE_ROOT", used, 1);
	else if (v->letter == 'D')
		snprintf(s_override[v->letter - 'A'],
			 sizeof s_override[0], "%s", used);
	s_init = 0;
	rd_cache_close_all();
	s_storage_generation++;
	v->lost_notified = 0;

	if (v->letter == 'C')
	{
		if (result == 1)
		{
			s_overlay_broken = 0;
			s_overlay_lost_notified = 0;
		}
		else if (!s_overlay_lost_notified)
		{
			s_overlay_lost_notified = 1;
			mmb_storage_notice(
				"!MMCORE C: recovered after standby: reboot to restore session storage");
		}
	}
	if (fresh && result == 1)
	{
		char msg[64];

		snprintf(msg, sizeof msg,
			 "!MMCORE %c: storage recovered after standby",
			 v->letter);
		mmb_storage_notice(msg);
	}
}

static void storage_note_lost(storage_volume *v)
{
	int overlay = volume_overlay(v);

	if (overlay)
	{
		s_overlay_broken = 1;
		if (!s_overlay_lost_notified)
		{
			s_overlay_lost_notified = 1;
			mmb_storage_notice(
				"!MMCORE reset after standby: reboot to recover storage");
		}
	}
	if (!v->lost_notified)
	{
		char msg[64];

		v->lost_notified = 1;
		if (!overlay)
		{
			snprintf(msg, sizeof msg,
				 "!MMCORE %c: storage unavailable after standby",
				 v->letter);
			mmb_storage_notice(msg);
		}
	}
}

static void storage_revalidate_all(void)
{
	int i;

	if (!storage_recovery_enabled())
		return;
	storage_volume_map();
	for (i = 0; i < s_vol_count; i++)
	{
		storage_volume *v = &s_vols[i];
		char used[P_BUF];
		int fresh = 0;
		int result;

		if (s_in_recovery)
			return;
		s_in_recovery = 1;
		result = recover_volume(v, used, sizeof used, &fresh);
		s_in_recovery = 0;
		if (result)
			storage_apply_recovered(v, used, result, fresh);
		else
			storage_note_lost(v);
	}
}

static int storage_recover_letter(int letter)
{
	storage_volume *v;
	char used[P_BUF];
	int fresh = 0;
	int result;

	if (!storage_recovery_enabled())
		return 0;
	storage_volume_map();
	v = storage_volume_for(letter);
	if (!v || s_in_recovery)
		return 0;
	s_in_recovery = 1;
	result = recover_volume(v, used, sizeof used, &fresh);
	s_in_recovery = 0;
	if (!result)
		return 0;
	storage_apply_recovered(v, used, result, fresh);
	return 1;
}

static int storage_io_error(int err)
{
	if (err == EIO || err == ENODEV || err == EROFS)
		return 1;
#ifdef ESTALE
	if (err == ESTALE)
		return 1;
#endif
	return 0;
}

static int storage_io_retry(int letter, int err)
{
	if (!storage_io_error(err))
		return 0;
	return storage_recover_letter(letter);
}

static void storage_guard(void)
{
	storage_check_resume();
}

static void storage_check_resume(void)
{
#if defined(__linux__)
	struct timespec bt, mt;
	long long off;

	if (!storage_recovery_enabled())
		return;
	storage_volume_map();
	if (clock_gettime(CLOCK_BOOTTIME, &bt) != 0 ||
	    clock_gettime(CLOCK_MONOTONIC, &mt) != 0)
		return;
	off = (long long)bt.tv_sec * 1000 + bt.tv_nsec / 1000000
	    - ((long long)mt.tv_sec * 1000 + mt.tv_nsec / 1000000);
	if (!s_suspend_tracked)
	{
		s_suspend_tracked = 1;
		s_suspend_offset_ms = off;
		return;
	}
	/* BOOTTIME - MONOTONIC only grows while the machine is suspended. */
	if (off - s_suspend_offset_ms >= 2000)
	{
		s_suspend_offset_ms = off;
		storage_revalidate_all();
	}
	else if (off < s_suspend_offset_ms)
		s_suspend_offset_ms = off; /* clock source reset/oddity */
#endif
}

void mmb_storage_revalidate(void)
{
	storage_revalidate_all();
}

int mmb_storage_persistence_lost(void)
{
	const storage_mount_ops *ops = storage_ops();
	const char *root;

	if (!storage_recovery_enabled())
		return 0;
	storage_check_resume();
	if (s_overlay_broken)
		return 1;
	storage_volume_map();
	root = getenv("MMB_DRIVE_ROOT");
	if (!root || !root[0])
		root = s_vol_count ? s_vols[0].path : "/media/mmcore";
	return ops->is_mountpoint(root) ? 0 : 1;
}

#ifdef MMB_STORAGE_TEST
int mmb_storage_test_io_error(int letter, int err)
{
	return storage_io_retry(letter, err);
}
#endif

/* Close the cached handle only when it is the file being modified, so a write
 * to another file on the same volume (e.g. COPY's destination) does not force
 * the source handle to be reopened for every chunk (mirrors #984). */
static void rd_cache_close_path(int letter, const char *full)
{
	int idx;

	if (!drive_of(letter))
		return;
	idx = letter - 'A';
	if (s_rd[idx] && s_rd_path[idx][0] && strcmp(s_rd_path[idx], full) == 0)
		rd_cache_close(letter);
}

/* The cached stream can outlive the file or its mount: after a suspend/resume
 * the volume may be remounted underneath it (or the file replaced), leaving
 * the FILE* on an old inode that reads back EOF. Only reuse a handle whose
 * inode still matches the path. */
static int rd_cache_matches(int letter, const char *full)
{
	int idx = letter - 'A';
	struct stat path_st, fd_st;

	if (!s_rd[idx])
		return 0;
	if (stat(full, &path_st) != 0 ||
	    fstat(fileno(s_rd[idx]), &fd_st) != 0)
		return 0;
	return path_st.st_dev == fd_st.st_dev && path_st.st_ino == fd_st.st_ino;
}

static int fat_write_impl(int letter, const char *path, const void *data,
			  unsigned n, int append)
{
	char full[P_BUF];
	FILE *f;

	ensure_root();
	if (!build_path(letter, path, full, sizeof full, 0))
		return -1;
	/* A write replaces (or extends) the bytes a cached read handle would
	 * serve, so drop it before touching the file. */
	rd_cache_close_path(letter, full);
	mkdirs_parent(full);
	f = fopen(full, append ? "ab" : "wb");
	if (!f)
		return -1;
	if (n && fwrite(data, 1, n, f) != n)
	{
		int err = errno;

		fclose(f);
		errno = err;
		return -1;
	}
	if (fclose(f) != 0)
		return -1;
	return 0;
}

int mmb_fat_write(int letter, const char *path, const void *data, unsigned n,
		  int append)
{
	for (;;)
	{
		int err;

		storage_guard();
		errno = 0;
		if (fat_write_impl(letter, path, data, n, append) == 0)
			return 0;
		err = errno;
		if (!storage_io_retry(letter, err))
			return -1;
	}
}

/* Streaming write handle. A FILE* alone is not enough: after a remount the
 * open stream names the old inode, so wwrite must reopen the original path on
 * the recovered mount and continue at the byte offset it had reached. */
typedef struct {
	FILE *f;
	int letter;
	int append;
	char path[P_BUF];
	unsigned long long pos;
	unsigned gen;
} fat_wstream;

static int wstream_reopen(fat_wstream *w)
{
	char full[P_BUF];
	FILE *f;
	long at;

	ensure_root();
	if (!build_path(w->letter, w->path, full, sizeof full, 0))
		return -1;
	if (w->append)
		f = fopen(full, "ab");
	else if (w->pos > 0)
	{
		f = fopen(full, "r+b");
		if (!f)
			f = fopen(full, "w+b");
	}
	else
		f = fopen(full, "wb");
	if (!f)
		return -1;
	at = (long)w->pos;
	if (!w->append && at > 0 && fseek(f, at, SEEK_SET) != 0)
	{
		fclose(f);
		return -1;
	}
	if (w->f)
		fclose(w->f);
	w->f = f;
	w->gen = mmb_storage_generation();
	return 0;
}

void *mmb_fat_wopen(int letter, const char *path, int append)
{
	char full[P_BUF];
	fat_wstream *w;
	FILE *f;
	long at;

	for (;;)
	{
		int err;

		storage_guard();
		ensure_root();
		errno = 0;
		if (!build_path(letter, path, full, sizeof full, 0))
		{
			err = errno;
		}
		else
		{
			/* A streaming writer takes over the file; drop any cached
			 * read handle so it cannot serve the pre-write bytes. */
			rd_cache_close_path(letter, full);
			mkdirs_parent(full);
			f = fopen(full, append ? "ab" : "wb");
			if (f)
			{
				at = append ? ftell(f) : 0;
				if (at < 0)
					at = 0;
				w = malloc(sizeof *w);
				if (!w)
				{
					fclose(f);
					return 0;
				}
				w->f = f;
				w->letter = letter;
				w->append = append;
				snprintf(w->path, sizeof w->path, "%s", path);
				w->pos = (unsigned long long)at;
				w->gen = mmb_storage_generation();
				return w;
			}
			err = errno;
		}
		if (!storage_io_retry(letter, err))
			return 0;
	}
}

int mmb_fat_wwrite(void *handle, const void *data, unsigned n)
{
	fat_wstream *w = handle;
	const unsigned char *p = data;
	unsigned done = 0;

	if (!w || !w->f)
		return -1;
	if (!n)
		return 0;
	if (w->gen != mmb_storage_generation() && wstream_reopen(w) != 0)
		return -1;
	for (;;)
	{
		size_t wrote;
		int err;

		errno = 0;
		wrote = fwrite(p + done, 1, n - done, w->f);
		done += (unsigned)wrote;
		w->pos += wrote;
		if (done == n)
			return 0;
		err = errno;
		if (!storage_io_retry(w->letter, err))
			return -1;
		if (wstream_reopen(w) != 0)
			return -1;
	}
}

int mmb_fat_wclose(void *handle)
{
	fat_wstream *w = handle;

	if (!w)
		return -1;
	if (w->f && fclose(w->f) != 0)
	{
		free(w);
		return -1;
	}
	free(w);
	return 0;
}

static int fat_read_at_impl(int letter, const char *path, unsigned pos,
			    void *data, unsigned n, unsigned *got)
{
	char full[P_BUF];
	int idx;
	FILE *f;

	ensure_root();
	if (got)
		*got = 0;
	if (!build_path(letter, path, full, sizeof full, 1))
		return -1;
	idx = letter - 'A';
	/* #1151: reuse the open handle for the same file. The cached stream is
	 * already positioned at s_rd_pos, so an in-order chunk needs no seek at
	 * all; only a backward move (or any move off the cached position) does. */
	if (s_rd[idx] && strcmp(s_rd_path[idx], full) == 0 &&
	    rd_cache_matches(letter, full))
	{
		f = s_rd[idx];
		if (pos != s_rd_pos[idx] &&
		    fseek(f, (long)pos, SEEK_SET) != 0)
		{
			rd_cache_close(letter);
			return -1;
		}
	}
	else
	{
		f = fopen(full, "rb");
		if (!f)
		{
			/* The file exists but cannot be opened: make sure no stale
			 * same-path handle lingers. */
			rd_cache_close_path(letter, full);
			return -1;
		}
		rd_cache_close(letter);
		s_rd[idx] = f;
		snprintf(s_rd_path[idx], sizeof s_rd_path[idx], "%s", full);
		if (pos && fseek(f, (long)pos, SEEK_SET) != 0)
		{
			rd_cache_close(letter);
			return -1;
		}
	}
	s_rd_pos[idx] = pos;
	if (n)
	{
		size_t r = fread(data, 1, n, f);

		if (ferror(f))
		{
			int err = errno;

			rd_cache_close(letter);
			errno = err;
			return -1;
		}
		if (r == 0)
		{
			struct stat st;

			/* A stream invalidated by a remount can report EOF while
			 * the file still has bytes; reopen once and retry. */
			if (stat(full, &st) == 0 && st.st_size > (off_t)pos)
			{
				rd_cache_close(letter);
				f = fopen(full, "rb");
				if (!f)
					return -1;
				s_rd[idx] = f;
				snprintf(s_rd_path[idx], sizeof s_rd_path[idx],
					 "%s", full);
				if (pos && fseek(f, (long)pos, SEEK_SET) != 0)
				{
					rd_cache_close(letter);
					return -1;
				}
				r = fread(data, 1, n, f);
			}
		}
		if (got)
			*got = (unsigned)r;
		s_rd_pos[idx] = pos + (unsigned)r;
	}
	return 0;
}

int mmb_fat_read_at(int letter, const char *path, unsigned pos, void *data,
		    unsigned n, unsigned *got)
{
	for (;;)
	{
		int err;

		storage_guard();
		errno = 0;
		if (fat_read_at_impl(letter, path, pos, data, n, got) == 0)
			return 0;
		err = errno;
		if (!storage_io_retry(letter, err))
			return -1;
	}
}

static const char *drive_kind(int letter)
{
	if (letter == 'C')
		return "SD";
	if (letter == 'H')
		return "NVME";
	return "USB";
}

/* Friendly volume name: the basename of an explicitly mounted (--drive)
 * directory, so a USB mount shows its folder name instead of a bare letter.
 * The always-present default C: directory stays label-less. */
int mmb_fat_label(int letter, char *out, int outsz)
{
	char db[P_BUF];
	const char *p, *base = 0;

	if (out && outsz > 0)
		out[0] = 0;
	storage_guard();
	ensure_root();
	if (!drive_of(letter) || !out || outsz <= 0)
		return -1;
	drive_dir_raw(letter, db, sizeof db);
	if (!is_dir(db))
		return -1;
	if (s_has_override[letter - 'A'])
	{
		for (p = s_override[letter - 'A']; *p; p++)
			if (*p == '/')
				base = p + 1;
		if (!base)
			base = s_override[letter - 'A'];
		if (base[0])
		{
			snprintf(out, (size_t)outsz, "%s", base);
			return 0;
		}
	}
	return 0;
}

int mmb_fat_eject(int letter)
{
	char db[P_BUF];
	char msg[48];

	if (!drive_of(letter) || letter == 'C')
		return -1;
	storage_guard();
	ensure_root();
	drive_dir_raw(letter, db, sizeof db);
	if (!is_dir(db) || s_ejected[letter - 'A'])
		return -1;
	rd_cache_close(letter);
	s_ejected[letter - 'A'] = 1;
	/* Host writes are synchronous, so there is nothing to flush. */
	snprintf(msg, sizeof msg, "%c: %s ejected", (char)letter,
		 drive_kind(letter));
	if (s_notify_ready)
		mmb_storage_notice(msg);
	return 0;
}

void mmb_fat_drive_line(int letter, char *out, int outsz)
{
	char db[P_BUF];
	char lab[P_BUF];
	const char *kind;

	if (out && outsz > 0)
		out[0] = 0;
	if (!drive_of(letter))
		return;
	kind = drive_kind(letter);
	storage_guard();
	ensure_root();
	drive_dir_raw(letter, db, sizeof db);
	lab[0] = 0;
	if (is_dir(db) && mmb_fat_label(letter, lab, sizeof lab) == 0 && lab[0])
		snprintf(out, (size_t)outsz, "%c: %s \"%s\"", (char)letter, kind,
			 lab);
	else if (is_dir(db))
		snprintf(out, (size_t)outsz, "%c: %s", (char)letter, kind);
	else if (letter == 'C')
		snprintf(out, (size_t)outsz, "%c: %s (no media)", (char)letter,
			 kind);
}

/* Watch the host drive directories so a mounted/unmounted (or ejected)
 * volume reports itself the way a physical USB stick would. */
void mmb_storage_poll(void)
{
	int letter;

	storage_guard();
	ensure_root();
	for (letter = DRIVE_LO; letter <= DRIVE_HI; letter++)
	{
		char db[P_BUF];
		int idx = letter - 'A';
		int present;

		drive_dir_raw(letter, db, sizeof db);
		present = is_dir(db);
		if (present == s_present[idx])
			continue;
		s_present[idx] = present;
		/* A (re)insert clears any prior eject. */
		s_ejected[idx] = 0;
		if (!present)
			rd_cache_close(letter);
		if (!s_notify_ready || letter == 'C')
			continue;
		if (present)
		{
			char msg[48];
			snprintf(msg, sizeof msg, "%c: %s mounted",
				 (char)letter, drive_kind(letter));
			mmb_storage_notice(msg);
		}
		else
		{
			char msg[48];
			snprintf(msg, sizeof msg, "%c: %s removed",
				 (char)letter, drive_kind(letter));
			mmb_storage_notice(msg);
		}
	}
	s_notify_ready = 1;
}

void mmb_storage_unmount(void)
{
	/* Nothing to flush: writes are synchronous. */
	rd_cache_close_all();
	memset(s_ejected, 0, sizeof s_ejected);
	memset(s_present, 0, sizeof s_present);
	s_notify_ready = 0;
}
