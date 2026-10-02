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
int mmb_fat_list(int letter, const char *dir, const char *pat, char *out, int outsz,
		 int *truncated)
{
	(void)letter; (void)dir; (void)pat;
	if (outsz > 0)
		out[0] = 0;
	if (truncated)
		*truncated = 0;
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

/* #676: a listing capped below the folder size must keep the sorted-first
 * `max` entries, not an arbitrary first-N slice in raw node order. Seed the
 * names in descending order so node order is the reverse of sorted order, and
 * cap well below the count: the old code kept CAP39..CAP30, the fix keeps
 * CAP00..CAP09. */
static void test_bounded_listing(void)
{
	enum { N = 40, CAP = 10 };
	mmb_dirent ents[CAP];
	char path[32];
	int i, n, truncated = 0;

	for (i = N - 1; i >= 0; i--)
	{
		snprintf(path, sizeof path, "A:/CAP%02d.TXT", i);
		check(mmb_vfs_write(path, "x", 1, 0) == 0, "seed capped file");
	}
	n = mmb_vfs_list_entries("A:/*.TXT", ents, CAP, &truncated);
	check(n == CAP, "capped listing count");
	check(truncated == 1, "capped listing truncated");
	for (i = 0; i < CAP; i++)
	{
		char want[32];
		snprintf(want, sizeof want, "CAP%02d.TXT", i);
		check(strcmp(ents[i].name, want) == 0, "capped listing sorted-first");
	}
}

/* #693: mmb_vfs_list must flag a listing the caller's buffer cut, so a
 * consumer can report it instead of silently dropping names. */
static void test_list_truncation(void)
{
	enum { N = 12 };
	char path[32];
	char small[96];
	char big[4096];
	int i, truncated;

	for (i = 0; i < N; i++)
	{
		snprintf(path, sizeof path, "A:/TRUNC%02d.TXT", i);
		check(mmb_vfs_write(path, "x", 1, 0) == 0, "seed trunc file");
	}
	truncated = 0;
	check(mmb_vfs_list("A:/TRUNC*.TXT", small, (int)sizeof small, &truncated) == 0,
	      "small listing rc");
	check(truncated == 1, "small listing flagged truncated");
	check(small[0] != 0, "small listing kept some names");

	truncated = 1;
	check(mmb_vfs_list("A:/TRUNC*.TXT", big, (int)sizeof big, &truncated) == 0,
	      "roomy listing rc");
	check(truncated == 0, "roomy listing not truncated");
}

/* #1028: a ramdisk rename may change the parent directory, so a FILES move
 * between folders (A:/M1 -> A:/M2) reparents the node like FAT. The node
 * keeps its data; the source path disappears and the destination resolves. */
static void test_rename_reparent(void)
{
	char buf[16];
	unsigned got;

	check(mmb_vfs_mkdir("A:/M1") == 0, "mkdir M1");
	check(mmb_vfs_mkdir("A:/M2") == 0, "mkdir M2");
	check(mmb_vfs_write("A:/M1/F.TXT", "hello", 5, 0) == 0, "seed F.TXT");

	check(mmb_vfs_rename("A:/M1/F.TXT", "A:/M2/F.TXT") == 0, "reparent file");
	check(mmb_vfs_exists("A:/M1/F.TXT") == 0, "reparent source gone");
	check(mmb_vfs_exists("A:/M2/F.TXT") == 1, "reparent dest present");
	check(mmb_vfs_size("A:/M2/F.TXT") == 5, "reparent dest size");
	got = 0;
	check(mmb_vfs_read_at("A:/M2/F.TXT", 0, buf, sizeof buf, &got) == 0 &&
	      got == 5 && memcmp(buf, "hello", 5) == 0, "reparent dest content");
	check(mmb_vfs_size("A:/M1/F.TXT") < 0, "reparent source size gone");

	check(mmb_vfs_rename("A:/M2/F.TXT", "A:/M2/G.TXT") == 0, "rename in place");
	check(mmb_vfs_exists("A:/M2/G.TXT") == 1, "in-place dest present");

	/* A clash with an existing name in the destination folder is refused. */
	check(mmb_vfs_write("A:/M1/H.TXT", "x", 1, 0) == 0, "seed H.TXT");
	check(mmb_vfs_rename("A:/M2/G.TXT", "A:/M1/H.TXT") == -1, "clash refused");
	check(mmb_vfs_exists("A:/M2/G.TXT") == 1, "clash source kept");

	/* A directory reparents too, but never into its own subtree. */
	check(mmb_vfs_mkdir("A:/M2/SUB") == 0, "mkdir SUB");
	check(mmb_vfs_mkdir("A:/M2/SUB/DEEP") == 0, "mkdir DEEP");
	check(mmb_vfs_rename("A:/M2/SUB", "A:/M1/SUB") == 0, "reparent dir");
	check(mmb_vfs_isdir("A:/M1/SUB/DEEP") == 1, "moved subtree reachable");
	check(mmb_vfs_rename("A:/M1/SUB", "A:/M1/SUB/DEEP") == -1, "cycle refused");
	check(mmb_vfs_isdir("A:/M1/SUB") == 1, "dir kept after cycle refusal");

	/* The volume root is never a rename target. */
	check(mmb_vfs_rename("A:/M1/H.TXT", "A:/") == -1, "root target refused");
}

int main(void)
{
	static unsigned char chunk[CHUNK];
	static unsigned char back[CHUNK];
	unsigned total = CHUNK * CHUNKS;
	unsigned got;
	int h, i;

	G.plat = &g_plat;
	mmb_vfs_init();

	test_bounded_listing();
	test_list_truncation();
	test_rename_reparent();

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
