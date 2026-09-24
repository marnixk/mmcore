"""PAINT sprite-restore cursor runtime (#639).

``mmbasic/src/paint_cursors.c`` saves the pixels under the current 32x32 tool
sprite, paints the sprite from the baked art (#632), and puts the saved block
back on restore. These are host tests: the real cursor module is compiled
against a small shim that provides an in-memory screen, then the driver moves
the cursor and pixel-compares the restored background, checks the per-tool and
idle/active shapes against the art, and confirms the canvas (what PCX save
reads) is never touched by the cursor.
"""
import os
import shutil
import subprocess

import pytest

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(REPO, "mmbasic", "src")
CURSORS_C = os.path.join(SRC, "paint_cursors.c")
ART_C = os.path.join(SRC, "paint_cursor_art.c")

PT_W, PT_H = 640, 360
PT_CANVAS_W, PT_CANVAS_H = 576, 312
PT_CANVAS_X, PT_CANVAS_Y = 64, 16

# The 14 baked sprites (arrow + 13 raster tools).
TOOLS = [
    "arrow", "pencil", "line", "rectangle", "ellipse", "circle", "fill",
    "eraser", "pick", "grab", "magnify", "airbrush", "spray", "text",
]

# app tool id -> baked sprite name. The three filled variants (#719) reuse
# their outline sibling's cursor art.
TOOL_IDS = {
    "pencil": "pencil", "eraser": "eraser", "line": "line", "text": "text",
    "rectangle": "rectangle", "rectangle_filled": "rectangle",
    "ellipse": "ellipse", "ellipse_filled": "ellipse",
    "circle": "circle", "circle_filled": "circle", "fill": "fill",
    "pick": "pick", "airbrush": "airbrush", "spray": "spray",
    "grab": "grab", "magnify": "magnify",
}
PT_TOOL_COUNT = 16

SHIM_H = r"""
#ifndef MMB_PRIV_H
#define MMB_PRIV_H
#include <stddef.h>
typedef struct mmb_platform_shim {
    unsigned (*get_pixel)(int x, int y);
} mmb_platform_shim;
typedef struct mmb_global_shim {
    mmb_platform_shim *plat;
} mmb_global_shim;
extern mmb_global_shim G;
#endif
"""

