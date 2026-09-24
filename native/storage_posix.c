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

#define DRIVE_LO 'C'
#define DRIVE_HI 'H'
#define DRIVE_N (DRIVE_HI - DRIVE_LO + 1)
#define P_BUF 1024

static char s_cwd[26][128]; /* indexed by letter - 'A' */
static int s_init;
static char s_override[26][P_BUF];
static int s_has_override[26];
static int s_ejected[26];
static int s_present[26];
static int s_notify_ready;

static int drive_of(int letter);
static void drive_dir_raw(int letter, char *out, int outsz);

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
	s_cwd['C' - 'A'][0] = '/';
	s_cwd['C' - 'A'][1] = 0;
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

static void init_cwd(void)
{
	int i;

	for (i = 0; i < 26; i++)
		if (!s_cwd[i][0])
		{
			s_cwd[i][0] = '/';
			s_cwd[i][1] = 0;
		}
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

	ensure_root();
	if (!drive_of(letter) || s_ejected[letter - 'A'])
		return 0;
	drive_dir_raw(letter, db, sizeof db);
	return is_dir(db);
}

int mmb_fat_chdir(int letter, const char *path)
{
	char full[P_BUF];

	ensure_root();
	if (!build_path(letter, path, full, sizeof full, 1) || !is_dir(full))
		return -1;
	if (!path || !path[0] || strcmp(path, "/") == 0)
		snprintf(s_cwd[letter - 'A'], sizeof s_cwd[0], "/");
	else
		snprintf(s_cwd[letter - 'A'], sizeof s_cwd[0], "%s", path);
	return 0;
}

const char *mmb_fat_cwd(int letter)
{
	char db[P_BUF];

	ensure_root();
	init_cwd();
	if (!drive_of(letter))
		return "/";
	drive_dir_raw(letter, db, sizeof db);
	if (!is_dir(db))
		return "/";
	return s_cwd[letter - 'A'][0] ? s_cwd[letter - 'A'] : "/";
}

int mmb_fat_mkdir(int letter, const char *path)
{
	char full[P_BUF];

	ensure_root();
	if (!build_path(letter, path, full, sizeof full, 0))
		return -1;
	mkdirs(full);
	return is_dir(full) ? 0 : -1;
}

int mmb_fat_rmdir(int letter, const char *path)
{
	char full[P_BUF];

	ensure_root();
	if (!build_path(letter, path, full, sizeof full, 1))
		return -1;
	return rmdir(full) == 0 ? 0 : -1;
}

int mmb_fat_unlink(int letter, const char *path)
{
	char full[P_BUF];

	ensure_root();
	if (!build_path(letter, path, full, sizeof full, 1))
		return -1;
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
		 int outsz)
{
	char full[P_BUF];
	DIR *d;
	struct dirent *e;

	ensure_root();
	if (out && outsz > 0)
		out[0] = 0;
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
	int n = 0;

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

		if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
			continue;
		if (!glob_ci(pat, e->d_name))
			continue;
		if (n >= max)
		{
			if (truncated)
				*truncated = 1;
			break;
		}
		isd = dirent_is_dir(full, e);
		memset(&out[n], 0, sizeof(out[n]));
		snprintf(out[n].name, sizeof(out[n].name), "%s", e->d_name);
		out[n].is_dir = isd;
		out[n].size = -1;
		if (!isd)
		{
			struct stat st;

			snprintf(sub, sizeof sub, "%s/%s", full, e->d_name);
			if (stat(sub, &st) == 0)
				out[n].size = (int)st.st_size;
		}
		n++;
	}
	closedir(d);
	return n;
}

int mmb_fat_write(int letter, const char *path, const void *data, unsigned n,
		  int append)
{
	char full[P_BUF];
	FILE *f;

	ensure_root();
	if (!build_path(letter, path, full, sizeof full, 0))
		return -1;
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

void *mmb_fat_wopen(int letter, const char *path, int append)
{
	char full[P_BUF];
	FILE *f;

	ensure_root();
	if (!build_path(letter, path, full, sizeof full, 0))
		return 0;
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
	FILE *f;

	ensure_root();
	if (got)
		*got = 0;
	if (!build_path(letter, path, full, sizeof full, 1))
		return -1;
	f = fopen(full, "rb");
	if (!f)
		return -1;
	if (fseek(f, (long)pos, SEEK_SET) != 0)
	{
		fclose(f);
		return -1;
	}
	if (n)
	{
		size_t r = fread(data, 1, n, f);

		if (got)
			*got = (unsigned)r;
	}
	fclose(f);
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
	memset(s_ejected, 0, sizeof s_ejected);
	memset(s_present, 0, sizeof s_present);
	s_notify_ready = 0;
}
