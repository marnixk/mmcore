/*
 * PAINT - a Deluxe Paint-style pixel paint app (Paintbrush / DPaint / MCGA).
 *
 * The canvas is a true bitmap framebuffer: one canvas pixel is one screen
 * pixel, drawn straight onto the HDMI framebuffer, not one TUI character
 * cell. Colours are byte indices into a 256-entry MCGA/VGA palette; indices
 * 0..15 are the classic IBM 16 so sketches read the same as before.
 *
 * Chrome follows Deluxe Paint: the canvas fills the left of the screen while
 * a narrow tool column runs down the right edge - brush shapes on top, the
 * painting tools below, then the nested FG/BG colour indicator and the
 * MCGA paint set. F9 hides the menu/title bars and F10 the tool column so
 * the canvas can be painted full-screen; the same keys bring them back.
 *
 * Mouse rules match DPaint: left button picks/uses the foreground colour,
 * right button the background. On the canvas left drag paints FG (the
 * brush), right drag paints BG (erase onto the page). `,` is the eyedropper
 * and samples the canvas into FG. Without a mouse the keyboard works on its
 * own (Tab swaps the active drawing colour). "+" in the header means FG,
 * "TAB" means the keyboard paints BG.
 *
 * Brushes: ten built-in shapes are implied by round/square plus size, and any
 * rectangle of the canvas can be grabbed with the scissor tool (`g`) to become
 * a custom brush; the BG index is transparent when it is stamped.
 *
 * Saving writes a plain RGB PNG the same size as the canvas (the same bitmap
 * format SPRITE uses) so sketches round-trip.
 */
#include "mmb_priv.h"
#include "tui.h"

#define PT_MAX_W 640
#define PT_MAX_H 480
#define PT_UNDO  MMB_UNDO_DEPTH

/* Screen-pixel layout. The canvas keeps its top-left origin so the pixel
 * coordinate math is stable; chrome is drawn around it. */
#define PT_MARGIN   8
#define PT_PX0      8	/* canvas left, screen pixels */
#define PT_PY0      48	/* canvas top, screen pixels (below the title bars) */
#define PT_STATUS_H 32	/* two text rows reserved at the bottom */
#define PT_PAL_COLS 16
#define PT_PAL_ROWS 16
#define PT_PAL_SW   8
#define PT_PAL_W    (PT_PAL_COLS * PT_PAL_SW) /* 128 */
#define PT_CELL     30	/* tool/brush icon cell, screen pixels */
#define PT_GAP      2

#define PT_PENCIL 0
#define PT_LINE   1
#define PT_RECT   2
#define PT_CIRCLE 3
#define PT_FILL   4
#define PT_ERASER 5
#define PT_PICK   6
#define PT_GRAB   7
#define PT_TOOL_MAX 8

#define PT_BR_ROUND  0
#define PT_BR_SQUARE 1
#define PT_BR_CUSTOM 2

/* Icon kinds for the right-hand tool column. */
#define PT_IC_PENCIL 0
#define PT_IC_LINE   1
#define PT_IC_RECT   2
#define PT_IC_CIRCLE 3
#define PT_IC_FILL   4
#define PT_IC_ERASER 5
#define PT_IC_PICK   6
#define PT_IC_GRAB   7
#define PT_IC_MAG    8
#define PT_IC_ROUND  9
#define PT_IC_SQUARE 10
#define PT_IC_CUSTOM 11

#define PT_ESC_NONE 0
#define PT_ESC_GOT  1
#define PT_ESC_CSI  2
#define PT_ESC_SS3  3
#define PT_ESC_IDLE_MS 60

#define PT_DLG_NONE   0
#define PT_DLG_OPEN   1
#define PT_DLG_SAVEAS 2
#define PT_DLG_COLOUR 3

#define PT_BG_PANEL 0x00202020u
#define PT_BG_DARK  0x00000000u
#define PT_INK      0x00DDDDDDu

typedef struct {
	int active;
	char path[160];
	char label[96];
	int w, h;
	int colour;	/* foreground index */
	int bg;		/* background index */
	int paint_bg;	/* keyboard draws with BG when set */
	int tool;
	int brush;	/* PT_BR_* */
	int brush_size;
	int cx, cy;
	int dirty;
	int esc_state, esc_at, esc_num, esc_has, alt_pend;
	int have_mouse;
	int mouse_down;
	int mouse_btn;	/* 1 left, 2 right */
	int stroke_col;
	int last_px, last_py;
	int have_anchor;
	int anchor_px, anchor_py;
	int zoom;
	int magnify;
	int view_x, view_y;
	int hide_menu, hide_tools, show_xy;
	char status[96];
	int dialog;
	int dlg_target;	/* 0 = FG, 1 = BG */
	char dlg[160];
	int dlglen;
	unsigned char *pix;
	unsigned char *base;
	unsigned char *undo;
	int *stack;
	int undo_n, undo_pos;
	int have_base;
	unsigned char *brush_pix;
	int brush_w, brush_h, brush_bg;
	int have_custom;
} pt_state;

static pt_state PT;

static unsigned pt_pal[256];
static int pt_pal_ready;

static void pt_make_path(char *dst, int dstsz, const char *in);
static void pt_open(const char *name, int have_w, int want_w, int have_h,
		    int want_h);
static void pt_set_tool(int tool);

static const char *const PT_TOOL_NAME[] = {
	"PENCIL", "LINE", "RECT", "CIRCLE", "FILL", "ERASER", "PICK", "GRAB"
};

static const char *const PT_BRUSH_NAME[] = { "ROUND", "SQUARE", "CUSTOM" };

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

/* ---- MCGA 256 palette ------------------------------------------------ */

/*
 * Build the indexed palette. 0..15 are the IBM 16 (unchanged from the old
 * cell-based app, so saved sketches look identical); 16..31 a grey ramp;
 * 32..247 a 6x6x6 RGB cube; 248..255 a few extra primaries. This is the
 * classic 256-colour MCGA look, exposed as swatches and by index.
 */
static void pt_pal_init(void)
{
	static const int lv[6] = { 0, 95, 135, 175, 215, 255 };
	int i, r, g, b;

	if (pt_pal_ready)
		return;
	for (i = 0; i < 16; i++)
		pt_pal[i] = mmb_ibm_colour(i);
	for (i = 0; i < 16; i++)
	{
		unsigned v = (unsigned)(i * 17);
		pt_pal[16 + i] = (v << 16) | (v << 8) | v;
	}
	i = 32;
	for (r = 0; r < 6; r++)
		for (g = 0; g < 6; g++)
			for (b = 0; b < 6; b++)
				pt_pal[i++] = ((unsigned)lv[r] << 16) |
					      ((unsigned)lv[g] << 8) |
					      (unsigned)lv[b];
	pt_pal[248] = 0x000000u;
	pt_pal[249] = 0x404040u;
	pt_pal[250] = 0x808080u;
	pt_pal[251] = 0xC0C0C0u;
	pt_pal[252] = 0xFF0000u;
	pt_pal[253] = 0x00FF00u;
	pt_pal[254] = 0x0000FFu;
	pt_pal[255] = 0xFFFF00u;
	pt_pal_ready = 1;
}

