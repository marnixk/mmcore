/*
 * PAINT - shared internal API for the Dr. Halo-style paint app.
 *
 * This header is FROZEN by the scaffold ticket (#634). Every cross-module
 * type and prototype is declared here up front so the palette, tools, undo,
 * menus, cursor, file and text modules can be implemented in separate .c
 * files without touching each other. cmd_paint.c owns the lifecycle, layout
 * and event loop; it calls into these hooks and owns the shared state.
 *
 * Screen: 640x360 (MODE 18). Layout, in screen pixels:
 *
 *   row 0..15    menu bar (one 8x16 text row), full width
 *   x 0..31      tool column, one icon per tool, below the menu bar
 *   y 328..359   palette strip: 4 rows x 64 swatches of 8x8,
 *                FG/BG indicator at the left end
 *   the canvas  is the remaining area (608x312), one canvas pixel per
 *               screen pixel, black (index 0) on entry
 */
#ifndef MMB_PAINT_H
#define MMB_PAINT_H

#include "mmb_priv.h"

/* ---- screen / layout --------------------------------------------------- */

#define PT_MODE     18		/* gfx.c kModes entry: 640x360 */

#define PT_W        640
#define PT_H        360

#define PT_MENU_H   16		/* menu bar text row, screen pixels */
#define PT_TOOL_W   32		/* left tool column width */
#define PT_PAL_COLS 64
#define PT_PAL_ROWS 4
#define PT_PAL_SW   8		/* swatch size, screen pixels */
#define PT_PAL_W    (PT_PAL_COLS * PT_PAL_SW)	/* 512 */
#define PT_PAL_H    (PT_PAL_ROWS * PT_PAL_SW)	/* 32 */
#define PT_PAL_X    PT_TOOL_W			/* strip starts beside tools */
#define PT_PAL_Y    (PT_H - PT_PAL_H)		/* 328 */

/* FG/BG indicator: the 32x32 cell at the far left of the bottom band. */
#define PT_IND_X    0
#define PT_IND_Y    PT_PAL_Y
#define PT_IND_W    PT_TOOL_W
#define PT_IND_H    PT_PAL_H

/* Canvas: between the menu bar, the tool column and the palette strip. */
#define PT_CANVAS_X PT_TOOL_W
#define PT_CANVAS_Y PT_MENU_H
#define PT_CANVAS_W (PT_W - PT_TOOL_W)		/* 608 */
#define PT_CANVAS_H (PT_PAL_Y - PT_CANVAS_Y)	/* 312 */

#define PT_MAX_W PT_CANVAS_W
#define PT_MAX_H PT_CANVAS_H

/* ---- tools ------------------------------------------------------------- */

enum pt_tool {
	PT_TOOL_PENCIL = 0,
	PT_TOOL_LINE,
	PT_TOOL_RECT,
	PT_TOOL_ELLIPSE,
	PT_TOOL_CIRCLE,
	PT_TOOL_FILL,
	PT_TOOL_ERASER,
	PT_TOOL_PICK,
	PT_TOOL_GRAB,
	PT_TOOL_MAGNIFY,
	PT_TOOL_AIRBRUSH,
	PT_TOOL_SPRAY,
	PT_TOOL_TEXT,
	PT_TOOL_COUNT
};

/* ---- menus ------------------------------------------------------------- */

enum pt_menu {
	PT_MENU_NONE = -1,
	PT_MENU_FILE = 0,
	PT_MENU_EDIT,
	PT_MENU_HELP,
	PT_MENU_COUNT
};

/* ---- events ------------------------------------------------------------ */

enum pt_event_type {
	PT_EV_NONE = 0,
	PT_EV_MOUSE_MOVE,
	PT_EV_MOUSE_DOWN,	/* button pressed */
	PT_EV_MOUSE_UP,		/* button released */
	PT_EV_WHEEL,
	PT_EV_KEY
};

#define PT_BTN_LEFT  1
#define PT_BTN_RIGHT 2

typedef struct pt_event {
	int type;
	int sx, sy;		/* screen pixel coords (mouse) */
	int cx, cy;		/* canvas coords, valid on the canvas */
	int on_canvas;
	int button;		/* PT_BTN_* */
	int delta;		/* wheel ticks */
	int key;		/* PT_EV_KEY: byte */
} pt_event;

/* ---- shared state ------------------------------------------------------ */

