/*
 * PAINT - rectangular selection and clipboard (#644).
 *
 * Strong definitions replace the weak stubs at the bottom of cmd_paint.c.
 * The SELECT tool drags out a rectangle; the boundary is drawn over the
 * composed frame as animated "marching ants" (screen pixels only -- the
 * selection is never baked into PT.canvas, so a saved PCX or a screenshot
 * taken through the canvas API never carries it).
 *
 * Pressing inside an existing selection lifts it and drags it (the source is
 * cleared to the background index while it floats). Cut / Copy / Paste and
 * Clear-in-selection use a pixel clipboard; paste skips any clipboard pixel
 * equal to the stored background index so it is transparent. Every canvas
 * edit (move, cut, paste, clear) records exactly one pt_undo_push().
 */
#include "paint.h"

/* ---- module state ------------------------------------------------------ */

static int s_active;			/* a selection rectangle exists */
static int s_x, s_y, s_w, s_h;		/* normalized, canvas coords */

/* The selection being dragged out right now. */
enum { SEL_NONE = 0, SEL_NEW, SEL_MOVE };
static int s_drag;
static int s_ax, s_ay;			/* press anchor, canvas coords */

/* Float: the lifted pixels of a move, plus the pre-move base. */
static unsigned char *s_float;
static int s_fw, s_fh, s_fbg;
static int s_have_base;			/* PT.scratch holds the post-lift canvas */
static int s_pdx, s_pdy;		/* previous move offset, for damage */

/* Clipboard. */
static unsigned char *s_clip;
static int s_clip_w, s_clip_h, s_clip_bg;
static int s_clip_valid;

/* Marching-ants animation phase. */
static int s_phase;
static unsigned s_phase_at;

#define PT_ANTS_MS 220

/* ---- small helpers ----------------------------------------------------- */

static int iabs(int v)
{
	return v < 0 ? -v : v;
}

static int clamp(int v, int lo, int hi)
{
	if (v < lo)
		return lo;
	if (v > hi)
		return hi;
	return v;
}

static void sel_free(unsigned char **p)
{
	if (*p)
	{
		if (G.plat && G.plat->free)
			G.plat->free(*p);
		*p = 0;
	}
}

static unsigned char *sel_alloc(unsigned n)
{
	if (!G.plat || !G.plat->alloc || n == 0)
		return 0;
	return (unsigned char *)G.plat->alloc(n);
}

static void sel_damage(void)
{
	if (!s_active)
		return;
	pt_damage(PT_CANVAS_X + s_x, PT_CANVAS_Y + s_y, s_w, s_h);
}

static void fill_rect_canvas(int x, int y, int w, int h, int idx)
{
	int i, j;

	for (j = 0; j < h; j++)
		for (i = 0; i < w; i++)
			pt_canvas_set(x + i, y + j, idx);
}

/* ---- lifecycle --------------------------------------------------------- */

void pt_select_init(void)
{
	sel_free(&s_clip);
	sel_free(&s_float);
	s_active = 0;
	s_drag = SEL_NONE;
	s_x = s_y = s_w = s_h = 0;
	s_fw = s_fh = s_fbg = 0;
	s_have_base = 0;
	s_pdx = s_pdy = 0;
	s_clip_w = s_clip_h = s_clip_bg = 0;
	s_clip_valid = 0;
	s_phase = 0;
	s_phase_at = 0;
}

/* ---- selection geometry ------------------------------------------------ */

int pt_select_has(void)
{
	return s_active ? 1 : 0;
}

int pt_select_hit(int cx, int cy)
{
	if (!s_active)
		return 0;
	return cx >= s_x && cy >= s_y && cx < s_x + s_w && cy < s_y + s_h;
}

int pt_select_rect(int *x, int *y, int *w, int *h)
{
	if (!s_active)
		return 0;
	if (x)
		*x = s_x;
	if (y)
		*y = s_y;
	if (w)
		*w = s_w;
	if (h)
		*h = s_h;
	return 1;
}

void pt_select_all(void)
{
	if (!PT.canvas)
		return;
	sel_damage();
	s_x = 0;
	s_y = 0;
	s_w = PT.width;
	s_h = PT.height;
	s_active = 1;
	sel_damage();
	pt_request_redraw();
}

void pt_select_none(void)
{
	if (s_drag == SEL_MOVE)
		pt_select_cancel();
	if (!s_active)
		return;
	sel_damage();
	s_active = 0;
	s_w = s_h = 0;
	pt_request_redraw();
}

/* ---- clipboard --------------------------------------------------------- */

int pt_select_clip_has(void)
{
	return s_clip_valid ? 1 : 0;
}

int pt_select_clip_size(int *w, int *h)
{
	if (!s_clip_valid)
		return 0;
	if (w)
		*w = s_clip_w;
	if (h)
		*h = s_clip_h;
	return 1;
}

