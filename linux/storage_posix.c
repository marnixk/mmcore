/*
 * POSIX storage backend for the native build (LN-08).
 *
 * Physical drives C:-H: map to directories under a configurable root
 * (MMB_DRIVE_ROOT, default $HOME/.mmbasic) as <root>/C, <root>/D, ... A:/B:
 * are the in-RAM volumes in mmbasic/src/vfs.c and never reach this file.
 *
 * MMBasic/FatFs is case-insensitive and treats '/' as the separator; the host
 * filesystem is case-sensitive, so every lookup resolves path components by
 * scanning the parent directory (case-insensitively) before use.
 */
#include "mmb_priv.h"

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

static const char *root_dir(void)
{
	const char *env = getenv("MMB_DRIVE_ROOT");
	const char *home;

	if (env && *env)
		return env;
	home = getenv("HOME");
	if (home && *home)
		return home;
	return ".";
}

static int drive_of(int letter)
{
	return letter >= DRIVE_LO && letter <= DRIVE_HI;
}

static void ensure_root(void)
{
	const char *root = root_dir();
	char letter;

	if (s_init)
		return;
	s_init = 1;
	mkdir(root, 0777);
	/* Every drive directory is created up front; empty drives still show. */
	for (letter = DRIVE_LO; letter <= DRIVE_HI; letter++)
	{
		char db[P_BUF];

		snprintf(db, sizeof db, "%s/%c", root, letter);
		mkdir(db, 0777);
		s_cwd[letter - 'A'][0] = '/';
		s_cwd[letter - 'A'][1] = 0;
	}
}

static void drive_dir_raw(int letter, char *out, int outsz)
{
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
	if (!drive_of(letter))
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
	char sub[P_BUF];
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
		snprintf(sub, sizeof sub, "%s/%s", full, e->d_name);
		isd = is_dir(sub);
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

void mmb_fat_drive_line(int letter, char *out, int outsz)
{
	char db[P_BUF];
	const char *kind = "USB";

	if (out && outsz > 0)
		out[0] = 0;
	if (!drive_of(letter))
		return;
	if (letter == 'C')
		kind = "SD";
	else if (letter == 'H')
		kind = "NVME";
	ensure_root();
	drive_dir_raw(letter, db, sizeof db);
	if (is_dir(db))
		snprintf(out, (size_t)outsz, "%c: %s", (char)letter, kind);
	else if (letter == 'C')
		snprintf(out, (size_t)outsz, "%c: %s (no media)", (char)letter, kind);
}

void mmb_storage_poll(void)
{
	ensure_root();
}

void mmb_storage_unmount(void)
{
	/* Nothing to flush: writes are synchronous. */
}
