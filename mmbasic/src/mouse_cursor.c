/*
 * MOUSE - BASIC software cursor runtime (#792).
 *
 * The cursor is a small sprite composited on top of the finished frame, not
 * into any PAGE: mmb_mouse_cursor_present() copies the composed block under
 * the icon into a scratch buffer, stamps the opaque sprite pixels there, and
 * presents that block through mmb_gfx_present_native(). The page buffers are
 * only ever read, so moving the pointer can never dirty a PAGE or change what
 * PIXEL()/SPRITE/PCX save see. This is the same save-last-block idea as
 * PAINT's sprite cursor (paint_cursors.c), but it uses the shared gfx present
 * path instead of PAINT's pt_* API.
 *
 * The event half classifies pointer motion and button presses so core.c can
 * drive ON MOUSEMOVE / ON MOUSECLICK handlers like it drives ON KEY.
 */
#include "mmb_priv.h"
#include <string.h>

#define MMB_CURSOR_MAX_W 16
#define MMB_CURSOR_MAX_H 16

enum {
	MMB_CURSOR_POINTER = 0,
	MMB_CURSOR_HAND,
	MMB_CURSOR_CROSSHAIR,
	MMB_CURSOR_QUESTION,
	MMB_CURSOR_DENY,
	MMB_CURSOR_COUNT
};

typedef struct {
	unsigned char w, h;
	signed char hot_x, hot_y; /* hotspot within the icon */
	const char *rows[MMB_CURSOR_MAX_H];
} mmb_cursor_art;

/* ' ' transparent, '#' black, '+' white. */
static const mmb_cursor_art s_art[MMB_CURSOR_COUNT] = {
	{ 12, 16, 0, 0, {
		"#           ",
		"##          ",
		"#+#         ",
		"#++#        ",
		"#+++#       ",
		"#++++#      ",
		"#+++++#     ",
		"#++++++#    ",
		"#+++++++#   ",
		"#++++++++#  ",
		"#+++++##### ",
		"#++#++#     ",
		"#+# #++#    ",
		"##  #++#    ",
		"#    #++#   ",
		"      ##    ",
	} },
	{ 12, 16, 0, 0, {
		"    ##      ",
		"   #++#     ",
		"   #++#     ",
		"   #++#     ",
		"  ##++#     ",
		" #++++#     ",
		"##++++#     ",
		"#+++++#     ",
		"#+++++#     ",
		"#+++++#     ",
		"#+++++#     ",
		" #++++#     ",
		" #++++#     ",
		"  #++#      ",
		"   ##       ",
		"            ",
	} },
	{ 16, 16, 7, 6, {
		"       ##       ",
		"       ##       ",
		"       ##       ",
		"       ##       ",
		"       ##       ",
		"       ##       ",
		"  #####++#####  ",
		"  #####++#####  ",
		"       ##       ",
		"       ##       ",
		"       ##       ",
		"       ##       ",
		"       ##       ",
		"       ##       ",
		"       ##       ",
		"                ",
	} },
	{ 16, 16, 0, 0, {
		"   #####        ",
		"  ##+++##       ",
		" ##+++++##      ",
		" ##+###+##      ",
		"   #+++#        ",
		"   #++##        ",
		"    ##+##       ",
		"     #+##       ",
		"     #+#        ",
		"     #+#        ",
		"                ",
		"     #+#        ",
		"     #+#        ",
		"     ###        ",
		"                ",
		"                ",
	} },
	{ 16, 16, 7, 7, {
		"     #####      ",
		"   ##+++++##    ",
		"  #+++++++++#   ",
		" #++++#####++#  ",
		" #++##++++#++#  ",
		"#++##++++++#++# ",
		"#++#++++++++#++#",
		"#+#++++++++++#+#",
		"#+#++++++++++#+#",
		"#++#++++++++#++#",
		"#++##++++++#++# ",
		" #++##++++#++#  ",
		" #++++#####++#  ",
		"  #+++++++++#   ",
		"   ##+++++##    ",
		"     #####      ",
	} },
};

/* One context per virtual console so a console switch never lifts the wrong
 * screen's pointer (#766 in PAINT). */
typedef struct {
	int on;
	int type;
	int last_buttons;
	int have_pos;
	int last_x, last_y;
	/* Footprint of the sprite currently composited into the frame. */
	int have_blit;
	int bx, by, bw, bh;
	int blit_fw, blit_fh;
	/* Event drain state, rebuilt from one pointer read per pump. */
	int phase;
	int move_pending;
	int pending_clicks;
} mmb_mouse_ctx;