void pt_select_copy(void)
{
	int i, j;

	if (!s_active || s_w <= 0 || s_h <= 0 || !PT.canvas)
		return;
	sel_free(&s_clip);
	s_clip = sel_alloc((unsigned)s_w * (unsigned)s_h);
	if (!s_clip)
	{
		s_clip_valid = 0;
		strncpy(PT.status, "?Out of memory", sizeof(PT.status) - 1);
		return;
	}
	for (j = 0; j < s_h; j++)
		for (i = 0; i < s_w; i++)
			s_clip[(size_t)j * s_w + i] =
				(unsigned char)pt_canvas_get(s_x + i, s_y + j);
	s_clip_w = s_w;
	s_clip_h = s_h;
	s_clip_bg = PT.bg;		/* paste skips this index */
	s_clip_valid = 1;
	strncpy(PT.status, "Copied", sizeof(PT.status) - 1);
}

void pt_select_cut(void)
{
	if (!s_active)
		return;
	pt_select_copy();
	if (!s_clip_valid)
		return;
	pt_undo_push();
	fill_rect_canvas(s_x, s_y, s_w, s_h, PT.bg);
	sel_damage();
	pt_request_redraw();
	strncpy(PT.status, "Cut", sizeof(PT.status) - 1);
}

void pt_select_clear(void)
{
	if (!s_active)
		return;
	pt_undo_push();
	fill_rect_canvas(s_x, s_y, s_w, s_h, PT.bg);
	sel_damage();
	pt_request_redraw();
	strncpy(PT.status, "Cleared selection", sizeof(PT.status) - 1);
}

void pt_select_paste(void)
{
	int dx, dy, i, j, px;

	if (!s_clip_valid || !PT.canvas)
	{
		strncpy(PT.status, "Clipboard empty", sizeof(PT.status) - 1);
		return;
	}
	pt_undo_push();

	/* Top-left at the pointer, clamped so the whole clipboard stays on the
	 * canvas when it fits. */
	dx = clamp(PT.cursor_x, 0, PT.width - 1);
	dy = clamp(PT.cursor_y, 0, PT.height - 1);
	if (s_clip_w < PT.width)
		dx = clamp(dx, 0, PT.width - s_clip_w);
	if (s_clip_h < PT.height)
		dy = clamp(dy, 0, PT.height - s_clip_h);

	for (j = 0; j < s_clip_h; j++)
		for (i = 0; i < s_clip_w; i++)
		{
			px = s_clip[(size_t)j * s_clip_w + i];
			if (px == s_clip_bg)
				continue;	/* background is transparent */
			pt_canvas_set(dx + i, dy + j, px);
		}

	sel_damage();
	s_x = dx;
	s_y = dy;
	s_w = s_clip_w;
	s_h = s_clip_h;
	s_active = 1;
	sel_damage();
	pt_request_redraw();
	strncpy(PT.status, "Pasted", sizeof(PT.status) - 1);
}

/* ---- float (move) ------------------------------------------------------ */

static void float_free(void)
{
	sel_free(&s_float);
	s_fw = s_fh = 0;
}

static void float_stamp(int dx, int dy)
{
	int i, j;

	if (!s_float)
		return;
	for (j = 0; j < s_fh; j++)
		for (i = 0; i < s_fw; i++)
		{
			int v = s_float[(size_t)j * s_fw + i];

			if (v == s_fbg)
				continue;
			pt_canvas_set(s_x + dx + i, s_y + dy + j, v);
		}
}

static void float_restore_base(void)
{
	if (s_have_base && PT.scratch && PT.canvas)
		memcpy(PT.canvas, PT.scratch, (size_t)PT.width * PT.height);
}

static void move_damage(int dx, int dy)
{
	/* Old and new float positions both changed. */
	pt_damage_canvas(s_x + s_pdx - 1, s_y + s_pdy - 1, s_fw + 2, s_fh + 2);
	pt_damage_canvas(s_x + dx - 1, s_y + dy - 1, s_fw + 2, s_fh + 2);
}

/* ---- drag -------------------------------------------------------------- */

