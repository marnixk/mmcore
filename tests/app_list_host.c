/* Host test for the grouped app-row model (#1524).
 *
 * Compiles the real mmbasic/src/app_list.c + app_category.c against a tiny
 * allocator shim. Covers fixed category order, non-empty headers only, unknown
 * categories under OTHER, case-insensitive sorting, filter (label and
 * summary), header-free flat picker ordering, category filters, HUB pinning,
 * the BUILT-IN tail group, header-skipping navigation and category jumps.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mmb_priv.h"
#include "app_list.h"
#include "app_category.h"

mmb_globals_t G;

static int fails;

static void check(int cond, const char *what)
{
	if (!cond)
	{
		printf("FAIL: %s\n", what);
		fails++;
	}
}

static void *host_alloc(unsigned n)
{
	return malloc(n ? n : 1);
}

static void host_free(void *p)
{
	free(p);
}

static mmb_plat_t s_plat = { host_alloc, host_free };

static int row_header(const struct app_list *al, int i, int cat,
		      int count)
{
	if (i < 0 || i >= al->nrow)
		return 0;
	return al->rows[i].kind == APP_LIST_HEADER &&
	       al->rows[i].category == cat && al->rows[i].count == count;
}

static int row_app(const struct app_list *al, int i, const char *label)
{
	if (i < 0 || i >= al->nrow)
		return 0;
	return al->rows[i].kind == APP_LIST_APP &&
	       strcmp(al->rows[i].label, label) == 0;
}

static int row_cat(const struct app_list *al, int i, int cat)
{
	if (i < 0 || i >= al->nrow)
		return 0;
	return al->rows[i].kind == APP_LIST_APP &&
	       al->rows[i].category == cat;
}

static struct app_list_src S(const char *label, const char *summary, int cat,
			     int payload)
{
	struct app_list_src s;

	s.label = label;
	s.summary = summary;
	s.category = cat;
	s.payload = payload;
	return s;
}

static void test_grouping(void)
{
	struct app_list al;
	struct app_list_src src[5];

	src[0] = S("Zeta", 0, 0, 0);  /* GAMES */
	src[1] = S("alpha", 0, 0, 1); /* GAMES */
	src[2] = S("Mid", "middle", 3, 2); /* AUDIO */
	src[3] = S("bogus", 0, mmb_app_category_index("BOGUS"), 3);
	src[4] = S("Beta", 0, MMB_APP_CATEGORY_OTHER, 4);

	app_list_init(&al);
	check(app_list_build(&al, src, 5, "", -1, APP_LIST_GROUPED) == 0,
	      "grouped build");
	check(al.nrow == 8, "grouped row count");
	check(row_header(&al, 0, 0, 2), "GAMES header first, count 2");
	check(row_app(&al, 1, "alpha"), "GAMES sorted alpha first");
	check(row_app(&al, 2, "Zeta"), "GAMES sorted Zeta second");
	check(row_header(&al, 3, 3, 1), "AUDIO header, count 1");
	check(row_app(&al, 4, "Mid"), "AUDIO app");
	check(row_header(&al, 5, MMB_APP_CATEGORY_OTHER, 2),
	      "OTHER header after AUDIO");
	check(row_app(&al, 6, "Beta"), "OTHER sorted Beta first");
	check(row_app(&al, 7, "bogus"), "unknown category under OTHER");
	check(row_cat(&al, 7, MMB_APP_CATEGORY_OTHER), "unknown -> OTHER index");
	check(!row_header(&al, 6, 1, 0), "no empty DEMOS header");
	app_list_free(&al);

	/* An out-of-range category index is normalised to OTHER too. */
	src[0] = S("Oops", 0, 99, 0);
	app_list_init(&al);
	check(app_list_build(&al, src, 1, "", -1, APP_LIST_GROUPED) == 0,
	      "invalid category build");
	check(al.nrow == 2 && row_header(&al, 0, MMB_APP_CATEGORY_OTHER, 1),
	      "invalid category grouped under OTHER");
	app_list_free(&al);
}

