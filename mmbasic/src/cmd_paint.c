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

pt_state pt_console_state[MMB_MAX_CONSOLES];

static int s_force_mouse;	/* test-only override (#633) */
/* Key/timer state is per console too: a switch can land while PAINT is
 * mid-sequence or holding an in-flight Esc, and the interrupted console must
 * not inherit the other screen's pending key. */
static int s_alt_pend[MMB_MAX_CONSOLES];
static int s_saved_mode[MMB_MAX_CONSOLES], s_saved_bits[MMB_MAX_CONSOLES];

/* ---- escape sequences (#726) ------------------------------------------- *
 * Terminal navigation keys (arrows, Home/End/PageUp/Down, function keys,
 * Shift+arrows) arrive as multi-byte CSI/SS3 sequences and the REPL front end
 * replays them byte-by-byte, so PAINT sees the leading 0x1b before the rest
 * arrives. Buffer it the way WordPad does (cmd_wordpad.c:handle_escape): a
 * complete sequence is consumed, while a lone Esc resolves in mmb_paint_poll()
 * after the idle window and only then quits / closes a menu. */
#define PT_ESC_IDLE_MS 60
enum { PT_ESC_NONE = 0, PT_ESC_GOT, PT_ESC_CSI, PT_ESC_SS3 };
static int s_esc_state[MMB_MAX_CONSOLES];
static unsigned s_esc_at[MMB_MAX_CONSOLES];

/* ---- frame damage (#700) ---------------------------------------------- *
 * One screen-space bounding box (inclusive pixels) per frame. Every edit
 * unions its rectangle here; pt_redraw() recomposites only that box and
 * pt_present() DMAs only those rows. This replaces the old full-canvas blit
 * plus full-frame present that flickered on real hardware. */
/* One frame's damage per virtual console: a pending band on one screen must
 * not be presented or cleared by another console's redraw (#766). */
typedef struct {
	int dmg_valid;
	int dmg_x0, dmg_y0, dmg_x1, dmg_y1;
	/* The cursor only needs its rectangle presented; its saved background
	 * makes a canvas recomposite unnecessary, and recompositing would erase
	 * any overlay text it sits on. So cursor motion unions into a separate
	 * present band. */
	int pres_valid;
	int pres_x0, pres_y0, pres_x1, pres_y1;
	int full_frame;		/* this frame is a full chrome+canvas repaint */
} pt_frame_state;

static pt_frame_state s_frame_state[MMB_MAX_CONSOLES];
#define FRAME (s_frame_state[g_console])

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
	pt_damage_canvas(cx, cy, 1, 1);
}

/* Clamp a rectangle to the screen and union it into a box. Returns 0 when the
 * rectangle is empty after clamping. */
static int box_union(int *valid, int *bx0, int *by0, int *bx1, int *by1,
		     int x, int y, int w, int h)
{
	int x1, y1;

	if (w < 1 || h < 1)
		return 0;
	x1 = x + w - 1;
	y1 = y + h - 1;
	if (x < 0)
		x = 0;
	if (y < 0)
		y = 0;
	if (x1 > PT_W - 1)
		x1 = PT_W - 1;
	if (y1 > PT_H - 1)
		y1 = PT_H - 1;
	if (x1 < x || y1 < y)
		return 0;

	if (!*valid)
	{
		*bx0 = x;
		*by0 = y;
		*bx1 = x1;
		*by1 = y1;
		*valid = 1;
	}
	else
	{
		if (x < *bx0)
			*bx0 = x;
		if (y < *by0)
			*by0 = y;
		if (x1 > *bx1)
			*bx1 = x1;
		if (y1 > *by1)
			*by1 = y1;
	}
	return 1;
}

void pt_damage(int x, int y, int w, int h)
{
	box_union(&FRAME.dmg_valid, &FRAME.dmg_x0, &FRAME.dmg_y0, &FRAME.dmg_x1, &FRAME.dmg_y1,
		  x, y, w, h);
	PT.dirty = 1;
}

