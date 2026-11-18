#include "hub_layout.h"

/* Wide when the screen is at least 1.55:1 (16:9, 16:10); everything squarer
 * uses the narrow stacking layout. */
int hub_layout_mode(int w, int h)
{
	if (w <= 0 || h <= 0)
		return HUB_LAYOUT_NARROW;
	return (long)w * 100 >= (long)h * 155 ? HUB_LAYOUT_WIDE
					      : HUB_LAYOUT_NARROW;
}

static void rect_set(hub_rect *r, int x, int y, int w, int h)
{
	r->x = x;
	r->y = y;
	r->w = w < 0 ? 0 : w;
	r->h = h < 0 ? 0 : h;
}

int hub_rect_contains(const hub_rect *r, int x, int y)
{
	return r && x >= r->x && y >= r->y && x < r->x + r->w &&
	       y < r->y + r->h;
}

void hub_layout_compute(hub_layout *L, int w, int h, int n_tabs, int tab_w)
{
	int p, m, gap, tab_y, content_y, content_h;
	int per_row, rows = 1;

	if (!L)
		return;
	if (w < 16)
		w = 16;
	if (h < 16)
		h = 16;
	p = w / 640 < h / 360 ? w / 640 : h / 360;
	if (p < 1)
		p = 1;
	L->w = w;
	L->h = h;
	L->p = p;
	L->mode = hub_layout_mode(w, h);
	L->margin = m = 4 * p;
	L->gap = gap = 4 * p;
	L->row_h = 44 * p;

	/* Full-width wordmark bar. The cube and the logo sit inside it. */
	rect_set(&L->header, 0, 0, w, 34 * p);

	/* Tabs are always one row. When the labels do not fit, the UI scrolls
	 * the strip; it never wraps. `tab_w` is the caller's natural label width
	 * and is kept for that scroller. */
	if (tab_w < 16 * p)
		tab_w = 16 * p;
	L->tab_w = tab_w;
	/* Tall enough for the mockup tab art at 1:1 (about 46px at 2x). */
	L->tab_h = 24 * p;
	L->tab_gap = 2 * p;
	per_row = n_tabs > 0 ? n_tabs : 1;
	rows = 1;
	L->tabs_per_row = per_row;
	L->tab_rows = rows;
	tab_y = L->header.y + L->header.h + gap;
	rect_set(&L->tabs, m, tab_y, w - 2 * m, L->tab_h);

	content_y = L->tabs.y + L->tabs.h + gap;
	content_h = h - m - content_y;
	if (content_h < 40 * p)
		content_h = 40 * p;
	rect_set(&L->content, m, content_y, w - 2 * m, content_h);

	/* Listing: wide splits into list + preview; narrow gives the list the
	 * whole panel (the selected row expands with an inline preview). */
	if (L->mode == HUB_LAYOUT_WIDE)
	{
		int lw = (L->content.w - gap) * 60 / 100;

		if (lw < 120 * p)
			lw = 120 * p;
		if (lw > L->content.w - gap - 120 * p)
			lw = L->content.w - gap - 120 * p;
		if (lw < 1)
			lw = L->content.w;
		rect_set(&L->list, L->content.x, L->content.y, lw,
			 L->content.h);
		rect_set(&L->preview, L->content.x + lw + gap,
			 L->content.y, L->content.w - lw - gap,
			 L->content.h);
	}
	else
	{
		L->list = L->content;
		rect_set(&L->preview, L->content.x, L->content.y, 0, 0);
	}

	/* Detail screen: the whole screen is one panel, no header/tabs.
	 * Title, thumbnail and button slots are generous on a large screen and
	 * shrink so a tiny framebuffer still has a non-empty about region. */
	rect_set(&L->detail, m, m, w - 2 * m, h - 2 * m);
	if (L->mode == HUB_LAYOUT_WIDE)
	{
		int dw = (L->detail.w - gap) * 55 / 100;
		int dh = L->detail.h;
		int thumbs_h = 34 * p;
		int title_h = 108 * p;
		int chips_h = 28 * p;
		int button_h = 48 * p;
		int shot_h;
		int rx;
		int need;

		if (dw < 100 * p)
			dw = 100 * p;
		if (dw > L->detail.w - gap - 100 * p)
			dw = L->detail.w - gap - 100 * p;
		need = title_h + chips_h + button_h + 3 * gap + 16 * p;
		if (need > dh)
		{
			title_h = 28 * p;
			chips_h = 16 * p;
			button_h = 22 * p;
			thumbs_h = 14 * p;
		}
		shot_h = dw * 9 / 16 + 8 * p;
		if (shot_h < dh - thumbs_h - 2 * gap)
			shot_h = dh - thumbs_h - 2 * gap;
		if (shot_h > dh - thumbs_h - gap)
			shot_h = dh - thumbs_h - gap;
		if (shot_h < 16 * p)
			shot_h = 16 * p;
		if (shot_h > dh - thumbs_h - gap)
			shot_h = dh - thumbs_h - gap;
		if (shot_h < 1)
			shot_h = 1;
		{
			int group_h = shot_h + gap + thumbs_h;
			int top = L->detail.y + (dh - group_h) / 2;

			if (top < L->detail.y)
				top = L->detail.y;
			if (top + group_h > L->detail.y + dh)
				top = L->detail.y;
			rect_set(&L->detail_shot, L->detail.x, top, dw, shot_h);
			rect_set(&L->detail_thumbs, L->detail.x,
				 top + shot_h + gap, dw, thumbs_h);
		}
		rx = L->detail.x + dw + gap;
		rect_set(&L->detail_title, rx, L->detail.y,
			 L->detail.w - dw - gap, title_h);
		rect_set(&L->detail_chips, rx,
			 L->detail_title.y + title_h,
			 L->detail.w - dw - gap, chips_h);
		rect_set(&L->detail_button, rx,
			 L->detail.y + L->detail.h - button_h - 12 * p,
			 L->detail.w - dw - gap, button_h);
		rect_set(&L->detail_about, rx,
			 L->detail_chips.y + L->detail_chips.h + gap,
			 L->detail.w - dw - gap,
			 L->detail_button.y -
				 (L->detail_chips.y + L->detail_chips.h + gap));
		if (L->detail_about.h < 1)
			L->detail_about.h = 1;
	}
	else
	{
		int dh = L->detail.h;
		int shot_h = L->detail.w * 9 / 16 + 6 * p;
		int thumbs_h = 24 * p;
		int title_h = 72 * p;
		int chips_h = 26 * p;
		int button_h = 44 * p;
		int gaps = 6 * p;
		int over;

		if (shot_h > dh * 42 / 100)
			shot_h = dh * 42 / 100;
		over = shot_h + thumbs_h + title_h + chips_h + button_h +
		       gaps + 12 * p - dh;
		if (over > 0)
		{
			int cut = shot_h - 18 * p;

			if (cut < 0)
				cut = 0;
			if (cut > over)
				cut = over;
			shot_h -= cut;
			over -= cut;
			cut = title_h - 18 * p;
			if (cut < 0)
				cut = 0;
			if (cut > over)
				cut = over;
			title_h -= cut;
			over -= cut;
			cut = thumbs_h - 8 * p;
			if (cut < 0)
				cut = 0;
			if (cut > over)
				cut = over;
			thumbs_h -= cut;
			over -= cut;
			cut = button_h - 16 * p;
			if (cut < 0)
				cut = 0;
			if (cut > over)
				cut = over;
			button_h -= cut;
		}
		if (shot_h < 16 * p)
			shot_h = 16 * p;
		rect_set(&L->detail_shot, L->detail.x, L->detail.y,
			 L->detail.w, shot_h);
		rect_set(&L->detail_thumbs, L->detail.x,
			 L->detail.y + shot_h + p, L->detail.w, thumbs_h);
		rect_set(&L->detail_title, L->detail.x,
			 L->detail_thumbs.y + L->detail_thumbs.h + p,
			 L->detail.w, title_h);
		rect_set(&L->detail_chips, L->detail.x,
			 L->detail_title.y + L->detail_title.h,
			 L->detail.w, chips_h);
		rect_set(&L->detail_button, L->detail.x,
			 L->detail_chips.y + L->detail_chips.h + gap,
			 L->detail.w, button_h);
		rect_set(&L->detail_about, L->detail.x,
			 L->detail_button.y + L->detail_button.h + gap,
			 L->detail.w,
			 L->detail.y + L->detail.h -
				 (L->detail_button.y + L->detail_button.h +
				  gap));
		if (L->detail_about.h < 1)
			L->detail_about.h = 1;
	}
}
