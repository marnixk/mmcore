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
void tui_fill(int x, int y, int w, int h, int ch, int fg, int bg);
void tui_hline(int x, int y, int w, int left, int mid, int right, int fg, int bg);
void tui_vline(int x, int y, int h, int ch, int fg, int bg);
void tui_frame(int x, int y, int w, int h, int fg, int bg);
void tui_cursor(int x, int y, int vis);
void tui_flush(void);

#endif