void pt_damage_canvas(int x, int y, int w, int h)
{
	pt_damage(PT_CANVAS_X + x, PT_CANVAS_Y + y, w, h);
}

void pt_damage_present(int x, int y, int w, int h)
{
	box_union(&FRAME.pres_valid, &FRAME.pres_x0, &FRAME.pres_y0, &FRAME.pres_x1,
		  &FRAME.pres_y1, x, y, w, h);
	PT.dirty = 1;
}

/* Blit only the damaged canvas region (or the whole canvas on a full frame).
 * No black under-fill for partial damage: every pixel in the region is written
 * from PT.canvas, so nothing flashes. */
void pt_draw_canvas(void)
{
	int x0, y0, x1, y1, x, y;

	if (!PT.canvas)
		return;

	if (FRAME.full_frame)
	{
		x0 = 0;
		y0 = 0;
		x1 = PT.width - 1;
		y1 = PT.height - 1;
		/* Cover any margin the canvas does not reach. */
		pt_fill_rect(PT_CANVAS_X, PT_CANVAS_Y, PT_CANVAS_W, PT_CANVAS_H,
			     0x000000u);
	}
	else if (!FRAME.dmg_valid)
	{
		return;
	}
	else
	{
		if (FRAME.dmg_x1 < PT_CANVAS_X || FRAME.dmg_y1 < PT_CANVAS_Y ||
		    FRAME.dmg_x0 > PT_CANVAS_X + PT.width - 1 ||
		    FRAME.dmg_y0 > PT_CANVAS_Y + PT.height - 1)
			return;
		x0 = FRAME.dmg_x0 - PT_CANVAS_X;
		y0 = FRAME.dmg_y0 - PT_CANVAS_Y;
		x1 = FRAME.dmg_x1 - PT_CANVAS_X;
		y1 = FRAME.dmg_y1 - PT_CANVAS_Y;
		if (x0 < 0)
			x0 = 0;
		if (y0 < 0)
			y0 = 0;
		if (x1 > PT.width - 1)
			x1 = PT.width - 1;
		if (y1 > PT.height - 1)
			y1 = PT.height - 1;
	}
	if (x1 < x0 || y1 < y0)
		return;

	for (y = y0; y <= y1; y++)
	{
		const unsigned char *row = PT.canvas + (size_t)y * PT.width;

		x = x0;
		while (x <= x1)
		{
			unsigned char c = row[x];
			int sx = x;

			while (x <= x1 && row[x] == c)
				x++;
			pt_fill_rect(PT_CANVAS_X + sx, PT_CANVAS_Y + y, x - sx,
				     1, pt_palette_rgb(c));
		}
	}
}

void pt_present(void)
{
	int y0, y1;

	if (!G.plat || !G.plat->tui_present)
		return;
	if (!FRAME.dmg_valid && !FRAME.pres_valid)
	{
		G.plat->tui_present(0, PT_H - 1);
		return;
	}
	y0 = FRAME.dmg_valid ? FRAME.dmg_y0 : PT_H;
	y1 = FRAME.dmg_valid ? FRAME.dmg_y1 : -1;
	if (FRAME.pres_valid)
	{
		if (FRAME.pres_y0 < y0)
			y0 = FRAME.pres_y0;
		if (FRAME.pres_y1 > y1)
			y1 = FRAME.pres_y1;
	}
	G.plat->tui_present(y0, y1);
}

void pt_request_redraw(void)
{
	PT.full_redraw = 1;
	PT.dirty = 1;
}

