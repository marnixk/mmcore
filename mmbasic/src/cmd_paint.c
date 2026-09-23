/*
 * PAINT - a Dr. Genie / Paintbrush-style pixel paint app.
 *
 * The canvas is drawn one TUI character cell per canvas pixel, in a fixed
 * 16-colour IBM palette, so the on-screen colours and the saved bitmap agree.
 * Tools: pencil, eraser, line, rectangle, circle, flood fill, colour picker
 * and undo. A USB mouse drives the same tools when present (see #513); the
 * keyboard remains fully usable without one.
 *
 * The canvas is a plain RGB PNG (the same bitmap format SPRITE uses), saved
 * and reloaded through the filesystem so sketches round-trip.
 */
#include "mmb_priv.h"
#include "tui.h"

#define PT_MAX_W 96
#define PT_MAX_H 48
#define PT_UNDO  6

#define PT_OX 2 /* canvas left column */
#define PT_OY 4 /* canvas top row */

#define PT_CELL_W 8
#define PT_CELL_H 16

#define PT_PENCIL 0
#define PT_LINE   1
#define PT_RECT   2
#define PT_CIRCLE 3
#define PT_FILL   4
#define PT_ERASER 5
#define PT_PICK   6

#define PT_ESC_NONE 0
#define PT_ESC_GOT  1
#define PT_ESC_CSI  2
#define PT_ESC_SS3  3
#define PT_ESC_IDLE_MS 60

typedef struct {
	int active;
	char path[160];
	char label[96];
	int w, h;
	int colour;
	int tool;
	int cx, cy;
	int dirty;
	int esc_state, esc_at, alt_pend;
	int have_mouse;
	int mouse_down;
	int last_px, last_py;
	int have_anchor;
	int anchor_px, anchor_py;
	char status[96];
	unsigned char pix[PT_MAX_W * PT_MAX_H];
	unsigned char undo[PT_UNDO][PT_MAX_W * PT_MAX_H];
	int undo_n, undo_pos;
	unsigned char base[PT_MAX_W * PT_MAX_H];
	int have_base;
} pt_state;

static pt_state PT;

static const char *const PT_TOOL_NAME[] = {
	"PENCIL", "LINE", "RECT", "CIRCLE", "FILL", "ERASER", "PICK"
};

static int pt_abs(int v)
{
	return v < 0 ? -v : v;
}

static int pt_clamp(int v, int lo, int hi)
{
	if (v < lo)
		return lo;
	if (v > hi)
		return hi;
	return v;
}

static const char *pt_base(const char *p)
{
	const char *s = p;
	while (p && *p)
	{
		if (*p == '/' || *p == ':' || *p == '\\')
			s = p + 1;
		p++;
	}
	return s;
}

static unsigned pt_colour_rgb(int i)
{
	return mmb_ibm_colour(i);
}

/* Load the fixed IBM palette into the TUI so canvas indices render exactly. */
static void pt_apply_palette(void)
{
	static unsigned pal[16];
	static int ready;
	int i;
	if (!ready)
	{
		for (i = 0; i < 16; i++)
			pal[i] = mmb_ibm_colour(i);
		ready = 1;
	}
	tui_set_palette(pal);
}

static int pt_nearest(unsigned rgb)
{
	int best = 0, bd = 1 << 30, i;
	int r = (int)((rgb >> 16) & 255), g = (int)((rgb >> 8) & 255);
	int b = (int)(rgb & 255);
	for (i = 0; i < 16; i++)
	{
		unsigned c = pt_colour_rgb(i);
		int dr = r - (int)((c >> 16) & 255);
		int dg = g - (int)((c >> 8) & 255);
		int db = b - (int)(c & 255);
		int d = dr * dr + dg * dg + db * db;
		if (d < bd)
		{
			bd = d;
			best = i;
		}
	}
	return best;
}

/* ---- canvas primitives ---------------------------------------------- */

static void pt_put(int x, int y, int col)
{
	if (x < 0 || y < 0 || x >= PT.w || y >= PT.h)
		return;
	PT.pix[y * PT.w + x] = (unsigned char)col;
}