DRIVER = r"""
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "paint.h"
#include "paint_cursor_art.h"

#define BG 0x112233u
#define NPX (PT_W * PT_H)

static unsigned fb[NPX];
static unsigned snap[NPX];
static unsigned char canvas_buf[PT_CANVAS_W * PT_CANVAS_H];

mmb_global_shim G;
static mmb_platform_shim g_plat;

pt_state PT;

static unsigned drv_get_pixel(int x, int y)
{
	if (x < 0 || y < 0 || x >= PT_W || y >= PT_H)
		return 0;
	return fb[y * PT_W + x];
}

/* The cursor now reads the TUI composition buffer through tui_get_px (#701);
 * here that is the same in-memory screen the module draws into. */
unsigned tui_get_px(int x, int y)
{
	return drv_get_pixel(x, y);
}

void pt_damage_present(int x, int y, int w, int h)
{
	(void)x; (void)y; (void)w; (void)h;
}

void pt_plot(int x, int y, unsigned rgb)
{
	if (x < 0 || y < 0 || x >= PT_W || y >= PT_H)
		return;
	fb[y * PT_W + x] = rgb;
}

void pt_fill_rect(int x, int y, int w, int h, unsigned rgb)
{
	int i, j;
	for (j = 0; j < h; j++)
		for (i = 0; i < w; i++)
			pt_plot(x + i, y + j, rgb);
}

unsigned pt_palette_rgb(int idx)
{
	static const unsigned ibm[16] = {
		0x000000u, 0x0000AAu, 0x00AA00u, 0x00AAAAu,
		0xAA0000u, 0xAA00AAu, 0xAA5500u, 0xAAAAAAu,
		0x555555u, 0x5555FFu, 0x55FF55u, 0x55FFFFu,
		0xFF5555u, 0xFF55FFu, 0xFFFF55u, 0xFFFFFFu
	};
	if (idx < 0)
		idx = 0;
	if (idx > 15)
		idx &= 15;
	return ibm[idx];
}

int pt_screen_to_canvas(int sx, int sy, int *cx, int *cy)
{
	int x = sx - PT_CANVAS_X, y = sy - PT_CANVAS_Y;
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
	if (cx < 0 || cy < 0 || cx >= PT.width || cy >= PT.height)
		return 0;
	return PT.canvas[(size_t)cy * PT.width + cx];
}

void pt_canvas_set(int cx, int cy, int idx)
{
	if (cx < 0 || cy < 0 || cx >= PT.width || cy >= PT.height)
		return;
	PT.canvas[(size_t)cy * PT.width + cx] = (unsigned char)idx;
}

void pt_draw_canvas(void) {}
void pt_present(void) {}
void pt_redraw(void) {}
void pt_request_redraw(void) {}

static int art_index(int tool)
{
	switch (tool) {
	case 0: return PCA_TOOL_PENCIL;
	case 1: return PCA_TOOL_ERASER;
	case 2: return PCA_TOOL_LINE;
	case 3: return PCA_TOOL_TEXT;
	case 4: return PCA_TOOL_RECTANGLE;
	case 5: return PCA_TOOL_RECTANGLE;
	case 6: return PCA_TOOL_ELLIPSE;
	case 7: return PCA_TOOL_ELLIPSE;
	case 8: return PCA_TOOL_CIRCLE;
	case 9: return PCA_TOOL_CIRCLE;
	case 10: return PCA_TOOL_FILL;
	case 11: return PCA_TOOL_PICK;
	case 12: return PCA_TOOL_AIRBRUSH;
	case 13: return PCA_TOOL_SPRAY;
	case 14: return PCA_TOOL_GRAB;
	case 15: return PCA_TOOL_MAGNIFY;
	default: return PCA_TOOL_ARROW;
	}
}

static void fill_bg(void)
{
	int i;
	for (i = 0; i < NPX; i++)
		fb[i] = BG;
}

static int all_bg(void)
{
	int i;
	for (i = 0; i < NPX; i++)
		if (fb[i] != BG)
			return 0;
	return 1;
}

static int same_snap(void)
{
	int i;
	for (i = 0; i < NPX; i++)
		if (fb[i] != snap[i])
			return 0;
	return 1;
}

/* Verify every pixel of the 32x32 footprint matches the art (or BG when the
 * art is transparent), and that nothing outside the footprint changed. */
static int check_shape(int tool, int active)
{
	int idx = art_index(tool);
	const pca_sprite_t *sp = &pca_sprites[idx];
	const uint8_t *art = active ? sp->active : sp->idle;
	int hx = active ? sp->active_hotspot_x : sp->idle_hotspot_x;
	int hy = active ? sp->active_hotspot_y : sp->idle_hotspot_y;
	int sx = 100, sy = 100, ox = sx - hx, oy = sy - hy;
	int x, y;

	fill_bg();
	pt_cursor_init();
	pt_cursor_draw(sx, sy, tool, active);

	for (y = 0; y < PT_H; y++) {
		for (x = 0; x < PT_W; x++) {
			int in = x >= ox && x < ox + PCA_CURSOR_W &&
				 y >= oy && y < oy + PCA_CURSOR_H;
			unsigned want = BG;
			if (in) {
				uint8_t c = art[(y - oy) * PCA_CURSOR_W + (x - ox)];
				if (c != PCA_CURSOR_TRANSPARENT)
					want = pt_palette_rgb(c);
			}
			if (fb[y * PT_W + x] != want) {
				printf("FAIL shape tool=%d active=%d at %d,%d\n",
				       tool, active, x, y);
				return 0;
			}
		}
	}

	/* Restore must return the buffer exactly to the background. */
	pt_cursor_restore();
	if (!all_bg()) {
		printf("FAIL shape restore tool=%d active=%d\n", tool, active);
		return 0;
	}
	return 1;
}

/* Over the non-canvas chrome the cursor is the plain arrow, not the tool. */
static int check_ui_arrow(void)
{
	int sx = 10, sy = 8;	/* menu bar: not over the canvas */
	const pca_sprite_t *sp = &pca_sprites[PCA_TOOL_ARROW];
	const uint8_t *art = sp->idle;
	int hx = sp->idle_hotspot_x, hy = sp->idle_hotspot_y;
	int ox = sx - hx, oy = sy - hy, x, y;

	fill_bg();
	pt_cursor_init();
	pt_cursor_draw(sx, sy, 0, 0);	/* pencil selected, but over the UI */

	for (y = 0; y < PT_H; y++) {
		for (x = 0; x < PT_W; x++) {
			int in = x >= ox && x < ox + PCA_CURSOR_W &&
				 y >= oy && y < oy + PCA_CURSOR_H;
			unsigned want = BG;

			if (in) {
				uint8_t c = art[(y - oy) * PCA_CURSOR_W + (x - ox)];
				if (c != PCA_CURSOR_TRANSPARENT)
					want = pt_palette_rgb(c);
			}
			if (fb[y * PT_W + x] != want)
				return 0;
		}
	}
	pt_cursor_restore();
	return all_bg();
}

static unsigned signature(int tool, int active)
{
	int idx = art_index(tool);
	const pca_sprite_t *sp = &pca_sprites[idx];
	const uint8_t *art = active ? sp->active : sp->idle;
	int hx = active ? sp->active_hotspot_x : sp->idle_hotspot_x;
	int hy = active ? sp->active_hotspot_y : sp->idle_hotspot_y;
	int ox = 100 - hx, oy = 100 - hy;
	unsigned h = 2166136261u;
	int x, y;

	fill_bg();
	pt_cursor_init();
	pt_cursor_draw(100, 100, tool, active);

	for (y = 0; y < PCA_CURSOR_H; y++) {
		for (x = 0; x < PCA_CURSOR_W; x++) {
			int px = ox + x, py = oy + y;
			uint8_t c = art[y * PCA_CURSOR_W + x];
			if (c == PCA_CURSOR_TRANSPARENT)
				continue;
			h ^= (unsigned)(px * 131 + py);
			h *= 16777619u;
			h ^= (unsigned)c;
			h *= 16777619u;
		}
	}
	pt_cursor_restore();
	return h;
}

int main(void)
{
	int t, active, fails = 0;

	g_plat.get_pixel = drv_get_pixel;
	G.plat = &g_plat;
	PT.width = PT_CANVAS_W;
	PT.height = PT_CANVAS_H;

	/* 1. Draw changes pixels; restore puts them back exactly. */
	fill_bg();
	memcpy(snap, fb, sizeof(fb));
	pt_cursor_init();
	pt_cursor_draw(100, 100, 0, 0);
	if (same_snap()) {
		printf("FAIL restore_drew_nothing\n");
		fails++;
	} else {
		pt_cursor_restore();
		if (!same_snap()) {
			printf("FAIL restore_exact\n");
			fails++;
		} else {
			printf("OK restore_exact\n");
		}
	}

	/* 2. Moving lifts the old sprite: no trail anywhere. */
	fill_bg();
	pt_cursor_init();
	pt_cursor_draw(200, 120, 5, 0);
	pt_cursor_draw(300, 160, 5, 0);
	pt_cursor_restore();
	if (!all_bg()) {
		printf("FAIL move_exact\n");
		fails++;
	} else {
		printf("OK move_exact\n");
	}

	/* 3. Restore is idempotent and a no-op with nothing on screen. */
	pt_cursor_restore();
	pt_cursor_restore();
	if (!all_bg()) {
		printf("FAIL restore_idempotent\n");
		fails++;
	} else {
		printf("OK restore_idempotent\n");
	}

	/* 4. Every tool/state draws exactly its art and restores. t ==
	 * PT_TOOL_COUNT is the out-of-range id that maps to the arrow. */
	{
		int shapes_ok = 1;
		for (t = 0; t <= (int)PT_TOOL_COUNT; t++)
			for (active = 0; active <= 1; active++)
				if (!check_shape(t, active))
					shapes_ok = 0;
		if (shapes_ok)
			printf("OK all_shapes\n");
		else {
			printf("FAIL all_shapes\n");
			fails++;
		}
	}

	/* 5. Over the chrome the cursor is the arrow, not the selected tool. */
	if (check_ui_arrow())
		printf("OK ui_arrow\n");
	else {
		printf("FAIL ui_arrow\n");
		fails++;
	}

	/* 6. Shape signatures: per-tool distinct and idle != active. */
	for (t = 0; t < (int)PT_TOOL_COUNT; t++)
		printf("SIG %d %u %u\n", t, signature(t, 0), signature(t, 1));
	printf("SIGARROW %u %u\n", signature(PT_TOOL_COUNT, 0),
	       signature(PT_TOOL_COUNT, 1));

	/* 6. Unknown tool id falls back to the arrow art. */
	if (signature(999, 0) != signature(PT_TOOL_COUNT, 0)) {
		printf("FAIL default_arrow\n");
		fails++;
	} else {
		printf("OK default_arrow\n");
	}

	/* 7. Partial off-screen and corner sprites still restore exactly. */
	fill_bg();
	memcpy(snap, fb, sizeof(fb));
	pt_cursor_init();
	pt_cursor_draw(0, 0, 0, 0);
	pt_cursor_draw(PT_W - 1, PT_H - 1, 0, 1);
	pt_cursor_draw(-8, -8, 3, 0);
	pt_cursor_restore();
	if (!same_snap()) {
		printf("FAIL edge_restore\n");
		fails++;
	} else {
		printf("OK edge_restore\n");
	}

	/* 8. Cursor is never written into the canvas a PCX save reads. */
	PT.width = PT_CANVAS_W;
	PT.height = PT_CANVAS_H;
	PT.canvas = canvas_buf;
	memset(canvas_buf, 0, sizeof(canvas_buf));
	memcpy(snap, fb, sizeof(fb));
	pt_cursor_init();
	pt_cursor_draw(PT_CANVAS_X + 50, PT_CANVAS_Y + 50, 0, 1);
	{
		int i, dirty = 0;
		for (i = 0; i < (int)sizeof(canvas_buf); i++)
			if (canvas_buf[i] != 0)
				dirty = 1;
		if (dirty) {
			printf("FAIL canvas_clean\n");
			fails++;
		} else {
			printf("OK canvas_clean\n");
		}
	}
	pt_cursor_restore();

	printf("%s\n", fails ? "FAILURES" : "ALL OK");
	return fails ? 1 : 0;
}
"""