void pt_redraw(void)
{
	int full;

	if (!PT.active)
		return;

	full = PT.full_redraw;
	PT.full_redraw = 0;
	FRAME.full_frame = full;

	if (full)
		pt_damage(0, 0, PT_W, PT_H);

	/* Lift the previous sprite before recomposing, so the saved block can
	 * never revert a freshly drawn pixel (#701). pt_cursor_restore() damages
	 * its own rectangle. */
	pt_cursor_restore();

	if (!FRAME.dmg_valid && !FRAME.pres_valid)
	{
		PT.dirty = 0;
		FRAME.full_frame = 0;
		return;
	}

	tui_begin();
	if (full)
		tui_invalidate();

	/* Resync any stale text cells (spaces, or a previous frame's overlay)
	 * into the composition buffer *before* the raw pixel passes, so a blit
	 * can never paint blank cells over the canvas / chrome. */
	tui_flush_no_present();

	/* Canvas first: this also paints over any old dropdown / dialog pixels
	 * whose rectangle was damaged when it closed. */
	pt_draw_canvas();

	/* The selection marquee is drawn over the canvas as screen pixels; it is
	 * never part of PT.canvas, so a PCX save never carries it. */
	pt_select_draw();

	/* Chrome is persistent: the tool column and palette only change when
	 * PT.full_redraw is set, so a cursor move or a menu hover never touches
	 * them. The exception is a dropdown at menu column 1, which overlaps the
	 * tool column: pt_draw_canvas only repaints x >= PT_CANVAS_X, so the part
	 * of a closed/switched dropdown over the tools would otherwise linger.
	 * Damage there forces the column back before pt_menus_draw overlays. */
	if (full)
	{
		pt_tools_draw();
		pt_palette_draw();
		pt_width_draw();
	}
	else if (FRAME.dmg_valid && FRAME.dmg_x0 < PT_TOOL_W &&
		 FRAME.dmg_y1 >= PT_CANVAS_Y && FRAME.dmg_y0 < PT_PAL_Y)
	{
		pt_tools_draw();
	}

	/* Compose the menu bar / open dropdown / dialog. paint_menus.c keeps the
	 * text-cell bookkeeping (blank + accept / invalidate) scoped to the
	 * overlay rectangle and marks its damage in the event handlers. */
	pt_menus_draw();

	/* The file picker is a modal overlay too (paint_file.c owns its cell
	 * bookkeeping). Composed here so it survives the frame instead of being
	 * painted once and wiped. */
	pt_file_draw();

	/* Write changed text cells into the composition buffer without presenting:
	 * the single damage-band present below carries them too. */
	tui_flush_no_present();

	/* Capture the cursor background from the finished frame and stamp the
	 * sprite last, so it never samples itself and never gets overpainted.
	 * While a menu/dialog is open the pointer is forced to the UI arrow even
	 * over the canvas its dropdown covers (#788). */
	pt_cursor_draw(PT.cursor_sx, PT.cursor_sy, PT.tool, PT.mouse_down,
		pt_menus_active());

	pt_present();

	PT.dirty = 0;
	FRAME.dmg_valid = 0;
	FRAME.pres_valid = 0;
	FRAME.full_frame = 0;
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
	s_esc_state[g_console] = PT_ESC_NONE;
	s_alt_pend[g_console] = 0;

	if (PT.canvas)
		G.plat->free(PT.canvas);
	if (PT.scratch)
		G.plat->free(PT.scratch);
	PT.canvas = 0;
	PT.scratch = 0;

	tui_end();
	if (s_saved_mode[g_console] != G.gfx.mode ||
	    s_saved_bits[g_console] != G.gfx.bits)
		mmb_gfx_set_mode(s_saved_mode[g_console], s_saved_bits[g_console]);
	mmb_gfx_reset_console(1);
	G.home_prompt = 0;
	/* Release this console's heap-backed module state (undo snapshots, text
	 * glyphs, tool brush, selection clipboard / float) before the session
	 * struct is cleared (#766). */
	pt_undo_clear();
	pt_text_init();
	pt_tools_init();
	pt_select_init();
	pt_file_init();
	memset(&PT, 0, sizeof(PT));
	mmb_console_write("\r\n");
	mmb_console_write(mmb_prompt());
}