static mmb_mouse_ctx s_mc[MMB_MAX_CONSOLES];
#define MC (s_mc[g_console])

/* Map a MOUSE CURSOR name (or a 0..4 index string) onto a type id. Returns -1
 * for an unknown name so the caller can raise ?SYNTAX ERROR. */
int mmb_mouse_cursor_type_from_name(const char *name)
{
	char up[MMB_MAX_NAME];
	int i;
	if (!name || !name[0])
		return -1;
	for (i = 0; i < MMB_MAX_NAME - 1 && name[i]; i++)
		up[i] = name[i];
	up[i] = 0;
	mmb_upper(up);
	if (!strcmp(up, "POINTER") || !strcmp(up, "ARROW"))
		return MMB_CURSOR_POINTER;
	if (!strcmp(up, "HAND"))
		return MMB_CURSOR_HAND;
	if (!strcmp(up, "CROSSHAIR") || !strcmp(up, "CROSS"))
		return MMB_CURSOR_CROSSHAIR;
	if (!strcmp(up, "QUESTIONMARK") || !strcmp(up, "QUESTION"))
		return MMB_CURSOR_QUESTION;
	if (!strcmp(up, "DENY") || !strcmp(up, "NO"))
		return MMB_CURSOR_DENY;
	for (i = 0; i < MMB_CURSOR_COUNT; i++)
		if (up[0] == (char)('0' + i) && up[1] == 0)
			return i;
	return -1;
}

const char *mmb_mouse_cursor_type_name(int type)
{
	switch (type)
	{
	case MMB_CURSOR_HAND:		return "hand";
	case MMB_CURSOR_CROSSHAIR:	return "crosshair";
	case MMB_CURSOR_QUESTION:	return "questionmark";
	case MMB_CURSOR_DENY:		return "deny";
	default:			return "pointer";
	}
}

void mmb_mouse_cursor_set_on(int on)
{
	int was = MC.on;
	MC.on = on ? 1 : 0;
	MC.phase = 0;
	MC.move_pending = 0;
	MC.pending_clicks = 0;
	MC.have_pos = 0;
	if (!MC.on && was && MC.have_blit)
	{
		/* Lift the sprite now: the overlay erases the saved block on the
		 * next present. Ask for one so MOUSE OFF takes effect at once. */
		mmb_gfx_dirty_add(MC.bx, MC.by, MC.bw, MC.bh);
		if (G.running)
			mmb_gfx_present();
		return;
	}
	if (MC.on)
	{
		/* Seed the settled position so ON MOUSEMOVE only fires on real
		 * movement, and show the cursor right away. */
		mmb_mouse_state m;
		if (mmb_mouse_read(&m) && m.present)
		{
			MC.have_pos = 1;
			MC.last_x = m.x;
			MC.last_y = m.y;
			MC.last_buttons = m.buttons;
		}
		mmb_mouse_cursor_refresh();
	}
}

void mmb_mouse_cursor_set_type(int type)
{
	if (type < 0 || type >= MMB_CURSOR_COUNT)
		type = MMB_CURSOR_POINTER;
	if (MC.type == type)
		return;
	MC.type = type;
	/* A live pointer must redraw with the new icon. */
	if (MC.on)
		mmb_mouse_cursor_refresh();
}

/* Add the AABB the icon would occupy at (sx, sy) to the present dirty set. */
static void cursor_damage(int sx, int sy)
{
	const mmb_cursor_art *art =
		&s_art[MC.type >= 0 && MC.type < MMB_CURSOR_COUNT ? MC.type : 0];
	int x0 = sx - art->hot_x;
	int y0 = sy - art->hot_y;
	mmb_gfx_dirty_add(x0, y0, art->w, art->h);
}

/* Called from the event pump when the pointer moved: damage the old and new
 * footprints and present so the sprite follows without touching a PAGE. */
static void cursor_track(int sx, int sy)
{
	if (!MC.on || !G.running || G.gfx.w <= 0 || G.gfx.h <= 0)
		return;
	if (MC.have_blit)
		mmb_gfx_dirty_add(MC.bx, MC.by, MC.bw, MC.bh);
	cursor_damage(sx, sy);
	mmb_gfx_present();
}

void mmb_mouse_cursor_refresh(void)
{
	mmb_mouse_state m;
	if (MC.on && G.running && mmb_mouse_read(&m) && m.present)
		cursor_track(m.x, m.y);
}