@pytest.fixture(scope="module")
def run(tmp_path_factory):
    if shutil.which("cc") is None:
        pytest.skip("needs a host C toolchain (cc)")
    tmp = tmp_path_factory.mktemp("paint_cursors")
    (tmp / "mmb_priv.h").write_text(SHIM_H)
    (tmp / "driver.c").write_text(DRIVER)
    exe = tmp / "driver"
    subprocess.run(
        [
            "cc", "-std=c11", "-O0", "-Wall", "-Wextra", "-Werror",
            "-I", str(tmp), "-I", SRC,
            "-I", os.path.join(REPO, "mmbasic", "include"),
            "-o", str(exe),
            str(tmp / "driver.c"), CURSORS_C, ART_C,
        ],
        check=True,
        cwd=REPO,
    )
    proc = subprocess.run([str(exe)], check=False, capture_output=True, text=True)
    return proc


def test_cursor_compiles_and_all_checks_pass(run):
    out = run.stdout + run.stderr
    assert run.returncode == 0, out
    assert "ALL OK" in out, out
    assert "FAIL" not in out, out


def test_restore_is_exact_and_moves_leave_no_trail(run):
    out = run.stdout
    for marker in ("OK restore_exact", "OK move_exact", "OK restore_idempotent"):
        assert marker in out, out


