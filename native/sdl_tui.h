/*
 * Character-cell TUI hooks for the SDL2 backend (LN-06).
 *
 * EDIT, FILES, IHELP and WORDPAD render through mmb_platform's tui_* hooks
 * (mmbasic/src/tui.c and direct calls from WORDPAD). Pixels are written into
 * the shared software framebuffer in native RGB555; tui_present uploads it.
 */
#ifndef MMB_SDL_TUI_H
#define MMB_SDL_TUI_H

int sdl_tui_cols(void);
int sdl_tui_rows(void);
void sdl_tui_prepare(void);
void sdl_tui_glyph(int col, int row, unsigned ch, unsigned fg_rgb, unsigned bg_rgb);
void sdl_tui_present(int y0, int y1);

/* Read a pixel from the TUI composition surface as RGB888. On SDL the
 * framebuffer is shared, so this matches get_pixel; it exists so PAINT's
 * cursor reads the same buffer everywhere. */
unsigned sdl_tui_get_px(int x, int y);
void sdl_tui_scroll(int x, int y, int w, int h, int dy, unsigned fill_rgb);
void sdl_tui_glyph2x(int col, int row, unsigned ch, unsigned fg_rgb, unsigned bg_rgb);
void sdl_tui_glyph_n(int col, int row, unsigned ch, unsigned fg_rgb, unsigned bg_rgb,
		     int scale);
void sdl_tui_glyph_n_px(int x_px, int y_px, unsigned ch, unsigned fg_rgb,
			unsigned bg_rgb, int scale, int ink_only);
void sdl_tui_fill_px(int x_px, int y_px, int w, int h, unsigned rgb);
void sdl_tui_set_font(const unsigned char *font);

#endif
