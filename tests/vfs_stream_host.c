/* Host test for the A: ramdisk streaming writer in mmbasic/src/vfs.c.
 *
 * FTP STOR opens the target once (mmb_vfs_wopen), appends received chunks
 * (mmb_vfs_wwrite) and closes once (mmb_vfs_wclose). On the ramdisk the
 * backing buffer grows geometrically instead of by one chunk per call. This
 * checks the byte stream is exact for a multi-hundred-KB upload and that the
 * append/truncate resume at the right offset.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mmb_priv.h"

mmb_test_globals G;
mmb_test_globals *g_mmb[MMB_MAX_CONSOLES] = { &G, 0, 0, 0 };

static int fails;

static void check(int cond, const char *what)
{
	if (!cond)
	{
		printf("FAIL: %s\n", what);
		fails++;
	}
}

int mmb_keyword_eq(const char *a, const char *b)
{
	for (; *a && *b; a++, b++)
	{
		char ca = *a, cb = *b;
		if (ca >= 'a' && ca <= 'z')
			ca = (char)(ca - 32);
		if (cb >= 'a' && cb <= 'z')
			cb = (char)(cb - 32);
		if (ca != cb)
			return 0;
	}
	return *a == 0 && *b == 0;
}

/* ---- FAT volume stubs (the test stays on A:) -------------------------- */
int mmb_fat_ready(int letter) { (void)letter; return 0; }
int mmb_fat_chdir(int letter, const char *path) { (void)letter; (void)path; return -1; }
int mmb_fat_mkdir(int letter, const char *path) { (void)letter; (void)path; return -1; }
int mmb_fat_rmdir(int letter, const char *path) { (void)letter; (void)path; return -1; }
int mmb_fat_unlink(int letter, const char *path) { (void)letter; (void)path; return -1; }
int mmb_fat_rename(int letter, const char *a, const char *b) { (void)letter; (void)a; (void)b; return -1; }
int mmb_fat_list(int letter, const char *dir, const char *pat, char *out, int outsz)
{
	(void)letter; (void)dir; (void)pat;
	if (outsz > 0)
		out[0] = 0;
	return -1;
}
int mmb_fat_list_entries(int letter, const char *dir, const char *pat,
			 mmb_dirent *out, int max, int *truncated)
{
	(void)letter; (void)dir; (void)pat; (void)out; (void)max;
	if (truncated)
		*truncated = 0;
	return -1;
}
int mmb_fat_write(int letter, const char *path, const void *data, unsigned n, int append)
{
	(void)letter; (void)path; (void)data; (void)n; (void)append;
	return -1;
}
void *mmb_fat_wopen(int letter, const char *path, int append)
{
	(void)letter; (void)path; (void)append;
	return 0;
}
int mmb_fat_wwrite(void *handle, const void *data, unsigned n)
{
	(void)handle; (void)data; (void)n;
	return -1;
}
int mmb_fat_wclose(void *handle) { (void)handle; return -1; }
int mmb_fat_read_at(int letter, const char *path, unsigned pos, void *data, unsigned n, unsigned *got)
{
	(void)letter; (void)path; (void)pos; (void)data; (void)n;
	if (got)
		*got = 0;
	return -1;
}
int mmb_fat_size(int letter, const char *path) { (void)letter; (void)path; return -1; }
int mmb_fat_exists(int letter, const char *path) { (void)letter; (void)path; return 0; }
int mmb_fat_isdir(int letter, const char *path) { (void)letter; (void)path; return 0; }
const char *mmb_fat_cwd(int letter) { (void)letter; return "/"; }
void mmb_fat_drive_line(int letter, char *out, int outsz) { (void)letter; if (outsz > 0) out[0] = 0; }

/* ---- interpreter stubs (the test never reaches the package/DRIVE path) - */
void mmb_error(const char *msg) { (void)msg; }
void mmb_skip_sp(void) {}
void mmb_syntax(void) {}
void mmb_out(const char *s) { (void)s; }
void mmb_console_write(const char *s) { (void)s; }
int mmb_fat_eject(int letter) { (void)letter; return 0; }
int mmb_files_notice(const char *msg) { (void)msg; return 0; }
mmb_val mmb_expr(void) { mmb_val v; memset(&v, 0, sizeof v); return v; }
int mmb_zip_foreach(const void *buf, unsigned n,
		    int (*cb)(const char *path, const void *data, unsigned len, void *ctx),
		    void *ctx)
{
	(void)buf; (void)n; (void)cb; (void)ctx;
	return -1;
}

static void *talloc(unsigned n) { return malloc(n); }
static void tfree(void *p) { free(p); }
static mmb_test_plat g_plat = { talloc, tfree };

#define CHUNK  1024
#define CHUNKS 300

int main(void)
{
	static unsigned char chunk[CHUNK];
	static unsigned char back[CHUNK];
	unsigned total = CHUNK * CHUNKS;
	unsigned got;
	int h, i;

	G.plat = &g_plat;
	mmb_vfs_init();

	for (i = 0; i < CHUNK; i++)
		chunk[i] = (unsigned char)(i * 31 + 7);

	h = mmb_vfs_wopen("A:/BIG.BIN", 0);
	check(h >= 0, "wopen create");
	for (i = 0; i < CHUNKS; i++)
		check(mmb_vfs_wwrite(h, chunk, CHUNK) == 0, "wwrite chunk");
	check(mmb_vfs_wclose(h) == 0, "wclose");
	check(mmb_vfs_size("A:/BIG.BIN") == (int)total, "streamed size");

	for (i = 0; i < CHUNKS; i++)
	{
		got = 0;
		if (mmb_vfs_read_at("A:/BIG.BIN", (unsigned)(i * CHUNK), back, CHUNK, &got) != 0 ||
		    got != CHUNK || memcmp(back, chunk, CHUNK) != 0)
		{
			check(0, "read back chunk content");
			break;
		}
	}

	/* append resumes at the end and keeps existing bytes */
	h = mmb_vfs_wopen("A:/BIG.BIN", 1);
	check(h >= 0, "wopen append");
	memset(chunk, 0xAB, CHUNK);
	check(mmb_vfs_wwrite(h, chunk, CHUNK) == 0, "append chunk");
	check(mmb_vfs_wclose(h) == 0, "append close");
	check(mmb_vfs_size("A:/BIG.BIN") == (int)(total + CHUNK), "append size");
	got = 0;
	check(mmb_vfs_read_at("A:/BIG.BIN", total, back, CHUNK, &got) == 0 && got == CHUNK,
	      "append read");
	check(back[0] == 0xAB && back[CHUNK - 1] == 0xAB, "append content");

	/* reopen without append truncates */
	h = mmb_vfs_wopen("A:/BIG.BIN", 0);
	check(h >= 0, "wopen truncate");
	check(mmb_vfs_wwrite(h, chunk, 10) == 0, "truncate write");
	check(mmb_vfs_wclose(h) == 0, "truncate close");
	check(mmb_vfs_size("A:/BIG.BIN") == 10, "truncate size");
	got = 0;
	check(mmb_vfs_read_at("A:/BIG.BIN", 10, back, 1, &got) == 0 && got == 0, "truncate eof");

	if (fails)
	{
		printf("%d check(s) failed\n", fails);
		return 1;
	}
	printf("all checks passed\n");
	return 0;
}