def test_every_tool_shape_drawn_from_art(run):
    assert "OK all_shapes" in run.stdout


def test_arrow_over_chrome_and_tool_over_canvas(run):
    """Hovering non-canvas UI shows the arrow, even with a tool selected."""
    assert "OK ui_arrow" in run.stdout, run.stdout


def test_shapes_follow_tool_and_idle_active_differ(run):
    sigs = {}
    for line in run.stdout.splitlines():
        parts = line.split()
        if not parts:
            continue
        if parts[0] == "SIG":
            sigs[parts[1]] = (parts[2], parts[3])
        elif parts[0] == "SIGARROW":
            sigs["arrow"] = (parts[1], parts[2])
    assert len(sigs) == PT_TOOL_COUNT + 1
    # Every tool maps to a baked sprite; the filled variants share their
    # outline sibling, so the 16 tools collapse onto the 14 sprites.
    assert len({v[0] for v in sigs.values()}) == len(TOOLS)
    assert len({v[1] for v in sigs.values()}) == len(TOOLS)
    # Idle and active art differ for every tool.
    for idx, (idle, active) in sigs.items():
        assert idle != active, idx


def test_unknown_tool_falls_back_to_arrow(run):
    assert "OK default_arrow" in run.stdout


def test_edges_restore_and_canvas_stays_clean(run):
    assert "OK edge_restore" in run.stdout
    assert "OK canvas_clean" in run.stdout


def test_tool_id_mapping_covers_art():
    """Every PT tool id maps onto a baked sprite; filled variants share."""
    assert len(TOOL_IDS) == PT_TOOL_COUNT
    assert set(TOOL_IDS.values()) == set(TOOLS) - {"arrow"}


