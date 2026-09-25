/*
 * PAINT - sprite-restore cursor runtime (#639).
 *
 * The cursor is drawn from the 32x32 per-tool art (#632) with the
 * sprite-restore technique: before the sprite is painted the pixels it
 * covers are copied into a small save buffer, and the previous sprite is
 * lifted first so a moving pointer leaves no trail. pt_cursor_restore()
 * puts the last saved block back, which is what keeps the cursor out of
 * saved PCX captures and screenshots.
 *
 * This file owns only its save buffer and the art lookup; it draws through
 * the shared pt_plot()/pt_palette_rgb() helpers and reads the composed frame
 * through tui_get_px(). That reads the TUI composition buffer (s_tui_pix on a
 * bare Pi) rather than the HDMI framebuffer, which still shows the previous
 * frame including the old cursor position (#701).
 */
#include "mmb_priv.h"
#include "tui.h"
#include "paint.h"
#include "paint_cursor_art.h"

/* Saved pixels under the current sprite (clamped to the screen). */
static unsigned s_bg[PCA_CURSOR_PIXELS];
static int s_bg_x, s_bg_y, s_bg_w, s_bg_h;
static int s_have;

/* Map the app's tool id onto the baked cursor art. Unknown ids fall back to
 * the plain arrow so a fresh or out-of-range tool still draws a pointer. */
static int pt_cursor_art(int tool)
{
	switch (tool)
	{
	case PT_TOOL_PENCIL:	return PCA_TOOL_PENCIL;
	case PT_TOOL_LINE:	return PCA_TOOL_LINE;
	case PT_TOOL_RECT:	return PCA_TOOL_RECTANGLE;
	case PT_TOOL_RECT_FILLED:	return PCA_TOOL_RECTANGLE;
	case PT_TOOL_ELLIPSE:	return PCA_TOOL_ELLIPSE;
	case PT_TOOL_ELLIPSE_FILLED:	return PCA_TOOL_ELLIPSE;
	case PT_TOOL_CIRCLE:	return PCA_TOOL_CIRCLE;
	case PT_TOOL_CIRCLE_FILLED:	return PCA_TOOL_CIRCLE;
	case PT_TOOL_FILL:	return PCA_TOOL_FILL;
	case PT_TOOL_ERASER:	return PCA_TOOL_ERASER;
	case PT_TOOL_PICK:	return PCA_TOOL_PICK;
	case PT_TOOL_GRAB:	return PCA_TOOL_GRAB;
	case PT_TOOL_MAGNIFY:	return PCA_TOOL_MAGNIFY;
	case PT_TOOL_AIRBRUSH:	return PCA_TOOL_AIRBRUSH;
	case PT_TOOL_SPRAY:	return PCA_TOOL_SPRAY;
	case PT_TOOL_TEXT:	return PCA_TOOL_TEXT;
	case PT_TOOL_SELECT:	return PCA_TOOL_SELECT;
	default:		return PCA_TOOL_ARROW;
	}
}

static unsigned pt_cursor_get(int x, int y)
{
	return tui_get_px(x, y);
}

void pt_cursor_init(void)
{
	s_have = 0;
	s_bg_x = s_bg_y = 0;
	s_bg_w = s_bg_h = 0;
}

void pt_cursor_restore(void)
{
	int row, col;

	if (!s_have)
		return;
	/* The pixels being put back leave the old cursor footprint: damage it so
	 * the canvas/chrome pass recomposites there (#700). */
	pt_damage_present(s_bg_x, s_bg_y, s_bg_w, s_bg_h);
	for (row = 0; row < s_bg_h; row++)
		for (col = 0; col < s_bg_w; col++)
			pt_plot(s_bg_x + col, s_bg_y + row,
				s_bg[(size_t)row * s_bg_w + col]);
	s_have = 0;
	s_bg_w = s_bg_h = 0;
}

void pt_cursor_draw(int sx, int sy, int tool, int active)
{
	int idx, hx, hy, ox, oy, x0, y0, x1, y1, x, y;
	int pcx, pcy, on_canvas;
	const pca_sprite_t *sp;
	const uint8_t *art;

	/* Lift the sprite from the previous position before scanning afresh. */
	pt_cursor_restore();

	/* No selected tool: no canvas cursor is active at all. */
	if (tool < 0)
		return;

	/* The tool sprite only reads over the canvas; over the menu bar, tool
	 * column, palette and line-width bar the plain arrow is the right
	 * pointer. */
	on_canvas = pt_screen_to_canvas(sx, sy, &pcx, &pcy);
	idx = on_canvas ? pt_cursor_art(tool) : PCA_TOOL_ARROW;
	sp = &pca_sprites[idx];
	if (active)
	{
		art = sp->active;
		hx = sp->active_hotspot_x;
		hy = sp->active_hotspot_y;
	}
	else
	{
		art = sp->idle;
		hx = sp->idle_hotspot_x;
		hy = sp->idle_hotspot_y;
	}

	ox = sx - (int)hx;
	oy = sy - (int)hy;

	/* Clamp the 32x32 footprint to the screen and copy it. */
	x0 = ox < 0 ? 0 : ox;
	y0 = oy < 0 ? 0 : oy;
	x1 = ox + PCA_CURSOR_W;
	if (x1 > PT_W)
		x1 = PT_W;
	y1 = oy + PCA_CURSOR_H;
	if (y1 > PT_H)
		y1 = PT_H;

	if (x1 > x0 && y1 > y0)
	{
		s_bg_x = x0;
		s_bg_y = y0;
		s_bg_w = x1 - x0;
		s_bg_h = y1 - y0;
		/* Background is captured from the fully composed frame (this runs
		 * last), so the sprite never saves a stale copy of itself. */
		for (y = 0; y < s_bg_h; y++)
			for (x = 0; x < s_bg_w; x++)
				s_bg[(size_t)y * s_bg_w + x] =
					pt_cursor_get(x0 + x, y0 + y);
		s_have = 1;
		/* The new sprite covers this rectangle; damage it for the present. */
		pt_damage_present(x0, y0, s_bg_w, s_bg_h);
	}
	else
	{
		s_have = 0;
	}

	/* Paint the opaque sprite pixels over the saved block. */
	for (y = 0; y < PCA_CURSOR_H; y++)
	{
		int py = oy + y;

		if (py < 0 || py >= PT_H)
			continue;
		for (x = 0; x < PCA_CURSOR_W; x++)
		{
			int px = ox + x;
			uint8_t c = art[(size_t)y * PCA_CURSOR_W + x];

			if (c == PCA_CURSOR_TRANSPARENT)
				continue;
			if (px < 0 || px >= PT_W)
				continue;
			pt_plot(px, py, pt_palette_rgb(c));
		}
	}
}