typedef struct pt_state {
	int active;
	int have_mouse;		/* a pointer was seen this session */

	/* canvas, palette indices, PT.width * PT.height bytes */
	int width, height;
	unsigned char *canvas;
	unsigned char *scratch;	/* same size; live-preview backup */
	int scratch_valid;

	int fg, bg;		/* foreground/background palette indices */
	int tool;		/* enum pt_tool */

	int cursor_x, cursor_y;		/* canvas coords */
	int cursor_sx, cursor_sy;	/* screen coords */

	int mouse_down;		/* a button is held on the canvas */
	int mouse_button;	/* PT_BTN_* of the held button */
	int last_cx, last_cy;

	int have_anchor;
	int anchor_x, anchor_y;

	int menu;		/* open menu id, or PT_MENU_NONE */
	int dialog;		/* a modal dialog owns input when non-zero */

	int dirty;		/* something changed: redraw before present */
	int full_redraw;	/* chrome plus canvas, not just the canvas */

	int zoom;		/* magnify factor, >= 1 */
	int view_x, view_y;	/* magnified view origin, canvas coords */

	int undo_depth;
	int redo_depth;

	char path[160];		/* save/open target */
	char label[96];		/* basename of path */
	char status[96];	/* one-line status hint */
} pt_state;

extern pt_state PT;

/* ---- lifecycle (cmd_paint.c, wired into core/frontend/session) ---------- */

void mmb_cmd_paint(void);
int mmb_in_paint(void);
const char *mmb_paint_key(char c);
void mmb_paint_poll(void);

/* Test-only override: the native harness reports a mouse as present so
 * headless builds can enter PAINT. No effect on real targets. */
void pt_force_mouse(int on);
int pt_mouse_present(void);

/* ---- screen drawing helpers (cmd_paint.c) ------------------------------ */

void pt_plot(int x, int y, unsigned rgb);
void pt_fill_rect(int x, int y, int w, int h, unsigned rgb);
/* Mark a screen-space rectangle changed for the next frame. Damage from every
 * edit is unioned into one bounding box; pt_redraw() then only recomposites
 * what moved and pt_present() DMAs only those rows (#700). */
void pt_damage(int x, int y, int w, int h);
/* Same, in canvas coordinates (the canvas sits at PT_CANVAS_X/Y). */
void pt_damage_canvas(int x, int y, int w, int h);
/* Mark a rectangle that only needs presenting, not recompositing. The cursor
 * uses this: its saved background already restores the pixels, so a canvas
 * repaint would needlessly erase any overlay it sits on. */
void pt_damage_present(int x, int y, int w, int h);
void pt_draw_canvas(void);		/* blit the damaged PT.canvas region 1:1 */
void pt_present(void);			/* flush the damaged rows to HDMI */
void pt_redraw(void);			/* recompose the damaged frame */
void pt_request_redraw(void);		/* request a full chrome+canvas frame */

/* Map a screen pixel to canvas coords; returns 1 when inside the canvas. */
int pt_screen_to_canvas(int sx, int sy, int *cx, int *cy);

/* Palette index at a canvas coord, or 0 when out of range. */
int pt_canvas_get(int cx, int cy);
void pt_canvas_set(int cx, int cy, int idx);

/* ---- palette module (#635) --------------------------------------------- */

void pt_palette_init(void);
unsigned pt_palette_rgb(int idx);	/* fixed VGA 256, clamped */
void pt_palette_draw(void);		/* strip + FG/BG indicator */
int pt_palette_hit(int sx, int sy, int *idx);	/* 1 when over a swatch */
int pt_palette_indicator_hit(int sx, int sy);
void pt_palette_select(int idx, int button);	/* 1 = FG, 2 = BG */
void pt_palette_swap(void);

/* ---- tools module (#636, bonus #641/#642) ------------------------------ */

void pt_tools_init(void);
void pt_tools_draw(void);		/* tool column icons + selection */
int pt_tools_hit(int sx, int sy, int *tool);	/* 1 when over a tool cell */
void pt_tool_select(int tool);
void pt_tool_begin(int cx, int cy, int button);
void pt_tool_motion(int cx, int cy, int button);
void pt_tool_end(int cx, int cy, int button);
void pt_tool_cancel(void);

/* ---- undo module (#637) ------------------------------------------------ */

void pt_undo_init(void);
void pt_undo_push(void);
void pt_undo(void);
void pt_redo(void);
void pt_undo_clear(void);

/* ---- menus module (#638) ----------------------------------------------- */

void pt_menus_init(void);
void pt_menus_draw(void);		/* menu bar + open dropdown */
int pt_menus_mouse(int sx, int sy, int button, int down); /* 1 = consumed */
int pt_menus_key(int key);		/* 1 = consumed */
int pt_menus_active(void);
void pt_menus_close(void);

/* ---- cursor module (#639) ---------------------------------------------- */

void pt_cursor_init(void);
void pt_cursor_draw(int sx, int sy, int tool, int active);
void pt_cursor_restore(void);

/* ---- file module (#640) ------------------------------------------------ */

void pt_file_init(void);
void pt_file_new(void);
void pt_file_open(void);
void pt_file_save(void);
void pt_file_save_as(void);
int pt_file_dialog_active(void);
int pt_file_key(int key);		/* 1 = consumed by an open dialog */

/* ---- text module (#642) ------------------------------------------------ */

void pt_text_init(void);
int pt_text_active(void);
int pt_text_key(int key);		/* 1 = consumed */

#endif /* MMB_PAINT_H */