static void test_hub(void)
{
	struct app_list al;
	struct app_list_src src[1];

	src[0] = S("Game", 0, 0, 0);
	app_list_init(&al);
	check(app_list_build(&al, src, 1, "", -1,
			     APP_LIST_GROUPED | APP_LIST_HUB) == 0,
	      "hub build");
	check(al.nrow == 4, "hub rows");
	check(row_header(&al, 0, APP_LIST_CAT_HUB, 1), "HUB header first");
	check(al.rows[1].kind == APP_LIST_APP &&
		      al.rows[1].payload == APP_LIST_PAYLOAD_HUB,
	      "HUB app row");
	check(row_header(&al, 2, 0, 1), "category follows HUB");
	app_list_free(&al);

	app_list_init(&al);
	check(app_list_build(&al, src, 1, "", -1, APP_LIST_GROUPED) == 0,
	      "no-hub build");
	check(row_header(&al, 0, 0, 1), "no HUB header when disabled");
	app_list_free(&al);

	/* HUB is a grouped-mode pin; the flat picker never shows it. */
	app_list_init(&al);
	check(app_list_build(&al, src, 1, "", -1, APP_LIST_HUB) == 0,
	      "flat hub build");
	check(al.nrow == 1 && row_app(&al, 0, "Game"), "flat list ignores HUB");
	app_list_free(&al);
}

static void test_filter(void)
{
	struct app_list al;
	struct app_list_src src[3];

	src[0] = S("Alpha", "space shooter", 0, 0); /* GAMES */
	src[1] = S("Beats", "sound demo", 3, 1);    /* AUDIO */
	src[2] = S("Notes", 0, 4, 2);               /* PRODUCTIVITY */

	app_list_init(&al);
	check(app_list_build(&al, src, 3, "sound", -1, APP_LIST_GROUPED) == 0,
	      "summary filter build");
	check(al.nrow == 2, "summary filter rows");
	check(row_header(&al, 0, 3, 1), "only AUDIO header survives filter");
	check(row_app(&al, 1, "Beats"), "summary match keeps Beats");
	check(al.napp == 1, "one visible app");
	app_list_free(&al);

	app_list_init(&al);
	check(app_list_build(&al, src, 3, "alpha", -1, APP_LIST_GROUPED) == 0,
	      "label filter build");
	check(al.nrow == 2 && row_header(&al, 0, 0, 1) &&
		      row_app(&al, 1, "Alpha"),
	      "label filter hides other headers");
	app_list_free(&al);

	app_list_init(&al);
	check(app_list_build(&al, src, 3, "zzz", -1, APP_LIST_GROUPED) == 0,
	      "empty filter build");
	check(al.nrow == 0 && al.napp == 0, "no matches -> no rows");
	check(app_list_first(&al) == -1 && app_list_clamp(&al, 5) == -1,
	      "empty list navigation");
	app_list_free(&al);
}

static void test_navigation(void)
{
	struct app_list al;
	struct app_list_src src[4];

	src[0] = S("A1", 0, 0, 0);
	src[1] = S("A2", 0, 0, 1);
	src[2] = S("B1", 0, 3, 2);
	src[3] = S("C1", 0, 9, 3);

	app_list_init(&al);
	check(app_list_build(&al, src, 4, "", -1, APP_LIST_GROUPED) == 0,
	      "nav build");
	check(app_list_first(&al) == 1, "first app skips header");
	check(app_list_last(&al) == 6, "last app");
	check(app_list_next(&al, 1, 1) == 2, "down within group");
	check(app_list_next(&al, 2, 1) == 4, "down skips next header");
	check(app_list_next(&al, 4, 1) == 6, "down skips header again");
	check(app_list_next(&al, 6, 1) == -1, "down at end");
	check(app_list_next(&al, 4, -1) == 2, "up skips header");
	check(app_list_next(&al, 1, -1) == -1, "up at start");
	check(app_list_skip(&al, 1, 1, 2) == 4, "page of 2 apps");
	check(app_list_skip(&al, 1, 1, 99) == 6, "page clamps at last");
	check(app_list_jump_category(&al, 1, 1) == 4,
	      "right jumps to first AUDIO app");
	check(app_list_jump_category(&al, 4, 1) == 6,
	      "right jumps to first OTHER app");
	check(app_list_jump_category(&al, 6, 1) == -1, "right at last group");
	check(app_list_jump_category(&al, 4, -1) == 1,
	      "left jumps to first GAMES app");
	check(app_list_jump_category(&al, 1, -1) == -1, "left at first group");
	check(app_list_clamp(&al, 3) == 2, "clamp header to previous app");
	app_list_free(&al);

	/* Jump includes the pinned HUB group when present. */
	src[0] = S("A1", 0, 0, 0);
	app_list_init(&al);
	check(app_list_build(&al, src, 1, "", -1,
			     APP_LIST_GROUPED | APP_LIST_HUB) == 0,
	      "hub nav build");
	check(app_list_jump_category(&al, 3, -1) == 1, "left reaches HUB");
	check(app_list_jump_category(&al, 1, 1) == 3, "HUB -> GAMES");
	app_list_free(&al);
}