static void pt_hline(int x0, int x1, int y, int col)
{
	int x;
	if (y < 0 || y >= PT.h)
		return;
	if (x0 > x1)
	{
		int t = x0;
		x0 = x1;
		x1 = t;
	}
	for (x = x0; x <= x1; x++)
		pt_put(x, y, col);
}

static void pt_line(int x0, int y0, int x1, int y1, int col)
{
	int dx = pt_abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
	int dy = -pt_abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
	int err = dx + dy;
	for (;;)
	{
		pt_put(x0, y0, col);
		if (x0 == x1 && y0 == y1)
			break;
		{
			int e2 = 2 * err;
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
}

static void pt_rect(int x0, int y0, int x1, int y1, int col, int fill)
{
	int y, t;
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
	if (fill)
	{
		for (y = y0; y <= y1; y++)
			pt_hline(x0, x1, y, col);
		return;
	}
	pt_hline(x0, x1, y0, col);
	pt_hline(x0, x1, y1, col);
	for (y = y0; y <= y1; y++)
	{
		pt_put(x0, y, col);
		pt_put(x1, y, col);
	}
}

static void pt_circle_pts(int cx, int cy, int x, int y, int col)
{
	pt_put(cx + x, cy + y, col);
	pt_put(cx - x, cy + y, col);
	pt_put(cx + x, cy - y, col);
	pt_put(cx - x, cy - y, col);
	pt_put(cx + y, cy + x, col);
	pt_put(cx - y, cy + x, col);
	pt_put(cx + y, cy - x, col);
	pt_put(cx - y, cy - x, col);
}

static void pt_circle(int cx, int cy, int r, int col, int fill)
{
	int x = 0, y = r, d = 1 - r, i;
	if (r < 0)
		r = 0;
	if (fill)
	{
		pt_hline(cx - r, cx + r, cy, col);
		while (x <= y)
		{
			for (i = -x; i <= x; i++)
			{
				pt_put(cx + i, cy + y, col);
				pt_put(cx + i, cy - y, col);
			}
			for (i = -y; i <= y; i++)
			{
				pt_put(cx + i, cy + x, col);
				pt_put(cx + i, cy - x, col);
			}
			if (d < 0)
				d += 2 * x + 3;
			else
			{
				d += 2 * (x - y) + 5;
				y--;
			}
			x++;
		}
		return;
	}
	while (x <= y)
	{
		pt_circle_pts(cx, cy, x, y, col);
		if (d < 0)
			d += 2 * x + 3;
		else
		{
			d += 2 * (x - y) + 5;
			y--;
		}
		x++;
	}
}

static void pt_flood(int sx, int sy, int col)
{
	static int stack[PT_MAX_W * PT_MAX_H];
	int sp = 0;
	int target;
	if (sx < 0 || sy < 0 || sx >= PT.w || sy >= PT.h)
		return;
	target = PT.pix[sy * PT.w + sx];
	if (target == col)
		return;
	PT.pix[sy * PT.w + sx] = (unsigned char)col;
	stack[sp++] = sy * PT.w + sx;
	while (sp > 0)
	{
		int p = stack[--sp];
		int x = p % PT.w, y = p / PT.w;
		if (x > 0 && PT.pix[p - 1] == target)
		{
			PT.pix[p - 1] = (unsigned char)col;
			stack[sp++] = p - 1;
		}
		if (x < PT.w - 1 && PT.pix[p + 1] == target)
		{
			PT.pix[p + 1] = (unsigned char)col;
			stack[sp++] = p + 1;
		}
		if (y > 0 && PT.pix[p - PT.w] == target)
		{
			PT.pix[p - PT.w] = (unsigned char)col;
			stack[sp++] = p - PT.w;
		}
		if (y < PT.h - 1 && PT.pix[p + PT.w] == target)
		{
			PT.pix[p + PT.w] = (unsigned char)col;
			stack[sp++] = p + PT.w;
		}
	}
}

/* ---- undo / shape preview ------------------------------------------- */

static void pt_undo_push(void)
{
	memcpy(PT.undo[PT.undo_pos], PT.pix, (unsigned)(PT.w * PT.h));
	PT.undo_pos = (PT.undo_pos + 1) % PT_UNDO;
	if (PT.undo_n < PT_UNDO)
		PT.undo_n++;
}

static void pt_undo(void)
{
	if (PT.undo_n <= 0)
	{
		strncpy(PT.status, "Nothing to undo", sizeof(PT.status) - 1);
		return;
	}
	PT.undo_pos = (PT.undo_pos - 1 + PT_UNDO) % PT_UNDO;
	memcpy(PT.pix, PT.undo[PT.undo_pos], (unsigned)(PT.w * PT.h));
	PT.undo_n--;
	PT.dirty = 1;
	strncpy(PT.status, "Undo", sizeof(PT.status) - 1);
}

static void pt_snapshot_base(void)
{
	memcpy(PT.base, PT.pix, (unsigned)(PT.w * PT.h));
	PT.have_base = 1;
}

static void pt_restore_base(void)
{
	if (PT.have_base)
		memcpy(PT.pix, PT.base, (unsigned)(PT.w * PT.h));
}

static void pt_draw_shape(int x0, int y0, int x1, int y1)
{
	switch (PT.tool)
	{
	case PT_LINE:
		pt_line(x0, y0, x1, y1, PT.colour);
		break;
	case PT_RECT:
		pt_rect(x0, y0, x1, y1, PT.colour, 0);
		break;
	case PT_CIRCLE:
	{
		int dx = x1 - x0, dy = y1 - y0;
		int r = (pt_abs(dx) + pt_abs(dy)) / 2;
		pt_circle(x0, y0, r, PT.colour, 0);
		break;
	}
	default:
		break;
	}
}

/* ---- save / load ---------------------------------------------------- */

static int pt_save(void)
{
	int n = PT.w * PT.h, i;
	unsigned char *rgb;
	unsigned char *png = 0;
	unsigned pngn = 0;

	rgb = G.plat->alloc((unsigned)n * 3u);
	if (!rgb)
	{
		strncpy(PT.status, "Save failed", sizeof(PT.status) - 1);
		return -1;
	}
	for (i = 0; i < n; i++)
	{
		unsigned c = pt_colour_rgb(PT.pix[i]);
		rgb[i * 3 + 0] = (unsigned char)((c >> 16) & 255);
		rgb[i * 3 + 1] = (unsigned char)((c >> 8) & 255);
		rgb[i * 3 + 2] = (unsigned char)(c & 255);
	}
	if (mmb_png_encode_rgb(rgb, PT.w, PT.h, &png, &pngn) != 0)
	{
		G.plat->free(rgb);
		strncpy(PT.status, "Save failed", sizeof(PT.status) - 1);
		return -1;
	}
	G.plat->free(rgb);
	if (mmb_vfs_write(PT.path, png, pngn, 0) != 0)
	{
		G.plat->free(png);
		strncpy(PT.status, "Save failed", sizeof(PT.status) - 1);
		return -1;
	}
	G.plat->free(png);
	PT.dirty = 0;
	sprintf(PT.status, "Saved %s", PT.label);
	return 0;
}

/* Decode a PNG into an RGBA buffer; returns 1 on success. Caller frees pix. */
static int pt_load_rgba(uint32_t **pix_out, int *w_out, int *h_out)
{
	int sz;
	unsigned got = 0;
	unsigned char *file;
	uint32_t *pix = 0;
	int w = 0, h = 0;

	*pix_out = 0;
	*w_out = *h_out = 0;
	sz = mmb_vfs_size(PT.path);
	if (sz <= 0)
		return 0;
	file = G.plat->alloc((unsigned)sz + 1);
	if (!file)
		return 0;
	if (mmb_vfs_read(PT.path, file, (unsigned)sz, &got) != 0 ||
	    mmb_png_decode_rgba(file, got, &pix, &w, &h) != 0)
	{
		G.plat->free(file);
		if (pix)
			G.plat->free(pix);
		return 0;
	}
	G.plat->free(file);
	*pix_out = pix;
	*w_out = w;
	*h_out = h;
	return 1;
}

/* ---- layout / rendering --------------------------------------------- */

static int pt_max_w(void)
{
	int w = tui_cols() - 4;
	if (w > PT_MAX_W)
		w = PT_MAX_W;
	if (w < 8)
		w = 8;
	return w;
}

static int pt_max_h(void)
{
	int h = tui_rows() - 6;
	if (h > PT_MAX_H)
		h = PT_MAX_H;
	if (h < 8)
		h = 8;
	return h;
}

static void pt_redraw(void)
{
	int cols, rows, x, y, i;
	char line[128];

	if (!PT.active || !G.plat || !G.plat->tui_glyph)
		return;
	cols = tui_cols();
	rows = tui_rows();
	tui_begin();
	pt_apply_palette();
	tui_clear(TUI_BRWHITE, TUI_BLACK);

	sprintf(line, "PAINT  %s  %dx%d  %s", PT.label, PT.w, PT.h,
		PT.dirty ? "*" : " ");
	tui_pad(0, 0, line, cols, TUI_BRWHITE, TUI_BRBLUE);

	/* Palette swatches on row 1. */
	for (i = 0; i < 16; i++)
	{
		int px = PT_OX + i;
		tui_put(px, 1, ' ', TUI_WHITE, i);
		if (i == PT.colour)
			tui_put(px, 1, '_', TUI_BRWHITE, i);
	}
	sprintf(line, " %s  COLOUR %d", PT_TOOL_NAME[PT.tool], PT.colour);
	tui_puts(PT_OX + 18, 1, line, TUI_BRYELLOW, TUI_BRBLACK);

	/* Canvas frame + pixels. */
	tui_frame(PT_OX - 1, PT_OY - 1, PT.w + 2, PT.h + 2, TUI_BRCYAN,
		  TUI_BRBLACK);
	for (y = 0; y < PT.h; y++)
		for (x = 0; x < PT.w; x++)
			tui_put(PT_OX + x, PT_OY + y, ' ', TUI_WHITE,
				PT.pix[y * PT.w + x]);
	if (PT.cx >= 0 && PT.cy >= 0 && PT.cx < PT.w && PT.cy < PT.h)
		tui_cursor(PT_OX + PT.cx, PT_OY + PT.cy, 1);

	/* Status / help line. */
	tui_pad(0, rows - 1, PT.status, cols, TUI_BRBLACK, TUI_BRYELLOW);

	{
		int hy = PT_OY + PT.h + 2;
		if (hy < rows - 1)
		{
			sprintf(line,
				"pencil/eraser  line  rect  circle  fill  pick  undo");
			tui_puts(PT_OX, hy, line, TUI_BRCYAN, TUI_BRBLACK);
			if (hy + 1 < rows - 1)
				tui_puts(PT_OX, hy + 1,
					 "arrows move  space draw  s save  x clear  Alt+X quit",
					 TUI_WHITE, TUI_BRBLACK);
		}
	}
	tui_flush();
}

static void pt_leave(void)
{
	tui_end();
	mmb_gfx_cls(G.gfx.bg);
	G.home_prompt = 0;
	memset(&PT, 0, sizeof(PT));
	mmb_console_write("\r\n");
	mmb_console_write(mmb_prompt());
}

/* ---- editing operations --------------------------------------------- */

static void pt_set_tool(int tool)
{
	PT.tool = tool;
	PT.have_anchor = 0;
	PT.have_base = 0;
	strncpy(PT.status, PT_TOOL_NAME[tool], sizeof(PT.status) - 1);
}

static void pt_move(int dx, int dy)
{
	PT.cx = pt_clamp(PT.cx + dx, 0, PT.w - 1);
	PT.cy = pt_clamp(PT.cy + dy, 0, PT.h - 1);
}

/* Apply the current tool at the cursor (keyboard Space/Enter). */
static void pt_apply(void)
{
	if (PT.cx < 0 || PT.cy < 0 || PT.cx >= PT.w || PT.cy >= PT.h)
		return;
	switch (PT.tool)
	{
	case PT_PENCIL:
	case PT_ERASER:
		pt_undo_push();
		pt_put(PT.cx, PT.cy, PT.colour);
		PT.dirty = 1;
		break;
	case PT_FILL:
		pt_undo_push();
		pt_flood(PT.cx, PT.cy, PT.colour);
		PT.dirty = 1;
		break;
	case PT_PICK:
		PT.colour = PT.pix[PT.cy * PT.w + PT.cx];
		pt_set_tool(PT_PENCIL);
		break;
	default:
		if (!PT.have_anchor)
		{
			PT.anchor_px = PT.cx;
			PT.anchor_py = PT.cy;
			PT.have_anchor = 1;
			pt_snapshot_base();
			strncpy(PT.status, "Move, then Space/Enter", sizeof(PT.status) - 1);
		}
		else
		{
			pt_restore_base();
			pt_undo_push();
			pt_draw_shape(PT.anchor_px, PT.anchor_py, PT.cx, PT.cy);
			PT.dirty = 1;
			PT.have_anchor = 0;
			PT.have_base = 0;
			pt_set_tool(PT.tool);
		}
		break;
	}
}

static void pt_clear(void)
{
	pt_undo_push();
	memset(PT.pix, 0, (unsigned)(PT.w * PT.h));
	PT.dirty = 1;
	strncpy(PT.status, "Cleared", sizeof(PT.status) - 1);
}

static void pt_cycle_colour(int d)
{
	int v = PT.colour + d;
	if (v < 0)
		v = 15;
	if (v > 15)
		v = 0;
	PT.colour = v;
	PT.have_anchor = 0;
}

static void pt_click_palette(int mx)
{
	int i = mx - PT_OX;
	if (i >= 0 && i < 16)
		PT.colour = i;
}

/* ---- key handling --------------------------------------------------- */

static int pt_escape(char c)
{
	if (PT.esc_state == PT_ESC_GOT)
	{
		if (c == '[')
		{
			PT.esc_state = PT_ESC_CSI;
			return 1;
		}
		if (c == 'O')
		{
			PT.esc_state = PT_ESC_SS3;
			return 1;
		}
		PT.esc_state = PT_ESC_NONE;
		return 0;
	}
	if (PT.esc_state == PT_ESC_SS3)
	{
		PT.esc_state = PT_ESC_NONE;
		return 1;
	}
	if (PT.esc_state == PT_ESC_CSI)
	{
		PT.esc_state = PT_ESC_NONE;
		if (c == 'A')
			pt_move(0, -1);
		else if (c == 'B')
			pt_move(0, 1);
		else if (c == 'C')
			pt_move(1, 0);
		else if (c == 'D')
			pt_move(-1, 0);
		return 1;
	}
	return 0;
}

static void pt_handle_key(char c)
{
	if (c == ' ' || c == '\r' || c == '\n')
		pt_apply();
	else if (c == 'p' || c == 'P')
		pt_set_tool(PT_PENCIL);
	else if (c == 'e' || c == 'E')
		pt_set_tool(PT_ERASER);
	else if (c == 'l' || c == 'L')
		pt_set_tool(PT_LINE);
	else if (c == 'r' || c == 'R')
		pt_set_tool(PT_RECT);
	else if (c == 'o' || c == 'O')
		pt_set_tool(PT_CIRCLE);
	else if (c == 'f' || c == 'F')
		pt_set_tool(PT_FILL);
	else if (c == 'i' || c == 'I')
		pt_set_tool(PT_PICK);
	else if (c == 'u' || c == 'U')
		pt_undo();
	else if (c == 'x' || c == 'X')
		pt_clear();
	else if (c == 's' || c == 'S')
		pt_save();
	else if (c == ']')
		pt_cycle_colour(1);
	else if (c == '[')
		pt_cycle_colour(-1);
	else if (c >= '0' && c <= '9')
		PT.colour = c - '0';
}

const char *mmb_paint_key(char c)
{
	G.outn = 0;
	G.out[0] = 0;
	if (!PT.active)
		return G.out;
	if (PT.alt_pend)
	{
		PT.alt_pend = 0;
		if (c == 'x' || c == 'X')
		{
			pt_leave();
			return G.out;
		}
	}
	if ((unsigned char)c == 1)
	{
		PT.alt_pend = 1;
		return G.out;
	}
	if (PT.esc_state && pt_escape(c))
	{
		if (PT.active)
			pt_redraw();
		return G.out;
	}
	if (c == 27)
	{
		PT.esc_state = PT_ESC_GOT;
		PT.esc_at = mmb_now_ms();
		return G.out;
	}
	if (c == 24) /* Ctrl+X */
	{
		pt_leave();
		return G.out;
	}
	pt_handle_key(c);
	if (PT.active)
		pt_redraw();
	return G.out;
}

void mmb_paint_poll(void)
{
	mmb_mouse_state m;
	int have, mx, my, px, py, left, right, changed = 0;

	if (!PT.active)
		return;
	if (PT.esc_state == PT_ESC_GOT &&
	    mmb_now_ms() - PT.esc_at >= PT_ESC_IDLE_MS)
	{
		PT.esc_state = PT_ESC_NONE;
		pt_leave();
		return;
	}

	/* Floor the scrollback ring and keep the frame fresh on every tick. */
	have = mmb_mouse_read(&m);
	if (!have)
	{
		if (PT.have_mouse)
		{
			PT.have_mouse = 0;
			pt_redraw();
		}
		return;
	}
	if (!PT.have_mouse)
	{
		PT.have_mouse = 1;
		changed = 1;
	}

	mx = m.x / PT_CELL_W;
	my = m.y / PT_CELL_H;
	px = mx - PT_OX;
	py = my - PT_OY;
	left = (m.buttons & 1) != 0;
	right = (m.buttons & 2) != 0;

	/* Clicking a palette swatch selects a colour. */
	if (left && !PT.mouse_down && my == 1)
	{
		pt_click_palette(mx);
		changed = 1;
	}
	else if (right && !PT.mouse_down && px >= 0 && py >= 0 && px < PT.w &&
		 py < PT.h)
	{
		PT.colour = PT.pix[py * PT.w + px];
		changed = 1;
	}

	if (left && !PT.mouse_down && px >= 0 && py >= 0 && px < PT.w && py < PT.h)
	{
		PT.mouse_down = 1;
		PT.last_px = px;
		PT.last_py = py;
		PT.cx = px;
		PT.cy = py;
		if (PT.tool == PT_FILL)
		{
			pt_undo_push();
			pt_flood(px, py, PT.colour);
			PT.dirty = 1;
		}
		else if (PT.tool == PT_PICK)
		{
			PT.colour = PT.pix[py * PT.w + px];
			pt_set_tool(PT_PENCIL);
		}
		else if (PT.tool == PT_PENCIL || PT.tool == PT_ERASER)
		{
			pt_undo_push();
			pt_put(px, py, PT.colour);
			PT.dirty = 1;
		}
		else
		{
			PT.anchor_px = px;
			PT.anchor_py = py;
			PT.have_anchor = 1;
			pt_snapshot_base();
			pt_draw_shape(px, py, px, py);
		}
		changed = 1;
	}
	else if (left && PT.mouse_down)
	{
		if (px >= 0 && py >= 0 && px < PT.w && py < PT.h)
		{
			if (PT.tool == PT_PENCIL || PT.tool == PT_ERASER)
			{
				if (px != PT.last_px || py != PT.last_py)
					pt_line(PT.last_px, PT.last_py, px, py, PT.colour);
			}
			else if (PT.have_anchor)
			{
				pt_restore_base();
				pt_draw_shape(PT.anchor_px, PT.anchor_py, px, py);
			}
			PT.last_px = px;
			PT.last_py = py;
			PT.cx = px;
			PT.cy = py;
			changed = 1;
		}
	}
	else if (!left && PT.mouse_down)
	{
		PT.mouse_down = 0;
		if (PT.have_anchor)
		{
			pt_restore_base();
			if (px >= 0 && py >= 0 && px < PT.w && py < PT.h)
			{
				pt_undo_push();
				pt_draw_shape(PT.anchor_px, PT.anchor_py, px, py);
			}
			PT.dirty = 1;
			PT.have_anchor = 0;
			PT.have_base = 0;
		}
		changed = 1;
	}
	else if (!left && !right && px >= 0 && py >= 0 && px < PT.w && py < PT.h &&
		 (px != PT.cx || py != PT.cy))
	{
		PT.cx = px;
		PT.cy = py;
		changed = 1;
	}

	if (changed && PT.active)
		pt_redraw();
}

int mmb_in_paint(void)
{
	return PT.active;
}

/* ---- entry ---------------------------------------------------------- */

static void pt_make_path(char *dst, int dstsz, const char *in)
{
	char tmp[160];
	const char *b;

	strncpy(tmp, in && in[0] ? in : "PAINT.PNG", sizeof(tmp) - 1);
	tmp[sizeof(tmp) - 1] = 0;
	b = pt_base(tmp);
	if (!strchr(b, '.'))
		strncat(tmp, ".png", sizeof(tmp) - strlen(tmp) - 1);
	if (!strchr(tmp, ':') && !strchr(tmp, '/'))
	{
		char full[160];
		sprintf(full, "A:/%s", tmp);
		mmb_vfs_resolve(full, dst, dstsz);
	}
	else
		mmb_vfs_resolve(tmp, dst, dstsz);
}

static void pt_open(const char *name, int have_w, int want_w, int have_h,
		    int want_h)
{
	uint32_t *img = 0;
	int iw = 0, ih = 0;
	int maxw, maxh, i;

	memset(&PT, 0, sizeof(PT));
	PT.colour = 4; /* red, as in the sprite editor */
	PT.tool = PT_PENCIL;
	pt_make_path(PT.path, sizeof(PT.path), name);
	strncpy(PT.label, pt_base(PT.path), sizeof(PT.label) - 1);
	PT.active = 1;

	maxw = pt_max_w();
	maxh = pt_max_h();
	if (pt_load_rgba(&img, &iw, &ih))
	{
		if (have_w && have_h)
		{
			PT.w = pt_clamp(want_w, 1, maxw);
			PT.h = pt_clamp(want_h, 1, maxh);
		}
		else if (iw >= 1 && ih >= 1 && iw <= maxw && ih <= maxh)
		{
			PT.w = iw;
			PT.h = ih;
		}
		else
		{
			PT.w = maxw;
			PT.h = maxh;
		}
	}
	else
	{
		PT.w = have_w ? pt_clamp(want_w, 1, maxw) : maxw;
		PT.h = have_h ? pt_clamp(want_h, 1, maxh) : maxh;
	}

	memset(PT.pix, 15, (unsigned)(PT.w * PT.h)); /* white paper */
	if (img)
	{
		int n = PT.w * PT.h;
		for (i = 0; i < n; i++)
		{
			int x = i % PT.w, y = i / PT.w;
			unsigned v;
			if (x >= iw || y >= ih)
				continue;
			v = img[y * iw + x];
			if ((v >> 24) == 0)
				continue;
			PT.pix[i] = (unsigned char)pt_nearest(v & 0xFFFFFFu);
		}
		G.plat->free(img);
		PT.dirty = 0;
		strncpy(PT.status, "Loaded", sizeof(PT.status) - 1);
	}
	else
	{
		PT.dirty = 0;
		strncpy(PT.status, "s save   Alt+X quit", sizeof(PT.status) - 1);
	}
	PT.status[sizeof(PT.status) - 1] = 0;
	mmb_hw_cursor(0);
	pt_redraw();
}

void mmb_cmd_paint(void)
{
	char name[160];
	int have_w = 0, have_h = 0, want_w = 0, want_h = 0;
	mmb_val v;

	name[0] = 0;
	mmb_skip_sp();
	if (*G.p && *G.p != ':' && *G.p != '\'')
	{
		v = mmb_expr();
		if (v.type != T_STR)
			mmb_syntax();
		strncpy(name, v.s, sizeof(name) - 1);
		name[sizeof(name) - 1] = 0;
		mmb_skip_sp();
		if (*G.p == ',')
		{
			G.p++;
			want_w = (int)mmb_as_int(mmb_expr());
			have_w = 1;
			mmb_skip_sp();
			if (*G.p == ',')
			{
				G.p++;
				want_h = (int)mmb_as_int(mmb_expr());
				have_h = 1;
			}
		}
	}
	pt_open(name, have_w, want_w, have_h, want_h);
}
