/* Host test for the HUB screen layout maths (#1526).
 *
 * Compiles mmbasic/src/hub_layout.c alone (it has no mmbasic dependency) and
 * checks the wide/narrow split, integer pixel unit and that every region is
 * disjoint at the supported resolutions.
 */
#include <stdio.h>
#include <stdlib.h>

#include "hub_layout.h"

static int fails;

static void check(int cond, const char *what)
{
	if (!cond)
	{
		printf("FAIL: %s\n", what);
		fails++;
	}
}

static int overlap(hub_rect a, hub_rect b)
{
	if (a.w <= 0 || a.h <= 0 || b.w <= 0 || b.h <= 0)
		return 0;
	return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h &&
	       b.y < a.y + a.h;
}

static long area(hub_rect r)
{
	return r.w > 0 && r.h > 0 ? (long)r.w * r.h : 0;
}

static void check_listing(hub_layout *L, int w, int h, int wide)
{
	check(L->w == w && L->h == h, "geometry stored");
	check(L->mode == (wide ? HUB_LAYOUT_WIDE : HUB_LAYOUT_NARROW),
	      "wide/narrow selection");
	check(L->p == (w / 640 < h / 360 ? w / 640 : h / 360),
	      "integer pixel unit");
	check(!overlap(L->header, L->tabs), "header/tabs disjoint");
	check(!overlap(L->header, L->content), "header/content disjoint");
	check(!overlap(L->tabs, L->content), "tabs/content disjoint");
	check(L->content.y + L->content.h == h - L->margin,
	      "content reaches the bottom margin");
	check(L->list.h == L->content.h, "list fills the content height");
	if (wide)
	{
		check(L->preview.w > 0 && L->preview.h > 0, "wide preview exists");
		check(!overlap(L->list, L->preview), "list/preview disjoint");
		check(area(L->list) * 100 >= area(L->content) * 58,
		      "list keeps ~60% of the content width");
		check(L->preview.w >= L->content.w * 35 / 100,
		      "preview keeps ~40% of the content width");
	}
	else
	{
		check(L->preview.w == 0 && L->preview.h == 0,
		      "narrow layout has no side preview");
		check(L->list.w == L->content.w, "narrow list spans the panel");
	}
	check(L->row_h == 56 * L->p, "row height is 56p");
	check(L->tab_rows >= 1, "tab rows");
}

static void check_detail(hub_layout *L)
{
	hub_rect *r[7];
	int i, j;

	r[0] = &L->detail_shot;
	r[1] = &L->detail_thumbs;
	r[2] = &L->detail_title;
	r[3] = &L->detail_chips;
	r[4] = &L->detail_button;
	r[5] = &L->detail_about;
	r[6] = &L->detail;
	for (i = 0; i < 6; i++)
	{
		check(r[i]->w > 0 && r[i]->h > 0, "detail region non-empty");
		check(r[i]->x >= L->detail.x && r[i]->y >= L->detail.y &&
		      r[i]->x + r[i]->w <= L->detail.x + L->detail.w &&
		      r[i]->y + r[i]->h <= L->detail.y + L->detail.h,
		      "detail region inside the panel");
	}
	for (i = 0; i < 6; i++)
		for (j = i + 1; j < 6; j++)
			check(!overlap(*r[i], *r[j]), "detail regions disjoint");
	check(L->detail_button.y + L->detail_button.h <=
	      L->detail.y + L->detail.h, "button inside the panel");
}

static void run(int w, int h, int wide)
{
	hub_layout L;

	hub_layout_compute(&L, w, h, 11, 100);
	check_listing(&L, w, h, wide);
	check_detail(&L);
	check(hub_layout_mode(w, h) == (wide ? HUB_LAYOUT_WIDE :
					 HUB_LAYOUT_NARROW),
	      "mode helper agrees");
}

int main(void)
{
	hub_layout L;

	run(1920, 1080, 1);
	run(1366, 768, 1);
	run(1280, 800, 1);
	run(1024, 768, 0);
	run(800, 600, 0);

	/* Threshold: exactly 1.55 is wide, just below is narrow. */
	check(hub_layout_mode(1550, 1000) == HUB_LAYOUT_WIDE, "1.55 is wide");
	check(hub_layout_mode(1549, 1000) == HUB_LAYOUT_NARROW,
	      "1.549 is narrow");

	/* No tab count / very narrow geometry must not produce negative rects. */
	hub_layout_compute(&L, 320, 240, 0, 0);
	check(L.list.w > 0 && L.list.h > 0, "tiny geometry still lays out");
	check(L.p == 1, "p clamps to 1");

	/* Tab wrapping: a narrow screen wraps the 11 tabs into several rows. */
	hub_layout_compute(&L, 800, 600, 11, 120);
	check(L.tab_rows >= 2, "narrow tabs wrap");
	check(L.tabs.y + L.tabs.h < L.content.y, "tabs stay above content");

	/* Wide tabs fit one row at 1920 with normal labels. */
	hub_layout_compute(&L, 1920, 1080, 11, 110);
	check(L.tab_rows == 1, "wide tabs fit one row");

	if (fails == 0)
		printf("all checks passed\n");
	return fails ? 1 : 0;
}