static unsigned pt_colour_rgb(int i)
{
	if (i < 0)
		i = 0;
	if (i > 255)
		i = 255;
	return pt_pal[i];
}

static int pt_nearest(unsigned rgb)
{
	int best = 0, bd = 1 << 30, i;
	int r = (int)((rgb >> 16) & 255), g = (int)((rgb >> 8) & 255);
	int b = (int)(rgb & 255);
	for (i = 0; i < 256; i++)
	{
		unsigned c = pt_pal[i];
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

/* ---- memory ---------------------------------------------------------- */

static void pt_free_buffers(void)
{
	if (PT.pix)
		G.plat->free(PT.pix);
	if (PT.base)
		G.plat->free(PT.base);
	if (PT.undo)
		G.plat->free(PT.undo);
	if (PT.stack)
		G.plat->free(PT.stack);
	if (PT.brush_pix)
		G.plat->free(PT.brush_pix);
	PT.pix = 0;
	PT.base = 0;
	PT.undo = 0;
	PT.stack = 0;
	PT.brush_pix = 0;
}

static int pt_alloc_buffers(int w, int h)
{
	unsigned n = (unsigned)w * (unsigned)h;

	PT.pix = G.plat->alloc(n);
	PT.base = G.plat->alloc(n);
	PT.undo = G.plat->alloc(n * PT_UNDO);
	PT.stack = G.plat->alloc(n * sizeof(int));
	if (!PT.pix || !PT.base || !PT.undo || !PT.stack)
	{
		pt_free_buffers();
		return -1;
	}
	return 0;
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
	int sp = 0;
	int target;
	if (sx < 0 || sy < 0 || sx >= PT.w || sy >= PT.h)
		return;
	target = PT.pix[sy * PT.w + sx];
	if (target == col)
		return;
	PT.pix[sy * PT.w + sx] = (unsigned char)col;
	PT.stack[sp++] = sy * PT.w + sx;
	while (sp > 0)
	{
		int p = PT.stack[--sp];
		int x = p % PT.w, y = p / PT.w;
		if (x > 0 && PT.pix[p - 1] == target)
		{
			PT.pix[p - 1] = (unsigned char)col;
			PT.stack[sp++] = p - 1;
		}
		if (x < PT.w - 1 && PT.pix[p + 1] == target)
		{
			PT.pix[p + 1] = (unsigned char)col;
			PT.stack[sp++] = p + 1;
		}
		if (y > 0 && PT.pix[p - PT.w] == target)
		{
			PT.pix[p - PT.w] = (unsigned char)col;
			PT.stack[sp++] = p - PT.w;
		}
		if (y < PT.h - 1 && PT.pix[p + PT.w] == target)
		{
			PT.pix[p + PT.w] = (unsigned char)col;
			PT.stack[sp++] = p + PT.w;
		}
	}
}

/* ---- brushes --------------------------------------------------------- */

/* Stamp the round brush of size n centred on (x,y). */
static void pt_stamp_round(int x, int y, int n, int col)
{
	int dx, dy, r = n / 2;
	for (dy = -r; dy <= r; dy++)
		for (dx = -r; dx <= r; dx++)
			if (dx * dx + dy * dy <= (n * n) / 4)
				pt_put(x + dx, y + dy, col);
}

/* Paste a grabbed custom brush centred on (x,y); its BG index is transparent. */
static void pt_stamp_custom(int x, int y)
{
	int i, j, ox, oy;
	if (!PT.have_custom || !PT.brush_pix)
		return;
	ox = PT.brush_w / 2;
	oy = PT.brush_h / 2;
	for (j = 0; j < PT.brush_h; j++)
		for (i = 0; i < PT.brush_w; i++)
		{
			int v = PT.brush_pix[j * PT.brush_w + i];
			if (v == PT.brush_bg)
				continue;
			pt_put(x - ox + i, y - oy + j, v);
		}
}

static void pt_stamp(int x, int y, int col)
{
	if (PT.brush == PT_BR_CUSTOM && PT.have_custom)
	{
		pt_stamp_custom(x, y);
		return;
	}
	if (PT.brush == PT_BR_SQUARE)
	{
		int h = (PT.brush_size - 1) / 2;
		pt_rect(x - h, y - h, x - h + PT.brush_size - 1,
			y - h + PT.brush_size - 1, col, 1);
		return;
	}
	pt_stamp_round(x, y, PT.brush_size, col);
}

static void pt_stamp_line(int x0, int y0, int x1, int y1, int col)
{
	int dx = pt_abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
	int dy = -pt_abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
	int err = dx + dy;
	for (;;)
	{
		pt_stamp(x0, y0, col);
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

/* Capture the rectangle [x0,y0]-[x1,y1] as the current brush (#610). */
static void pt_grab_brush(int x0, int y0, int x1, int y1)
{
	int t, bw, bh, i;
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
	bw = x1 - x0 + 1;
	bh = y1 - y0 + 1;
	if (bw < 1 || bh < 1)
		return;
	if (PT.brush_pix)
	{
		G.plat->free(PT.brush_pix);
		PT.brush_pix = 0;
	}
	PT.brush_pix = G.plat->alloc((unsigned)bw * (unsigned)bh);
	if (!PT.brush_pix)
	{
		strncpy(PT.status, "No room for brush", sizeof(PT.status) - 1);
		return;
	}
	for (i = 0; i < bh; i++)
		memcpy(PT.brush_pix + i * bw, PT.pix + (y0 + i) * PT.w + x0,
		       (unsigned)bw);
	PT.brush_w = bw;
	PT.brush_h = bh;
	PT.brush_bg = PT.bg;
	PT.have_custom = 1;
	PT.brush = PT_BR_CUSTOM;
	pt_set_tool(PT_PENCIL);
	sprintf(PT.status, "Brush %dx%d", bw, bh);
}

/* ---- undo / shape preview ------------------------------------------- */

static void pt_undo_push(void)
{
	unsigned n = (unsigned)PT.w * PT.h;
	if (!PT.undo)
		return;
	memcpy(PT.undo + (size_t)PT.undo_pos * n, PT.pix, n);
	PT.undo_pos = (PT.undo_pos + 1) % PT_UNDO;
	if (PT.undo_n < PT_UNDO)
		PT.undo_n++;
}

static void pt_undo(void)
{
	unsigned n = (unsigned)PT.w * PT.h;
	if (PT.undo_n <= 0)
	{
		strncpy(PT.status, "Nothing to undo", sizeof(PT.status) - 1);
		return;
	}
	PT.undo_pos = (PT.undo_pos - 1 + PT_UNDO) % PT_UNDO;
	memcpy(PT.pix, PT.undo + (size_t)PT.undo_pos * n, n);
	PT.undo_n--;
	PT.dirty = 1;
	strncpy(PT.status, "Undo", sizeof(PT.status) - 1);
}

static void pt_snapshot_base(void)
{
	unsigned n = (unsigned)PT.w * PT.h;
	memcpy(PT.base, PT.pix, n);
	PT.have_base = 1;
}

static void pt_restore_base(void)
{
	unsigned n = (unsigned)PT.w * PT.h;
	if (PT.have_base)
		memcpy(PT.pix, PT.base, n);
}

static void pt_draw_shape(int x0, int y0, int x1, int y1, int col)
{
	switch (PT.tool)
	{
	case PT_LINE:
		pt_stamp_line(x0, y0, x1, y1, col);
		break;
	case PT_RECT:
		pt_rect(x0, y0, x1, y1, col, 0);
		break;
	case PT_CIRCLE:
	{
		int dx = x1 - x0, dy = y1 - y0;
		int r = (pt_abs(dx) + pt_abs(dy)) / 2;
		pt_circle(x0, y0, r, col, 0);
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

	if (!PT.pix || n <= 0)
		return -1;
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

/* ---- layout --------------------------------------------------------- */

static int pt_scr_w(void)
{
	return tui_cols() * 8;
}

static int pt_scr_h(void)
{
	return tui_rows() * 16;
}

/* Left edge of the right-hand tool column. */
static int pt_col_x(void)
{
	return pt_scr_w() - PT_PAL_W - PT_MARGIN;
}

/* Top of the palette, anchored above the status rows. */
static int pt_pal_y(void)
{
	int y = pt_scr_h() - PT_STATUS_H - PT_MARGIN - PT_PAL_W;
	if (y < PT_PY0 + 8)
		y = PT_PY0 + 8;
	return y;
}

static int pt_brush_base(void)
{
	return PT_PY0;
}

static int pt_tools_base(void)
{
	return PT_PY0 + PT_CELL + 4;
}

/* Cell rectangle for tool/brush button index (4 per row). */
static void pt_cell_rect(int base, int index, int *x, int *y)
{
	int col = index % 4, row = index / 4;
	*x = pt_col_x() + col * (PT_CELL + PT_GAP);
	*y = base + row * (PT_CELL + PT_GAP);
}

static int pt_indicator_y(void)
{
	int y = pt_pal_y() - PT_CELL - 14;
	if (y < pt_tools_base() + 3 * (PT_CELL + PT_GAP))
		y = pt_tools_base() + 3 * (PT_CELL + PT_GAP);
	return y;
}

static int pt_max_w(void)
{
	int w = pt_col_x() - PT_PX0 - PT_MARGIN;
	if (w > PT_MAX_W)
		w = PT_MAX_W;
	if (w < 8)
		w = 8;
	return w;
}

static int pt_max_h(void)
{
	int h = pt_scr_h() - PT_PY0 - PT_STATUS_H;
	if (h > PT_MAX_H)
		h = PT_MAX_H;
	if (h < 8)
		h = 8;
	return h;
}

/* Pixel area the canvas may use, accounting for hidden chrome. */
static int pt_avail_w(void)
{
	int x1 = PT.hide_tools ? pt_scr_w() : pt_col_x() - PT_MARGIN;
	int w = x1 - PT_PX0;
	if (w < 1)
		w = 1;
	return w;
}

static int pt_avail_h(void)
{
	int y1 = PT.hide_menu ? pt_scr_h() : pt_scr_h() - PT_STATUS_H;
	int h = y1 - PT_PY0;
	if (h < 1)
		h = 1;
	return h;
}

static int pt_default_w(void)
{
	int w = pt_max_w();
	return w > 320 ? 320 : w;
}

static int pt_default_h(void)
{
	int h = pt_max_h();
	return h > 200 ? 200 : h;
}

/* ---- pixel rendering ------------------------------------------------ */

static void pt_fill(int x, int y, int w, int h, unsigned rgb)
{
	if (w < 1 || h < 1)
		return;
	if (G.plat && G.plat->tui_fill_px)
		G.plat->tui_fill_px(x, y, w, h, rgb);
}

static void pt_frame_px(int x, int y, int w, int h, unsigned rgb)
{
	if (w < 2 || h < 2)
		return;
	pt_fill(x, y, w, 1, rgb);
	pt_fill(x, y + h - 1, w, 1, rgb);
	pt_fill(x, y, 1, h, rgb);
	pt_fill(x + w - 1, y, 1, h, rgb);
}

/* Screen-space primitives used by the tool-column icons. */
static void pt_spx(int x, int y, unsigned c)
{
	pt_fill(x, y, 1, 1, c);
}

static void pt_sline(int x0, int y0, int x1, int y1, unsigned c)
{
	int dx = pt_abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
	int dy = -pt_abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
	int err = dx + dy;
	for (;;)
	{
		pt_spx(x0, y0, c);
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

static void pt_srect(int x, int y, int w, int h, unsigned c, int fill)
{
	int i, j;
	if (w < 1 || h < 1)
		return;
	if (fill)
	{
		pt_fill(x, y, w, h, c);
		return;
	}
	for (i = 0; i < w; i++)
	{
		pt_spx(x + i, y, c);
		pt_spx(x + i, y + h - 1, c);
	}
	for (j = 0; j < h; j++)
	{
		pt_spx(x, y + j, c);
		pt_spx(x + w - 1, y + j, c);
	}
}

static void pt_scircle(int cx, int cy, int r, unsigned c, int fill)
{
	int x = 0, y = r, d = 1 - r, i;
	if (r < 1)
	{
		pt_spx(cx, cy, c);
		return;
	}
	if (fill)
	{
		for (i = -r; i <= r; i++)
		{
			pt_spx(cx + i, cy, c);
		}
		while (x <= y)
		{
			for (i = -x; i <= x; i++)
			{
				pt_spx(cx + i, cy + y, c);
				pt_spx(cx + i, cy - y, c);
			}
			for (i = -y; i <= y; i++)
			{
				pt_spx(cx + i, cy + x, c);
				pt_spx(cx + i, cy - x, c);
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
		pt_spx(cx + x, cy + y, c);
		pt_spx(cx - x, cy + y, c);
		pt_spx(cx + x, cy - y, c);
		pt_spx(cx - x, cy - y, c);
		pt_spx(cx + y, cy + x, c);
		pt_spx(cx - y, cy + x, c);
		pt_spx(cx + y, cy - x, c);
		pt_spx(cx - y, cy - x, c);
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

/* Blit one colour-swatch button glyph. */
static void pt_icon(int x, int y, int sz, int kind, unsigned fg, unsigned bg)
{
	int x0 = x + 3, y0 = y + 3, x1 = x + sz - 4, y1 = y + sz - 4;
	pt_srect(x, y, sz, sz, bg, 1);
	switch (kind)
	{
	case PT_IC_PENCIL:
		pt_sline(x1, y0, x0 + 3, y1 - 3, fg);
		pt_sline(x1, y0 + 1, x0 + 3, y1 - 2, fg);
		pt_sline(x0, y1, x0, y1 - 3, fg);
		pt_sline(x0, y1, x0 + 3, y1 - 3, fg);
		break;
	case PT_IC_LINE:
		pt_sline(x0, y1, x1, y0, fg);
		pt_sline(x0, y1 - 1, x1 - 1, y0, fg);
		break;
	case PT_IC_RECT:
		pt_srect(x0, y0, x1 - x0 + 1, y1 - y0 + 1, fg, 0);
		break;
	case PT_IC_CIRCLE:
		pt_scircle((x0 + x1) / 2, (y0 + y1) / 2, (x1 - x0) / 2, fg, 0);
		break;
	case PT_IC_FILL:
		pt_srect(x0 + 3, y1 - 3, x1 - x0 - 6, 4, fg, 1);
		pt_srect(x0 + 5, y1 - 7, x1 - x0 - 10, 4, fg, 1);
		pt_sline(x1 - 1, y0, x1 - 1, y0 + 4, fg);
		pt_sline(x1 - 2, y0, x1 - 2, y0 + 4, fg);
		break;
	case PT_IC_ERASER:
		pt_srect(x0 + 2, y1 - 5, x1 - x0 - 4, 5, fg, 1);
		pt_sline(x0 + 4, y0 + 4, x1 - 2, y0 + 4, fg);
		break;
	case PT_IC_PICK:
		pt_sline(x0, y1, x1 - 2, y0 + 2, fg);
		pt_sline(x0 + 1, y1, x1 - 1, y0 + 3, fg);
		pt_srect(x0, y1 - 3, 4, 4, fg, 1);
		break;
	case PT_IC_GRAB:
		pt_srect(x0, y0, x1 - x0 + 1, y1 - y0 + 1, fg, 0);
		pt_sline(x0 + 3, y0, x0 + 3, y0 + 3, fg);
		pt_sline(x1 - 3, y0, x1 - 3, y0 + 3, fg);
		pt_sline(x0 + 3, y1, x0 + 3, y1 - 3, fg);
		pt_sline(x1 - 3, y1, x1 - 3, y1 - 3, fg);
		break;
	case PT_IC_MAG:
		pt_scircle(x0 + 5, y0 + 5, 5, fg, 0);
		pt_sline(x0 + 9, y0 + 9, x1, y1, fg);
		pt_sline(x0 + 10, y0 + 9, x1, y1 - 1, fg);
		break;
	case PT_IC_ROUND:
		pt_scircle((x0 + x1) / 2, (y0 + y1) / 2, (x1 - x0) / 2 - 1,
			   fg, 1);
		break;
	case PT_IC_SQUARE:
		pt_srect(x0 + 2, y0 + 2, x1 - x0 - 3, y1 - y0 - 3, fg, 1);
		break;
	case PT_IC_CUSTOM:
		pt_srect(x0 + 1, y0 + 1, 5, 5, fg, 1);
		pt_srect(x1 - 5, y1 - 5, 5, 5, fg, 1);
		pt_srect(x1 - 5, y0 + 1, 5, 5, fg, 0);
		pt_srect(x0 + 1, y1 - 5, 5, 5, fg, 0);
		break;
	default:
		break;
	}
}

/* ---- chrome rendering ----------------------------------------------- */

/* Blit the canvas at 1:1, batching horizontal runs of one colour. */
static void pt_draw_canvas(void)
{
	int x, y;
	pt_fill(PT_PX0, PT_PY0, pt_avail_w(), pt_avail_h(), PT_BG_DARK);
	if (PT.magnify && PT.zoom > 1)
	{
		int vw = pt_avail_w() / PT.zoom, vh = pt_avail_h() / PT.zoom;
		int vx, vy;
		for (vy = 0; vy < vh; vy++)
		{
			int cy = PT.view_y + vy;
			const unsigned char *row;
			if (cy < 0 || cy >= PT.h)
				continue;
			row = PT.pix + cy * PT.w;
			vx = 0;
			while (vx < vw)
			{
				int cx = PT.view_x + vx;
				unsigned char c;
				int run;
				if (cx < 0 || cx >= PT.w)
				{
					vx++;
					continue;
				}
				c = row[cx];
				run = 1;
				while (vx + run < vw &&
				       PT.view_x + vx + run < PT.w &&
				       row[PT.view_x + vx + run] == c)
					run++;
				pt_fill(PT_PX0 + vx * PT.zoom,
					PT_PY0 + vy * PT.zoom,
					run * PT.zoom, PT.zoom, pt_pal[c]);
				vx += run;
			}
		}
		return;
	}
	for (y = 0; y < PT.h; y++)
	{
		const unsigned char *row = PT.pix + y * PT.w;
		x = 0;
		while (x < PT.w)
		{
			unsigned char c = row[x];
			int x0 = x;
			while (x < PT.w && row[x] == c)
				x++;
			pt_fill(PT_PX0 + x0, PT_PY0 + y, x - x0, 1,
				pt_pal[c]);
		}
	}
}

static void pt_draw_palette(void)
{
	int px = pt_col_x(), py = pt_pal_y(), r, c;
	int bgx, bgy, fgx, fgy;
	unsigned border = 0x00AAAAAAu;

	pt_frame_px(px - 2, py - 2, PT_PAL_W + 4, PT_PAL_W + 4, border);
	for (r = 0; r < PT_PAL_ROWS; r++)
		for (c = 0; c < PT_PAL_COLS; c++)
		{
			int idx = r * PT_PAL_COLS + c;
			pt_fill(px + c * PT_PAL_SW, py + r * PT_PAL_SW,
				PT_PAL_SW, PT_PAL_SW, pt_pal[idx]);
		}
	bgx = px + (PT.bg % PT_PAL_COLS) * PT_PAL_SW;
	bgy = py + (PT.bg / PT_PAL_COLS) * PT_PAL_SW;
	pt_frame_px(bgx - 1, bgy - 1, PT_PAL_SW + 2, PT_PAL_SW + 2,
		    0x00FFFF00u);
	fgx = px + (PT.colour % PT_PAL_COLS) * PT_PAL_SW;
	fgy = py + (PT.colour / PT_PAL_COLS) * PT_PAL_SW;
	pt_frame_px(fgx - 2, fgy - 2, PT_PAL_SW + 4, PT_PAL_SW + 4,
		    0x00FFFFFFu);
}

/* Nested rectangles: outer = BG, inner = FG (DPaint colour indicator). */
static void pt_draw_indicator(void)
{
	int x = pt_col_x(), y = pt_indicator_y();
	int bx = x + 4, by = y + 4;

	pt_fill(x, y, PT_PAL_W, PT_CELL + 8, PT_BG_PANEL);
	pt_frame_px(bx - 2, by - 2, PT_CELL + 6, PT_CELL + 6, PT_INK);
	pt_fill(bx, by, PT_CELL, PT_CELL, pt_pal[PT.bg]);
	pt_frame_px(bx + 6, by + 6, 18, 18, 0x00FFFFFFu);
	pt_fill(bx + 8, by + 8, 14, 14, pt_pal[PT.colour]);
}

static void pt_draw_tools(void)
{
	int bx, by, i, sx, sy;
	static const int tool_icons[9] = {
		PT_IC_PENCIL, PT_IC_LINE, PT_IC_RECT, PT_IC_CIRCLE, PT_IC_FILL,
		PT_IC_ERASER, PT_IC_PICK, PT_IC_GRAB, PT_IC_MAG
	};

	/* Brush shapes (#610), a single row above the tools. */
	for (i = 0; i < 3; i++)
	{
		int kind = (i == PT_BR_ROUND) ? PT_IC_ROUND :
			   (i == PT_BR_SQUARE) ? PT_IC_SQUARE : PT_IC_CUSTOM;
		unsigned fg = PT_INK;
		pt_cell_rect(pt_brush_base(), i, &bx, &by);
		if (i == PT_BR_CUSTOM && !PT.have_custom)
			fg = 0x00707070u;
		pt_icon(bx, by, PT_CELL, kind, fg, PT_BG_PANEL);
		if (PT.brush == i)
			pt_frame_px(bx - 1, by - 1, PT_CELL + 2, PT_CELL + 2,
				    0x00FFFFFFu);
	}

	/* Painting tools, four per row. */
	for (i = 0; i < 9; i++)
	{
		unsigned fg = PT_INK;
		pt_cell_rect(pt_tools_base(), i, &sx, &sy);
		if (i == PT_IC_MAG && !PT.magnify)
			fg = PT_INK;
		pt_icon(sx, sy, PT_CELL, tool_icons[i], fg, PT_BG_PANEL);
		if (i < PT_TOOL_MAX && PT.tool == i)
			pt_frame_px(sx - 1, sy - 1, PT_CELL + 2, PT_CELL + 2,
				    0x00FFFFFFu);
		else if (i == PT_IC_MAG && PT.magnify)
			pt_frame_px(sx - 1, sy - 1, PT_CELL + 2, PT_CELL + 2,
				    0x00FFFFFFu);
	}
}

static void pt_draw_cursor(void)
{
	int x, y;
	unsigned c;
	int lum;
	unsigned ink;
	if (PT.cx < 0 || PT.cy < 0 || PT.cx >= PT.w || PT.cy >= PT.h)
		return;
	if (PT.magnify && PT.zoom > 1)
	{
		x = PT_PX0 + (PT.cx - PT.view_x) * PT.zoom;
		y = PT_PY0 + (PT.cy - PT.view_y) * PT.zoom;
		c = pt_pal[PT.pix[PT.cy * PT.w + PT.cx]];
		lum = (int)((c >> 16) & 255) + (int)((c >> 8) & 255) +
		      (int)(c & 255);
		ink = lum > 384 ? 0x000000u : 0xFFFFFFu;
		pt_frame_px(x, y, PT.zoom + 1, PT.zoom + 1, ink);
		return;
	}
	x = PT_PX0 + PT.cx;
	y = PT_PY0 + PT.cy;
	c = pt_pal[PT.pix[PT.cy * PT.w + PT.cx]];
	lum = (int)((c >> 16) & 255) + (int)((c >> 8) & 255) +
	      (int)(c & 255);
	ink = lum > 384 ? 0x000000u : 0xFFFFFFu;
	pt_fill(x - 1, y - 1, 3, 1, ink);
	pt_fill(x - 1, y + 1, 3, 1, ink);
	pt_fill(x - 1, y, 1, 1, ink);
	pt_fill(x + 1, y, 1, 1, ink);
}

static void pt_present_pixels(void)
{
	int sh = pt_scr_h();
	if (sh <= PT_PY0)
		return;
	if (G.plat && G.plat->tui_present)
		G.plat->tui_present(PT_PY0, sh - 1);
}

static void pt_redraw(void)
{
	int cols, rows;
	char line[192];

	if (!PT.active || !G.plat || !G.plat->tui_glyph || !PT.pix)
		return;
	cols = tui_cols();
	rows = tui_rows();
	tui_begin();
	tui_clear(TUI_BRWHITE, TUI_BLACK);

	if (!PT.hide_menu)
	{
		sprintf(line, "PAINT  %s  %dx%d  MCGA 256  %s", PT.label,
			PT.w, PT.h, PT.dirty ? "*" : " ");
		tui_pad(0, 0, line, cols, TUI_BRWHITE, TUI_BRBLUE);

		sprintf(line, " %s  FG %d  BG %d  %s %d  ZOOM %dx%s",
			PT_TOOL_NAME[PT.tool], PT.colour, PT.bg,
			PT_BRUSH_NAME[PT.brush], PT.brush_size, PT.zoom,
			PT.magnify ? "*" : "");
		if (PT.show_xy)
		{
			char xy[32];
			sprintf(xy, "  X %d Y %d", PT.cx, PT.cy);
			strncat(line, xy, sizeof(line) - strlen(line) - 1);
		}
		tui_pad(0, 1, line, cols, TUI_BRYELLOW, TUI_BRBLACK);
	}

	if (PT.dialog)
	{
		const char *prompt = "Open: ";
		if (PT.dialog == PT_DLG_SAVEAS)
			prompt = "Save as: ";
		else if (PT.dialog == PT_DLG_COLOUR &&
			 PT.dlg_target == 1)
			prompt = "BG colour 0-255: ";
		else if (PT.dialog == PT_DLG_COLOUR)
			prompt = "Colour 0-255: ";
		sprintf(line, "%s%s_", prompt, PT.dlg);
		tui_pad(0, rows - 2, line, cols, TUI_BLACK, TUI_BRCYAN);
	}
	else if (!PT.hide_menu)
		tui_pad(0, rows - 2,
			"p pencil e eraser l line r rect c circle f fill i pick g grab",
			cols, TUI_BRCYAN, TUI_BLACK);

	if (!PT.hide_menu)
		tui_pad(0, rows - 1,
			PT.status[0] ? PT.status
				     : "u undo x clear  k FG b BG  , pick  Tab draw  m magnify  < > zoom  = - size  F9/F10 hide",
			cols, TUI_BRBLACK, TUI_BRYELLOW);

	tui_flush();

	if (PT.hide_tools)
		pt_fill(pt_col_x() - PT_MARGIN, 0,
			pt_scr_w() - (pt_col_x() - PT_MARGIN), pt_scr_h(),
			PT_BG_DARK);
	pt_draw_canvas();
	if (!PT.hide_tools)
	{
		pt_draw_tools();
		pt_draw_indicator();
		pt_draw_palette();
	}
	pt_draw_cursor();
	pt_present_pixels();
}

static void pt_leave(void)
{
	pt_free_buffers();
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

static int pt_mag_view_w(void)
{
	int vw = pt_avail_w() / PT.zoom;
	return vw < 1 ? 1 : vw;
}

static int pt_mag_view_h(void)
{
	int vh = pt_avail_h() / PT.zoom;
	return vh < 1 ? 1 : vh;
}

static void pt_clamp_view(void)
{
	int maxx = PT.w - pt_mag_view_w();
	int maxy = PT.h - pt_mag_view_h();
	if (maxx < 0)
		maxx = 0;
	if (maxy < 0)
		maxy = 0;
	PT.view_x = pt_clamp(PT.view_x, 0, maxx);
	PT.view_y = pt_clamp(PT.view_y, 0, maxy);
}

static void pt_move(int dx, int dy)
{
	PT.cx = pt_clamp(PT.cx + dx, 0, PT.w - 1);
	PT.cy = pt_clamp(PT.cy + dy, 0, PT.h - 1);
	if (PT.magnify && PT.zoom > 1)
	{
		int vw = pt_mag_view_w(), vh = pt_mag_view_h();
		if (PT.cx < PT.view_x)
			PT.view_x = PT.cx;
		if (PT.cx >= PT.view_x + vw)
			PT.view_x = PT.cx - vw + 1;
		if (PT.cy < PT.view_y)
			PT.view_y = PT.cy;
		if (PT.cy >= PT.view_y + vh)
			PT.view_y = PT.cy - vh + 1;
		pt_clamp_view();
	}
}

/* Keyboard drawing colour: FG unless the user toggled to BG. */
static int pt_kb_colour(void)
{
	return PT.paint_bg ? PT.bg : PT.colour;
}

static void pt_pick_cursor(void)
{
	if (PT.cx < 0 || PT.cy < 0 || PT.cx >= PT.w || PT.cy >= PT.h)
		return;
	PT.colour = PT.pix[PT.cy * PT.w + PT.cx];
	sprintf(PT.status, "FG %d", PT.colour);
}

/* Apply the current tool at the cursor (keyboard Space/Enter). */
static void pt_apply(void)
{
	int col = pt_kb_colour();
	if (PT.cx < 0 || PT.cy < 0 || PT.cx >= PT.w || PT.cy >= PT.h)
		return;
	switch (PT.tool)
	{
	case PT_PENCIL:
	case PT_ERASER:
		pt_undo_push();
		pt_stamp(PT.cx, PT.cy, PT.tool == PT_ERASER ? PT.bg : col);
		PT.dirty = 1;
		break;
	case PT_FILL:
		pt_undo_push();
		pt_flood(PT.cx, PT.cy, col);
		PT.dirty = 1;
		break;
	case PT_PICK:
		pt_pick_cursor();
		pt_set_tool(PT_PENCIL);
		break;
	case PT_GRAB:
		if (!PT.have_anchor)
		{
			PT.anchor_px = PT.cx;
			PT.anchor_py = PT.cy;
			PT.have_anchor = 1;
			pt_snapshot_base();
			strncpy(PT.status, "Move, then Space/Enter",
				sizeof(PT.status) - 1);
		}
		else
		{
			pt_restore_base();
			pt_grab_brush(PT.anchor_px, PT.anchor_py, PT.cx,
				      PT.cy);
			PT.have_anchor = 0;
			PT.have_base = 0;
		}
		break;
	default:
		if (!PT.have_anchor)
		{
			PT.anchor_px = PT.cx;
			PT.anchor_py = PT.cy;
			PT.have_anchor = 1;
			pt_snapshot_base();
			strncpy(PT.status, "Move, then Space/Enter",
				sizeof(PT.status) - 1);
		}
		else
		{
			pt_restore_base();
			pt_undo_push();
			pt_draw_shape(PT.anchor_px, PT.anchor_py, PT.cx,
				      PT.cy, col);
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
	memset(PT.pix, 15, (unsigned)(PT.w * PT.h));
	PT.dirty = 1;
	strncpy(PT.status, "Cleared", sizeof(PT.status) - 1);
}

static void pt_cycle_colour(int d)
{
	int v = PT.colour + d;
	if (v < 0)
		v = 255;
	if (v > 255)
		v = 0;
	PT.colour = v;
	PT.have_anchor = 0;
}

static void pt_brush_size_set(int n)
{
	PT.brush_size = pt_clamp(n, 1, 64);
	sprintf(PT.status, "Brush %d", PT.brush_size);
}

static void pt_cycle_brush(void)
{
	if (PT.brush == PT_BR_ROUND)
		PT.brush = PT_BR_SQUARE;
	else if (PT.brush == PT_BR_SQUARE)
		PT.brush = PT.have_custom ? PT_BR_CUSTOM : PT_BR_ROUND;
	else
		PT.brush = PT_BR_ROUND;
	sprintf(PT.status, "Brush %s", PT_BRUSH_NAME[PT.brush]);
}

static void pt_zoom_center(void)
{
	int vw = pt_mag_view_w(), vh = pt_mag_view_h();
	PT.view_x = PT.cx - vw / 2;
	PT.view_y = PT.cy - vh / 2;
	pt_clamp_view();
}

static void pt_zoom_set(int z)
{
	PT.zoom = pt_clamp(z, 1, 16);
	if (PT.zoom > 1)
	{
		if (!PT.magnify)
			PT.magnify = 1;
		pt_zoom_center();
	}
	else
		PT.magnify = 0;
	sprintf(PT.status, "Zoom %dx", PT.zoom);
}

static void pt_toggle_magnify(void)
{
	if (PT.magnify)
	{
		PT.magnify = 0;
		strncpy(PT.status, "Magnify off", sizeof(PT.status) - 1);
	}
	else
	{
		if (PT.zoom < 2)
			PT.zoom = 2;
		PT.magnify = 1;
		pt_zoom_center();
		sprintf(PT.status, "Magnify %dx", PT.zoom);
	}
}

static void pt_dialog_begin(int kind, int target)
{
	PT.dialog = kind;
	PT.dlg_target = target;
	PT.dlglen = 0;
	PT.dlg[0] = 0;
	if (kind == PT_DLG_SAVEAS)
	{
		strncpy(PT.dlg, PT.path, sizeof(PT.dlg) - 1);
		PT.dlg[sizeof(PT.dlg) - 1] = 0;
		PT.dlglen = (int)strlen(PT.dlg);
	}
	/* The colour dialog starts empty so a fresh index can be typed. */
}

static void pt_dialog_accept(void)
{
	int kind = PT.dialog;
	int target = PT.dlg_target;
	char text[160];
	PT.dialog = 0;
	strncpy(text, PT.dlg, sizeof(text) - 1);
	text[sizeof(text) - 1] = 0;
	if (kind == PT_DLG_OPEN)
	{
		if (!text[0])
		{
			strncpy(PT.status, "Open cancelled",
				sizeof(PT.status) - 1);
			return;
		}
		/* pt_open() clears PT, so pass a copy, not PT.dlg. */
		pt_open(text, 0, 0, 0, 0);
		return;
	}
	if (kind == PT_DLG_SAVEAS)
	{
		if (!text[0])
		{
			strncpy(PT.status, "Save cancelled",
				sizeof(PT.status) - 1);
			return;
		}
		pt_make_path(PT.path, sizeof(PT.path), text);
		strncpy(PT.label, pt_base(PT.path), sizeof(PT.label) - 1);
		pt_save();
		return;
	}
	if (kind == PT_DLG_COLOUR)
	{
		int v = 0, i;
		for (i = 0; text[i]; i++)
			if (text[i] >= '0' && text[i] <= '9')
				v = v * 10 + (text[i] - '0');
		if (target == 1)
			PT.bg = pt_clamp(v, 0, 255);
		else
			PT.colour = pt_clamp(v, 0, 255);
	}
}

static void pt_dialog_key(char c)
{
	if (c == 27)
	{
		PT.dialog = 0;
		strncpy(PT.status, "Cancelled", sizeof(PT.status) - 1);
		return;
	}
	if (c == '\r' || c == '\n')
	{
		pt_dialog_accept();
		return;
	}
	if (c == 8 || c == 127)
	{
		if (PT.dlglen > 0)
			PT.dlg[--PT.dlglen] = 0;
		return;
	}
	if (c >= 32 && c < 127 && PT.dlglen < (int)sizeof(PT.dlg) - 1)
	{
		PT.dlg[PT.dlglen++] = c;
		PT.dlg[PT.dlglen] = 0;
	}
}

/* Open a dialog / run a file command from a key. */
static void pt_file_key(char c)
{
	if (PT.dialog)
	{
		pt_dialog_key(c);
		return;
	}
	if (c == 'o' || c == 'O')
	{
		pt_dialog_begin(PT_DLG_OPEN, 0);
		strncpy(PT.status, "Open", sizeof(PT.status) - 1);
	}
	else if (c == 's' || c == 'S')
	{
		pt_save();
	}
	else if (c == 'a' || c == 'A')
	{
		pt_dialog_begin(PT_DLG_SAVEAS, 0);
		strncpy(PT.status, "Save as", sizeof(PT.status) - 1);
	}
	else if (c == 'n' || c == 'N')
	{
		pt_clear();
	}
	PT.have_anchor = 0;
}

/* ---- key handling --------------------------------------------------- */

static int pt_escape(char c)
{
	if (PT.esc_state == PT_ESC_GOT)
	{
		if (c == '[')
		{
			PT.esc_state = PT_ESC_CSI;
			PT.esc_num = 0;
			PT.esc_has = 0;
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
		if (c >= '0' && c <= '9')
		{
			PT.esc_num = PT.esc_num * 10 + (c - '0');
			PT.esc_has = 1;
			return 1;
		}
		PT.esc_state = PT_ESC_NONE;
		if (c == '~')
		{
			/* F9 hides the menu bars, F10 the tool column (#609). */
			if (PT.esc_has && PT.esc_num == 20)
			{
				PT.hide_menu = !PT.hide_menu;
				strncpy(PT.status,
					PT.hide_menu ? "Menu hidden"
						     : "Menu shown",
					sizeof(PT.status) - 1);
			}
			else if (PT.esc_has && PT.esc_num == 21)
			{
				PT.hide_tools = !PT.hide_tools;
				strncpy(PT.status,
					PT.hide_tools ? "Tools hidden"
						      : "Tools shown",
					sizeof(PT.status) - 1);
			}
			return 1;
		}
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
	else if (c == 'c' || c == 'C')
		pt_set_tool(PT_CIRCLE);
	else if (c == 'f' || c == 'F')
		pt_set_tool(PT_FILL);
	else if (c == 'i' || c == 'I')
		pt_set_tool(PT_PICK);
	else if (c == 'g' || c == 'G')
		pt_set_tool(PT_GRAB);
	else if (c == 'm' || c == 'M')
		pt_toggle_magnify();
	else if (c == ',')
		pt_pick_cursor();
	else if (c == '.')
		pt_cycle_brush();
	else if (c == '>')
		pt_zoom_set(PT.zoom < 2 ? 2 : PT.zoom * 2);
	else if (c == '<')
		pt_zoom_set(PT.zoom <= 2 ? 1 : PT.zoom / 2);
	else if (c == '=')
		pt_brush_size_set(PT.brush_size + 1);
	else if (c == '-')
		pt_brush_size_set(PT.brush_size - 1);
	else if (c == 'h')
		pt_brush_size_set(PT.brush_size / 2);
	else if (c == 'H')
		pt_brush_size_set(PT.brush_size * 2);
	else if (c == 'b' || c == 'B')
	{
		pt_dialog_begin(PT_DLG_COLOUR, 1);
		strncpy(PT.status, "BG colour", sizeof(PT.status) - 1);
	}
	else if (c == 't' || c == 'T')
	{
		PT.show_xy = !PT.show_xy;
		sprintf(PT.status, "Coords %s", PT.show_xy ? "on" : "off");
	}
	else if (c == 'u' || c == 'U')
		pt_undo();
	else if (c == 'x' || c == 'X')
		pt_clear();
	else if (c == 'k' || c == 'K')
	{
		pt_dialog_begin(PT_DLG_COLOUR, 0);
		strncpy(PT.status, "FG colour", sizeof(PT.status) - 1);
	}
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

	if (PT.dialog)
	{
		if ((unsigned char)c == 1)
		{
			PT.alt_pend = 1;
			return G.out;
		}
		if (PT.alt_pend)
		{
			PT.alt_pend = 0;
			PT.dialog = 0;
			strncpy(PT.status, "Cancelled", sizeof(PT.status) - 1);
			pt_redraw();
			return G.out;
		}
		pt_dialog_key(c);
		if (PT.active)
			pt_redraw();
		return G.out;
	}

	if (PT.alt_pend)
	{
		PT.alt_pend = 0;
		if (c == 'x' || c == 'X')
		{
			pt_leave();
			return G.out;
		}
		if (c == 'o' || c == 'O')
		{
			pt_file_key('o');
			if (PT.active)
				pt_redraw();
			return G.out;
		}
		if (c == 's' || c == 'S')
		{
			pt_file_key('a');
			if (PT.active)
				pt_redraw();
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
	if (c == 26) /* Ctrl+Z: shared undo chord (#532) */
	{
		pt_undo();
		if (PT.active)
			pt_redraw();
		return G.out;
	}
	if (c == 9) /* Tab: keyboard FG/BG drawing toggle (#608) */
	{
		PT.paint_bg = !PT.paint_bg;
		sprintf(PT.status, "Drawing with %s",
			PT.paint_bg ? "BG" : "FG");
		if (PT.active)
			pt_redraw();
		return G.out;
	}
	if (c == 'o' || c == 'O' || c == 's' || c == 'S' || c == 'a' ||
	    c == 'A' || c == 'n' || c == 'N')
		pt_file_key(c);
	else
		pt_handle_key(c);
	if (PT.active)
		pt_redraw();
	return G.out;
}

/* ---- chrome hit testing --------------------------------------------- */

static int pt_pal_hit(int mx, int my, int *idx)
{
	int px = pt_col_x(), py = pt_pal_y();
	int c, r;
	if (mx < px || my < py || mx >= px + PT_PAL_W || my >= py + PT_PAL_W)
		return 0;
	c = (mx - px) / PT_PAL_SW;
	r = (my - py) / PT_PAL_SW;
	*idx = r * PT_PAL_COLS + c;
	return 1;
}

/* Returns 1 and fills type/index for brush buttons (0), tool buttons (1)
 * and the FG/BG indicator (2). */
static int pt_chrome_hit(int mx, int my, int *type, int *idx)
{
	int i, x, y;
	for (i = 0; i < 3; i++)
	{
		pt_cell_rect(pt_brush_base(), i, &x, &y);
		if (mx >= x && my >= y && mx < x + PT_CELL && my < y + PT_CELL)
		{
			*type = 0;
			*idx = i;
			return 1;
		}
	}
	for (i = 0; i < 9; i++)
	{
		pt_cell_rect(pt_tools_base(), i, &x, &y);
		if (mx >= x && my >= y && mx < x + PT_CELL && my < y + PT_CELL)
		{
			*type = 1;
			*idx = i;
			return 1;
		}
	}
	x = pt_col_x();
	y = pt_indicator_y();
	if (mx >= x && my >= y && mx < x + PT_PAL_W && my < y + PT_CELL + 8)
	{
		*type = 2;
		*idx = 0;
		return 1;
	}
	return 0;
}

/* ---- mouse canvas mapping ------------------------------------------- */

static int pt_canvas_at(int mx, int my, int *px, int *py)
{
	int x, y;
	if (PT.magnify && PT.zoom > 1)
	{
		if (mx < PT_PX0 || my < PT_PY0 ||
		    mx >= PT_PX0 + pt_mag_view_w() * PT.zoom ||
		    my >= PT_PY0 + pt_mag_view_h() * PT.zoom)
			return 0;
		x = PT.view_x + (mx - PT_PX0) / PT.zoom;
		y = PT.view_y + (my - PT_PY0) / PT.zoom;
	}
	else
	{
		x = mx - PT_PX0;
		y = my - PT_PY0;
	}
	if (x < 0 || y < 0 || x >= PT.w || y >= PT.h)
		return 0;
	*px = x;
	*py = y;
	return 1;
}

/* ---- mouse handling -------------------------------------------------- */

static void pt_mouse_press(int px, int py, int col, int btn)
{
	PT.mouse_down = 1;
	PT.mouse_btn = btn;
	PT.stroke_col = col;
	PT.last_px = px;
	PT.last_py = py;
	PT.cx = px;
	PT.cy = py;
	switch (PT.tool)
	{
	case PT_FILL:
		pt_undo_push();
		pt_flood(px, py, col);
		PT.dirty = 1;
		break;
	case PT_PICK:
		PT.colour = PT.pix[py * PT.w + px];
		break;
	case PT_PENCIL:
	case PT_ERASER:
		pt_undo_push();
		pt_stamp(px, py, PT.tool == PT_ERASER ? PT.bg : col);
		PT.dirty = 1;
		break;
	case PT_GRAB:
		PT.anchor_px = px;
		PT.anchor_py = py;
		PT.have_anchor = 1;
		pt_snapshot_base();
		break;
	default:
		PT.anchor_px = px;
		PT.anchor_py = py;
		PT.have_anchor = 1;
		pt_snapshot_base();
		pt_draw_shape(px, py, px, py, col);
		break;
	}
}

static void pt_mouse_drag(int px, int py)
{
	if (PT.tool == PT_PENCIL || PT.tool == PT_ERASER)
	{
		if (px != PT.last_px || py != PT.last_py)
			pt_stamp_line(PT.last_px, PT.last_py, px, py,
				      PT.tool == PT_ERASER ? PT.bg
							   : PT.stroke_col);
	}
	else if (PT.tool == PT_GRAB)
	{
		if (PT.have_anchor)
		{
			pt_restore_base();
			pt_rect(PT.anchor_px, PT.anchor_py, px, py,
				pt_kb_colour(), 0);
		}
	}
	else if (PT.have_anchor)
	{
		pt_restore_base();
		pt_draw_shape(PT.anchor_px, PT.anchor_py, px, py,
			      PT.stroke_col);
	}
	PT.last_px = px;
	PT.last_py = py;
	PT.cx = px;
	PT.cy = py;
}

static void pt_mouse_release(int px, int py)
{
	PT.mouse_down = 0;
	if (PT.tool == PT_GRAB)
	{
		pt_restore_base();
		if (PT.have_anchor)
			pt_grab_brush(PT.anchor_px, PT.anchor_py, px, py);
		PT.have_anchor = 0;
		PT.have_base = 0;
		return;
	}
	if (PT.have_anchor)
	{
		pt_restore_base();
		if (px >= 0 && py >= 0 && px < PT.w && py < PT.h)
		{
			pt_undo_push();
			pt_draw_shape(PT.anchor_px, PT.anchor_py, px, py,
				      PT.stroke_col);
		}
		PT.dirty = 1;
		PT.have_anchor = 0;
		PT.have_base = 0;
	}
}

void mmb_paint_poll(void)
{
	mmb_mouse_state m;
	int have, mx, my, px, py, left, right, changed = 0, hidx;
	int type, idx;

	if (!PT.active)
		return;
	if (PT.esc_state == PT_ESC_GOT && !PT.dialog &&
	    mmb_now_ms() - PT.esc_at >= PT_ESC_IDLE_MS)
	{
		PT.esc_state = PT_ESC_NONE;
		pt_leave();
		return;
	}
	if (PT.dialog)
		return;

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

	mx = m.x;
	my = m.y;
	left = (m.buttons & 1) != 0;
	right = (m.buttons & 2) != 0;

	if (!PT.mouse_down)
	{
		if (left || right)
		{
			if (pt_pal_hit(mx, my, &hidx))
			{
				if (left)
					PT.colour = hidx;
				else
					PT.bg = hidx;
				changed = 1;
			}
			else if (pt_chrome_hit(mx, my, &type, &idx))
			{
				if (type == 2)
				{
					int t = PT.colour;
					PT.colour = PT.bg;
					PT.bg = t;
					strncpy(PT.status, "FG/BG swapped",
						sizeof(PT.status) - 1);
				}
				else if (type == 0)
				{
					if (idx != PT_BR_CUSTOM ||
					    PT.have_custom)
						PT.brush = idx;
				}
				else if (idx < PT_TOOL_MAX)
					pt_set_tool(idx);
				else
					pt_toggle_magnify();
				changed = 1;
			}
			else if (pt_canvas_at(mx, my, &px, &py))
			{
				int col = right ? PT.bg : PT.colour;
				pt_mouse_press(px, py, col, left ? 1 : 2);
				changed = 1;
			}
		}
		else if (pt_canvas_at(mx, my, &px, &py))
		{
			if (px != PT.cx || py != PT.cy)
			{
				PT.cx = px;
				PT.cy = py;
				changed = 1;
			}
		}
	}
	else
	{
		int held = (PT.mouse_btn == 1) ? left : right;
		if (held && pt_canvas_at(mx, my, &px, &py))
		{
			pt_mouse_drag(px, py);
			changed = 1;
		}
		else if (!held)
		{
			if (!pt_canvas_at(mx, my, &px, &py))
			{
				px = PT.cx;
				py = PT.cy;
			}
			pt_mouse_release(px, py);
			changed = 1;
		}
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

	pt_free_buffers();
	memset(&PT, 0, sizeof(PT));
	pt_pal_init();
	PT.colour = 4; /* red, as in the sprite editor */
	PT.bg = 15;    /* white page colour, so RMB erases onto paper */
	PT.tool = PT_PENCIL;
	PT.brush = PT_BR_ROUND;
	PT.brush_size = 1;
	PT.zoom = 1;
	PT.show_xy = 1;
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
		PT.w = have_w ? pt_clamp(want_w, 1, maxw) : pt_default_w();
		PT.h = have_h ? pt_clamp(want_h, 1, maxh) : pt_default_h();
	}

	if (pt_alloc_buffers(PT.w, PT.h) != 0)
	{
		PT.active = 0;
		mmb_error("?OUT OF MEMORY");
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
		strncpy(PT.status, "o open  s save  a save as",
			sizeof(PT.status) - 1);
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
