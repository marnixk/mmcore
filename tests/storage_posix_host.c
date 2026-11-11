/* Host test for the POSIX native storage backend (#1151, #1493).
 *
 * native/storage_posix.c's mmb_fat_read_at() used to fopen/fseek/fread/fclose
 * on every call, so a chunked reader reopened (and reseeked) the file for every
 * chunk. It now keeps one open FILE* per volume slot, mirroring the Circle
 * backend (#984). This compiles the real backend against a tiny shim and checks
 * the handle is reused across forward reads, invalidated on write/rename/unlink,
 * and still returns the right bytes for backward / out-of-order reads.
 *
 * The same binary then drives the standby-recovery path through fake mount ops
 * (#1493): installed ``mmcore.sys=``/``mmcore.data=`` boot, a late device,
 * two sleep/wake cycles, an open streaming writer across a remount, and
 * D:/MMCORE-DATA recovery. No loop device is needed; the fake backend is the
 * mount seam the real Linux build fills with mount/umount helpers.
 *
 * "One open handle" is counted by scanning the process fd table and matching
 * the target file's dev/inode, so it needs no libc fopen interposition.
 */
/* mmb_priv.h must come before <stdio.h>: it declares `sprintf`, and the macOS
 * SDK turns that name into a macro once <stdio.h> is seen (the native build
 * includes it the same way, so this mirrors the real translation unit). */
#include "mmb_priv.h"
#include "storage_posix.h"

#include <dirent.h>
#include <errno.h>
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

static int put_text(const char *path, const char *text)
{
	FILE *f = fopen(path, "wb");
	if (!f)
		return 0;
	fputs(text, f);
	fclose(f);
	return 1;
}

static int file_text_is(const char *path, const char *want)
{
	char buf[64];
	FILE *f = fopen(path, "rb");
	size_t n;

	if (!f)
		return 0;
	n = fread(buf, 1, sizeof buf - 1, f);
	fclose(f);
	buf[n] = 0;
	return strcmp(buf, want) == 0;
}

#ifdef MMB_STORAGE_TEST
/* ---- fake mount backend ---------------------------------------------- *
 * Each volume has an explicit label and a current mounted/writable state.
 * A mount can be made to fail (a dead superblock still owns the path) or to
 * fail a few times first (a device that re-enumerates after resume). */
#define FAKE_VOL_MAX 6

typedef struct {
	char path[600];
	char label[64];
	int mounted;
	int mount_ok;
	int remount_ok;
	int writable;
	int delay;
	int mounts;
} fake_vol;

static fake_vol fv[FAKE_VOL_MAX];
static int nfv;
static int fake_overlay_ok;
static int fake_label_mismatch;

static void fake_reset(void)
{
	memset(fv, 0, sizeof fv);
	nfv = 0;
	fake_overlay_ok = 0;
	fake_label_mismatch = 0;
}

static fake_vol *fake_get(const char *path)
{
	int i;

	for (i = 0; i < nfv; i++)
		if (strcmp(fv[i].path, path) == 0)
			return &fv[i];
	return 0;
}

static fake_vol *fake_add(const char *path, const char *label)
{
	fake_vol *v = &fv[nfv++];

	memset(v, 0, sizeof *v);
	snprintf(v->path, sizeof v->path, "%s", path);
	snprintf(v->label, sizeof v->label, "%s", label);
	return v;
}

static int fake_is_mount(const char *path)
{
	fake_vol *v = fake_get(path);

	return v && v->mounted;
}

static int fake_mount(const char *path, const char *label)
{
	fake_vol *v = fake_get(path);
	char sub[700];

	if (!v)
		return -1;
	if (strcmp(v->label, label) != 0)
	{
		fake_label_mismatch++;
		return -1;
	}
	if (v->delay > 0)
	{
		v->delay--;
		return -1;
	}
	if (!v->mount_ok)
		return -1;
	mkdir(path, 0777);
	snprintf(sub, sizeof sub, "%s/C", path);
	mkdir(sub, 0777);
	v->mounted = 1;
	v->writable = 1;
	v->mounts++;
	return 0;
}

