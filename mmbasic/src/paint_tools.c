/*
 * PAINT - core drawing tools and canvas primitives (#636).
 *
 * Strong definitions for the tool hooks the scaffold (#634) declared in
 * paint.h. The module owns no chrome: it draws the tool column, reports hits
 * and mutates PT.canvas (and PT.scratch, the live-preview backup) only.
 *
 * Tools: pencil, line, rectangle, ellipse, circle, flood fill (exact match),
 * eraser, colour pick, grab / custom brush and magnify. Left button uses the
 * foreground index, right the background, on every tool.
 *
 * Shape tools (line / rect / ellipse / circle) back the canvas up into
 * PT.scratch on begin, repaint the shape on every motion and commit the final
 * shape on release; pt_tool_cancel() drops the preview.
 *
 * Shift constraints square / circle / 45-degree drags. The frozen event API
 * has no modifier channel, so cmd_paint.c cannot forward the key yet; the
 * module exposes pt_tool_modifiers() which the native tests (and a later
 * scaffold hook) drive.
 *
 * Bonus tools (#641): airbrush is a dwell-based soft brush (samples pile up
 * while the pointer stays put), spray scatters uniform dots over a wider disk,
 * and magnify steps the zoom factor with the view recentred on the click. The
 * text tool lands in #642 and registers here; it is inert for now.
 */
#include "paint.h"

/* Tool column: PT_TOOL_COUNT cells share the strip under the menu bar. The
 * width stays the frozen PT_TOOL_W; the height is split evenly so all tools
 * fit above the palette. */
#define PT_CELL_H ((PT_PAL_Y - PT_CANVAS_Y) / PT_TOOL_COUNT)

/* A grab has to move this far before it defines a new brush instead of
 * stamping the existing one. */
#define PT_DRAG_MIN 2

/* The text tool lives in paint_text.c (#642); a canvas click opens its caret. */
void pt_text_begin(int cx, int cy, int button);

/* ---- module state ------------------------------------------------------ */

static int s_shift;		/* Shift held (pt_tool_modifiers) */
static int s_moved;		/* grab: the press turned into a drag */

static unsigned char *s_brush;	/* custom brush, PT_MAX_W * PT_MAX_H */
static int s_brush_w, s_brush_h;
static int s_brush_bg;		/* index treated as transparent when stamped */

/* Ink for the shared line/ellipse rasterisers: canvas index or screen RGB. */
static int s_cink;
static unsigned s_sink;

/* ---- small helpers ----------------------------------------------------- */

static int iabs(int v)
{
	return v < 0 ? -v : v;
}

static int tool_pen(int button)
{
	return button == PT_BTN_RIGHT ? PT.bg : PT.fg;
}

static void cplot(int x, int y)
{
	pt_canvas_set(x, y, s_cink);
}

static void splot(int x, int y)
{
	pt_fill_rect(x, y, 1, 1, s_sink);
}

/* ---- primitives -------------------------------------------------------- */

static void line_plot(int x0, int y0, int x1, int y1, void (*pl)(int, int))
{
	int dx = iabs(x1 - x0);
	int sx = x0 < x1 ? 1 : -1;
	int dy = -iabs(y1 - y0);
	int sy = y0 < y1 ? 1 : -1;
	int err = dx + dy;

	for (;;)
	{
		int e2;

		pl(x0, y0);
		if (x0 == x1 && y0 == y1)
			break;
		e2 = 2 * err;
		if (e2 >= dy)
		{
			err += dy;
			x0 += sx;
		}
		if (e2 <= dx)
		{
			err += dx;
			y0 += sy;
		}
	}
}

static void canvas_line(int x0, int y0, int x1, int y1, int c)
{
	s_cink = c;
	line_plot(x0, y0, x1, y1, cplot);
}

/* Integer midpoint ellipse inscribed in the (x0,y0)-(x1,y1) box (Zingl). */
static void ellipse_plot(int x0, int y0, int x1, int y1, void (*pl)(int, int))
{
	long a = iabs(x1 - x0);
	long b = iabs(y1 - y0);
	long b1 = b & 1;
	long dx = 4 * (1 - a) * b * b;
	long dy = 4 * (b1 + 1) * a * a;
	long err = dx + dy + b1 * a * a;
	long e2;

	if (x0 > x1)
	{
		x0 = x1;
		x1 += (int)a;
	}
	if (y0 > y1)
		y0 = y1;
	y0 += (int)(b + 1) / 2;
	y1 = y0 - (int)b1;
	a *= 8 * a;
	b1 = 8 * b * b;

	do
	{
		pl(x1, y0);
		pl(x0, y0);
		pl(x0, y1);
		pl(x1, y1);
		e2 = 2 * err;
		if (e2 <= dy)
		{
			y0++;
			y1--;
			err += dy += a;
		}
		if (e2 >= dx || 2 * err > dy)
		{
			x0++;
			x1--;
			err += dx += b1;
		}
	} while (x0 <= x1);

	while (y0 - y1 < b)
	{
		pl(x0 - 1, y0);
		pl(x1 + 1, y0++);
		pl(x0 - 1, y1);
		pl(x1 + 1, y1--);
	}
}

