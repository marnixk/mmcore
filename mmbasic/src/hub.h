#ifndef MMB_HUB_H
#define MMB_HUB_H

/* HUB engine (#1525): non-UI part of the built-in package hub. Everything
 * here is host-testable and free of console drawing; the HUB screens ticket
 * (part 4) builds on this API. */

enum {
	HUB_NOT_INSTALLED = 0,
	HUB_INSTALLED = 1,
	HUB_UPDATE = 2,
	HUB_NEEDS_NEWER = 3
};

typedef struct hub_app {
	char *id;
	char *file;
	char *title;
	char *author;
	char *category;
	char *version;
	char *summary;
	char *minver;
	char *sha256;
	char *updated;
	char *icon;
	char *thumb;
	char *hero; /* optional info/<id>/HERO.PNG; empty when the index omits it */
	char *about;
	char *changes; /* INFO/CHANGES.TXT text, empty when the file is absent */
	char **shots;
	int nshots;
	long size;
	int cat_index;
	int state;
	char *installed_path;
	char *installed_version;
} hub_app;

/* Progress callback for downloads. `total` is 0 when unknown. Return non-zero
 * to cancel; the partial download is discarded and no file is written. */
typedef int (*hub_progress_fn)(unsigned bytes_done, unsigned bytes_total,
			       void *ctx);

/* GET <OPTION HUB URL>index.json, parse it and compute each app's state.
 * Returns 0 on success, -1 with a friendly message in `err` on failure:
 * "OFFLINE", "SERVER ERROR <code>" or "UNSUPPORTED INDEX ...". */
int hub_load_index(char *err, int errsz);

int hub_count(void);
hub_app *hub_get(int i);

/* Fill `out` with the app indices whose category is `cat_index` (-1 = ALL),
 * in index (display) order. Returns the total number of matching apps (which
 * may exceed `max`). */
int hub_filter(int cat_index, int *out, int max);

/* Download `<OPTION HUB URL><relpath>` through a small in-memory LRU cache.
 * An empty relpath succeeds with *buf = NULL and *n = 0 without a request.
 * On success *buf is a caller-owned copy (release with G.plat->free). */
int hub_fetch(const char *relpath, unsigned char **buf, unsigned *n,
	      char *err, int errsz);

/* Install or update app `i`. Verifies size and SHA-256 before writing, writes
 * `<target>.tmp` and renames it into place. Returns 0 on success. */
int hub_install(int i, hub_progress_fn cb, void *ctx, char *err, int errsz);

/* Delete the installed .APP for app `i` and recompute its state. */
int hub_remove(int i, char *err, int errsz);

/* Free every loaded app (the HUB URL option is untouched). */
void hub_free(void);

/* Command entry point: HUB / HUB LIST / HUB INSTALL id$ / HUB UPDATE /
 * HUB REMOVE id$ / HUB FETCH relpath$. */
void mmb_cmd_hub(void);

#endif
