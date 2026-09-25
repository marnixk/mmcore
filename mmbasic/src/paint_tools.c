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
#include "paint_tool_icons.h"

/* A grab has to move this far before it defines a new brush instead of
 * stamping the existing one. */
#define PT_DRAG_MIN 2

/* The text tool lives in paint_text.c (#642); a canvas click opens its caret. */
void pt_text_begin(int cx, int cy, int button);

/* ---- module state ------------------------------------------------------ */

/* One set of tool state per virtual console: a live shape preview, grab brush
 * and airbrush noise sequence must not cross consoles (#766). */
typedef struct {
	int shift;		/* Shift held (pt_tool_modifiers) */
	int moved;		/* grab: the press turned into a drag */

	/* Bounding box of the live shape preview currently on the canvas, so the
	 * next preview_restore() can damage the pixels it is about to revert
	 * (#700). */
	int pv_valid;
	int pv_x0, pv_y0, pv_x1, pv_y1;

	unsigned char *brush;	/* custom brush, PT_MAX_W * PT_MAX_H */
	int brush_w, brush_h;
	int brush_bg;		/* index treated as transparent when stamped */

	/* Ink for the shared line/ellipse rasterisers: canvas index or RGB. */
	int cink;
	unsigned sink;

	int pen_w;
	unsigned noise;
} pt_tools_state;

static pt_tools_state s_tools_state[MMB_MAX_CONSOLES];
#define TOOLS (s_tools_state[g_console])

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
	pt_canvas_set(x, y, TOOLS.cink);
}

