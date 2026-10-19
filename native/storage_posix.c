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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#if defined(__linux__)
#include <fcntl.h>
#include <time.h>
#include <sys/wait.h>
#endif

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

	storage_check_resume();
	ensure_root();
	if (!drive_of(letter) || s_ejected[letter - 'A'])
		return 0;
	drive_dir_raw(letter, db, sizeof db);
	return is_dir(db);
}

/* Validate that a directory-absolute path names a directory. The current
 * directory is session state and lives in the per-console mmb; only the drive
 * mount table is global. */
int mmb_fat_chdir(int letter, const char *path)
{
	char full[P_BUF];

	ensure_root();
	if (!path || !path[0] || strcmp(path, "/") == 0)
	{
		char db[P_BUF];
		if (!drive_of(letter))
			return -1;
		drive_dir_raw(letter, db, sizeof db);
		return is_dir(db) ? 0 : -1;
	}
	if (!build_path(letter, path, full, sizeof full, 1) || !is_dir(full))
		return -1;
	return 0;
}

int mmb_fat_mkdir(int letter, const char *path)
{
	char full[P_BUF];

	ensure_root();
	if (!build_path(letter, path, full, sizeof full, 0))
		return -1;
	/* A directory change can introduce or remove a name the cached path
	 * resolution depended on; drop the volume's handle to be safe. */
	rd_cache_close(letter);
	mkdirs(full);
	return is_dir(full) ? 0 : -1;
}

int mmb_fat_rmdir(int letter, const char *path)
{
	char full[P_BUF];

	ensure_root();
	if (!build_path(letter, path, full, sizeof full, 1))
		return -1;
	rd_cache_close(letter);
	return rmdir(full) == 0 ? 0 : -1;
}

int mmb_fat_unlink(int letter, const char *path)
{
	char full[P_BUF];

	ensure_root();
	if (!build_path(letter, path, full, sizeof full, 1))
		return -1;
	rd_cache_close(letter);
	return unlink(full) == 0 ? 0 : -1;
}

int mmb_fat_rename(int letter, const char *from, const char *to)
{
	char a[P_BUF], b[P_BUF];

	ensure_root();
	if (!build_path(letter, from, a, sizeof a, 1))
		return -1;
	if (!build_path(letter, to, b, sizeof b, 0))
		return -1;
	/* The cached handle can name either end of the rename. */
	rd_cache_close(letter);
	return rename(a, b) == 0 ? 0 : -1;
}

int mmb_fat_exists(int letter, const char *path)
{
	char full[P_BUF];
	struct stat st;

	ensure_root();
	if (!build_path(letter, path, full, sizeof full, 1))
		return 0;
	return stat(full, &st) == 0;
}

int mmb_fat_size(int letter, const char *path)
{
	char full[P_BUF];
	struct stat st;

	ensure_root();
	if (!build_path(letter, path, full, sizeof full, 1))
		return -1;
	if (stat(full, &st) != 0 || S_ISDIR(st.st_mode))
		return -1;
	return (int)st.st_size;
}

int mmb_fat_isdir(int letter, const char *path)
{
	char full[P_BUF];
	struct stat st;

	ensure_root();
	if (!path || !path[0] || strcmp(path, "/") == 0)
		return 1; /* drive root is always a directory */
	if (!build_path(letter, path, full, sizeof full, 1))
		return 0;
	return stat(full, &st) == 0 && S_ISDIR(st.st_mode);
}