static int fake_remount(const char *path)
{
	fake_vol *v = fake_get(path);

	if (v && v->remount_ok)
	{
		v->mounted = 1;
		v->writable = 1;
		return 0;
	}
	return -1;
}

static int fake_overlay_writable(void)
{
	return fake_overlay_ok;
}

static int fake_root_writable(const char *path)
{
	fake_vol *v = fake_get(path);

	return v && v->mounted && v->writable;
}

static int fake_unmount(const char *path)
{
	fake_vol *v = fake_get(path);

	if (v)
	{
		v->mounted = 0;
		v->writable = 0;
	}
	return 0;
}

static storage_mount_ops fake_ops = {
	fake_is_mount,
	fake_mount,
	fake_remount,
	fake_overlay_writable,
	fake_root_writable,
	0,
	fake_unmount
};

static void use_fake_ops(const char *recovery_path)
{
	fake_ops.recovery_path = recovery_path;
	mmb_storage_test_set_ops(&fake_ops);
}

/* 1. An installed command line (mmcore.sys=) must enable recovery, and both
 * MMCORE-SYS (C:) and MMCORE-DATA (D:) must remount by their own labels. */
static void scenario_installed(const char *base)
{
	char croot[600], droot[600];
	fake_vol *c, *d;

	fake_reset();
	snprintf(croot, sizeof croot, "%s/inst-c", base);
	snprintf(droot, sizeof droot, "%s/inst-d", base);
	mkdir(croot, 0777);
	mkdir(droot, 0777);
	setenv("MMB_DRIVE_ROOT", croot, 1);
	storage_posix_mount('D', droot);
	mmb_storage_test_set_cmdline(
		"mmcore.sys=LABEL=MMCORE-SYS mmcore.data=LABEL=MMCORE-DATA");
	mmb_storage_test_set_overlay(0);
	mmb_storage_test_set_recovery(2, 0);
	c = fake_add(croot, "MMCORE-SYS");
	c->mounted = 1;
	c->mount_ok = 1;
	c->remount_ok = 0;
	c->writable = 0;
	d = fake_add(droot, "MMCORE-DATA");
	d->mounted = 1;
	d->mount_ok = 1;
	d->remount_ok = 0;
	d->writable = 0;
	use_fake_ops(0);

	mmb_storage_revalidate();

	check(fake_label_mismatch == 0, "installed labels are explicit");
	check(c->mounted && c->writable && c->mounts == 1,
	      "MMCORE-SYS recovered at the original C: path");
	check(d->mounted && d->writable && d->mounts == 1,
	      "MMCORE-DATA recovered at the original D: path");
	check(getenv("MMB_DRIVE_ROOT") &&
		      strcmp(getenv("MMB_DRIVE_ROOT"), croot) == 0,
	      "installed C: keeps its original mountpoint");
	check(!mmb_storage_persistence_lost(), "installed storage healthy");
}

/* 2. The SD/USB device can appear only seconds after userspace resumed: the
 * recovery retries the label mount with backoff. */
static void scenario_delayed_device(const char *base)
{
	char croot[600];
	fake_vol *c;

	fake_reset();
	snprintf(croot, sizeof croot, "%s/delay-c", base);
	mkdir(croot, 0777);
	setenv("MMB_DRIVE_ROOT", croot, 1);
	mmb_storage_test_set_cmdline("mmcore.live=1");
	mmb_storage_test_set_overlay(0);
	mmb_storage_test_set_recovery(5, 0);
	c = fake_add(croot, "MMCORE");
	c->mount_ok = 1;
	c->delay = 3;
	use_fake_ops(0);

	mmb_storage_revalidate();

	check(c->mounted && c->mounts == 1,
	      "late device mounted after the retry budget");
	check(fake_label_mismatch == 0, "late mount used MMCORE");
}