static void splot(int x, int y)
{
	pt_fill_rect(x, y, 1, 1, TOOLS.sink);
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

/* ---- pen width (#718) --------------------------------------------------- */

/* The five selectable pen widths; the active one is PT.width_idx. */
static const int s_pen_widths[PT_WB_COUNT] = { 1, 2, 3, 4, 6 };

int pt_pen_width(void)
{
	int i = PT.width_idx;

	if (i < 0)
		i = 0;
	if (i >= PT_WB_COUNT)
		i = PT_WB_COUNT - 1;
	return s_pen_widths[i];
}

/* Stamp the pen as a square of the current width centred on (x, y). */
static void cstamp(int x, int y)
{
	int o = -((TOOLS.pen_w - 1) / 2);
	int i, j;

	for (j = 0; j < TOOLS.pen_w; j++)
		for (i = 0; i < TOOLS.pen_w; i++)
			pt_canvas_set(x + o + i, y + o + j, TOOLS.cink);
}

static void canvas_line(int x0, int y0, int x1, int y1, int c)
{
	TOOLS.cink = c;
	TOOLS.pen_w = pt_pen_width();
	line_plot(x0, y0, x1, y1, TOOLS.pen_w > 1 ? cstamp : cplot);
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
	TOOLS.cink = c;
	TOOLS.pen_w = pt_pen_width();
	ellipse_plot(x0, y0, x1, y1, TOOLS.pen_w > 1 ? cstamp : cplot);
}

static void canvas_circle(int cx, int cy, int r, int c)
{
	if (r < 0)
		r = -r;
	canvas_ellipse_box(cx - r, cy - r, cx + r, cy + r, c);
}

static void canvas_rect(int x0, int y0, int x1, int y1, int c)
{
	int t;

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
	canvas_line(x0, y0, x1, y0, c);
	canvas_line(x0, y1, x1, y1, c);
	canvas_line(x0, y0, x0, y1, c);
	canvas_line(x1, y0, x1, y1, c);
}

/* ---- filled shapes (#719) ---------------------------------------------- */

static void canvas_rect_fill(int x0, int y0, int x1, int y1, int c)
{
	int x, y, t;

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
	for (y = y0; y <= y1; y++)
		for (x = x0; x <= x1; x++)
			pt_canvas_set(x, y, c);
}

/* Integer floor square root, no libm (the tool tests link without -lm). */
static long isqrt_l(long long v)
{
	long long bit = 1LL << 62;
	long long r = 0;

	if (v <= 0)
		return 0;
	while (bit > v)
		bit >>= 2;
	while (bit)
	{
		if (v >= r + bit)
		{
			v -= r + bit;
			r = (r >> 1) + bit;
		}
		else
			r >>= 1;
		bit >>= 2;
	}
	return (long)r;
}

/* Filled ellipse as one horizontal span per scanline, from the exact region
 * inequality ((2x-cx)/A)^2 + ((2y-cy)/B)^2 <= 1. Unlike the old concentric
 * outlines this leaves no dotted gaps on a wide (non-circular) ellipse, so the
 * filled circle / ellipse tools use the same solid-fill mechanic as CIRCLE. */
static void canvas_ellipse_fill(int x0, int y0, int x1, int y1, int c)
{
	long aa, bb, cx2, cy2;
	int t, y;

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

	cx2 = (long)x0 + x1;
	cy2 = (long)y0 + y1;
	aa = (long)(x1 - x0) * (x1 - x0);	/* (2a)^2 */
	bb = (long)(y1 - y0) * (y1 - y0);	/* (2b)^2 */
	if (bb == 0)				/* degenerate: one scanline */
		bb = 1;

	for (y = y0; y <= y1; y++)
	{
		long dy2 = 2L * y - cy2;
		long dx2 = isqrt_l(aa * (bb - dy2 * dy2) / bb);
		long xl = (cx2 - dx2 + 1) >> 1;	/* ceil((cx2-dx2)/2) */
		long xr = (cx2 + dx2) >> 1;	/* floor((cx2+dx2)/2) */
		int x;

		for (x = (int)xl; x <= (int)xr; x++)
			pt_canvas_set(x, y, c);
	}
}

static void canvas_circle_fill(int cx, int cy, int r, int c)
{
	if (r < 0)
		r = -r;
	canvas_ellipse_fill(cx - r, cy - r, cx + r, cy + r, c);
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

/* Remember the canvas rectangle the live preview now covers. The pen is a
 * square centred on the path, so the drawn extent overhangs the nominal shape
 * box by up to (pen width - 1) pixels; pad the box so the next revert damages
 * (and so re-presents) the overhang too. Without this, shrinking a thick
 * outline left its outer edge on screen. */
static void preview_note(int x0, int y0, int x1, int y1)
{
	int t;
	int pad = pt_pen_width();

	x0 -= pad;
	y0 -= pad;
	x1 += pad;
	y1 += pad;

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
	if (x1 > PT.width - 1)
		x1 = PT.width - 1;
	if (y1 > PT.height - 1)
		y1 = PT.height - 1;
	if (x1 < x0 || y1 < y0)
	{
		TOOLS.pv_valid = 0;
		return;
	}
	TOOLS.pv_x0 = x0;
	TOOLS.pv_y0 = y0;
	TOOLS.pv_x1 = x1;
	TOOLS.pv_y1 = y1;
	TOOLS.pv_valid = 1;
}

static void preview_restore(void)
{
	if (PT.scratch && PT.canvas && PT.scratch_valid)
	{
		/* The revert erases the previous preview, which the per-pixel
		 * pt_canvas_set() damage does not cover. */
		if (TOOLS.pv_valid)
			pt_damage_canvas(TOOLS.pv_x0, TOOLS.pv_y0,
					 TOOLS.pv_x1 - TOOLS.pv_x0 + 1,
					 TOOLS.pv_y1 - TOOLS.pv_y0 + 1);
		memcpy(PT.canvas, PT.scratch, (size_t)PT.width * PT.height);
	}
	TOOLS.pv_valid = 0;
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
	if (!TOOLS.brush && G.plat && G.plat->alloc)
		TOOLS.brush = (unsigned char *)
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
	if (!TOOLS.brush)
		return;
	TOOLS.brush_w = x1 - x0 + 1;
	TOOLS.brush_h = y1 - y0 + 1;
	TOOLS.brush_bg = PT.bg;
	for (j = 0; j < TOOLS.brush_h; j++)
		for (i = 0; i < TOOLS.brush_w; i++)
			TOOLS.brush[(size_t)j * TOOLS.brush_w + i] =
				(unsigned char)pt_canvas_get(x0 + i, y0 + j);
}

static void grab_stamp(int x, int y)
{
	int i, j;

	if (!TOOLS.brush || TOOLS.brush_w <= 0 || TOOLS.brush_h <= 0)
		return;
	for (j = 0; j < TOOLS.brush_h; j++)
		for (i = 0; i < TOOLS.brush_w; i++)
		{
			int v = TOOLS.brush[(size_t)j * TOOLS.brush_w + i];

			if (v == TOOLS.brush_bg)
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

static unsigned noise_next(void)
{
	TOOLS.noise = TOOLS.noise * 1664525u + 1013904223u;
	return TOOLS.noise >> 8;
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

/* The 16x16 glyphs are baked pixel art (paint_tool_icons.h). Tone 1 is the
 * icon colour and tone 2 the cell background, so internal detail reads on both
 * the selected and unselected highlight. */
static void draw_tool_icon(int tool, int ox, int oy, unsigned fg, unsigned bg)
{
	const uint8_t *art;
	int x, y;

	if (tool < 0 || tool >= PTI_TOOL_COUNT)
		return;
	art = pti_icons[tool];
	for (y = 0; y < PTI_ICON_H; y++)
	{
		for (x = 0; x < PTI_ICON_W; x++)
		{
			uint8_t v = art[y * PTI_ICON_W + x];

			if (v == PTI_ICON_TRANSPARENT)
				continue;
			TOOLS.sink = (v == PTI_ICON_CUT) ? bg : fg;
			splot(ox + x, oy + y);
		}
	}
}

/* ---- public hooks ------------------------------------------------------ */

void pt_tools_init(void)
{
	if (TOOLS.brush && G.plat && G.plat->free)
		G.plat->free(TOOLS.brush);
	TOOLS.brush = 0;
	TOOLS.shift = 0;
	TOOLS.moved = 0;
	TOOLS.pv_valid = 0;
	TOOLS.brush_w = 0;
	TOOLS.brush_h = 0;
	TOOLS.brush_bg = PT.bg;
	TOOLS.cink = 0;
	TOOLS.sink = 0;
	TOOLS.pen_w = 1;
	TOOLS.noise = 0x13579BDFu;
	PT.width_idx = 0;
}

void pt_tools_draw(void)
{
	int i;

	pt_fill_rect(0, PT_CANVAS_Y, PT_TOOL_W, PT_PAL_Y - PT_CANVAS_Y,
		     0x202020u);
	for (i = 0; i < PT_TOOL_COUNT; i++)
	{
		int col = i % PT_TOOL_COLS;
		int row = i / PT_TOOL_COLS;
		int x0 = col * PT_CELL_W;
		int y0 = PT_CANVAS_Y + row * PT_CELL_H;
		int sel = (i == PT.tool);

		if (y0 + PT_CELL_H > PT_PAL_Y)
			break;
		pt_fill_rect(x0, y0, PT_CELL_W, PT_CELL_H,
			     sel ? 0x505050u : 0x181818u);
		pt_fill_rect(x0 + PT_CELL_W - 1, y0, 1, PT_CELL_H, 0x303030u);
		pt_fill_rect(x0, y0 + PT_CELL_H - 1, PT_CELL_W, 1, 0x303030u);
		draw_tool_icon(i, x0 + (PT_CELL_W - PTI_ICON_W) / 2,
			       y0 + (PT_CELL_H - PTI_ICON_H) / 2,
			       sel ? 0xFFFFFFu : 0xC0C0C0u,
			       sel ? 0x505050u : 0x181818u);
	}
}

int pt_tools_hit(int sx, int sy, int *tool)
{
	int col, row, t;

	if (sx < 0 || sx >= PT_TOOL_W || sy < PT_CANVAS_Y || sy >= PT_PAL_Y)
		return 0;
	col = sx / PT_CELL_W;
	row = (sy - PT_CANVAS_Y) / PT_CELL_H;
	if (col < 0 || col >= PT_TOOL_COLS || row < 0 || row >= PT_TOOL_ROWS)
		return 0;
	t = row * PT_TOOL_COLS + col;
	if (t < 0 || t >= PT_TOOL_COUNT)
		return 0;
	if (tool)
		*tool = t;
	return 1;
}

/* ---- line-width selector (#718) ---------------------------------------- */

void pt_width_draw(void)
{
	int i, cw = PT_WB_W / PT_WB_COUNT;

	pt_fill_rect(PT_WB_X, PT_WB_Y, PT_WB_W, PT_WB_H, 0x00303030u);
	for (i = 0; i < PT_WB_COUNT; i++)
	{
		int x0 = PT_WB_X + i * cw;
		int sel = (i == PT.width_idx);
		int w = s_pen_widths[i];

		if (sel)
			pt_fill_rect(x0 + 1, PT_WB_Y + 1, cw - 2, PT_WB_H - 2,
				     0x00505050u);
		/* Sample bar drawn as thick as the pen, in the FG colour. */
		pt_fill_rect(x0 + 3, PT_WB_Y + (PT_WB_H - w) / 2, cw - 6, w,
			     pt_palette_rgb(PT.fg));
	}
	pt_fill_rect(PT_WB_X, PT_WB_Y, PT_WB_W, 1, 0x00FFFFFFu);
	pt_fill_rect(PT_WB_X, PT_WB_Y + PT_WB_H - 1, PT_WB_W, 1, 0x00FFFFFFu);
	pt_fill_rect(PT_WB_X, PT_WB_Y, 1, PT_WB_H, 0x00FFFFFFu);
	pt_fill_rect(PT_WB_X + PT_WB_W - 1, PT_WB_Y, 1, PT_WB_H, 0x00FFFFFFu);
	for (i = 1; i < PT_WB_COUNT; i++)
		pt_fill_rect(PT_WB_X + i * cw, PT_WB_Y + 1, 1, PT_WB_H - 2,
			     0x00808080u);
}

int pt_width_hit(int sx, int sy, int *idx)
{
	int c, cw = PT_WB_W / PT_WB_COUNT;

	if (sx < PT_WB_X || sy < PT_WB_Y ||
	    sx >= PT_WB_X + PT_WB_W || sy >= PT_WB_Y + PT_WB_H)
		return 0;
	c = (sx - PT_WB_X) / cw;
	if (c < 0)
		c = 0;
	if (c >= PT_WB_COUNT)
		c = PT_WB_COUNT - 1;
	if (idx)
		*idx = c;
	return 1;
}

void pt_width_select(int idx)
{
	if (idx < 0)
		idx = 0;
	if (idx >= PT_WB_COUNT)
		idx = PT_WB_COUNT - 1;
	PT.width_idx = idx;
	pt_request_redraw();
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
	TOOLS.shift = shift ? 1 : 0;
}

void pt_tool_begin(int cx, int cy, int button)
{
	int c = tool_pen(button);

	PT.have_anchor = 1;
	PT.anchor_x = cx;
	PT.anchor_y = cy;
	PT.last_cx = cx;
	PT.last_cy = cy;
	TOOLS.moved = 0;
	TOOLS.pv_valid = 0;

	switch (PT.tool)
	{
	case PT_TOOL_PENCIL:
		pt_undo_push();
		canvas_line(cx, cy, cx, cy, c);
		break;
	case PT_TOOL_ERASER:
		pt_undo_push();
		canvas_line(cx, cy, cx, cy, PT.bg);
		break;
	case PT_TOOL_LINE:
	case PT_TOOL_RECT:
	case PT_TOOL_RECT_FILLED:
	case PT_TOOL_ELLIPSE:
	case PT_TOOL_ELLIPSE_FILLED:
	case PT_TOOL_CIRCLE:
	case PT_TOOL_CIRCLE_FILLED:
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
	case PT_TOOL_SELECT:
		pt_select_begin(cx, cy, button);
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
		if (TOOLS.shift)
			constrain_line(PT.anchor_x, PT.anchor_y, &x, &y);
		preview_restore();
		canvas_line(PT.anchor_x, PT.anchor_y, x, y, c);
		preview_note(PT.anchor_x, PT.anchor_y, x, y);
		break;
	case PT_TOOL_RECT:
		if (TOOLS.shift)
			constrain_square(PT.anchor_x, PT.anchor_y, &x, &y);
		preview_restore();
		canvas_rect(PT.anchor_x, PT.anchor_y, x, y, c);
		preview_note(PT.anchor_x, PT.anchor_y, x, y);
		break;
	case PT_TOOL_RECT_FILLED:
		if (TOOLS.shift)
			constrain_square(PT.anchor_x, PT.anchor_y, &x, &y);
		preview_restore();
		canvas_rect_fill(PT.anchor_x, PT.anchor_y, x, y, c);
		preview_note(PT.anchor_x, PT.anchor_y, x, y);
		break;
	case PT_TOOL_ELLIPSE:
		if (TOOLS.shift)
			constrain_square(PT.anchor_x, PT.anchor_y, &x, &y);
		preview_restore();
		canvas_ellipse_box(PT.anchor_x, PT.anchor_y, x, y, c);
		preview_note(PT.anchor_x, PT.anchor_y, x, y);
		break;
	case PT_TOOL_ELLIPSE_FILLED:
		if (TOOLS.shift)
			constrain_square(PT.anchor_x, PT.anchor_y, &x, &y);
		preview_restore();
		canvas_ellipse_fill(PT.anchor_x, PT.anchor_y, x, y, c);
		preview_note(PT.anchor_x, PT.anchor_y, x, y);
		break;
	case PT_TOOL_CIRCLE:
	{
		int r = iabs(x - PT.anchor_x);
		int ry = iabs(y - PT.anchor_y);

		if (ry > r)
			r = ry;
		preview_restore();
		canvas_circle(PT.anchor_x, PT.anchor_y, r, c);
		preview_note(PT.anchor_x - r, PT.anchor_y - r,
			     PT.anchor_x + r, PT.anchor_y + r);
		break;
	}
	case PT_TOOL_CIRCLE_FILLED:
	{
		int r = iabs(x - PT.anchor_x);
		int ry = iabs(y - PT.anchor_y);

		if (ry > r)
			r = ry;
		preview_restore();
		canvas_circle_fill(PT.anchor_x, PT.anchor_y, r, c);
		preview_note(PT.anchor_x - r, PT.anchor_y - r,
			     PT.anchor_x + r, PT.anchor_y + r);
		break;
	}
	case PT_TOOL_GRAB:
		if (!TOOLS.moved &&
		    (iabs(cx - PT.anchor_x) >= PT_DRAG_MIN ||
		     iabs(cy - PT.anchor_y) >= PT_DRAG_MIN))
		{
			TOOLS.moved = 1;
			preview_begin();
		}
		if (TOOLS.moved)
		{
			preview_restore();
			canvas_rect(PT.anchor_x, PT.anchor_y, cx, cy, c);
			preview_note(PT.anchor_x, PT.anchor_y, cx, cy);
		}
		break;
	case PT_TOOL_AIRBRUSH:
		dab_line(air_dab, PT.last_cx, PT.last_cy, cx, cy, c);
		break;
	case PT_TOOL_SPRAY:
		dab_line(spray_dab, PT.last_cx, PT.last_cy, cx, cy, c);
		break;
	case PT_TOOL_SELECT:
		pt_select_motion(cx, cy);
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
		if (TOOLS.shift)
			constrain_line(PT.anchor_x, PT.anchor_y, &x, &y);
		preview_restore();
		canvas_line(PT.anchor_x, PT.anchor_y, x, y, c);
		PT.scratch_valid = 0;
		break;
	case PT_TOOL_RECT:
		if (TOOLS.shift)
			constrain_square(PT.anchor_x, PT.anchor_y, &x, &y);
		preview_restore();
		canvas_rect(PT.anchor_x, PT.anchor_y, x, y, c);
		PT.scratch_valid = 0;
		break;
	case PT_TOOL_RECT_FILLED:
		if (TOOLS.shift)
			constrain_square(PT.anchor_x, PT.anchor_y, &x, &y);
		preview_restore();
		canvas_rect_fill(PT.anchor_x, PT.anchor_y, x, y, c);
		PT.scratch_valid = 0;
		break;
	case PT_TOOL_ELLIPSE:
		if (TOOLS.shift)
			constrain_square(PT.anchor_x, PT.anchor_y, &x, &y);
		preview_restore();
		canvas_ellipse_box(PT.anchor_x, PT.anchor_y, x, y, c);
		PT.scratch_valid = 0;
		break;
	case PT_TOOL_ELLIPSE_FILLED:
		if (TOOLS.shift)
			constrain_square(PT.anchor_x, PT.anchor_y, &x, &y);
		preview_restore();
		canvas_ellipse_fill(PT.anchor_x, PT.anchor_y, x, y, c);
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
	case PT_TOOL_CIRCLE_FILLED:
	{
		int r = iabs(x - PT.anchor_x);
		int ry = iabs(y - PT.anchor_y);

		if (ry > r)
			r = ry;
		preview_restore();
		canvas_circle_fill(PT.anchor_x, PT.anchor_y, r, c);
		PT.scratch_valid = 0;
		break;
	}
	case PT_TOOL_GRAB:
		if (TOOLS.moved)
		{
			preview_restore();
			grab_capture(PT.anchor_x, PT.anchor_y, cx, cy);
			PT.scratch_valid = 0;
		}
		else if (TOOLS.brush && TOOLS.brush_w > 0)
		{
			pt_undo_push();
			grab_stamp(cx, cy);
		}
		break;
	case PT_TOOL_SELECT:
		pt_select_end(cx, cy);
		break;
	default:
		break;
	}
	PT.have_anchor = 0;
	TOOLS.moved = 0;
}

void pt_tool_cancel(void)
{
	if (PT.tool == PT_TOOL_SELECT)
		pt_select_cancel();
	if (PT.scratch_valid)
	{
		preview_restore();
		PT.scratch_valid = 0;
	}
	PT.have_anchor = 0;
	TOOLS.moved = 0;
}