int mmb_fat_list(int letter, const char *dir, const char *pat, char *out,
		 int outsz, int *truncated)
{
	char full[P_BUF];
	DIR *d;
	struct dirent *e;

	ensure_root();
	if (out && outsz > 0)
		out[0] = 0;
	if (truncated)
		*truncated = 0;
	if (!dir || !dir[0] || strcmp(dir, "/") == 0)
		drive_dir_raw(letter, full, sizeof full);
	else if (!build_path(letter, dir, full, sizeof full, 1))
		return -1;
	d = opendir(full);
	if (!d)
		return -1;
	if (!pat || !pat[0])
		pat = "*"; /* a bare directory (e.g. DIR "C:/") lists everything */
	while ((e = readdir(d)))
	{
		int isd;
		size_t used;

		if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
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
	closedir(d);
	return 0;
}

int mmb_fat_list_entries(int letter, const char *dir, const char *pat,
			 mmb_dirent *out, int max, int *truncated)
{
	char full[P_BUF];
	char sub[P_BUF];
	DIR *d;
	struct dirent *e;
	int n = 0, total = 0, i;
	struct stat st;

	ensure_root();
	if (truncated)
		*truncated = 0;
	if (!out || max <= 0)
		return -1;
	if (!dir || !dir[0] || strcmp(dir, "/") == 0)
		drive_dir_raw(letter, full, sizeof full);
	else if (!build_path(letter, dir, full, sizeof full, 1))
		return -1;
	d = opendir(full);
	if (!d)
		return -1;
	if (!pat || !pat[0])
		pat = "*";
	while ((e = readdir(d)))
	{
		int isd;
		mmb_dirent ent;

		if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
			continue;
		if (!glob_ci(pat, e->d_name))
			continue;
		isd = dirent_is_dir(full, e);
		memset(&ent, 0, sizeof(ent));
		snprintf(ent.name, sizeof(ent.name), "%s", e->d_name);
		ent.is_dir = isd;
		ent.size = -1; /* stat()ed lazily below, only for the kept entries */
		total++;
		/* Keep the sorted-first `max` regardless of readdir order (#676). */
		mmb_dirent_offer(out, &n, max, &ent);
	}
	closedir(d);
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

/* ---- live-ISO persistence guard --------------------------------------- *
 * On the live ISO the C: home lives on the MMCORE volume mounted at
 * MMB_DRIVE_ROOT. Suspend-to-RAM can reset the USB/SD controller, after which
 * the mount may be gone or read-only; mmcore keeps running from the squashfs
 * and would silently write configuration to the RAM overlay. Detect a resume
 * (the gap between CLOCK_BOOTTIME and CLOCK_MONOTONIC grows), try to remount
 * the volume, and report whether C: is currently volatile. */
#if defined(__linux__)
static long long s_suspend_offset_ms;
static int s_suspend_tracked;
static int s_live_boot = -1;
static int s_overlay_backed = -1;
static int s_overlay_lost_notified;
static int s_overlay_broken;

/* --read-write seeds /.mmcore-rw and rw/upper + rw/work; live-init then uses
 * that volume as the root overlay's upper. Detect it at boot, before a resume
 * can drop the mount and make the paths unreadable. */
static int live_overlay_backed(void)
{
	if (s_overlay_backed < 0)
		s_overlay_backed =
			(is_dir("/media/mmcore/rw/upper") &&
			 is_dir("/media/mmcore/rw/work") &&
			 access("/media/mmcore/.mmcore-rw", F_OK) == 0) ? 1 : 0;
	return s_overlay_backed;
}

static int linux_is_live_boot(void)
{
	char buf[512];
	FILE *f;
	size_t n;

	if (s_live_boot >= 0)
		return s_live_boot;
	s_live_boot = 0;
	f = fopen("/proc/cmdline", "rb");
	if (!f)
		return 0;
	n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[n] = 0;
	if (strstr(buf, "mmcore.live=1"))
		s_live_boot = 1;
	return s_live_boot;
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

static void mount_label_from_root(const char *root, char *label, size_t cap)
{
	const char *base = strrchr(root, '/');
	size_t i;

	base = (base && base[1]) ? base + 1 : root;
	snprintf(label, cap, "%s", base);
	for (i = 0; label[i]; i++)
		if (label[i] >= 'a' && label[i] <= 'z')
			label[i] = (char)(label[i] - 'a' + 'A');
}

static int try_mount_root(const char *root)
{
	static const char *const helpers[] = {
		"/bin/mount", "/sbin/mount", "/usr/bin/mount"
	};
	char label[64];
	size_t i;

	mount_label_from_root(root, label, sizeof label);
	for (i = 0; i < sizeof(helpers) / sizeof(helpers[0]); i++)
	{
		char *argv[] = { (char *)helpers[i], "-o", "rw", "-L", label,
				 (char *)root, 0 };

		if (run_mount(argv) == 0 && is_mountpoint(root))
			return 1;
	}
	return 0;
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

static void revalidate_live_root(void)
{
	const char *env = getenv("MMB_DRIVE_ROOT");
	char fallback[] = "/media/mmcore";
	const char *root = (env && env[0]) ? env : fallback;
	int mounted = is_mountpoint(root);

	if (live_overlay_backed())
	{
		/* The root overlay's upper lives on this volume (--read-write).
		 * A normal resume leaves the mount in place: lift a read-only
		 * remount and carry on. If the volume dropped, mounting it again
		 * would restore C: but leave the overlay's cached upper stale
		 * (/tmp writes fail with EIO), so a reboot is the only safe
		 * recovery. */
		if (mounted)
		{
			(void)remount_root_rw(root);
			rd_cache_close_all();
			s_init = 0;
			if (root_overlay_writable())
				return;
		}
		s_overlay_broken = 1;
		if (!s_overlay_lost_notified)
		{
			s_overlay_lost_notified = 1;
			mmb_storage_notice("!MMCORE reset after suspend: reboot to recover storage");
		}
		return;
	}

	if (!mounted)
	{
		mkdir(root, 0777);
		mounted = try_mount_root(root);
		if (mounted && !(env && env[0]))
		{
			/* The volume arrived after startup: point C: at it from
			 * now on instead of the RAM-overlay home. */
			setenv("MMB_DRIVE_ROOT", root, 1);
			s_init = 0;
		}
	}
	if (!mounted)
		return;
	/* ext4 can come back read-only after a device reset. */
	(void)remount_root_rw(root);
	/* Drop cached handles and the cached root: C: is live again. */
	rd_cache_close_all();
	s_init = 0;
}

static void storage_check_resume(void)
{
	struct timespec bt, mt;
	long long off;

	if (!linux_is_live_boot())
		return;
	if (clock_gettime(CLOCK_BOOTTIME, &bt) != 0 ||
	    clock_gettime(CLOCK_MONOTONIC, &mt) != 0)
		return;
	off = (long long)bt.tv_sec * 1000 + bt.tv_nsec / 1000000
	    - ((long long)mt.tv_sec * 1000 + mt.tv_nsec / 1000000);
	if (!s_suspend_tracked)
	{
		s_suspend_tracked = 1;
		s_suspend_offset_ms = off;
		/* Cache --read-write at boot, while the volume is mounted. */
		(void)live_overlay_backed();
		return;
	}
	/* BOOTTIME - MONOTONIC only grows while the machine is suspended. */
	if (off - s_suspend_offset_ms >= 2000)
	{
		s_suspend_offset_ms = off;
		revalidate_live_root();
	}
	else if (off < s_suspend_offset_ms)
		s_suspend_offset_ms = off; /* clock source reset/oddity */
}
#else
static void storage_check_resume(void) { }
#endif

void mmb_storage_revalidate(void)
{
#if defined(__linux__)
	if (linux_is_live_boot())
		revalidate_live_root();
#endif
}

int mmb_storage_persistence_lost(void)
{
#if defined(__linux__)
	const char *root;
	int mounted;

	if (!linux_is_live_boot())
		return 0;
	storage_check_resume();
	if (s_overlay_broken)
		return 1;
	root = getenv("MMB_DRIVE_ROOT");
	mounted = is_mountpoint((root && root[0]) ? root : "/media/mmcore");
	return mounted ? 0 : 1;
#else
	return 0;
#endif
}

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
		fclose(f);
		return -1;
	}
	fclose(f);
	return 0;
}

int mmb_fat_write(int letter, const char *path, const void *data, unsigned n,
		  int append)
{
	int rc;

	storage_check_resume();
	rc = fat_write_impl(letter, path, data, n, append);
	if (rc != 0 && letter == 'C')
	{
		/* A reset persistence mount is the common cause of a write
		 * failure on the live ISO; try to recover once before giving
		 * up (the caller surfaces the failure). */
		mmb_storage_revalidate();
		rc = fat_write_impl(letter, path, data, n, append);
	}
	return rc;
}

void *mmb_fat_wopen(int letter, const char *path, int append)
{
	char full[P_BUF];
	FILE *f;

	ensure_root();
	if (!build_path(letter, path, full, sizeof full, 0))
		return 0;
	/* A streaming writer takes over the file; drop any cached read handle so
	 * it cannot serve the pre-write bytes. */
	rd_cache_close_path(letter, full);
	mkdirs_parent(full);
	f = fopen(full, append ? "ab" : "wb");
	return f; /* FILE* is the opaque handle */
}

int mmb_fat_wwrite(void *handle, const void *data, unsigned n)
{
	FILE *f = handle;

	if (!f)
		return -1;
	if (n && fwrite(data, 1, n, f) != n)
		return -1;
	return 0;
}

int mmb_fat_wclose(void *handle)
{
	FILE *f = handle;

	if (!f)
		return -1;
	return fclose(f) == 0 ? 0 : -1;
}

int mmb_fat_read_at(int letter, const char *path, unsigned pos, void *data,
		    unsigned n, unsigned *got)
{
	char full[P_BUF];
	int idx;
	FILE *f;

	/* A read can be the first storage access after a standby resume; let the
	 * guard revalidate/remount before resolving the path. */
	storage_check_resume();
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
