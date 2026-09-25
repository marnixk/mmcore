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

enum { SEL_NONE = 0, SEL_NEW, SEL_MOVE };

/* One selection state per virtual console: the rectangle, its drag, the float
 * and clipboard buffers and the ants phase must not cross consoles (#766). */
typedef struct {
	int active;			/* a selection rectangle exists */
	int x, y, w, h;			/* normalized, canvas coords */

	int drag;			/* SEL_NONE / SEL_NEW / SEL_MOVE */
	int ax, ay;			/* press anchor, canvas coords */

	/* Float: the lifted pixels of a move, plus the pre-move base. */
	unsigned char *flt;
	int fw, fh, fbg;
	int have_base;			/* PT.scratch holds the post-lift canvas */
	int pdx, pdy;			/* previous move offset, for damage */

	/* Clipboard. */
	unsigned char *clip;
	int clip_w, clip_h, clip_bg;
	int clip_valid;

	/* Marching-ants animation phase. */
	int phase;
	unsigned phase_at;
} pt_select_state;

static pt_select_state s_select_state[MMB_MAX_CONSOLES];
#define SELECT (s_select_state[g_console])

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
	if (!SELECT.active)
		return;
	pt_damage(PT_CANVAS_X + SELECT.x, PT_CANVAS_Y + SELECT.y, SELECT.w, SELECT.h);
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
	sel_free(&SELECT.clip);
	sel_free(&SELECT.flt);
	SELECT.active = 0;
	SELECT.drag = SEL_NONE;
	SELECT.x = SELECT.y = SELECT.w = SELECT.h = 0;
	SELECT.fw = SELECT.fh = SELECT.fbg = 0;
	SELECT.have_base = 0;
	SELECT.pdx = SELECT.pdy = 0;
	SELECT.clip_w = SELECT.clip_h = SELECT.clip_bg = 0;
	SELECT.clip_valid = 0;
	SELECT.phase = 0;
	SELECT.phase_at = 0;
}

/* ---- selection geometry ------------------------------------------------ */

int pt_select_has(void)
{
	return SELECT.active ? 1 : 0;
}

int pt_select_hit(int cx, int cy)
{
	if (!SELECT.active)
		return 0;
	return cx >= SELECT.x && cy >= SELECT.y && cx < SELECT.x + SELECT.w && cy < SELECT.y + SELECT.h;
}

int pt_select_rect(int *x, int *y, int *w, int *h)
{
	if (!SELECT.active)
		return 0;
	if (x)
		*x = SELECT.x;
	if (y)
		*y = SELECT.y;
	if (w)
		*w = SELECT.w;
	if (h)
		*h = SELECT.h;
	return 1;
}

void pt_select_all(void)
{
	if (!PT.canvas)
		return;
	sel_damage();
	SELECT.x = 0;
	SELECT.y = 0;
	SELECT.w = PT.width;
	SELECT.h = PT.height;
	SELECT.active = 1;
	sel_damage();
	pt_request_redraw();
}

void pt_select_none(void)
{
	if (SELECT.drag == SEL_MOVE)
		pt_select_cancel();
	if (!SELECT.active)
		return;
	sel_damage();
	SELECT.active = 0;
	SELECT.w = SELECT.h = 0;
	pt_request_redraw();
}

/* ---- clipboard --------------------------------------------------------- */

int pt_select_clip_has(void)
{
	return SELECT.clip_valid ? 1 : 0;
}

int pt_select_clip_size(int *w, int *h)
{
	if (!SELECT.clip_valid)
		return 0;
	if (w)
		*w = SELECT.clip_w;
	if (h)
		*h = SELECT.clip_h;
	return 1;
}

void pt_select_copy(void)
{
	int i, j;

	if (!SELECT.active || SELECT.w <= 0 || SELECT.h <= 0 || !PT.canvas)
		return;
	sel_free(&SELECT.clip);
	SELECT.clip = sel_alloc((unsigned)SELECT.w * (unsigned)SELECT.h);
	if (!SELECT.clip)
	{
		SELECT.clip_valid = 0;
		strncpy(PT.status, "?Out of memory", sizeof(PT.status) - 1);
		return;
	}
	for (j = 0; j < SELECT.h; j++)
		for (i = 0; i < SELECT.w; i++)
			SELECT.clip[(size_t)j * SELECT.w + i] =
				(unsigned char)pt_canvas_get(SELECT.x + i, SELECT.y + j);
	SELECT.clip_w = SELECT.w;
	SELECT.clip_h = SELECT.h;
	SELECT.clip_bg = PT.bg;		/* paste skips this index */
	SELECT.clip_valid = 1;
	strncpy(PT.status, "Copied", sizeof(PT.status) - 1);
}