/* Force the lifecycle teardown without a discard prompt. Used by the warm
 * reset (session.c), which must drop whatever is on screen before the
 * interpreter is rebuilt. */
void pt_paint_leave(void)
{
	pt_leave();
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
	s_esc_state[g_console] = PT_ESC_NONE;
	s_alt_pend[g_console] = 0;

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
	s_saved_mode[g_console] = G.gfx.mode;
	s_saved_bits[g_console] = G.gfx.bits;
	mmb_gfx_set_mode(PT_MODE, 8);

	pt_palette_init();
	pt_tools_init();
	pt_undo_init();
	pt_menus_init();
	pt_cursor_init();
	pt_file_init();
	pt_text_init();
	pt_select_init();

	PT.cursor_x = PT.cursor_y = 0;
	/* Start well inside the canvas: the tool-sprite hotspots sit low/right
	 * in their 32x32 art, so the old top-left corner left the first frame's
	 * cursor clipped off-screen (#PAINT no-cursor-on-entry). */
	PT.cursor_sx = PT_CANVAS_X + 32;
	PT.cursor_sy = PT_CANVAS_Y + 32;
	PT.active = 1;
	mmb_hw_cursor(0);
	PT.full_redraw = 1;
	pt_redraw();
}

/* ---- keyboard ---------------------------------------------------------- */