/* 3. Two sleep/wake cycles: the first recovers to a fallback path, the
 * second must still look for MMCORE (never the fallback's basename) and move
 * C: back to its original mountpoint. */
static void scenario_two_sleeps(const char *base)
{
	char croot[600], fallback[600];
	fake_vol *c, *f;

	fake_reset();
	snprintf(croot, sizeof croot, "%s/sleep-c", base);
	snprintf(fallback, sizeof fallback, "%s/sleep-recovered", base);
	mkdir(croot, 0777);
	setenv("MMB_DRIVE_ROOT", croot, 1);
	mmb_storage_test_set_cmdline("mmcore.live=1");
	mmb_storage_test_set_overlay(0);
	mmb_storage_test_set_recovery(2, 0);
	c = fake_add(croot, "MMCORE");
	c->mounted = 1;
	c->mount_ok = 0;
	c->remount_ok = 0;
	c->writable = 0;
	f = fake_add(fallback, "MMCORE");
	f->mount_ok = 1;
	use_fake_ops(fallback);

	mmb_storage_revalidate();
	check(getenv("MMB_DRIVE_ROOT") &&
		      strcmp(getenv("MMB_DRIVE_ROOT"), fallback) == 0,
	      "first wake recovers to the fallback mountpoint");
	check(f->mounts == 1 && fake_label_mismatch == 0,
	      "fallback mounted by MMCORE label");

	c->mount_ok = 1;
	mmb_storage_revalidate();
	check(getenv("MMB_DRIVE_ROOT") &&
		      strcmp(getenv("MMB_DRIVE_ROOT"), croot) == 0,
	      "second wake returns C: to its original mountpoint");
	check(c->mounts == 1 && f->mounts == 1,
	      "second recovery mounted the original path");
	check(fake_label_mismatch == 0,
	      "repeated sleeps never derive a label from the fallback path");
}

/* 4. A streaming writer open across a remount reopens the recovered mount and
 * continues at its byte offset. */
static void scenario_streaming_write(const char *base)
{
	char croot[600], fallback[600], fc[700];
	char old_file[700], new_file[700];
	fake_vol *c, *f;
	void *h;

	fake_reset();
	snprintf(croot, sizeof croot, "%s/stream-c", base);
	snprintf(fallback, sizeof fallback, "%s/stream-recovered", base);
	mkdir(croot, 0777);
	mkdir(fallback, 0777);
	snprintf(fc, sizeof fc, "%s/C", fallback);
	mkdir(fc, 0777);
	setenv("MMB_DRIVE_ROOT", croot, 1);
	mmb_storage_test_set_cmdline("mmcore.live=1");
	mmb_storage_test_set_overlay(0);
	mmb_storage_test_set_recovery(2, 0);
	c = fake_add(croot, "MMCORE");
	c->mounted = 1;
	c->mount_ok = 1;
	c->remount_ok = 1;
	c->writable = 1;
	f = fake_add(fallback, "MMCORE");
	f->mount_ok = 1;
	use_fake_ops(fallback);

	h = mmb_fat_wopen('C', "/STREAM.TXT", 0);
	check(h != 0, "streaming writer opened");
	check(mmb_fat_wwrite(h, "AAAA", 4) == 0, "stream first chunk");

	/* The recovered volume already holds the bytes written so far. */
	snprintf(new_file, sizeof new_file, "%s/C/STREAM.TXT", fallback);
	check(put_text(new_file, "AAAA"), "seed recovered stream file");
	c->writable = 0;
	c->remount_ok = 0;
	c->mount_ok = 0;

	mmb_storage_revalidate();
	check(getenv("MMB_DRIVE_ROOT") &&
		      strcmp(getenv("MMB_DRIVE_ROOT"), fallback) == 0,
	      "stream recovery used the fallback mountpoint");
	check(mmb_fat_wwrite(h, "BBBB", 4) == 0,
	      "stream write after remount");
	check(mmb_fat_wclose(h) == 0, "stream closed");
	snprintf(old_file, sizeof old_file, "%s/C/STREAM.TXT", croot);
	check(file_text_is(new_file, "AAAABBBB"),
	      "open stream continued on the recovered mount");
	check(!file_text_is(old_file, "AAAABBBB"),
	      "stream did not keep writing into the dead mount");
}

