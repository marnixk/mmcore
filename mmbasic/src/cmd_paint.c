/*
 * PAINT - a Dr. Halo / Dr. Genius-style full-screen pixel paint app.
 *
 * This file is the scaffold (#634): it owns the lifecycle, the 640x360
 * layout, the per-frame event loop and the shared state declared in paint.h.
 * The palette, tools, undo, menus, cursor, file and text modules live in
 * their own .c files and are called through the hooks declared in paint.h.
 * Until those land, weak stub implementations at the bottom of this file keep
 * the tree building and lay out the chrome.
 *
 * Screen: MODE 18 (640x360). One canvas pixel is one screen pixel. The menu
 * bar is text row 0, the tool column runs down the left edge, the fixed VGA
 * palette is a 4x64 swatch strip across the bottom with the FG/BG indicator
 * at its left end, and the canvas starts black (palette index 0).
 *
 * PAINT needs a mouse: with none attached (and no test override) it prints a
 * clear message and returns to the prompt without touching the screen.
 */
#include "mmb_priv.h"
#include "tui.h"
#include "paint.h"

pt_state PT;

static int s_force_mouse;	/* test-only override (#633) */
static int s_alt_pend;
static int s_saved_mode, s_saved_bits;

/* Weak fallback hook so the module tickets can supply strong definitions. */
#if defined(__GNUC__) || defined(__clang__)
#define PT_WEAK __attribute__((weak))
#else
#define PT_WEAK
#endif

/* ---- test override ----------------------------------------------------- */

void pt_force_mouse(int on)
{
	s_force_mouse = on ? 1 : 0;
}

int pt_mouse_present(void)
{
	mmb_mouse_state m;

	if (s_force_mouse)
		return 1;
	if (mmb_mouse_read(&m) && m.present)
		return 1;
	return 0;
}

/* ---- screen drawing helpers ------------------------------------------- */

void pt_fill_rect(int x, int y, int w, int h, unsigned rgb)
{
	if (w < 1 || h < 1)
		return;
	if (!G.plat || !G.plat->tui_fill_px)
		return;
	G.plat->tui_fill_px(x, y, w, h, rgb);
}

void pt_plot(int x, int y, unsigned rgb)
{
	pt_fill_rect(x, y, 1, 1, rgb);
}

int pt_screen_to_canvas(int sx, int sy, int *cx, int *cy)
{
	int x = sx - PT_CANVAS_X;
	int y = sy - PT_CANVAS_Y;

	if (x < 0 || y < 0 || x >= PT.width || y >= PT.height)
		return 0;
	if (cx)
		*cx = x;
	if (cy)
		*cy = y;
	return 1;
}

int pt_canvas_get(int cx, int cy)
{
	if (!PT.canvas || cx < 0 || cy < 0 || cx >= PT.width || cy >= PT.height)
		return 0;
	return PT.canvas[(size_t)cy * PT.width + cx];
}

void pt_canvas_set(int cx, int cy, int idx)
{
	if (!PT.canvas || cx < 0 || cy < 0 || cx >= PT.width || cy >= PT.height)
		return;
	PT.canvas[(size_t)cy * PT.width + cx] = (unsigned char)(idx & 255);
}

void pt_draw_canvas(void)
{
	int x, y;

	if (!PT.canvas)
		return;
	pt_fill_rect(PT_CANVAS_X, PT_CANVAS_Y, PT_CANVAS_W, PT_CANVAS_H,
		     0x000000u);
	for (y = 0; y < PT.height; y++)
	{
		const unsigned char *row = PT.canvas + (size_t)y * PT.width;
		x = 0;
		while (x < PT.width)
		{
			unsigned char c = row[x];
			int x0 = x;
			while (x < PT.width && row[x] == c)
				x++;
			pt_fill_rect(PT_CANVAS_X + x0, PT_CANVAS_Y + y, x - x0, 1,
				     pt_palette_rgb(c));
		}
	}
}

void pt_present(void)
{
	if (G.plat && G.plat->tui_present)
		G.plat->tui_present(0, PT_H - 1);
}

void pt_request_redraw(void)
{
	PT.dirty = 1;
}

void pt_redraw(void)
{
	if (!PT.active)
		return;

	tui_begin();
	tui_clear(TUI_BRWHITE, TUI_BLACK);
	pt_menus_draw();
	tui_flush();

	pt_draw_canvas();
	pt_tools_draw();
	pt_palette_draw();
	pt_cursor_draw(PT.cursor_sx, PT.cursor_sy, PT.tool, PT.mouse_down);
	pt_present();

	PT.dirty = 0;
}