const char *mmb_paint_key(char c)
{
	G.outn = 0;
	G.out[0] = 0;
	if (!PT.active)
		return G.out;

	/* An open dialog owns the keyboard. The modules mark their own damage
	 * (file/text request a full frame; menus damage only the menu band). */
	if (pt_file_dialog_active() && pt_file_key((unsigned char)c))
		return G.out;
	if (pt_text_active() && pt_text_key((unsigned char)c))
		return G.out;
	/* Decode an escape sequence before the menu sees the byte: the leading
	 * 0x1b of an arrow key is otherwise read as an Esc that closes the menu
	 * before the rest of the sequence arrives. A complete CSI/SS3 navigation
	 * key becomes a PT_KEY_* code for pt_menus_key(); a lone Esc is buffered
	 * and resolved by mmb_paint_poll() after PT_ESC_IDLE_MS. */
	if (s_esc_state[g_console] != PT_ESC_NONE)
	{
		if (s_esc_state[g_console] == PT_ESC_GOT)
		{
			if (c == '[')
			{
				s_esc_state[g_console] = PT_ESC_CSI;
				return G.out;
			}
			if (c == 'O')
			{
				s_esc_state[g_console] = PT_ESC_SS3;
				return G.out;
			}
			/* Not a sequence: drop the Esc and process c as usual. */
			s_esc_state[g_console] = PT_ESC_NONE;
		}
		else if (s_esc_state[g_console] == PT_ESC_SS3)
		{
			s_esc_state[g_console] = PT_ESC_NONE;
			if (c == 'A')
				pt_menus_key(PT_KEY_UP);
			else if (c == 'B')
				pt_menus_key(PT_KEY_DOWN);
			else if (c == 'C')
				pt_menus_key(PT_KEY_RIGHT);
			else if (c == 'D')
				pt_menus_key(PT_KEY_LEFT);
			return G.out;
		}
		else /* PT_ESC_CSI: consume until the final byte. */
		{
			unsigned char uc = (unsigned char)c;

			if (uc >= 0x40 && uc <= 0x7e)
			{
				s_esc_state[g_console] = PT_ESC_NONE;
				if (uc == 'A')
					pt_menus_key(PT_KEY_UP);
				else if (uc == 'B')
					pt_menus_key(PT_KEY_DOWN);
				else if (uc == 'C')
					pt_menus_key(PT_KEY_RIGHT);
				else if (uc == 'D')
					pt_menus_key(PT_KEY_LEFT);
			}
			return G.out;
		}
	}
	if (c == 27)			/* Esc: buffer, resolve after idle */
	{
		s_esc_state[g_console] = PT_ESC_GOT;
		s_esc_at[g_console] = mmb_now_ms();
		return G.out;
	}

	/* An open menu/dialog is next, now that a leading Esc has been claimed
	 * by the escape decoder above. */
	if (pt_menus_key((unsigned char)c))
		return G.out;

	if ((unsigned char)c == 1)	/* Alt prefix */
	{
		s_alt_pend[g_console] = 1;
		return G.out;
	}
	if (s_alt_pend[g_console])
	{
		s_alt_pend[g_console] = 0;
		if (c == 'x' || c == 'X')
		{
			if (!pt_menus_confirm_quit())
				pt_paint_leave();
			return G.out;
		}
	}

	if (c == 24)			/* Ctrl+X */
	{
		if (!pt_menus_confirm_quit())
			pt_paint_leave();
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

	/* A lone Esc buffered by the file picker cancels it once its idle window
	 * elapses; without this the dialog would wait for a second key (#760). */
	if (pt_file_poll())
		changed = 1;

	/* A buffered Esc that no key followed within the idle window is a real
	 * Esc: close an open menu/dialog, or quit (through the discard prompt
	 * when the canvas is dirty). */
	if (s_esc_state[g_console] == PT_ESC_GOT &&
	    mmb_now_ms() - s_esc_at[g_console] >= PT_ESC_IDLE_MS)
	{
		s_esc_state[g_console] = PT_ESC_NONE;
		if (pt_menus_active())
			pt_menus_key(27);
		else if (!pt_menus_confirm_quit())
			pt_paint_leave();
		if (!PT.active)
			return;
	}

	/* Animate the selection marquee; a phase change redraws its boundary. */
	if (pt_select_tick())
	{
		int ax, ay, aw, ah;

		if (pt_select_rect(&ax, &ay, &aw, &ah))
			pt_damage(PT_CANVAS_X + ax, PT_CANVAS_Y + ay, aw, ah);
		changed = 1;
	}

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
		/* First sighting: make sure the sprite gets composed even if the
		 * pointer has not moved yet. */
		pt_damage(PT.cursor_sx, PT.cursor_sy, 1, 1);
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

	/* The file picker is keyboard-only and draws over the canvas: swallow
	 * pointer input so a click cannot paint behind it. */
	if (pt_file_dialog_active())
	{
		if (changed)
			PT.dirty = 1;
		if (PT.dirty)
			pt_redraw();
		return;
	}

	if (!PT.mouse_down)
	{
		int button = left ? PT_BTN_LEFT :
			     (right ? PT_BTN_RIGHT : 0);
		int down = (left || right) ? 1 : 0;

		/* The menu module sees every pointer poll: a press opens,
		 * switches or chooses; a release clears its held state; and
		 * motion with no button drives the hover highlight (#708). It
		 * returns non-zero only when a menu or dialog consumed the
		 * event, so a plain move never steals canvas input. */
		if (pt_menus_mouse(sx, sy, button, down))
			changed = 1;
		else if (down)
		{
			int idx, tool, widx;

			if (pt_palette_indicator_hit(sx, sy))
				pt_palette_swap();
			else if (pt_width_hit(sx, sy, &widx))
				pt_width_select(widx);
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

	/* Cursor moves and canvas edits already marked their rectangles; do not
	 * turn every event into a full-frame redraw (#700). */
	if (changed)
		PT.dirty = 1;
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

/* ---- line-width selector (#718) ---------------------------------------- */

PT_WEAK void pt_width_draw(void)
{
}

PT_WEAK int pt_width_hit(int sx, int sy, int *idx)
{
	(void)sx;
	(void)sy;
	(void)idx;
	return 0;
}

PT_WEAK void pt_width_select(int idx)
{
	(void)idx;
}

PT_WEAK int pt_pen_width(void)
{
	return 1;
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
		int col = i % PT_TOOL_COLS;
		int row = i / PT_TOOL_COLS;
		int x = col * PT_CELL_W;
		int y = PT_CANVAS_Y + row * PT_CELL_H;
		unsigned c = (i == PT.tool) ? 0xFFFFFFu : 0x808080u;

		if (y + PT_CELL_H > PT_PAL_Y)
			break;
		pt_fill_rect(x + 1, y + 1, PT_CELL_W - 2, PT_CELL_H - 2,
			     (i == PT.tool) ? 0x404040u : 0x101010u);
		pt_fill_rect(x + 4, y + 4, PT_CELL_W - 8, PT_CELL_H - 8, c);
	}
}

PT_WEAK int pt_tools_hit(int sx, int sy, int *tool)
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

PT_WEAK void pt_cursor_draw(int sx, int sy, int tool, int active, int menu_open)
{
	(void)tool;
	(void)active;
	(void)menu_open;
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

PT_WEAK void pt_file_draw(void)
{
}

PT_WEAK int pt_file_key(int key)
{
	(void)key;
	return 0;
}

PT_WEAK int pt_file_poll(void)
{
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

/* ---- selection (#644) -------------------------------------------------- */

PT_WEAK void pt_select_init(void) {}
PT_WEAK int pt_select_has(void) { return 0; }
PT_WEAK int pt_select_clip_has(void) { return 0; }
PT_WEAK void pt_select_all(void) {}
PT_WEAK void pt_select_none(void) {}
PT_WEAK void pt_select_begin(int cx, int cy, int button)
{
	(void)cx;
	(void)cy;
	(void)button;
}
PT_WEAK void pt_select_motion(int cx, int cy)
{
	(void)cx;
	(void)cy;
}
PT_WEAK void pt_select_end(int cx, int cy)
{
	(void)cx;
	(void)cy;
}
PT_WEAK void pt_select_cancel(void) {}
PT_WEAK void pt_select_draw(void) {}
PT_WEAK int pt_select_tick(void) { return 0; }
PT_WEAK void pt_select_cut(void) {}
PT_WEAK void pt_select_copy(void) {}
PT_WEAK void pt_select_paste(void) {}
PT_WEAK void pt_select_clear(void) {}
PT_WEAK int pt_select_rect(int *x, int *y, int *w, int *h)
{
	(void)x;
	(void)y;
	(void)w;
	(void)h;
	return 0;
}
PT_WEAK int pt_select_clip_size(int *w, int *h)
{
	(void)w;
	(void)h;
	return 0;
}
PT_WEAK int pt_select_hit(int cx, int cy)
{
	(void)cx;
	(void)cy;
	return 0;
}

/* Cold-boot the PAINT layer on a warm reset (#763). warm_reset_close_apps()
 * has already torn down each active session, but every console's module state
 * is still rebuilt here: the init hooks free that console's heap buffers (undo
 * snapshots, text glyphs, tool brush, selection clipboard / float) and clear
 * its per-console state, so no allocation is left behind (#766). */
void mmb_paint_reset_all(void)
{
	int i;
	int save = g_console;

	for (i = 0; i < MMB_MAX_CONSOLES; i++)
		memset(&pt_console_state[i], 0, sizeof(pt_console_state[i]));
	memset(s_esc_state, 0, sizeof(s_esc_state));
	memset(s_esc_at, 0, sizeof(s_esc_at));
	memset(s_alt_pend, 0, sizeof(s_alt_pend));
	memset(s_saved_mode, 0, sizeof(s_saved_mode));
	memset(s_saved_bits, 0, sizeof(s_saved_bits));
	memset(s_frame_state, 0, sizeof(s_frame_state));

	for (i = 0; i < MMB_MAX_CONSOLES; i++)
	{
		g_console = i;
		pt_palette_init();
		pt_tools_init();
		pt_undo_init();
		pt_menus_init();
		pt_cursor_init();
		pt_file_init();
		pt_text_init();
		pt_select_init();
	}
	g_console = save;
}