# ---- #701 ghost regression -------------------------------------------------
#
# On a real Pi the HDMI surface read by get_pixel() still shows the previous
# frame (with the old cursor in it) while PAINT composes into the TUI buffer.
# The module must read the composition buffer via tui_get_px(). This driver
# models two surfaces: `fb` (composition, read by tui_get_px) and `presented`
# (stale HDMI, read by get_pixel, with a fake old cursor already stamped in).
# If the module ever reads the presented surface, restore leaves a ghost.

GHOST_DRIVER = r"""
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "paint.h"
#include "paint_cursor_art.h"

#define NPX (PT_W * PT_H)
static unsigned fb[NPX];
static unsigned presented[NPX];

mmb_global_shim G;
static mmb_platform_shim g_plat;

static unsigned pat(int x, int y)
{
	return 0x01000000u | (((unsigned)x * 7u + (unsigned)y * 13u) & 0xFFFFu);
}

static unsigned drv_get_pixel(int x, int y)	/* stale HDMI surface */
{
	if (x < 0 || y < 0 || x >= PT_W || y >= PT_H)
		return 0;
	return presented[y * PT_W + x];
}

unsigned tui_get_px(int x, int y)		/* live composition buffer */
{
	if (x < 0 || y < 0 || x >= PT_W || y >= PT_H)
		return 0;
	return fb[y * PT_W + x];
}

void pt_damage_present(int x, int y, int w, int h)
{
	(void)x; (void)y; (void)w; (void)h;
}
void pt_plot(int x, int y, unsigned rgb)
{
	if (x < 0 || y < 0 || x >= PT_W || y >= PT_H)
		return;
	fb[y * PT_W + x] = rgb;
}
void pt_fill_rect(int x, int y, int w, int h, unsigned rgb)
{
	int i, j;

	for (j = 0; j < h; j++)
		for (i = 0; i < w; i++)
			pt_plot(x + i, y + j, rgb);
}
unsigned pt_palette_rgb(int idx) { return 0x100u + (unsigned)idx; }

int pt_screen_to_canvas(int sx, int sy, int *cx, int *cy)
{
	(void)sx; (void)sy; (void)cx; (void)cy;
	return 0;
}

int main(void)
{
	int x, y;

	for (y = 0; y < PT_H; y++)
		for (x = 0; x < PT_W; x++)
			fb[y * PT_W + x] = pat(x, y);
	memcpy(presented, fb, sizeof fb);
	g_plat.get_pixel = drv_get_pixel;
	G.plat = &g_plat;

	/* The previous presented frame already contains the old cursor. */
	for (y = -3; y <= 3; y++)
		for (x = -3; x <= 3; x++)
			if (100 + x >= 0 && 100 + y >= 0 &&
			    100 + x < PT_W && 100 + y < PT_H)
				presented[(100 + y) * PT_W + (100 + x)] =
					0x00FF00FFu;

	pt_cursor_init();
	pt_cursor_draw(100, 100, 0, 0);
	pt_cursor_draw(140, 120, 0, 0);
	pt_cursor_restore();

	for (y = 0; y < PT_H; y++)
		for (x = 0; x < PT_W; x++)
			if (fb[y * PT_W + x] != pat(x, y))
			{
				printf("FAIL ghost at %d,%d\n", x, y);
				return 1;
			}
	printf("OK no_ghost\n");
	return 0;
}
"""


@pytest.fixture(scope="module")
def ghost_run(tmp_path_factory):
    if shutil.which("cc") is None:
        pytest.skip("needs a host C toolchain (cc)")
    tmp = tmp_path_factory.mktemp("paint_cursor_ghost")
    (tmp / "mmb_priv.h").write_text(SHIM_H)
    (tmp / "driver.c").write_text(GHOST_DRIVER)
    exe = tmp / "driver"
    subprocess.run(
        [
            "cc", "-std=c11", "-O0", "-Wall", "-Wextra", "-Werror",
            "-I", str(tmp), "-I", SRC,
            "-I", os.path.join(REPO, "mmbasic", "include"),
            "-o", str(exe),
            str(tmp / "driver.c"), CURSORS_C, ART_C,
        ],
        check=True,
        cwd=REPO,
    )
    return subprocess.run([str(exe)], check=False, capture_output=True, text=True)


def test_cursor_reads_composition_buffer_no_ghost(ghost_run):
    out = ghost_run.stdout + ghost_run.stderr
    assert ghost_run.returncode == 0, out
    assert "OK no_ghost" in out, out