/* 5. D:/MMCORE-DATA recovers on its own label, and a stale-mount I/O error at
 * any time (not just after a resume) drives the same recovery. */
static void scenario_data_and_io_error(const char *base)
{
	char croot[600], droot[600];
	fake_vol *c, *d;
	unsigned gen;

	fake_reset();
	snprintf(croot, sizeof croot, "%s/data-c", base);
	snprintf(droot, sizeof droot, "%s/data-d", base);
	mkdir(croot, 0777);
	mkdir(droot, 0777);
	setenv("MMB_DRIVE_ROOT", croot, 1);
	storage_posix_mount('D', droot);
	mmb_storage_test_set_cmdline(
		"mmcore.sys=LABEL=MMCORE-SYS mmcore.data=LABEL=MMCORE-DATA");
	mmb_storage_test_set_overlay(0);
	mmb_storage_test_set_recovery(2, 0);
	c = fake_add(croot, "MMCORE-SYS");
	c->mounted = 1;
	c->mount_ok = 1;
	c->remount_ok = 1;
	c->writable = 1;
	d = fake_add(droot, "MMCORE-DATA");
	d->mounted = 1;
	d->mount_ok = 1;
	d->remount_ok = 0;
	d->writable = 0;
	use_fake_ops(0);

	gen = mmb_storage_generation();
	check(mmb_storage_test_io_error('D', EIO) == 1,
	      "D: stale I/O error triggers recovery");
	check(d->mounted && d->writable && d->mounts == 1,
	      "MMCORE-DATA recovered by label");
	check(mmb_storage_generation() == gen + 1,
	      "generation bumped for open-handle invalidation");
	check(fake_label_mismatch == 0, "D: recovery used MMCORE-DATA");

	/* Non-stale errors must not run the recovery path. */
	check(mmb_storage_test_io_error('D', EACCES) == 0,
	      "non-stale errno is not a mount failure");
}

/* 6. A file read beforehand reopens on the recovered mount, and a later save
 * lands there rather than in the dead mount. */
static void scenario_read_reopen(const char *base)
{
	char croot[600], fallback[600], fc[700];
	char old_file[700], new_file[700], new_save[700], old_save[700];
	fake_vol *c, *f;

	fake_reset();
	snprintf(croot, sizeof croot, "%s/read-c", base);
	snprintf(fallback, sizeof fallback, "%s/read-recovered", base);
	mkdir(croot, 0777);
	snprintf(old_file, sizeof old_file, "%s/C/R.TXT", croot);
	mkdir(croot, 0777);
	snprintf(fc, sizeof fc, "%s/C", croot);
	mkdir(fc, 0777);
	setenv("MMB_DRIVE_ROOT", croot, 1);
	mmb_storage_test_set_cmdline("mmcore.live=1");
	mmb_storage_test_set_overlay(0);
	mmb_storage_test_set_recovery(2, 0);
	c = fake_add(croot, "MMCORE");
	c->mounted = 1;
	c->mount_ok = 0;
	c->remount_ok = 0;
	c->writable = 1;
	f = fake_add(fallback, "MMCORE");
	f->mount_ok = 1;
	use_fake_ops(fallback);

	check(put_text(old_file, "OLD"), "seed old R.TXT");
	snprintf(new_file, sizeof new_file, "%s/C/R.TXT", fallback);
	snprintf(new_save, sizeof new_save, "%s/C/SAVE.TXT", fallback);
	snprintf(old_save, sizeof old_save, "%s/C/SAVE.TXT", croot);
	mkdir(fallback, 0777);
	snprintf(fc, sizeof fc, "%s/C", fallback);
	mkdir(fc, 0777);
	check(put_text(new_file, "NEWDATA"), "seed recovered R.TXT");

	check(read_is('C', "/R.TXT", 0, "OLD", 3), "read before standby");
	c->writable = 0;
	mmb_storage_revalidate();
	check(getenv("MMB_DRIVE_ROOT") &&
		      strcmp(getenv("MMB_DRIVE_ROOT"), fallback) == 0,
	      "read recovery used the fallback mountpoint");
	check(read_is('C', "/R.TXT", 0, "NEWDATA", 7),
	      "open file re-read from recovered mount");
	check(mmb_fat_write('C', "/SAVE.TXT", "AFTER", 5, 0) == 0,
	      "save after recovery");
	check(file_text_is(new_save, "AFTER"),
	      "save landed on the recovered volume");
	check(access(old_save, F_OK) != 0,
	      "no silent write into the dead mount");
	check(fake_label_mismatch == 0, "read recovery used MMCORE");
}