void pt_select_begin(int cx, int cy, int button)
{
	(void)button;
	if (!PT.canvas)
		return;

	if (s_active && pt_select_hit(cx, cy))
	{
		/* Lift the selection; the source is cleared while it floats. */
		pt_undo_push();
		float_free();
		s_float = sel_alloc((unsigned)s_w * (unsigned)s_h);
		if (!s_float)
			return;
		{
			int i, j;

			for (j = 0; j < s_h; j++)
				for (i = 0; i < s_w; i++)
					s_float[(size_t)j * s_w + i] =
						(unsigned char)pt_canvas_get(
							s_x + i, s_y + j);
		}
		s_fw = s_w;
		s_fh = s_h;
		s_fbg = PT.bg;
		fill_rect_canvas(s_x, s_y, s_w, s_h, PT.bg);
		if (PT.scratch)
		{
			memcpy(PT.scratch, PT.canvas, (size_t)PT.width * PT.height);
			s_have_base = 1;
		}
		s_drag = SEL_MOVE;
		s_ax = cx;
		s_ay = cy;
		s_pdx = s_pdy = 0;
		float_stamp(0, 0);
		pt_request_redraw();
		return;
	}

	/* A fresh rectangle. A click that never becomes a drag deselects. */
	if (s_active)
	{
		sel_damage();
		s_active = 0;
	}
	s_drag = SEL_NEW;
	s_ax = cx;
	s_ay = cy;
	s_x = cx;
	s_y = cy;
	s_w = 1;
	s_h = 1;
	s_active = 1;
}

void pt_select_motion(int cx, int cy)
{
	int dx, dy;

	if (s_drag == SEL_NEW)
	{
		int x0 = s_ax < cx ? s_ax : cx;
		int y0 = s_ay < cy ? s_ay : cy;
		int x1 = s_ax >= cx ? s_ax : cx;
		int y1 = s_ay >= cy ? s_ay : cy;

		sel_damage();
		s_x = clamp(x0, 0, PT.width - 1);
		s_y = clamp(y0, 0, PT.height - 1);
		s_w = clamp(x1, 0, PT.width - 1) - s_x + 1;
		s_h = clamp(y1, 0, PT.height - 1) - s_y + 1;
		sel_damage();
		pt_request_redraw();
		return;
	}
	if (s_drag != SEL_MOVE)
		return;

	dx = cx - s_ax;
	dy = cy - s_ay;
	/* Keep the floating block inside the canvas. */
	dx = clamp(dx, -s_x, PT.width - (s_x + s_w));
	dy = clamp(dy, -s_y, PT.height - (s_y + s_h));
	float_restore_base();
	move_damage(dx, dy);
	float_stamp(dx, dy);
	s_pdx = dx;
	s_pdy = dy;
	pt_request_redraw();
}

void pt_select_end(int cx, int cy)
{
	if (s_drag == SEL_NEW)
	{
		int w = iabs(cx - s_ax) + 1;
		int h = iabs(cy - s_ay) + 1;

		s_drag = SEL_NONE;
		if (w < 2 && h < 2)
		{
			/* A click with no drag: deselect. */
			sel_damage();
			s_active = 0;
			s_w = s_h = 0;
		}
		pt_request_redraw();
		return;
	}
	if (s_drag == SEL_MOVE)
	{
		int dx = cx - s_ax;
		int dy = cy - s_ay;

		dx = clamp(dx, -s_x, PT.width - (s_x + s_w));
		dy = clamp(dy, -s_y, PT.height - (s_y + s_h));
		float_restore_base();
		move_damage(dx, dy);
		float_stamp(dx, dy);
		s_x += dx;
		s_y += dy;
		s_drag = SEL_NONE;
		s_have_base = 0;
		float_free();
		PT.scratch_valid = 0;
		sel_damage();
		pt_request_redraw();
		strncpy(PT.status, "Moved selection", sizeof(PT.status) - 1);
	}
}

void pt_select_cancel(void)
{
	if (s_drag == SEL_MOVE)
	{
		float_restore_base();
		s_have_base = 0;
		float_free();
		pt_request_redraw();
	}
	s_drag = SEL_NONE;
}

/* ---- marching ants ----------------------------------------------------- */

void pt_select_draw(void)
{
	int sx, sy, i;

	if (!s_active || s_w <= 0 || s_h <= 0)
		return;
	sx = PT_CANVAS_X + s_x + s_pdx * (s_drag == SEL_MOVE);
	sy = PT_CANVAS_Y + s_y + s_pdy * (s_drag == SEL_MOVE);

	for (i = 0; i < s_w; i++)
	{
		unsigned c = (((sx + i + sy) >> 2) ^ s_phase) & 1;

		pt_plot(sx + i, sy, c ? 0xFFFFFFu : 0x000000u);
		pt_plot(sx + i, sy + s_h - 1, c ? 0x000000u : 0xFFFFFFu);
	}
	for (i = 0; i < s_h; i++)
	{
		unsigned c = (((sx + sy + i) >> 2) ^ s_phase) & 1;

		pt_plot(sx, sy + i, c ? 0xFFFFFFu : 0x000000u);
		pt_plot(sx + s_w - 1, sy + i, c ? 0x000000u : 0xFFFFFFu);
	}
}

int pt_select_tick(void)
{
	unsigned now = mmb_now_ms();

	if (!s_active)
		return 0;
	if (now - s_phase_at < PT_ANTS_MS)
		return 0;
	s_phase_at = now;
	s_phase ^= 1;
	return 1;
}
