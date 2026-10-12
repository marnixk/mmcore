/* Host test for the POSIX native storage read cache (#1151).
 *
 * native/storage_posix.c's mmb_fat_read_at() used to fopen/fseek/fread/fclose
 * on every call, so a chunked reader reopened (and reseeked) the file for every
 * chunk. It now keeps one open FILE* per volume slot, mirroring the Circle
 * backend (#984). This compiles the real backend against a tiny shim and checks
 * the handle is reused across forward reads, invalidated on write/rename/unlink,
 * and still returns the right bytes for backward / out-of-order reads.
 *
 * "One open handle" is counted by scanning the process fd table and matching
 * the target file's dev/inode, so it needs no libc fopen interposition.
 */
/* mmb_priv.h must come before <stdio.h>: it declares `sprintf`, and the macOS
 * SDK turns that name into a macro once <stdio.h> is seen (the native build
 * includes it the same way, so this mirrors the real translation unit). */
#include "mmb_priv.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef __APPLE__
#define FD_DIR "/dev/fd"
#else
#define FD_DIR "/proc/self/fd"
#endif

static int fails;

static void check(int cond, const char *what)
{
	if (!cond)
	{
		printf("FAIL: %s\n", what);
		fails++;
	}
}

/* ---- shims for the symbols storage_posix.c pulls in ------------------- */
void mmb_dirent_offer(mmb_dirent *heap, int *n, int max, const mmb_dirent *e)
{
	if (*n < max)
		heap[(*n)++] = *e;
}

void mmb_storage_notice(const char *msg) { (void)msg; }

/* ---- fd-table helpers ------------------------------------------------- */
static int count_fds_ino(dev_t dev, ino_t ino)
{
	DIR *d = opendir(FD_DIR);
	struct dirent *e;
	struct stat st;
	int n = 0;

	if (!d)
		return -1;
	while ((e = readdir(d)))
	{
		char *end;
		long fd;

		if (e->d_name[0] < '0' || e->d_name[0] > '9')
			continue;
		fd = strtol(e->d_name, &end, 10);
		if (*end || fd < 0)
			continue;
		if (fstat((int)fd, &st) == 0 && st.st_dev == dev &&
		    st.st_ino == ino)
			n++;
	}
	closedir(d);
	return n;
}

/* Number of open handles on the file at hostpath (or -1 if it is gone). */
static int count_handles(const char *hostpath)
{
	struct stat st;

	if (stat(hostpath, &st) != 0)
		return -1;
	return count_fds_ino(st.st_dev, st.st_ino);
}

static int read_is(int letter, const char *path, unsigned pos, const char *want,
		   unsigned n)
{
	char buf[256];
	unsigned got = 0;

	if (n > sizeof buf)
		return 0;
	memset(buf, 0, sizeof buf);
	if (mmb_fat_read_at(letter, path, pos, buf, n, &got) != 0)
		return 0;
	return got == n && memcmp(buf, want, n) == 0;
}

static void write_file(const char *path, const char *data)
{
	check(mmb_fat_write('C', path, data, (unsigned)strlen(data), 0) == 0,
	      "seed mmb_fat_write");
}

int main(void)
{
	char tmpl[] = "/tmp/mmb_rdcache_XXXXXX";
	char *root = mkdtemp(tmpl);
	char fpath[512], gpath[512];
	char apath[512], bpath[512];
	char scratch[8];
	const char *body = "0123456789ABCDEFGHIJ"; /* 20 bytes */
	const char *neww = "NEWCONTENT";            /* 10 bytes */

	if (!root)
	{
		printf("FAIL: mkdtemp\n");
		return 1;
	}
	setenv("MMB_DRIVE_ROOT", root, 1);
	snprintf(fpath, sizeof fpath, "%s/C/F.TXT", root);
	snprintf(gpath, sizeof gpath, "%s/C/G.TXT", root);
	snprintf(apath, sizeof apath, "%s/C/A.TXT", root);
	snprintf(bpath, sizeof bpath, "%s/C/B.TXT", root);

	write_file("/F.TXT", body);

	/* 1. Forward chunked reads reuse one open handle. */
	check(read_is('C', "/F.TXT", 0, "01234", 5), "forward read 0");
	check(count_handles(fpath) == 1, "one handle after first read");
	check(read_is('C', "/F.TXT", 5, "56789", 5), "forward read 5");
	check(count_handles(fpath) == 1, "same handle reused at 5");
	check(read_is('C', "/F.TXT", 10, "ABCDE", 5), "forward read 10");
	check(count_handles(fpath) == 1, "same handle reused at 10");

	/* 2. Backward and out-of-order reads still return the right bytes. */
	check(read_is('C', "/F.TXT", 15, "FGHIJ", 5), "read at 15");
	check(read_is('C', "/F.TXT", 2, "2345", 4), "backward read at 2");
	check(read_is('C', "/F.TXT", 6, "6789", 4), "forward again at 6");
	check(count_handles(fpath) == 1, "handle still cached after seeks");

	/* 3. A read past EOF reports a short count. */
	{
		char buf[16];
		unsigned got = 99;

		memset(buf, 0, sizeof buf);
		check(mmb_fat_read_at('C', "/F.TXT", 18, buf, 10, &got) == 0 &&
			      got == 2 && memcmp(buf, "IJ", 2) == 0,
		      "short read at EOF");
	}

	/* 4. Writing the same path invalidates the cached handle, so the next
	 *    read sees the new bytes (not the old ones). */
	write_file("/F.TXT", neww);
	check(count_handles(fpath) == 0, "cache dropped on write");
	check(read_is('C', "/F.TXT", 0, neww, 10), "read after write");
	check(count_handles(fpath) == 1, "handle cached again after write");

	/* 5. Rename invalidates the handle under the old name. */
	check(mmb_fat_rename('C', "/F.TXT", "/G.TXT") == 0, "rename F->G");
	check(mmb_fat_read_at('C', "/F.TXT", 0, scratch, 1, 0) == -1,
	      "old path gone after rename");
	check(read_is('C', "/G.TXT", 0, neww, 10), "read renamed file");
	check(count_handles(gpath) == 1, "handle cached for new name");

	/* 6. Unlink drops the cached handle: no stale read can survive. */
	{
		struct stat st;

		check(stat(gpath, &st) == 0, "stat before unlink");
		check(mmb_fat_unlink('C', "/G.TXT") == 0, "unlink G");
		check(count_fds_ino(st.st_dev, st.st_ino) == 0,
		      "unlinked handle closed");
		check(mmb_fat_read_at('C', "/G.TXT", 0, scratch, 1, 0) == -1,
		      "read of unlinked file fails");
	}

	/* 7. Switching files reuses the single per-volume slot. */
	write_file("/A.TXT", "AAAA");
	write_file("/B.TXT", "BBBB");
	check(read_is('C', "/A.TXT", 0, "AAAA", 4), "read A");
	check(count_handles(apath) == 1 && count_handles(bpath) == 0,
	      "A cached, B not");
	check(read_is('C', "/B.TXT", 0, "BBBB", 4), "read B");
	check(count_handles(apath) == 0 && count_handles(bpath) == 1,
	      "switch closed A, cached B");

	/* 8. A missing file still fails. */
	check(mmb_fat_read_at('C', "/NOPE.TXT", 0, scratch, 1, 0) == -1,
	      "missing file");

	if (fails)
	{
		printf("%d check(s) failed\n", fails);
		return 1;
	}
	printf("all checks passed\n");
	return 0;
}