void pt_select_cut(void)
{
	if (!SELECT.active)
		return;
	pt_select_copy();
	if (!SELECT.clip_valid)
		return;
	pt_undo_push();
	fill_rect_canvas(SELECT.x, SELECT.y, SELECT.w, SELECT.h, PT.bg);
	sel_damage();
	pt_request_redraw();
	strncpy(PT.status, "Cut", sizeof(PT.status) - 1);
}

void pt_select_clear(void)
{
	if (!SELECT.active)
		return;
	pt_undo_push();
	fill_rect_canvas(SELECT.x, SELECT.y, SELECT.w, SELECT.h, PT.bg);
	sel_damage();
	pt_request_redraw();
	strncpy(PT.status, "Cleared selection", sizeof(PT.status) - 1);
}

void pt_select_paste(void)
{
	int dx, dy, i, j, px;

	if (!SELECT.clip_valid || !PT.canvas)
	{
		strncpy(PT.status, "Clipboard empty", sizeof(PT.status) - 1);
		return;
	}
	pt_undo_push();

	/* Top-left at the pointer, clamped so the whole clipboard stays on the
	 * canvas when it fits. */
	dx = clamp(PT.cursor_x, 0, PT.width - 1);
	dy = clamp(PT.cursor_y, 0, PT.height - 1);
	if (SELECT.clip_w < PT.width)
		dx = clamp(dx, 0, PT.width - SELECT.clip_w);
	if (SELECT.clip_h < PT.height)
		dy = clamp(dy, 0, PT.height - SELECT.clip_h);

	for (j = 0; j < SELECT.clip_h; j++)
		for (i = 0; i < SELECT.clip_w; i++)
		{
			px = SELECT.clip[(size_t)j * SELECT.clip_w + i];
			if (px == SELECT.clip_bg)
				continue;	/* background is transparent */
			pt_canvas_set(dx + i, dy + j, px);
		}

	sel_damage();
	SELECT.x = dx;
	SELECT.y = dy;
	SELECT.w = SELECT.clip_w;
	SELECT.h = SELECT.clip_h;
	SELECT.active = 1;
	sel_damage();
	pt_request_redraw();
	strncpy(PT.status, "Pasted", sizeof(PT.status) - 1);
}

/* ---- float (move) ------------------------------------------------------ */

static void float_free(void)
{
	sel_free(&SELECT.flt);
	SELECT.fw = SELECT.fh = 0;
}

static void float_stamp(int dx, int dy)
{
	int i, j;

	if (!SELECT.flt)
		return;
	for (j = 0; j < SELECT.fh; j++)
		for (i = 0; i < SELECT.fw; i++)
		{
			int v = SELECT.flt[(size_t)j * SELECT.fw + i];

			if (v == SELECT.fbg)
				continue;
			pt_canvas_set(SELECT.x + dx + i, SELECT.y + dy + j, v);
		}
}

static void float_restore_base(void)
{
	if (SELECT.have_base && PT.scratch && PT.canvas)
		memcpy(PT.canvas, PT.scratch, (size_t)PT.width * PT.height);
}

static void move_damage(int dx, int dy)
{
	/* Old and new float positions both changed. */
	pt_damage_canvas(SELECT.x + SELECT.pdx - 1, SELECT.y + SELECT.pdy - 1, SELECT.fw + 2, SELECT.fh + 2);
	pt_damage_canvas(SELECT.x + dx - 1, SELECT.y + dy - 1, SELECT.fw + 2, SELECT.fh + 2);
}

/* ---- drag -------------------------------------------------------------- */

void pt_select_begin(int cx, int cy, int button)
{
	(void)button;
	if (!PT.canvas)
		return;

	if (SELECT.active && pt_select_hit(cx, cy))
	{
		/* Lift the selection; the source is cleared while it floats. */
		pt_undo_push();
		float_free();
		SELECT.flt = sel_alloc((unsigned)SELECT.w * (unsigned)SELECT.h);
		if (!SELECT.flt)
			return;
		{
			int i, j;

			for (j = 0; j < SELECT.h; j++)
				for (i = 0; i < SELECT.w; i++)
					SELECT.flt[(size_t)j * SELECT.w + i] =
						(unsigned char)pt_canvas_get(
							SELECT.x + i, SELECT.y + j);
		}
		SELECT.fw = SELECT.w;
		SELECT.fh = SELECT.h;
		SELECT.fbg = PT.bg;
		fill_rect_canvas(SELECT.x, SELECT.y, SELECT.w, SELECT.h, PT.bg);
		if (PT.scratch)
		{
			memcpy(PT.scratch, PT.canvas, (size_t)PT.width * PT.height);
			SELECT.have_base = 1;
		}
		SELECT.drag = SEL_MOVE;
		SELECT.ax = cx;
		SELECT.ay = cy;
		SELECT.pdx = SELECT.pdy = 0;
		float_stamp(0, 0);
		pt_request_redraw();
		return;
	}

	/* A fresh rectangle. A click that never becomes a drag deselects. */
	if (SELECT.active)
	{
		sel_damage();
		SELECT.active = 0;
	}
	SELECT.drag = SEL_NEW;
	SELECT.ax = cx;
	SELECT.ay = cy;
	SELECT.x = cx;
	SELECT.y = cy;
	SELECT.w = 1;
	SELECT.h = 1;
	SELECT.active = 1;
}