static void canvas_ellipse_box(int x0, int y0, int x1, int y1, int c)
{
	s_cink = c;
	ellipse_plot(x0, y0, x1, y1, cplot);
}

static void canvas_circle(int cx, int cy, int r, int c)
{
	if (r < 0)
		r = -r;
	canvas_ellipse_box(cx - r, cy - r, cx + r, cy + r, c);
}

static void canvas_rect(int x0, int y0, int x1, int y1, int c)
{
	int i, t;

	if (x0 > x1)
	{
		t = x0;
		x0 = x1;
		x1 = t;
	}
	if (y0 > y1)
	{
		t = y0;
		y0 = y1;
		y1 = t;
	}
	for (i = x0; i <= x1; i++)
	{
		pt_canvas_set(i, y0, c);
		pt_canvas_set(i, y1, c);
	}
	for (i = y0; i <= y1; i++)
	{
		pt_canvas_set(x0, i, c);
		pt_canvas_set(x1, i, c);
	}
}

/* Exact-match scanline flood fill. The seed stack is allocated per fill; the
 * canvas never exceeds PT_MAX_W * PT_MAX_H so the bound is safe. */
static void canvas_flood(int sx, int sy, int c)
{
	int w = PT.width, h = PT.height;
	int target, *stack, sp = 0;
	unsigned cap;

	if (!PT.canvas || w <= 0 || h <= 0)
		return;
	if (sx < 0 || sy < 0 || sx >= w || sy >= h)
		return;
	target = pt_canvas_get(sx, sy);
	if (target == c)
		return;
	cap = (unsigned)w * (unsigned)h;
	if (cap == 0)
		return;
	if (!G.plat || !G.plat->alloc)
		return;
	stack = (int *)G.plat->alloc(cap * sizeof(int));
	if (!stack)
		return;

	stack[sp++] = sy * w + sx;
	while (sp > 0)
	{
		int idx = stack[--sp];
		int x, y, lx, rx, i;

		if (idx < 0 || idx >= (int)cap)
			continue;
		x = idx % w;
		y = idx / w;
		if (pt_canvas_get(x, y) != target)
			continue;

		lx = x;
		while (lx > 0 && pt_canvas_get(lx - 1, y) == target)
			lx--;
		rx = x;
		while (rx < w - 1 && pt_canvas_get(rx + 1, y) == target)
			rx++;
		for (i = lx; i <= rx; i++)
			pt_canvas_set(i, y, c);

		if (y > 0)
			for (i = lx; i <= rx; i++)
				if (pt_canvas_get(i, y - 1) == target &&
				    (i == lx || pt_canvas_get(i - 1, y - 1) != target))
				{
					if (sp < (int)cap)
						stack[sp++] = (y - 1) * w + i;
				}
		if (y < h - 1)
			for (i = lx; i <= rx; i++)
				if (pt_canvas_get(i, y + 1) == target &&
				    (i == lx || pt_canvas_get(i - 1, y + 1) != target))
				{
					if (sp < (int)cap)
						stack[sp++] = (y + 1) * w + i;
				}
	}
	if (G.plat->free)
		G.plat->free(stack);
}

/* ---- live preview ------------------------------------------------------ */

static void preview_begin(void)
{
	if (PT.scratch && PT.canvas && !PT.scratch_valid)
	{
		memcpy(PT.scratch, PT.canvas, (size_t)PT.width * PT.height);
		PT.scratch_valid = 1;
	}
}

static void preview_restore(void)
{
	if (PT.scratch && PT.canvas && PT.scratch_valid)
		memcpy(PT.canvas, PT.scratch, (size_t)PT.width * PT.height);
}

/* ---- Shift constraints ------------------------------------------------- */

