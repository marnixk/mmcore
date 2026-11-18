#ifndef HUB_CHROME_H
#define HUB_CHROME_H

/* Pixel-art chrome loaded from A:/hub as PNGs. Every draw returns 0 when the
 * asset is missing so the caller can fall back to the procedural chrome. */

void hub_chrome_load(int p);
void hub_chrome_free(void);
void hub_chrome_clip(int x0, int y0, int x1, int y1);
void hub_chrome_unclip(void);

int hub_chrome_has(const char *name);
int hub_chrome_w(const char *name);
int hub_chrome_h(const char *name);

/* Draw at the asset's pixel size for this p (nearest-neighbour). */
int hub_chrome_blit(const char *name, int x, int y);

/* Stretch the whole image into the rectangle. `pct` clips the draw to the
 * left-hand percentage (100 draws all of it). */
int hub_chrome_fit(const char *name, int x, int y, int w, int h, int pct);

/* 9-slice. `slice2` is the corner size in 2x-asset pixels. */
int hub_chrome_slice(const char *name, int x, int y, int w, int h, int slice2);

/* Preview/detail frame: four corner ornaments and tiled edges. */
int hub_chrome_frame(int x, int y, int w, int h);
/* Rim thickness in screen pixels (0 without the frame art). */
int hub_chrome_frame_band(void);

/* Frame geometry of scripts/gen_hub_pixel_art.py in 1x pixels: the inner
 * corners are cut by HUB_FRAME_CUT (x+y steps), so content inset by
 * HUB_FRAME_PAD from the rim never meets a corner ornament. */
#define HUB_FRAME_CUT 10
#define HUB_FRAME_PAD 5

/* Progress bar (bar-frame / bar-fill) in 1x pixels. */
#define HUB_BAR_INSET 5
/* bar-frame: 1 asset px of dark outline outside the visible silver rim. */
#define HUB_BAR_OUTLINE 1
#define HUB_BAR_SEG 18
#define HUB_BAR_GAP 2

/* Scrollbar arrow cap (button + gap) in 1x rows of scroll-track. */
#define HUB_SCROLL_CAP 9

/* Scrollbar track (stretched) and thumb. */
int hub_chrome_scroll(int x, int y, int w, int h, int thumb_y, int thumb_h);

/* Detail title face. `row` 0 is chrome, 1 is purple. Returns the width. */
int hub_chrome_title(const char *text, int x, int y, int row);
int hub_chrome_title_w(const char *text, int row);
int hub_chrome_title_h(void);

/* 9-slice pill for INSTALLED / UPDATE / NEEDS. The label is drawn separately. */
int hub_chrome_badge(const char *label, int x, int y, int w, int h);

/* Monospace UI face. `zoom` is an extra integer scale (1 or 2). */
int hub_chrome_ui(const char *text, int x, int y, unsigned rgb, int zoom);
int hub_chrome_ui_w(const char *text, int zoom);
void hub_chrome_ui_bearing(const char *text, int zoom, int *left, int *right);
int hub_chrome_ui_h(void);

#endif
