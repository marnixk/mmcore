#ifndef MMB_APP_LIST_H
#define MMB_APP_LIST_H

/*
 * Pure row model for the APPS launcher and Ctrl+Space picker (#1524).
 *
 * No TUI or filesystem dependency: the caller fills a flat array of entries
 * and this module builds the visible, grouped row list plus the navigation
 * helpers the widgets use. That keeps grouping, sorting, filtering and
 * Up/Down/Left/Right semantics host-testable (tests/app_list_host.c).
 */

#define APP_LIST_CAT_HUB     (-1)
#define APP_LIST_CAT_BUILTIN (-2)
#define APP_LIST_PAYLOAD_HUB (-1)

#define APP_LIST_HEADER 0
#define APP_LIST_APP    1

/* Build flags: group under headers (APPS launcher) and/or pin the HUB entry.
 * Without APP_LIST_GROUPED the rows are one flat label-sorted list (the
 * Ctrl+Space picker). */
#define APP_LIST_GROUPED 0x1
#define APP_LIST_HUB     0x2

struct app_list_src {
	const char *label;   /* required; compare/sort key */
	const char *summary; /* optional; filtered too */
	int category;        /* 0..MMB_APP_CATEGORY_COUNT-1, or APP_LIST_CAT_BUILTIN */
	int payload;         /* opaque caller index */
};

struct app_list_row {
	int kind;          /* APP_LIST_HEADER or APP_LIST_APP */
	int category;      /* group id; APP_LIST_CAT_HUB / APP_LIST_CAT_BUILTIN */
	int count;         /* header: number of visible apps in the group */
	int payload;       /* app: caller payload, or APP_LIST_PAYLOAD_HUB */
	const char *label; /* app: label; header: group title */
	const char *summary; /* app: optional summary, header: 0 */
};

struct app_list {
	struct app_list_row *rows;
	int nrow;
	int cap;
	int napp;
};

void app_list_init(struct app_list *al);
void app_list_free(struct app_list *al);

/* Rebuild `al` from `src`. `filter` is a case-insensitive substring matched
 * against each label and summary (NULL/empty = no filter). `cat_filter` is a
 * category index, APP_LIST_CAT_BUILTIN, or -1 for ALL. `flags` combines
 * APP_LIST_GROUPED (category headers, fixed order; the launcher) and
 * APP_LIST_HUB (pinned HUB header + entry first, grouped mode only).
 * Without GROUPED the list is flat and label-sorted (the picker).
 * Returns 0 on success, -1 when the list could not be allocated (it is then
 * left empty, never partially built). */
int app_list_build(struct app_list *al, const struct app_list_src *src, int n,
		   const char *filter, int cat_filter, int flags);

int app_list_is_app(const struct app_list *al, int row);
int app_list_first(const struct app_list *al);
int app_list_last(const struct app_list *al);
/* Nearest app row from `row` + `dir` (+1 down / -1 up), skipping headers.
 * Returns -1 when there is no further app row. */
int app_list_next(const struct app_list *al, int row, int dir);
/* Page: repeat app_list_next() up to `count` times. Stops at either end. */
int app_list_skip(const struct app_list *al, int row, int dir, int count);
/* First app of the previous/next group, including HUB and BUILT-IN groups. */
int app_list_jump_category(const struct app_list *al, int row, int dir);
/* Clamp `row` into the list and snap it onto an app row (nearest at or
 * before the requested row, else the next one). -1 when there are no apps. */
int app_list_clamp(const struct app_list *al, int row);

#endif
