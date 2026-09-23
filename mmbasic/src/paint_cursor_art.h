/* PAINT tool cursor art (#632).
 *
 * Self-contained, data-only. This header deliberately does not include the
 * shared paint.h: the cursor art is plain baked data and may be consumed by
 * the PAINT runtime without pulling in its state types. The sprite order is
 * fixed by the PCA_TOOL_* enum below.
 */
#ifndef PAINT_CURSOR_ART_H
#define PAINT_CURSOR_ART_H

#include <stdint.h>

#define PCA_CURSOR_W 32
#define PCA_CURSOR_H 32
#define PCA_CURSOR_PIXELS (PCA_CURSOR_W * PCA_CURSOR_H)
#define PCA_CURSOR_TRANSPARENT 255

typedef enum {
	PCA_TOOL_ARROW = 0,
	PCA_TOOL_PENCIL = 1,
	PCA_TOOL_LINE = 2,
	PCA_TOOL_RECTANGLE = 3,
	PCA_TOOL_ELLIPSE = 4,
	PCA_TOOL_CIRCLE = 5,
	PCA_TOOL_FILL = 6,
	PCA_TOOL_ERASER = 7,
	PCA_TOOL_PICK = 8,
	PCA_TOOL_GRAB = 9,
	PCA_TOOL_MAGNIFY = 10,
	PCA_TOOL_AIRBRUSH = 11,
	PCA_TOOL_SPRAY = 12,
	PCA_TOOL_TEXT = 13,
	PCA_TOOL_COUNT
} pca_tool_t;

/* One tool's idle and active art. Each bitmap is PCA_CURSOR_PIXELS bytes of
 * row-major default-VGA palette indices; PCA_CURSOR_TRANSPARENT marks a pixel
 * the sprite-restore runtime leaves untouched. The hotspot coordinates are in
 * sprite space (0..31) and are carried per state. */
typedef struct {
	const char *name;
	const uint8_t *idle;
	const uint8_t *active;
	uint8_t idle_hotspot_x;
	uint8_t idle_hotspot_y;
	uint8_t active_hotspot_x;
	uint8_t active_hotspot_y;
	uint8_t transparent;
} pca_sprite_t;

extern const pca_sprite_t pca_sprites[PCA_TOOL_COUNT];

extern const uint8_t pca_arrow_idle[PCA_CURSOR_PIXELS];
extern const uint8_t pca_arrow_active[PCA_CURSOR_PIXELS];
extern const uint8_t pca_pencil_idle[PCA_CURSOR_PIXELS];
extern const uint8_t pca_pencil_active[PCA_CURSOR_PIXELS];
extern const uint8_t pca_line_idle[PCA_CURSOR_PIXELS];
extern const uint8_t pca_line_active[PCA_CURSOR_PIXELS];
extern const uint8_t pca_rectangle_idle[PCA_CURSOR_PIXELS];
extern const uint8_t pca_rectangle_active[PCA_CURSOR_PIXELS];
extern const uint8_t pca_ellipse_idle[PCA_CURSOR_PIXELS];
extern const uint8_t pca_ellipse_active[PCA_CURSOR_PIXELS];
extern const uint8_t pca_circle_idle[PCA_CURSOR_PIXELS];
extern const uint8_t pca_circle_active[PCA_CURSOR_PIXELS];
extern const uint8_t pca_fill_idle[PCA_CURSOR_PIXELS];
extern const uint8_t pca_fill_active[PCA_CURSOR_PIXELS];
extern const uint8_t pca_eraser_idle[PCA_CURSOR_PIXELS];
extern const uint8_t pca_eraser_active[PCA_CURSOR_PIXELS];
extern const uint8_t pca_pick_idle[PCA_CURSOR_PIXELS];
extern const uint8_t pca_pick_active[PCA_CURSOR_PIXELS];
extern const uint8_t pca_grab_idle[PCA_CURSOR_PIXELS];
extern const uint8_t pca_grab_active[PCA_CURSOR_PIXELS];
extern const uint8_t pca_magnify_idle[PCA_CURSOR_PIXELS];
extern const uint8_t pca_magnify_active[PCA_CURSOR_PIXELS];
extern const uint8_t pca_airbrush_idle[PCA_CURSOR_PIXELS];
extern const uint8_t pca_airbrush_active[PCA_CURSOR_PIXELS];
extern const uint8_t pca_spray_idle[PCA_CURSOR_PIXELS];
extern const uint8_t pca_spray_active[PCA_CURSOR_PIXELS];
extern const uint8_t pca_text_idle[PCA_CURSOR_PIXELS];
extern const uint8_t pca_text_active[PCA_CURSOR_PIXELS];
#endif /* PAINT_CURSOR_ART_H */