static void constrain_line(int ax, int ay, int *px, int *py)
{
	int dx = *px - ax, dy = *py - ay;
	int adx = iabs(dx), ady = iabs(dy);

	if (adx >= 2 * ady)
		dy = 0;
	else if (ady >= 2 * adx)
		dx = 0;
	else
	{
		int m = (adx + ady) / 2;
		dx = dx < 0 ? -m : m;
		dy = dy < 0 ? -m : m;
	}
	*px = ax + dx;
	*py = ay + dy;
}

static void constrain_square(int ax, int ay, int *px, int *py)
{
	int dx = *px - ax, dy = *py - ay;
	int s = iabs(dx) > iabs(dy) ? iabs(dx) : iabs(dy);

	*px = ax + (dx < 0 ? -s : s);
	*py = ay + (dy < 0 ? -s : s);
}

/* ---- grab / custom brush ----------------------------------------------- */

static void ensure_brush(void)
{
	if (!s_brush && G.plat && G.plat->alloc)
		s_brush = (unsigned char *)
			G.plat->alloc((unsigned)PT_MAX_W * (unsigned)PT_MAX_H);
}

static void grab_capture(int x0, int y0, int x1, int y1)
{
	int i, j, t;

	if (x0 > x1)
	{
		t = x0;
		x0 = x1;
		x1 = t;
	}
	if (y0 > y1)
	{
		t = y0;
		y0 = y1;
		y1 = t;
	}
	if (x0 < 0)
		x0 = 0;
	if (y0 < 0)
		y0 = 0;
	if (x1 >= PT.width)
		x1 = PT.width - 1;
	if (y1 >= PT.height)
		y1 = PT.height - 1;
	if (x1 < x0 || y1 < y0)
		return;
	ensure_brush();
	if (!s_brush)
		return;
	s_brush_w = x1 - x0 + 1;
	s_brush_h = y1 - y0 + 1;
	s_brush_bg = PT.bg;
	for (j = 0; j < s_brush_h; j++)
		for (i = 0; i < s_brush_w; i++)
			s_brush[(size_t)j * s_brush_w + i] =
				(unsigned char)pt_canvas_get(x0 + i, y0 + j);
}

static void grab_stamp(int x, int y)
{
	int i, j;

	if (!s_brush || s_brush_w <= 0 || s_brush_h <= 0)
		return;
	for (j = 0; j < s_brush_h; j++)
		for (i = 0; i < s_brush_w; i++)
		{
			int v = s_brush[(size_t)j * s_brush_w + i];

			if (v == s_brush_bg)
				continue;	/* background is transparent */
			pt_canvas_set(x + i, y + j, v);
		}
}

/* ---- bonus tools: airbrush / spray / magnify (#641) -------------------- */

/* Airbrush lays down a handful of samples across a small disk, denser at the
 * centre, on every event, so dwelling darkens the spot. Spray scatters a few
 * uniform dots over a wider disk. Both draw from a tiny LCG seeded by
 * pt_tools_init(), which makes a stroke reproducible for the host tests. */
#define PT_AIR_R      6
#define PT_AIR_RATE   16
#define PT_SPRAY_R    8
#define PT_SPRAY_RATE 7

/* Magnify steps through powers of two; clicking again at the top returns to
 * 1:1. */
#define PT_ZOOM_MAX   8

static unsigned s_noise;

static unsigned noise_next(void)
{
	s_noise = s_noise * 1664525u + 1013904223u;
	return s_noise >> 8;
}

static void air_dab(int x, int y, int c)
{
	int i, r2 = PT_AIR_R * PT_AIR_R;

	pt_canvas_set(x, y, c);
	for (i = 0; i < PT_AIR_RATE; i++)
	{
		int dx = (int)(noise_next() % (2 * PT_AIR_R + 1)) - PT_AIR_R;
		int dy = (int)(noise_next() % (2 * PT_AIR_R + 1)) - PT_AIR_R;
		int d2 = dx * dx + dy * dy;

		if (d2 > r2)
			continue;
		/* Soft falloff: keep with probability 1 - d^2/r^2. */
		if ((int)(noise_next() % (unsigned)r2) >= r2 - d2)
			continue;
		pt_canvas_set(x + dx, y + dy, c);
	}
}

static void spray_dab(int x, int y, int c)
{
	int i, r2 = PT_SPRAY_R * PT_SPRAY_R;

	for (i = 0; i < PT_SPRAY_RATE; i++)
	{
		int dx = (int)(noise_next() % (2 * PT_SPRAY_R + 1)) - PT_SPRAY_R;
		int dy = (int)(noise_next() % (2 * PT_SPRAY_R + 1)) - PT_SPRAY_R;

		if (dx * dx + dy * dy > r2)
			continue;
		pt_canvas_set(x + dx, y + dy, c);
	}
}

