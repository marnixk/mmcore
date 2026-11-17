#include "mmb_priv.h"
#include "pkg_info.h"

/* Archive side of the shared PACKAGE.INF reader (#1523): locate the entry in
 * an .APP, size it from the central directory, then parse it. Kept apart from
 * pkg_info.c so the pure parser stays host-testable. */

static int pi_read_at(void *ctx, unsigned pos, void *buf, unsigned n, unsigned *got)
{
	return mmb_vfs_read_at((const char *)ctx, pos, buf, n, got);
}

int mmb_pkg_info_read(const char *path, struct mmb_pkg_info *out)
{
	unsigned zip_size, usz = 0, got = 0;
	char *buf;
	int sz, rc;

	if (!out)
		return -1;
	mmb_pkg_info_init(out);
	if (!path || !path[0])
		return -1;
	sz = mmb_vfs_size(path);
	if (sz < 0 || (unsigned)sz > MMB_ZIP_MAX_BYTES)
		return -1;
	zip_size = (unsigned)sz;
	if (mmb_zip_entry_size(pi_read_at, (void *)path, zip_size,
			       "PACKAGE.INF", &usz) != 0)
		return -1;
	if (usz > MMB_ZIP_MAX_BYTES)
		return -1;
	buf = G.plat->alloc(usz + 1);
	if (!buf)
		return -1;
	if (mmb_zip_read_entry(pi_read_at, (void *)path, zip_size,
			       "PACKAGE.INF", buf, usz + 1, &got) != 0)
	{
		G.plat->free(buf);
		return -1;
	}
	buf[got] = 0;
	rc = mmb_pkg_info_parse(buf, got, out);
	G.plat->free(buf);
	return rc;
}