/* ---- lifecycle --------------------------------------------------------- */

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

static void pt_make_path(char *dst, int dstsz, const char *in)
{
	char tmp[160];
	const char *b;

	strncpy(tmp, in && in[0] ? in : "PAINT.PCX", sizeof(tmp) - 1);
	tmp[sizeof(tmp) - 1] = 0;
	b = pt_base(tmp);
	if (!strchr(b, '.'))
		strncat(tmp, ".pcx", sizeof(tmp) - strlen(tmp) - 1);
	if (!strchr(tmp, ':') && !strchr(tmp, '/'))
	{
		char full[160];
		sprintf(full, "A:/%s", tmp);
		mmb_vfs_resolve(full, dst, dstsz);
	}
	else
		mmb_vfs_resolve(tmp, dst, dstsz);
}

static void pt_leave(void)
{
	if (!PT.active)
		return;
	PT.active = 0;

	if (PT.canvas)
		G.plat->free(PT.canvas);
	if (PT.scratch)
		G.plat->free(PT.scratch);
	PT.canvas = 0;
	PT.scratch = 0;

	tui_end();
	if (s_saved_mode != G.gfx.mode || s_saved_bits != G.gfx.bits)
		mmb_gfx_set_mode(s_saved_mode, s_saved_bits);
	mmb_gfx_reset_console(1);
	G.home_prompt = 0;
	memset(&PT, 0, sizeof(PT));
	mmb_console_write("\r\n");
	mmb_console_write(mmb_prompt());
}

static void pt_enter(const char *name, int have_w, int want_w, int have_h,
		     int want_h)
{
	int w = have_w ? want_w : PT_CANVAS_W;
	int h = have_h ? want_h : PT_CANVAS_H;

	memset(&PT, 0, sizeof(PT));
	PT.menu = PT_MENU_NONE;
	PT.tool = PT_TOOL_PENCIL;
	PT.zoom = 1;
	PT.fg = 15;
	PT.bg = 0;

	w = pt_clamp(w, 1, PT_MAX_W);
	h = pt_clamp(h, 1, PT_MAX_H);
	PT.width = w;
	PT.height = h;
	PT.canvas = G.plat->alloc((unsigned)w * (unsigned)h);
	PT.scratch = G.plat->alloc((unsigned)w * (unsigned)h);
	if (!PT.canvas || !PT.scratch)
	{
		if (PT.canvas)
			G.plat->free(PT.canvas);
		if (PT.scratch)
			G.plat->free(PT.scratch);
		PT.canvas = 0;
		PT.scratch = 0;
		mmb_error("?OUT OF MEMORY");
		return;
	}
	memset(PT.canvas, 0, (unsigned)w * (unsigned)h); /* black */

	pt_make_path(PT.path, sizeof(PT.path), name);
	strncpy(PT.label, pt_base(PT.path), sizeof(PT.label) - 1);
	strncpy(PT.status, "PAINT", sizeof(PT.status) - 1);

	/* Fixed 640x360 display; remember the caller's mode for exit. */
	s_saved_mode = G.gfx.mode;
	s_saved_bits = G.gfx.bits;
	mmb_gfx_set_mode(PT_MODE, 8);

	pt_palette_init();
	pt_tools_init();
	pt_undo_init();
	pt_menus_init();
	pt_cursor_init();
	pt_file_init();
	pt_text_init();

	PT.cursor_x = PT.cursor_y = 0;
	PT.cursor_sx = PT_CANVAS_X;
	PT.cursor_sy = PT_CANVAS_Y;
	PT.active = 1;
	mmb_hw_cursor(0);
	pt_redraw();
}

/* ---- keyboard ---------------------------------------------------------- */