static void test_flat_picker(void)
{
	struct app_list al;
	struct app_list_src src[5];

	src[0] = S("Zeta", 0, 0, 0);
	src[1] = S("alpha", 0, 0, 1);
	src[2] = S("Mid", "middle", 3, 2);
	src[3] = S("bogus", 0, MMB_APP_CATEGORY_OTHER, 3);
	src[4] = S("Editor", 0, APP_LIST_CAT_BUILTIN, 4);

	app_list_init(&al);
	check(app_list_build(&al, src, 5, "", -1, 0) == 0, "flat build");
	check(al.nrow == 5 && al.napp == 5, "flat has no headers");
	check(row_app(&al, 0, "alpha") && row_app(&al, 1, "bogus") &&
		      row_app(&al, 2, "Editor") && row_app(&al, 3, "Mid") &&
		      row_app(&al, 4, "Zeta"),
	      "flat list sorted by label (builtins included)");
	app_list_free(&al);

	app_list_init(&al);
	check(app_list_build(&al, src, 5, "", 0, 0) == 0, "flat GAMES");
	check(al.nrow == 2 && row_app(&al, 0, "alpha") &&
		      row_app(&al, 1, "Zeta"),
	      "flat category filter");
	app_list_free(&al);

	app_list_init(&al);
	check(app_list_build(&al, src, 5, "", MMB_APP_CATEGORY_OTHER, 0) == 0,
	      "flat OTHER");
	check(al.nrow == 1 && row_app(&al, 0, "bogus"),
	      "builtins are outside the OTHER filter");
	app_list_free(&al);

	/* #1546: the picker can filter on the BUILT-IN group too. */
	app_list_init(&al);
	check(app_list_build(&al, src, 5, "", APP_LIST_CAT_BUILTIN, 0) == 0,
	      "flat BUILT-IN");
	check(al.nrow == 1 && row_app(&al, 0, "Editor"),
	      "flat BUILT-IN filter keeps only builtins");
	app_list_free(&al);

	/* Category filter combines with the type filter. */
	app_list_init(&al);
	check(app_list_build(&al, src, 5, "z", 0, 0) == 0, "flat combined");
	check(al.nrow == 1 && row_app(&al, 0, "Zeta"), "type+category filter");
	app_list_free(&al);
}

static void test_builtin_tail(void)
{
	struct app_list al;
	struct app_list_src src[3];

	src[0] = S("Editor", 0, APP_LIST_CAT_BUILTIN, 0);
	src[1] = S("Alpha", 0, APP_LIST_CAT_BUILTIN, 1);
	src[2] = S("Game", 0, 0, 2);

	app_list_init(&al);
	check(app_list_build(&al, src, 3, "", -1, APP_LIST_GROUPED) == 0,
	      "builtin build");
	check(al.nrow == 5, "builtin rows");
	check(row_header(&al, 2, APP_LIST_CAT_BUILTIN, 2),
	      "BUILT-IN header last");
	check(row_app(&al, 3, "Editor") && row_app(&al, 4, "Alpha"),
	      "built-in order is the source order, not sorted");
	app_list_free(&al);
}

static void test_rescan_clamp(void)
{
	struct app_list al;
	struct app_list_src src[3];

	src[0] = S("A", 0, 0, 0);
	src[1] = S("B", 0, 0, 1);
	src[2] = S("C", 0, 0, 2);

	app_list_init(&al);
	check(app_list_build(&al, src, 3, "", -1, APP_LIST_GROUPED) == 0,
	      "clamp build");
	check(al.nrow == 4 && row_app(&al, 2, "B"), "B is row 2");
	/* B removed: keep the selection at the same place (C slides up). */
	check(app_list_build(&al, src, 3, "", -1, APP_LIST_GROUPED) == 0,
	      "rebuild");
	src[1] = src[2];
	check(app_list_build(&al, src, 2, "", -1, APP_LIST_GROUPED) == 0,
	      "rebuild without B");
	check(app_list_clamp(&al, 2) == 2 && row_app(&al, 2, "C"),
	      "clamp lands on next app after removal");
	check(app_list_clamp(&al, 99) == 2, "clamp past end -> last app");
	app_list_free(&al);
}

int main(void)
{
	G.plat = &s_plat;
	test_grouping();
	test_hub();
	test_filter();
	test_navigation();
	test_flat_picker();
	test_builtin_tail();
	test_rescan_clamp();
	if (fails)
	{
		printf("%d check(s) failed\n", fails);
		return 1;
	}
	printf("all checks passed\n");
	return 0;
}