typedef void (*pt_dab_fn)(int x, int y, int c);

/* Walk the segment so a fast drag stays connected; every stepped pixel gets a
 * dab, so dwelling in one place piles samples up. */
static void dab_line(pt_dab_fn dab, int x0, int y0, int x1, int y1, int c)
{
	int dx = iabs(x1 - x0);
	int sx = x0 < x1 ? 1 : -1;
	int dy = -iabs(y1 - y0);
	int sy = y0 < y1 ? 1 : -1;
	int err = dx + dy;

	for (;;)
	{
		int e2;

		dab(x0, y0, c);
		if (x0 == x1 && y0 == y1)
			break;
		e2 = 2 * err;
		if (e2 >= dy)
		{
			err += dy;
			x0 += sx;
		}
		if (e2 <= dx)
		{
			err += dx;
			y0 += sy;
		}
	}
}

/* Set the zoom factor and park the view window on the clicked canvas pixel,
 * clamped so the window never leaves the canvas. Level 1 restores 1:1. */
static void magnify_zoom(int level, int cx, int cy)
{
	int vw, vh;

	if (level < 1)
		level = 1;
	if (level > PT_ZOOM_MAX)
		level = PT_ZOOM_MAX;
	PT.zoom = level;
	if (level == 1)
	{
		PT.view_x = 0;
		PT.view_y = 0;
		return;
	}
	vw = PT.width / level;
	vh = PT.height / level;
	if (vw < 1)
		vw = 1;
	if (vh < 1)
		vh = 1;
	PT.view_x = cx - vw / 2;
	PT.view_y = cy - vh / 2;
	if (PT.view_x < 0)
		PT.view_x = 0;
	if (PT.view_y < 0)
		PT.view_y = 0;
	if (PT.view_x > PT.width - vw)
		PT.view_x = PT.width - vw;
	if (PT.view_y > PT.height - vh)
		PT.view_y = PT.height - vh;
}

/* ---- tool column ------------------------------------------------------- */

static int s_ico_x, s_ico_y;

static void idot(int x, int y)
{
	splot(s_ico_x + x, s_ico_y + y);
}

static void iline(int x0, int y0, int x1, int y1)
{
	line_plot(s_ico_x + x0, s_ico_y + y0, s_ico_x + x1, s_ico_y + y1,
		  splot);
}

static void iellipse(int x0, int y0, int x1, int y1)
{
	ellipse_plot(s_ico_x + x0, s_ico_y + y0, s_ico_x + x1, s_ico_y + y1,
		     splot);
}

static void irect(int x0, int y0, int x1, int y1)
{
	iline(x0, y0, x1, y0);
	iline(x1, y0, x1, y1);
	iline(x1, y1, x0, y1);
	iline(x0, y1, x0, y0);
}

static void draw_tool_icon(int tool, int ox, int oy, unsigned rgb)
{
	int k;

	s_sink = rgb;
	s_ico_x = ox;
	s_ico_y = oy;

	switch (tool)
	{
	case PT_TOOL_PENCIL:
		iline(2, 13, 11, 4);
		idot(12, 3);
		idot(13, 3);
		break;
	case PT_TOOL_LINE:
		iline(2, 13, 13, 2);
		idot(2, 13);
		idot(13, 2);
		break;
	case PT_TOOL_RECT:
		irect(2, 3, 13, 13);
		break;
	case PT_TOOL_ELLIPSE:
		iellipse(1, 3, 14, 13);
		break;
	case PT_TOOL_CIRCLE:
		iellipse(2, 1, 13, 12);
		break;
	case PT_TOOL_FILL:
		for (k = 0; k < 11; k++)
			iline(2 + k, 3 + k, 13 - k, 3 + k);
		break;
	case PT_TOOL_ERASER:
		for (k = 0; k < 9; k++)
			iline(3, 4 + k, 12, 4 + k);
		break;
	case PT_TOOL_PICK:
		iline(3, 12, 10, 5);
		idot(11, 4);
		idot(12, 3);
		idot(11, 3);
		idot(12, 4);
		break;
	case PT_TOOL_GRAB:
		irect(2, 3, 13, 13);
		iline(4, 5, 4, 11);
		iline(11, 5, 11, 11);
		break;
	case PT_TOOL_MAGNIFY:
		iellipse(1, 1, 10, 10);
		iline(9, 9, 13, 13);
		break;
	case PT_TOOL_AIRBRUSH:
		for (k = 0; k < 12; k++)
			idot(3 + (k * 5) % 10, 3 + (k * 3) % 10);
		break;
	case PT_TOOL_SPRAY:
		idot(2, 2);
		idot(7, 5);
		idot(12, 3);
		idot(4, 9);
		idot(10, 11);
		idot(13, 8);
		break;
	case PT_TOOL_TEXT:
		iline(3, 3, 12, 3);
		iline(7, 3, 7, 13);
		iline(4, 13, 10, 13);
		break;
	default:
		break;
	}
}