const char *mmb_paint_key(char c)
{
	G.outn = 0;
	G.out[0] = 0;
	if (!PT.active)
		return G.out;

	/* An open dialog owns the keyboard. */
	if (pt_file_dialog_active() && pt_file_key((unsigned char)c))
	{
		pt_request_redraw();
		return G.out;
	}
	if (pt_text_active() && pt_text_key((unsigned char)c))
	{
		pt_request_redraw();
		return G.out;
	}
	if (pt_menus_key((unsigned char)c))
	{
		pt_request_redraw();
		return G.out;
	}

	if ((unsigned char)c == 1)	/* Alt prefix */
	{
		s_alt_pend = 1;
		return G.out;
	}
	if (s_alt_pend)
	{
		s_alt_pend = 0;
		if (c == 'x' || c == 'X')
		{
			pt_leave();
			return G.out;
		}
	}

	if (c == 27)			/* Esc */
	{
		pt_leave();
		return G.out;
	}
	if (c == 24)			/* Ctrl+X */
	{
		pt_leave();
		return G.out;
	}
	return G.out;
}

/* ---- mouse / event loop ------------------------------------------------ */

void mmb_paint_poll(void)
{
	mmb_mouse_state m;
	int have, sx, sy, left, right, cx = 0, cy = 0, on, changed = 0;

	if (!PT.active)
		return;

	have = pt_mouse_present();
	if (!have)
	{
		if (PT.have_mouse)
		{
			PT.have_mouse = 0;
			pt_request_redraw();
		}
		if (PT.dirty)
			pt_redraw();
		return;
	}
	if (!PT.have_mouse)
	{
		PT.have_mouse = 1;
		changed = 1;
	}

	if (!mmb_mouse_read(&m))
	{
		m.x = PT.cursor_sx;
		m.y = PT.cursor_sy;
		m.buttons = 0;
	}
	sx = m.x;
	sy = m.y;
	left = (m.buttons & 1) != 0;
	right = (m.buttons & 2) != 0;

	if (sx != PT.cursor_sx || sy != PT.cursor_sy)
	{
		PT.cursor_sx = sx;
		PT.cursor_sy = sy;
		changed = 1;
	}
	on = pt_screen_to_canvas(sx, sy, &cx, &cy);
	if (on)
	{
		PT.cursor_x = cx;
		PT.cursor_y = cy;
	}

	if (!PT.mouse_down)
	{
		if (left || right)
		{
			int button = left ? PT_BTN_LEFT : PT_BTN_RIGHT;
			int idx, tool;

			if (pt_menus_mouse(sx, sy, button, 1))
				;
			else if (pt_palette_indicator_hit(sx, sy))
				pt_palette_swap();
			else if (pt_palette_hit(sx, sy, &idx))
				pt_palette_select(idx, button);
			else if (pt_tools_hit(sx, sy, &tool))
				pt_tool_select(tool);
			else if (on)
			{
				PT.mouse_down = 1;
				PT.mouse_button = button;
				PT.last_cx = cx;
				PT.last_cy = cy;
				pt_tool_begin(cx, cy, button);
			}
			changed = 1;
		}
	}
	else
	{
		int held = (PT.mouse_button == PT_BTN_RIGHT) ? right : left;

		if (held && on)
		{
			pt_tool_motion(cx, cy, PT.mouse_button);
			PT.last_cx = cx;
			PT.last_cy = cy;
			changed = 1;
		}
		else if (!held)
		{
			int rx = on ? cx : PT.last_cx;
			int ry = on ? cy : PT.last_cy;
			PT.mouse_down = 0;
			pt_tool_end(rx, ry, PT.mouse_button);
			changed = 1;
		}
	}

	if (changed)
		pt_request_redraw();
	if (PT.dirty)
		pt_redraw();
}

int mmb_in_paint(void)
{
	return PT.active;
}

/* ---- entry ------------------------------------------------------------- */

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

	/* No pointer, no PAINT. Leave the screen untouched for the prompt. */
	if (!pt_mouse_present())
	{
		mmb_console_write("PAINT needs a mouse: no pointer detected.\r\n");
		return;
	}

	pt_enter(name, have_w, want_w, have_h, want_h);
}

/* ======================================================================== *
 * Weak stubs for the module tickets. Strong definitions in paint_palette.c,
 * paint_tools.c, paint_undo.c, paint_menus.c, paint_cursors.c, paint_file.c
 * and paint_text.c override these at link time.
 * ======================================================================== */

/* ---- palette (#635) ---------------------------------------------------- */

static unsigned s_stub_pal[256];
static int s_stub_pal_ready;

