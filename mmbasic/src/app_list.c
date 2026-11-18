#include "mmb_priv.h"
#include "app_list.h"
#include "app_category.h"

/* Pure grouped-row builder (#1524). Kept free of TUI and VFS calls so the
 * host test can exercise the launcher/picker semantics without QEMU. */

static char al_upper(char c)
{
	return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
}

static int al_contains_ci(const char *hay, const char *needle)
{
	const char *h;

	if (!needle || !needle[0])
		return 1;
	if (!hay)
		return 0;
	for (h = hay; *h; h++)
	{
		const char *a = h, *b = needle;

		while (*a && *b && al_upper(*a) == al_upper(*b))
		{
			a++;
			b++;
		}
		if (!*b)
			return 1;
	}
	return 0;
}

/* Case-insensitive label order, with a stable byte compare as tie-break. */
static int al_label_cmp(const char *a, const char *b)
{
	if (!a)
		a = "";
	if (!b)
		b = "";
	while (*a && *b)
	{
		char ca = al_upper(*a), cb = al_upper(*b);

		if (ca != cb)
			return ca < cb ? -1 : 1;
		a++;
		b++;
	}
	if (*a == *b)
		return 0;
	return *a ? 1 : -1;
}

static int al_normal_category(int cat)
{
	if (cat == APP_LIST_CAT_BUILTIN)
		return cat;
	if (cat < 0 || cat >= mmb_app_category_count())
		return MMB_APP_CATEGORY_OTHER;
	return cat;
}

static void al_reset(struct app_list *al)
{
	if (!al)
		return;
	if (al->rows && G.plat)
		G.plat->free(al->rows);
	al->rows = 0;
	al->nrow = 0;
	al->cap = 0;
	al->napp = 0;
}

void app_list_init(struct app_list *al)
{
	if (!al)
		return;
	al->rows = 0;
	al->nrow = 0;
	al->cap = 0;
	al->napp = 0;
}

void app_list_free(struct app_list *al)
{
	al_reset(al);
}

static int al_reserve(struct app_list *al, int need)
{
	struct app_list_row *rows;
	int cap;

	if (need <= al->cap)
		return 0;
	cap = al->cap > 0 ? al->cap : 16;
	while (cap < need)
		cap *= 2;
	if (!G.plat || !G.plat->alloc)
		return -1;
	rows = G.plat->alloc((unsigned)cap * (unsigned)sizeof(*rows));
	if (!rows)
		return -1;
	if (al->rows)
	{
		memcpy(rows, al->rows,
		       (unsigned)al->nrow * (unsigned)sizeof(*rows));
		G.plat->free(al->rows);
	}
	al->rows = rows;
	al->cap = cap;
	return 0;
}

static int al_add(struct app_list *al, int kind, int category, int count,
		  int payload, const char *label, const char *summary)
{
	struct app_list_row *r;

	if (al_reserve(al, al->nrow + 1) != 0)
		return -1;
	r = &al->rows[al->nrow++];
	r->kind = kind;
	r->category = category;
	r->count = count;
	r->payload = payload;
	r->label = label;
	r->summary = summary;
	if (kind == APP_LIST_APP)
		al->napp++;
	return 0;
}

static int al_visible(const struct app_list_src *e, const char *filter)
{
	return al_contains_ci(e->label, filter) ||
	       al_contains_ci(e->summary, filter);
}

static int al_matches_cat(const struct app_list_src *e, int c)
{
	return al_normal_category(e->category) == c;
}

/* Insertion-sort the matching source indexes in `order` by label. */
static void al_sort_order(const struct app_list_src *src, int *order, int n)
{
	int i, j;

	for (i = 1; i < n; i++)
	{
		int v = order[i];

		j = i - 1;
		while (j >= 0 &&
		       al_label_cmp(src[order[j]].label, src[v].label) > 0)
		{
			order[j + 1] = order[j];
			j--;
		}
		order[j + 1] = v;
	}
}

/* Collect one category group and append its header and app rows. `sort`
 * orders the group by label; built-ins keep their source order, which is
 * deliberate (#793). */
static int al_group(struct app_list *al, const struct app_list_src *src, int n,
		    int *order, int category, const char *header,
		    const char *filter, int sort)
{
	int i, k = 0;

	for (i = 0; i < n; i++)
	{
		if (!al_matches_cat(&src[i], category))
			continue;
		if (!al_visible(&src[i], filter))
			continue;
		order[k++] = i;
	}
	if (k == 0)
		return 0;
	if (sort)
		al_sort_order(src, order, k);
	if (al_add(al, APP_LIST_HEADER, category, k, 0, header, 0) != 0)
		return -1;
	for (i = 0; i < k; i++)
	{
		const struct app_list_src *e = &src[order[i]];

		if (al_add(al, APP_LIST_APP, category, 0, e->payload, e->label,
			   e->summary) != 0)
			return -1;
	}
	return 0;
}

/* Flat picker: every matching app, regardless of category, sorted by label.
 * No header rows. */