void pt_select_motion(int cx, int cy)
{
	int dx, dy;

	if (SELECT.drag == SEL_NEW)
	{
		int x0 = SELECT.ax < cx ? SELECT.ax : cx;
		int y0 = SELECT.ay < cy ? SELECT.ay : cy;
		int x1 = SELECT.ax >= cx ? SELECT.ax : cx;
		int y1 = SELECT.ay >= cy ? SELECT.ay : cy;

		sel_damage();
		SELECT.x = clamp(x0, 0, PT.width - 1);
		SELECT.y = clamp(y0, 0, PT.height - 1);
		SELECT.w = clamp(x1, 0, PT.width - 1) - SELECT.x + 1;
		SELECT.h = clamp(y1, 0, PT.height - 1) - SELECT.y + 1;
		sel_damage();
		pt_request_redraw();
		return;
	}
	if (SELECT.drag != SEL_MOVE)
		return;

	dx = cx - SELECT.ax;
	dy = cy - SELECT.ay;
	/* Keep the floating block inside the canvas. */
	dx = clamp(dx, -SELECT.x, PT.width - (SELECT.x + SELECT.w));
	dy = clamp(dy, -SELECT.y, PT.height - (SELECT.y + SELECT.h));
	float_restore_base();
	move_damage(dx, dy);
	float_stamp(dx, dy);
	SELECT.pdx = dx;
	SELECT.pdy = dy;
	pt_request_redraw();
}

void pt_select_end(int cx, int cy)
{
	if (SELECT.drag == SEL_NEW)
	{
		int w = iabs(cx - SELECT.ax) + 1;
		int h = iabs(cy - SELECT.ay) + 1;

		SELECT.drag = SEL_NONE;
		if (w < 2 && h < 2)
		{
			/* A click with no drag: deselect. */
			sel_damage();
			SELECT.active = 0;
			SELECT.w = SELECT.h = 0;
		}
		pt_request_redraw();
		return;
	}
	if (SELECT.drag == SEL_MOVE)
	{
		int dx = cx - SELECT.ax;
		int dy = cy - SELECT.ay;

		dx = clamp(dx, -SELECT.x, PT.width - (SELECT.x + SELECT.w));
		dy = clamp(dy, -SELECT.y, PT.height - (SELECT.y + SELECT.h));
		float_restore_base();
		move_damage(dx, dy);
		float_stamp(dx, dy);
		SELECT.x += dx;
		SELECT.y += dy;
		SELECT.drag = SEL_NONE;
		SELECT.have_base = 0;
		float_free();
		PT.scratch_valid = 0;
		sel_damage();
		pt_request_redraw();
		strncpy(PT.status, "Moved selection", sizeof(PT.status) - 1);
	}
}

void pt_select_cancel(void)
{
	if (SELECT.drag == SEL_MOVE)
	{
		float_restore_base();
		SELECT.have_base = 0;
		float_free();
		pt_request_redraw();
	}
	SELECT.drag = SEL_NONE;
}

/* ---- marching ants ----------------------------------------------------- */

void pt_select_draw(void)
{
	int sx, sy, i;

	if (!SELECT.active || SELECT.w <= 0 || SELECT.h <= 0)
		return;
	sx = PT_CANVAS_X + SELECT.x + SELECT.pdx * (SELECT.drag == SEL_MOVE);
	sy = PT_CANVAS_Y + SELECT.y + SELECT.pdy * (SELECT.drag == SEL_MOVE);

	for (i = 0; i < SELECT.w; i++)
	{
		unsigned c = (((sx + i + sy) >> 2) ^ SELECT.phase) & 1;

		pt_plot(sx + i, sy, c ? 0xFFFFFFu : 0x000000u);
		pt_plot(sx + i, sy + SELECT.h - 1, c ? 0x000000u : 0xFFFFFFu);
	}
	for (i = 0; i < SELECT.h; i++)
	{
		unsigned c = (((sx + sy + i) >> 2) ^ SELECT.phase) & 1;

		pt_plot(sx, sy + i, c ? 0xFFFFFFu : 0x000000u);
		pt_plot(sx + SELECT.w - 1, sy + i, c ? 0x000000u : 0xFFFFFFu);
	}
}

int pt_select_tick(void)
{
	unsigned now = mmb_now_ms();

	if (!SELECT.active)
		return 0;
	if (now - SELECT.phase_at < PT_ANTS_MS)
		return 0;
	SELECT.phase_at = now;
	SELECT.phase ^= 1;
	return 1;
}