PT_WEAK void pt_palette_init(void)
{
	static const unsigned ibm[16] = {
		0x000000u, 0x0000AAu, 0x00AA00u, 0x00AAAAu,
		0xAA0000u, 0xAA00AAu, 0xAA5500u, 0xAAAAAAu,
		0x555555u, 0x5555FFu, 0x55FF55u, 0x55FFFFu,
		0xFF5555u, 0xFF55FFu, 0xFFFF55u, 0xFFFFFFu
	};
	static const int lv[6] = { 0, 51, 102, 153, 204, 255 };
	int i, r, g, b;

	if (s_stub_pal_ready)
		return;
	for (i = 0; i < 16; i++)
		s_stub_pal[i] = ibm[i];
	i = 16;
	for (r = 0; r < 6; r++)
		for (g = 0; g < 6; g++)
			for (b = 0; b < 6; b++)
				s_stub_pal[i++] = ((unsigned)lv[r] << 16) |
						  ((unsigned)lv[g] << 8) |
						  (unsigned)lv[b];
	for (i = 232; i < 256; i++)
	{
		unsigned v = (unsigned)((i - 232) * 255 / 23);
		s_stub_pal[i] = (v << 16) | (v << 8) | v;
	}
	s_stub_pal_ready = 1;
}

PT_WEAK unsigned pt_palette_rgb(int idx)
{
	if (idx < 0)
		idx = 0;
	if (idx > 255)
		idx = 255;
	pt_palette_init();
	return s_stub_pal[idx];
}

PT_WEAK void pt_palette_draw(void)
{
	int r, c;

	pt_fill_rect(0, PT_PAL_Y, PT_W, PT_PAL_H, 0x000000u);
	for (r = 0; r < PT_PAL_ROWS; r++)
		for (c = 0; c < PT_PAL_COLS; c++)
		{
			int idx = r * PT_PAL_COLS + c;
			pt_fill_rect(PT_PAL_X + c * PT_PAL_SW,
				     PT_PAL_Y + r * PT_PAL_SW, PT_PAL_SW,
				     PT_PAL_SW, pt_palette_rgb(idx));
		}
	/* FG/BG indicator at the left end: outer BG, inner FG. */
	pt_fill_rect(PT_IND_X, PT_IND_Y, PT_IND_W, PT_IND_H, 0x202020u);
	pt_fill_rect(PT_IND_X + 6, PT_IND_Y + 6, PT_IND_W - 12, PT_IND_H - 12,
		     pt_palette_rgb(PT.bg));
	pt_fill_rect(PT_IND_X + 12, PT_IND_Y + 12, PT_IND_W - 24, PT_IND_H - 24,
		     pt_palette_rgb(PT.fg));
}

PT_WEAK int pt_palette_hit(int sx, int sy, int *idx)
{
	int c, r;

	if (sx < PT_PAL_X || sy < PT_PAL_Y ||
	    sx >= PT_PAL_X + PT_PAL_W || sy >= PT_PAL_Y + PT_PAL_H)
		return 0;
	c = (sx - PT_PAL_X) / PT_PAL_SW;
	r = (sy - PT_PAL_Y) / PT_PAL_SW;
	if (idx)
		*idx = r * PT_PAL_COLS + c;
	return 1;
}

PT_WEAK int pt_palette_indicator_hit(int sx, int sy)
{
	return sx >= PT_IND_X && sy >= PT_IND_Y &&
	       sx < PT_IND_X + PT_IND_W && sy < PT_IND_Y + PT_IND_H;
}

PT_WEAK void pt_palette_select(int idx, int button)
{
	if (button == PT_BTN_RIGHT)
		PT.bg = idx;
	else
		PT.fg = idx;
}

PT_WEAK void pt_palette_swap(void)
{
	int t = PT.fg;
	PT.fg = PT.bg;
	PT.bg = t;
}

/* ---- tools (#636, #641, #642) ------------------------------------------ */

PT_WEAK void pt_tools_init(void)
{
}

PT_WEAK void pt_tools_draw(void)
{
	int i;

	pt_fill_rect(0, PT_CANVAS_Y, PT_TOOL_W, PT_PAL_Y - PT_CANVAS_Y,
		     0x202020u);
	for (i = 0; i < PT_TOOL_COUNT; i++)
	{
		int y = PT_CANVAS_Y + i * PT_TOOL_W;
		unsigned c = (i == PT.tool) ? 0xFFFFFFu : 0x808080u;
		if (y + PT_TOOL_W > PT_PAL_Y)
			break;
		pt_fill_rect(1, y + 1, PT_TOOL_W - 2, PT_TOOL_W - 2,
			     (i == PT.tool) ? 0x404040u : 0x101010u);
		pt_fill_rect(4, y + 4, PT_TOOL_W - 8, PT_TOOL_W - 8, c);
	}
}