/* ---- public hooks ------------------------------------------------------ */

void pt_tools_init(void)
{
	s_shift = 0;
	s_moved = 0;
	s_brush_w = 0;
	s_brush_h = 0;
	s_brush_bg = PT.bg;
	s_noise = 0x13579BDFu;
}

void pt_tools_draw(void)
{
	int i;

	pt_fill_rect(0, PT_CANVAS_Y, PT_TOOL_W, PT_PAL_Y - PT_CANVAS_Y,
		     0x202020u);
	for (i = 0; i < PT_TOOL_COUNT; i++)
	{
		int y0 = PT_CANVAS_Y + i * PT_CELL_H;
		int sel = (i == PT.tool);

		if (y0 + PT_CELL_H > PT_PAL_Y)
			break;
		pt_fill_rect(0, y0, PT_TOOL_W, PT_CELL_H,
			     sel ? 0x505050u : 0x181818u);
		pt_fill_rect(0, y0 + PT_CELL_H - 1, PT_TOOL_W, 1, 0x303030u);
		draw_tool_icon(i, 8, y0 + (PT_CELL_H - 16) / 2,
			       sel ? 0xFFFFFFu : 0xC0C0C0u);
	}
}

int pt_tools_hit(int sx, int sy, int *tool)
{
	int row;

	if (sx < 0 || sx >= PT_TOOL_W || sy < PT_CANVAS_Y || sy >= PT_PAL_Y)
		return 0;
	row = (sy - PT_CANVAS_Y) / PT_CELL_H;
	if (row < 0 || row >= PT_TOOL_COUNT)
		return 0;
	if (tool)
		*tool = row;
	return 1;
}

void pt_tool_select(int tool)
{
	if (tool < 0 || tool >= PT_TOOL_COUNT)
		return;
	if (tool == PT.tool)
		return;
	pt_tool_cancel();
	PT.tool = tool;
	pt_request_redraw();
}

/* Shift state for the shape constraints. Not in the frozen event API; the
 * tests drive it directly and a later scaffold revision can wire the key. */
void pt_tool_modifiers(int shift)
{
	s_shift = shift ? 1 : 0;
}

void pt_tool_begin(int cx, int cy, int button)
{
	int c = tool_pen(button);

	PT.have_anchor = 1;
	PT.anchor_x = cx;
	PT.anchor_y = cy;
	PT.last_cx = cx;
	PT.last_cy = cy;
	s_moved = 0;

	switch (PT.tool)
	{
	case PT_TOOL_PENCIL:
		pt_undo_push();
		pt_canvas_set(cx, cy, c);
		break;
	case PT_TOOL_ERASER:
		pt_undo_push();
		pt_canvas_set(cx, cy, PT.bg);
		break;
	case PT_TOOL_LINE:
	case PT_TOOL_RECT:
	case PT_TOOL_ELLIPSE:
	case PT_TOOL_CIRCLE:
		pt_undo_push();
		preview_begin();
		break;
	case PT_TOOL_FILL:
		pt_undo_push();
		canvas_flood(cx, cy, c);
		break;
	case PT_TOOL_PICK:
		if (button == PT_BTN_RIGHT)
			PT.bg = pt_canvas_get(cx, cy);
		else
			PT.fg = pt_canvas_get(cx, cy);
		break;
	case PT_TOOL_GRAB:
		break;
	case PT_TOOL_MAGNIFY:
		if (button == PT_BTN_RIGHT)
			magnify_zoom(PT.zoom / 2, cx, cy);
		else
			magnify_zoom(PT.zoom < PT_ZOOM_MAX ? PT.zoom * 2 : 1,
				     cx, cy);
		break;
	case PT_TOOL_AIRBRUSH:
		pt_undo_push();
		air_dab(cx, cy, c);
		break;
	case PT_TOOL_SPRAY:
		pt_undo_push();
		spray_dab(cx, cy, c);
		break;
	case PT_TOOL_TEXT:
		pt_text_begin(cx, cy, button);
		break;
	default:
		break;
	}
}