void mmb_mouse_cursor_present(const uint16_t *pg, int w, int h)
{
	uint16_t tmp[(size_t)MMB_CURSOR_MAX_W * MMB_CURSOR_MAX_H];
	const mmb_cursor_art *art;
	mmb_mouse_state m;
	int ox, oy, x0, y0, x1, y1, x, y, tw;

	/* Erase the previous footprint. The composed frame is clean there, so
	 * this only repaints pixels the page already owns. A different frame
	 * geometry means the saved rect belongs to a dead mode: drop it. */
	if (MC.have_blit && pg && MC.blit_fw == w && MC.blit_fh == h)
		mmb_gfx_present_native(MC.bx, MC.by, MC.bw, MC.bh,
				       pg + (size_t)MC.by * w + MC.bx, w);
	MC.have_blit = 0;

	if (!MC.on || !pg || w <= 0 || h <= 0)
		return;
	if (!mmb_mouse_read(&m) || !m.present)
		return;
	art = &s_art[MC.type >= 0 && MC.type < MMB_CURSOR_COUNT ? MC.type : 0];
	ox = m.x - art->hot_x;
	oy = m.y - art->hot_y;
	x0 = ox < 0 ? 0 : ox;
	y0 = oy < 0 ? 0 : oy;
	x1 = ox + art->w;
	if (x1 > w)
		x1 = w;
	y1 = oy + art->h;
	if (y1 > h)
		y1 = h;
	if (x1 <= x0 || y1 <= y0)
		return;

	tw = x1 - x0;
	for (y = 0; y < y1 - y0; y++)
		memcpy(&tmp[(size_t)y * tw], pg + (size_t)(y0 + y) * w + x0,
		       (size_t)tw * sizeof(uint16_t));
	for (y = 0; y < art->h; y++)
	{
		int py = oy + y;
		if (py < 0 || py >= h)
			continue;
		for (x = 0; x < art->w; x++)
		{
			char c = art->rows[y][x];
			int px = ox + x;
			if (c == ' ' || px < 0 || px >= w)
				continue;
			tmp[(size_t)(py - y0) * tw + (px - x0)] =
				(uint16_t)mmb_rgb_to_native(c == '#' ? 0x000000u
								     : 0xFFFFFFu);
		}
	}
	mmb_gfx_present_native(x0, y0, tw, y1 - y0, tmp, tw);
	MC.bx = x0;
	MC.by = y0;
	MC.bw = tw;
	MC.bh = y1 - y0;
	MC.blit_fw = w;
	MC.blit_fh = h;
	MC.have_blit = 1;
}

/* Classify the next pointer event for core.c. Returns 0 when drained,
 * 1 for a move (x,y), 2 for a click (x,y plus the button bit: 1 left,
 * 2 right, 4 middle). */
int mmb_mouse_take_event(int *x, int *y, int *button)
{
	if (x)
		*x = 0;
	if (y)
		*y = 0;
	if (button)
		*button = 0;
	if (!G.running || G.tick_busy)
		return 0;
	if (!MC.phase)
	{
		mmb_mouse_state m;
		if (!mmb_mouse_read(&m) || !m.present)
			return 0;
		MC.phase = 1;
		MC.move_pending = (!MC.have_pos || m.x != MC.last_x ||
				   m.y != MC.last_y);
		if (MC.move_pending)
		{
			MC.last_x = m.x;
			MC.last_y = m.y;
			MC.have_pos = 1;
			cursor_track(m.x, m.y);
		}
		MC.pending_clicks = m.buttons & ~MC.last_buttons;
		MC.last_buttons = m.buttons;
	}
	if (MC.move_pending)
	{
		MC.move_pending = 0;
		if (x)
			*x = MC.last_x;
		if (y)
			*y = MC.last_y;
		return 1;
	}
	if (MC.pending_clicks)
	{
		int bit = MC.pending_clicks & -MC.pending_clicks;
		MC.pending_clicks &= ~bit;
		if (x)
			*x = MC.last_x;
		if (y)
			*y = MC.last_y;
		if (button)
			*button = bit;
		return 2;
	}
	MC.phase = 0;
	return 0;
}

void mmb_mouse_cursor_reset_all(void)
{
	int i;
	for (i = 0; i < MMB_MAX_CONSOLES; i++)
	{
		memset(&s_mc[i], 0, sizeof(s_mc[i]));
		s_mc[i].type = MMB_CURSOR_POINTER;
	}
}