/* 7. If the volume never returns, report persistence lost instead of
 * pretending the session is safe. */
static void scenario_unrecoverable(const char *base)
{
	char croot[600], fallback[600];
	fake_vol *c, *f;

	fake_reset();
	snprintf(croot, sizeof croot, "%s/broken-c", base);
	snprintf(fallback, sizeof fallback, "%s/broken-recovered", base);
	mkdir(croot, 0777);
	setenv("MMB_DRIVE_ROOT", croot, 1);
	mmb_storage_test_set_cmdline("mmcore.live=1");
	mmb_storage_test_set_overlay(1);
	mmb_storage_test_set_recovery(1, 0);
	c = fake_add(croot, "MMCORE");
	c->mounted = 1;
	c->mount_ok = 0;
	c->remount_ok = 0;
	c->writable = 0;
	f = fake_add(fallback, "MMCORE");
	f->mount_ok = 0;
	fake_overlay_ok = 0;
	use_fake_ops(fallback);

	mmb_storage_revalidate();
	check(mmb_storage_persistence_lost(),
	      "lost store reported when every mount fails");
}
#endif

int main(void)
{
	char tmpl[] = "/tmp/mmb_rdcache_XXXXXX";
	char *root = mkdtemp(tmpl);
	char fpath[512], gpath[512];
	char apath[512], bpath[512];
	char rpath[512], rnew[512];
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
	snprintf(rpath, sizeof rpath, "%s/C/R.TXT", root);
	snprintf(rnew, sizeof rnew, "%s/C/R.NEW", root);

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

	/* 9. Replacing a file (new inode at the same path) invalidates the
	 * cached stream: this is the suspend/remount case in miniature. */
	write_file("/R.TXT", "OLD");
	check(read_is('C', "/R.TXT", 0, "OLD", 3), "read R before replace");
	write_file("/R.NEW", "NEWDATA");
	check(rename(rnew, rpath) == 0, "rename over cached R.TXT");
	check(read_is('C', "/R.TXT", 0, "NEWDATA", 7),
	      "replaced file read, not the stale handle");

	/* 10. Persistence health is a no-op off the live ISO (no mmcore.live=1
	 * on the test cmdline): revalidating must not report a lost store. */
	mmb_storage_revalidate();
	check(!mmb_storage_persistence_lost(),
	      "persistence not reported lost off the live ISO");

#ifdef MMB_STORAGE_TEST
	scenario_installed(root);
	mmb_storage_test_reset();
	scenario_delayed_device(root);
	mmb_storage_test_reset();
	scenario_two_sleeps(root);
	mmb_storage_test_reset();
	scenario_streaming_write(root);
	mmb_storage_test_reset();
	scenario_data_and_io_error(root);
	mmb_storage_test_reset();
	scenario_read_reopen(root);
	mmb_storage_test_reset();
	scenario_unrecoverable(root);
	mmb_storage_test_reset();
#endif

	if (fails)
	{
		printf("%d check(s) failed\n", fails);
		return 1;
	}
	printf("all checks passed\n");
	return 0;
}
