#ifndef MMB_PKG_INFO_H
#define MMB_PKG_INFO_H

#include "app_category.h"

/* Parsed PACKAGE.INF metadata (#1523). Every string is dynamically allocated
 * through the interpreter allocator (G.plat->alloc); mmb_pkg_info_free()
 * releases them. Strings are stored without surrounding whitespace and the
 * category is upper-cased. `cat_index` is always a valid app category index
 * (MMB_APP_CATEGORY_OTHER when missing or unknown). */
struct mmb_pkg_info {
	char *name;
	char *title;
	char *author;
	char *category;
	char *version;
	char *summary;
	char *minver;
	int cat_index;
};

void mmb_pkg_info_init(struct mmb_pkg_info *pi);
void mmb_pkg_info_free(struct mmb_pkg_info *pi);

/* Pure line parser: lowercase `key=value` lines, any line ending, unknown
 * keys ignored, duplicate keys last-wins, missing/garbage tolerated. `n` is
 * the byte length of `text`. Returns 0 on success, non-zero when out is NULL. */
int mmb_pkg_info_parse(const char *text, unsigned n, struct mmb_pkg_info *out);

/* Open an .APP archive and read its PACKAGE.INF. Never crashes on a missing
 * or corrupt archive: on failure `out` is left as a valid empty struct
 * (cat_index = MMB_APP_CATEGORY_OTHER) and the return is non-zero. */
int mmb_pkg_info_read(const char *path, struct mmb_pkg_info *out);

/* Set one field by INF key (name/title/author/category/version/summary/
 * minver); unknown keys are ignored. Frees any previous value. The category
 * is upper-cased and its index refreshed; an empty category clears it. */
int mmb_pkg_info_set(struct mmb_pkg_info *pi, const char *key, const char *value);

/* Serialise the struct as PACKAGE.INF text (lowercase keys, `\n` endings, no
 * quoting, `name` and `main=MAIN.BAS` always present, optional keys omitted
 * when empty, summary truncated to 80 chars). Allocates *out via the
 * interpreter allocator; the caller frees it with G.plat->free. Returns 0 on
 * success. */
int mmb_pkg_info_build(const struct mmb_pkg_info *pi, char **out, unsigned *n);

/* Compare dotted numeric versions. Missing parts are 0, a leading 'v' is
 * ignored, and anything after the first non-digit/non-dot char is ignored.
 * Returns <0, 0 or >0 for a<b, a==b, a>b. */
int mmb_version_cmp(const char *a, const char *b);

#endif
