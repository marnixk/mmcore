#ifndef MMB_HUB_LAYOUT_H
#define MMB_HUB_LAYOUT_H

/*
 * Pure layout maths for the HUB screens (#1526).
 *
 * The framebuffer is divided into integer `p`-unit regions so the pixel-art
 * style stays chunky at every resolution. Nothing here touches the console or
 * the hub engine: the host test (tests/hub_layout_host.c) drives this module
 * on its own at every supported resolution.
 */

typedef struct hub_rect {
	int x, y, w, h;
} hub_rect;

enum {
	HUB_LAYOUT_WIDE = 0,
	HUB_LAYOUT_NARROW = 1
};

typedef struct hub_layout {
	int w, h;
	int p;      /* integer pixel unit: max(1, min(W/640, H/360)) */
	int mode;   /* HUB_LAYOUT_WIDE or HUB_LAYOUT_NARROW */
	int margin;
	int gap;
	int row_h;  /* listing row height in pixels (44p) */

	/* Listing screen (also used for the loading screen's content box). */
	hub_rect header;
	hub_rect tabs;
	hub_rect content;
	hub_rect list;
	hub_rect preview;

	/* Tab control: `tabs` holds every row; tabs_per_row/rows describe it. */
	int tab_rows;
	int tab_h;
	int tab_gap;
	int tab_w;
	int tabs_per_row;

	/* Detail screen (full-screen panel). */
	hub_rect detail;
	hub_rect detail_shot;    /* big framed image */
	hub_rect detail_thumbs;  /* shot picker row / dots */
	hub_rect detail_title;   /* title + icon row */
	hub_rect detail_chips;   /* tag chips row */
	hub_rect detail_button;  /* one main button */
	hub_rect detail_about;   /* scrollable about text */
} hub_layout;

int hub_layout_mode(int w, int h);
void hub_layout_compute(hub_layout *L, int w, int h, int n_tabs, int tab_w);
int hub_rect_contains(const hub_rect *r, int x, int y);

#endif