PT_WEAK int pt_tools_hit(int sx, int sy, int *tool)
{
	int row;

	if (sx < 0 || sx >= PT_TOOL_W || sy < PT_CANVAS_Y || sy >= PT_PAL_Y)
		return 0;
	row = (sy - PT_CANVAS_Y) / PT_TOOL_W;
	if (row < 0 || row >= PT_TOOL_COUNT)
		return 0;
	if (tool)
		*tool = row;
	return 1;
}

PT_WEAK void pt_tool_select(int tool)
{
	if (tool >= 0 && tool < PT_TOOL_COUNT)
		PT.tool = tool;
}

PT_WEAK void pt_tool_begin(int cx, int cy, int button)
{
	(void)cx;
	(void)cy;
	(void)button;
}

PT_WEAK void pt_tool_motion(int cx, int cy, int button)
{
	(void)cx;
	(void)cy;
	(void)button;
}

PT_WEAK void pt_tool_end(int cx, int cy, int button)
{
	(void)cx;
	(void)cy;
	(void)button;
}

PT_WEAK void pt_tool_cancel(void)
{
}

/* ---- undo (#637) ------------------------------------------------------- */

PT_WEAK void pt_undo_init(void)
{
}

PT_WEAK void pt_undo_push(void)
{
}

PT_WEAK void pt_undo(void)
{
	strncpy(PT.status, "Nothing to undo", sizeof(PT.status) - 1);
}

PT_WEAK void pt_redo(void)
{
	strncpy(PT.status, "Nothing to redo", sizeof(PT.status) - 1);
}

PT_WEAK void pt_undo_clear(void)
{
}

/* ---- menus (#638) ------------------------------------------------------ */

PT_WEAK void pt_menus_init(void)
{
}

PT_WEAK void pt_menus_draw(void)
{
	tui_pad(0, 0, "File   Edit   Help", tui_cols(), TUI_BRWHITE, TUI_BRBLUE);
}

PT_WEAK int pt_menus_mouse(int sx, int sy, int button, int down)
{
	(void)sx;
	(void)sy;
	(void)button;
	(void)down;
	return 0;
}

PT_WEAK int pt_menus_key(int key)
{
	(void)key;
	return 0;
}

PT_WEAK int pt_menus_active(void)
{
	return 0;
}

PT_WEAK void pt_menus_close(void)
{
}

/* ---- cursor (#639) ----------------------------------------------------- */

PT_WEAK void pt_cursor_init(void)
{
}

PT_WEAK void pt_cursor_draw(int sx, int sy, int tool, int active)
{
	(void)tool;
	(void)active;
	if (sx < 0 || sy < 0)
		return;
	pt_fill_rect(sx - 3, sy, 7, 1, 0xFFFFFFu);
	pt_fill_rect(sx, sy - 3, 1, 7, 0xFFFFFFu);
}

PT_WEAK void pt_cursor_restore(void)
{
}

/* ---- file (#640) ------------------------------------------------------- */

PT_WEAK void pt_file_init(void)
{
}

PT_WEAK void pt_file_new(void)
{
	if (PT.canvas)
		memset(PT.canvas, 0, (unsigned)PT.width * PT.height);
	strncpy(PT.status, "New canvas", sizeof(PT.status) - 1);
	pt_request_redraw();
}

PT_WEAK void pt_file_open(void)
{
	strncpy(PT.status, "Open not available yet", sizeof(PT.status) - 1);
}

PT_WEAK void pt_file_save(void)
{
	strncpy(PT.status, "Save not available yet", sizeof(PT.status) - 1);
}

PT_WEAK void pt_file_save_as(void)
{
	strncpy(PT.status, "Save as not available yet", sizeof(PT.status) - 1);
}

PT_WEAK int pt_file_dialog_active(void)
{
	return 0;
}

PT_WEAK int pt_file_key(int key)
{
	(void)key;
	return 0;
}

/* ---- text (#642) ------------------------------------------------------- */

PT_WEAK void pt_text_init(void)
{
}

PT_WEAK int pt_text_active(void)
{
	return 0;
}

PT_WEAK int pt_text_key(int key)
{
	(void)key;
	return 0;
}