static int al_flat(struct app_list *al, const struct app_list_src *src, int n,
		   int *order, const char *filter, int cat_filter)
{
	int i, k = 0;

	for (i = 0; i < n; i++)
	{
		if ((cat_filter >= 0 || cat_filter == APP_LIST_CAT_BUILTIN) &&
		    al_normal_category(src[i].category) != cat_filter)
			continue;
		if (!al_visible(&src[i], filter))
			continue;
		order[k++] = i;
	}
	al_sort_order(src, order, k);
	for (i = 0; i < k; i++)
	{
		const struct app_list_src *e = &src[order[i]];

		if (al_add(al, APP_LIST_APP, al_normal_category(e->category), 0,
			   e->payload, e->label, e->summary) != 0)
			return -1;
	}
	return 0;
}

int app_list_build(struct app_list *al, const struct app_list_src *src, int n,
		   const char *filter, int cat_filter, int flags)
{
	int *order = 0;
	int cat, rc = 0;

	if (!al)
		return -1;
	al_reset(al);
	if (n < 0)
		n = 0;
	if (!filter)
		filter = "";

	if (n > 0)
	{
		if (!G.plat || !G.plat->alloc)
			return -1;
		order = G.plat->alloc((unsigned)n * (unsigned)sizeof(*order));
		if (!order)
			return -1;
	}

	if (!(flags & APP_LIST_GROUPED))
	{
		rc = al_flat(al, src, n, order, filter, cat_filter);
		goto done;
	}

	if ((flags & APP_LIST_HUB) &&
	    (cat_filter < 0 || cat_filter == APP_LIST_CAT_HUB) &&
	    al_contains_ci("HUB", filter))
	{
		if (al_add(al, APP_LIST_HEADER, APP_LIST_CAT_HUB, 1, 0, "HUB",
			   0) != 0 ||
		    al_add(al, APP_LIST_APP, APP_LIST_CAT_HUB, 0,
			   APP_LIST_PAYLOAD_HUB, "HUB", 0) != 0)
		{
			rc = -1;
			goto done;
		}
	}

	for (cat = 0; cat < mmb_app_category_count(); cat++)
	{
		if (cat_filter >= 0 && cat_filter != cat)
			continue;
		if (al_group(al, src, n, order, cat,
			     mmb_app_category_name(cat), filter, 1) != 0)
		{
			rc = -1;
			goto done;
		}
	}

	if (cat_filter < 0 || cat_filter == APP_LIST_CAT_BUILTIN)
	{
		if (al_group(al, src, n, order, APP_LIST_CAT_BUILTIN,
			     "BUILT-IN", filter, 0) != 0)
			rc = -1;
	}

done:
	if (order)
		G.plat->free(order);
	if (rc != 0)
		al_reset(al);
	return rc;
}

int app_list_is_app(const struct app_list *al, int row)
{
	if (!al || row < 0 || row >= al->nrow)
		return 0;
	return al->rows[row].kind == APP_LIST_APP;
}

int app_list_first(const struct app_list *al)
{
	int i;

	if (!al)
		return -1;
	for (i = 0; i < al->nrow; i++)
		if (al->rows[i].kind == APP_LIST_APP)
			return i;
	return -1;
}

int app_list_last(const struct app_list *al)
{
	int i;

	if (!al)
		return -1;
	for (i = al->nrow - 1; i >= 0; i--)
		if (al->rows[i].kind == APP_LIST_APP)
			return i;
	return -1;
}

int app_list_next(const struct app_list *al, int row, int dir)
{
	int i;

	if (!al || dir == 0)
		return -1;
	if (row < 0)
		return dir > 0 ? app_list_first(al) : app_list_last(al);
	for (i = row + dir; i >= 0 && i < al->nrow; i += dir)
		if (al->rows[i].kind == APP_LIST_APP)
			return i;
	return -1;
}

int app_list_skip(const struct app_list *al, int row, int dir, int count)
{
	int i;

	if (!al)
		return -1;
	if (row < 0)
		row = dir > 0 ? app_list_first(al) : app_list_last(al);
	for (i = 0; i < count; i++)
	{
		int next = app_list_next(al, row, dir);

		if (next < 0)
			break;
		row = next;
	}
	return row;
}

int app_list_jump_category(const struct app_list *al, int row, int dir)
{
	int i, header;

	if (!al || dir == 0 || al->nrow == 0)
		return -1;
	if (row < 0 || row >= al->nrow)
		return dir > 0 ? app_list_first(al) : app_list_last(al);

	header = -1;
	for (i = row; i >= 0; i--)
		if (al->rows[i].kind == APP_LIST_HEADER)
		{
			header = i;
			break;
		}
	if (header < 0)
		return app_list_first(al);

	for (i = header + dir; i >= 0 && i < al->nrow; i += dir)
		if (al->rows[i].kind == APP_LIST_HEADER)
			return app_list_next(al, i - 1, 1);
	return -1;
}

int app_list_clamp(const struct app_list *al, int row)
{
	int i;

	if (!al || al->napp == 0)
		return -1;
	if (row < 0)
		row = 0;
	if (row >= al->nrow)
		row = al->nrow - 1;
	for (i = row; i >= 0; i--)
		if (al->rows[i].kind == APP_LIST_APP)
			return i;
	for (i = row + 1; i < al->nrow; i++)
		if (al->rows[i].kind == APP_LIST_APP)
			return i;
	return -1;
}