void pt_tool_motion(int cx, int cy, int button)
{
	int c = tool_pen(button);
	int x = cx, y = cy;

	switch (PT.tool)
	{
	case PT_TOOL_PENCIL:
		canvas_line(PT.last_cx, PT.last_cy, cx, cy, c);
		break;
	case PT_TOOL_ERASER:
		canvas_line(PT.last_cx, PT.last_cy, cx, cy, PT.bg);
		break;
	case PT_TOOL_LINE:
		if (s_shift)
			constrain_line(PT.anchor_x, PT.anchor_y, &x, &y);
		preview_restore();
		canvas_line(PT.anchor_x, PT.anchor_y, x, y, c);
		break;
	case PT_TOOL_RECT:
		if (s_shift)
			constrain_square(PT.anchor_x, PT.anchor_y, &x, &y);
		preview_restore();
		canvas_rect(PT.anchor_x, PT.anchor_y, x, y, c);
		break;
	case PT_TOOL_ELLIPSE:
		if (s_shift)
			constrain_square(PT.anchor_x, PT.anchor_y, &x, &y);
		preview_restore();
		canvas_ellipse_box(PT.anchor_x, PT.anchor_y, x, y, c);
		break;
	case PT_TOOL_CIRCLE:
	{
		int r = iabs(x - PT.anchor_x);
		int ry = iabs(y - PT.anchor_y);

		if (ry > r)
			r = ry;
		preview_restore();
		canvas_circle(PT.anchor_x, PT.anchor_y, r, c);
		break;
	}
	case PT_TOOL_GRAB:
		if (!s_moved &&
		    (iabs(cx - PT.anchor_x) >= PT_DRAG_MIN ||
		     iabs(cy - PT.anchor_y) >= PT_DRAG_MIN))
		{
			s_moved = 1;
			preview_begin();
		}
		if (s_moved)
		{
			preview_restore();
			canvas_rect(PT.anchor_x, PT.anchor_y, cx, cy, c);
		}
		break;
	case PT_TOOL_AIRBRUSH:
		dab_line(air_dab, PT.last_cx, PT.last_cy, cx, cy, c);
		break;
	case PT_TOOL_SPRAY:
		dab_line(spray_dab, PT.last_cx, PT.last_cy, cx, cy, c);
		break;
	default:
		break;
	}
}

void pt_tool_end(int cx, int cy, int button)
{
	int c = tool_pen(button);
	int x = cx, y = cy;

	switch (PT.tool)
	{
	case PT_TOOL_LINE:
		if (s_shift)
			constrain_line(PT.anchor_x, PT.anchor_y, &x, &y);
		preview_restore();
		canvas_line(PT.anchor_x, PT.anchor_y, x, y, c);
		PT.scratch_valid = 0;
		break;
	case PT_TOOL_RECT:
		if (s_shift)
			constrain_square(PT.anchor_x, PT.anchor_y, &x, &y);
		preview_restore();
		canvas_rect(PT.anchor_x, PT.anchor_y, x, y, c);
		PT.scratch_valid = 0;
		break;
	case PT_TOOL_ELLIPSE:
		if (s_shift)
			constrain_square(PT.anchor_x, PT.anchor_y, &x, &y);
		preview_restore();
		canvas_ellipse_box(PT.anchor_x, PT.anchor_y, x, y, c);
		PT.scratch_valid = 0;
		break;
	case PT_TOOL_CIRCLE:
	{
		int r = iabs(x - PT.anchor_x);
		int ry = iabs(y - PT.anchor_y);

		if (ry > r)
			r = ry;
		preview_restore();
		canvas_circle(PT.anchor_x, PT.anchor_y, r, c);
		PT.scratch_valid = 0;
		break;
	}
	case PT_TOOL_GRAB:
		if (s_moved)
		{
			preview_restore();
			grab_capture(PT.anchor_x, PT.anchor_y, cx, cy);
			PT.scratch_valid = 0;
		}
		else if (s_brush && s_brush_w > 0)
		{
			pt_undo_push();
			grab_stamp(cx, cy);
		}
		break;
	default:
		break;
	}
	PT.have_anchor = 0;
	s_moved = 0;
}

void pt_tool_cancel(void)
{
	if (PT.scratch_valid)
	{
		preview_restore();
		PT.scratch_valid = 0;
	}
	PT.have_anchor = 0;
	s_moved = 0;
}
