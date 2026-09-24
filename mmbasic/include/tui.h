#ifndef MMB_TUI_H
#define MMB_TUI_H

/*
 * Offscreen character-cell TUI: compose a full frame in memory, then blit
 * only dirty cells to HDMI (pixel backbuffer + one copy of dirty scanlines).
 * Box-drawing codes are CP437-style; HDMI renders real line glyphs.
 */

#define TUI_MAX_COLS 256
#define TUI_MAX_ROWS 80

#define TUI_V  0xB3 /* │ */
#define TUI_H  0xC4 /* ─ */
#define TUI_TL 0xDA /* ┌ */
#define TUI_TR 0xBF /* ┐ */
#define TUI_BL 0xC0 /* └ */
#define TUI_BR 0xD9 /* ┘ */
#define TUI_LT 0xC3 /* ├ */
#define TUI_RT 0xB4 /* ┤ */
#define TUI_TT 0xC2 /* ┬ */
#define TUI_BT 0xC1 /* ┴ */
#define TUI_X  0xC5 /* ┼ */

#define TUI_BLACK    0
#define TUI_RED      1
#define TUI_GREEN    2
#define TUI_YELLOW   3
#define TUI_BLUE     4
#define TUI_MAGENTA  5
#define TUI_CYAN     6
#define TUI_WHITE    7
#define TUI_BRBLACK  8
#define TUI_BRRED    9
#define TUI_BRGREEN  10
#define TUI_BRYELLOW 11
#define TUI_BRBLUE   12
#define TUI_BRMAGENTA 13
#define TUI_BRCYAN   14
#define TUI_BRWHITE  15

void tui_begin(void);
void tui_end(void);
void tui_invalidate(void);
void tui_set_palette(const unsigned *rgb16);
int tui_cols(void);
int tui_rows(void);
void tui_clear(int fg, int bg);
void tui_put(int x, int y, int ch, int fg, int bg);
void tui_puts(int x, int y, const char *s, int fg, int bg);
void tui_pad(int x, int y, const char *s, int width, int fg, int bg);
void tui_status_hint(int row, const char *hint, int hot_fg, int fg, int bg);
void tui_status_hint_at(int x, int row, int width, const char *hint, int hot_fg,
			int fg, int bg);
void tui_fill(int x, int y, int w, int h, int ch, int fg, int bg);
void tui_hline(int x, int y, int w, int left, int mid, int right, int fg, int bg);
void tui_vline(int x, int y, int h, int ch, int fg, int bg);
void tui_frame(int x, int y, int w, int h, int fg, int bg);
void tui_cursor(int x, int y, int vis);
void tui_flush(void);
/* Flush changed cells into the pixel composition buffer without presenting.
 * A caller that owns the present (PAINT's damage band) uses this so one DMA
 * carries both its pixel edits and the changed text cells. */
void tui_flush_no_present(void);
/* Force the next flush to re-blit a cell rectangle, even when its content is
 * unchanged. Used when raw pixels were drawn over the cells (PAINT canvas). */
void tui_invalidate_rect(int x, int y, int w, int h);
/* Accept the current cell content as already shown for a rectangle, without
 * drawing it. Used when raw pixels have replaced the cells. */
void tui_accept_rect(int x, int y, int w, int h);
/* Read a pixel from the TUI composition buffer (what tui_* and the pixel
 * helpers compose into), as RGB888. Falls back to the platform get_pixel. */
unsigned tui_get_px(int x, int y);

/* ---- modal dialog shell (#590) ------------------------------------------
 * A dialog is a framed, centred inset panel drawn over the composed screen
 * instead of a full-screen takeover. Callers fill the backdrop (for example
 * with tui_clear) and then compose the panel interior with the normal tui_*
 * helpers; the title sits on its own bar under the top border. */
void tui_dialog_geom(int want_w, int want_h, int *x, int *y, int *w, int *h);
void tui_dialog_panel(int x, int y, int w, int h, const char *title,
		      int body_fg, int body_bg, int brd_fg, int brd_bg,
		      int title_fg, int title_bg);

/* ---- overlay dialogs (#623, #624) ---------------------------------------
 * A prompt-level dialog (SETTINGS, the app launcher) draws over the REPL.
 * ``tui_overlay_begin`` snapshots the visible screen before the TUI clears
 * it; ``tui_overlay_end`` repaints the snapshot after the dialog closes so
 * the prompt/scrollback return instead of a blank window. Both are no-ops
 * (end returns 0) when the platform has no console snapshot support. */
void tui_overlay_begin(void);
int tui_overlay_end(void);

#endif
