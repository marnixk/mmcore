#!/usr/bin/env python3
"""Generate the PAINT 32x32 tool cursors (#632).

The PAINT rewrite draws the cursor with the sprite-restore approach: a 32x32
palette-indexed bitmap is stamped under the pointer and the pixels underneath
are saved/restored. This script authors the art procedurally and bakes it into
``mmbasic/src/paint_cursor_art.c`` + ``.h``.

Design rules
------------
* 32x32, row-major, one byte per pixel holding a **default VGA 256** palette
  index.
* Index 255 is the transparent colour; the runtime never stamps it.
* Bodies are white (15) with a black (0) outline, the classic paint cursor
  look: legible over both light and dark canvas pixels.
* Each tool has an idle sprite and an active (button-down) sprite that differs.
* Hotspots are in sprite coordinates, 0..31, and must name a **non-transparent**
  pixel (the writing tip for the pointer tools, the centre for the shapes):
  a hotspot on a transparent pixel lands the pointer beside the drawn art,
  which is what #690 reported. ``build()`` enforces the invariant.

Run from the repo root:

    python3 assets/paint/gen_cursor_art.py            # write the C files
    python3 assets/paint/gen_cursor_art.py --preview  # ASCII preview only
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

W = H = 32
N = W * H

# Palette indices (default VGA 256).
T = 255       # transparent
BLACK = 0
WHITE = 15
GREY = 8      # dark grey
LGREY = 7
RED = 12
GREEN = 10
YELLOW = 14
CYAN = 11
BLUE = 9
MAGENTA = 13

REPO = Path(__file__).resolve().parents[2]
OUT_C = REPO / "mmbasic" / "src" / "paint_cursor_art.c"
OUT_H = REPO / "mmbasic" / "src" / "paint_cursor_art.h"

# Tools in the order the C header exposes them. The names must match the
# PCA_TOOL_* enum the generator emits.
TOOLS = [
    "arrow",
    "pencil",
    "line",
    "rectangle",
    "ellipse",
    "circle",
    "fill",
    "eraser",
    "pick",
    "grab",
    "magnify",
    "airbrush",
    "spray",
    "text",
]

# Tools whose active sprite is expected to differ (button-held feedback).
SELECTION_TOOLS = {
    "arrow", "pencil", "line", "rectangle", "ellipse", "circle", "fill",
    "eraser", "pick", "grab", "magnify", "airbrush", "spray", "text",
}


# --------------------------------------------------------------------------
# raster primitives -- all operate on a 32x32 boolean mask (list of rows)
# --------------------------------------------------------------------------

def blank_mask():
    return [[False] * W for _ in range(H)]


def put(mask, x, y, v=True):
    if 0 <= x < W and 0 <= y < H:
        mask[y][x] = v


def add_disc(mask, cx, cy, rx, ry=None):
    ry = rx if ry is None else ry
    if rx <= 0 or ry <= 0:
        return mask
    for y in range(H):
        for x in range(W):
            dx = (x - cx) / (rx + 0.35)
            dy = (y - cy) / (ry + 0.35)
            if dx * dx + dy * dy <= 1.0:
                mask[y][x] = True
    return mask


def add_ring(mask, cx, cy, r, rx=None):
    rx = r if rx is None else rx
    inner = blank_mask()
    add_disc(inner, cx, cy, max(0.0, rx - 2), max(0.0, r - 2))
    add_disc(mask, cx, cy, rx, r)
    for y in range(H):
        for x in range(W):
            if inner[y][x]:
                mask[y][x] = False


def add_rect(mask, x0, y0, x1, y1):
    for y in range(min(y0, y1), max(y0, y1) + 1):
        for x in range(min(x0, x1), max(x0, x1) + 1):
            put(mask, x, y)


def add_rect_outline(mask, x0, y0, x1, y1, t=2):
    add_rect(mask, x0, y0, x1, y1)
    cut = blank_mask()
    add_rect(cut, x0 + t, y0 + t, x1 - t, y1 - t)
    for y in range(H):
        for x in range(W):
            if cut[y][x]:
                mask[y][x] = False


def add_seg(mask, x0, y0, x1, y1, r):
    """Thick capsule from (x0,y0) to (x1,y1), radius r."""
    dx, dy = x1 - x0, y1 - y0
    steps = max(abs(dx), abs(dy), 1) * 3
    for i in range(steps + 1):
        t = i / steps
        cx = x0 + dx * t
        cy = y0 + dy * t
        for oy in range(-(r + 1), r + 2):
            for ox in range(-(r + 1), r + 2):
                if ox * ox + oy * oy <= (r + 0.4) ** 2:
                    put(mask, round(cx) + ox, round(cy) + oy)


def add_poly(mask, points):
    """Even-odd scanline polygon fill."""
    ys = [p[1] for p in points]
    y0, y1 = max(0, int(min(ys))), min(H - 1, int(max(ys)))
    n = len(points)
    for y in range(y0, y1 + 1):
        xs = []
        for i in range(n):
            ax, ay = points[i]
            bx, by = points[(i + 1) % n]
            if ay == by:
                continue
            if min(ay, by) <= y < max(ay, by):
                t = (y - ay) / (by - ay)
                xs.append(ax + (bx - ax) * t)
        xs.sort()
        for i in range(0, len(xs) - 1, 2):
            for x in range(int(round(xs[i])), int(round(xs[i + 1])) + 1):
                put(mask, x, y)


def union(*masks):
    out = blank_mask()
    for m in masks:
        for y in range(H):
            for x in range(W):
                if m[y][x]:
                    out[y][x] = True
    return out


def invert_of(base, sub):
    out = blank_mask()
    for y in range(H):
        for x in range(W):
            if base[y][x] and not sub[y][x]:
                out[y][x] = True
    return out


def outline_of(mask):
    """Cells just outside the mask (for a crisp black border)."""
    out = blank_mask()
    for y in range(H):
        for x in range(W):
            if mask[y][x]:
                continue
            for oy, ox in ((-1, 0), (1, 0), (0, -1), (0, 1)):
                ny, nx = y + oy, x + ox
                if 0 <= ny < H and 0 <= nx < W and mask[ny][nx]:
                    out[y][x] = True
                    break
    return out


def render(mask, body=WHITE, border=BLACK):
    """White body, one-pixel black outline all around the union."""
    g = [[T] * W for _ in range(H)]
    btn = outline_of(mask)
    for y in range(H):
        for x in range(W):
            if mask[y][x]:
                g[y][x] = body
            elif btn[y][x]:
                g[y][x] = border
    return g


def stamp(grid, mask, colour):
    for y in range(H):
        for x in range(W):
            if mask[y][x]:
                grid[y][x] = colour


def dot(grid, x, y, colour):
    if 0 <= x < W and 0 <= y < H:
        grid[y][x] = colour


def handle(grid, x, y, colour=BLACK):
    """A small filled square handle centred on (x,y)."""
    for oy in range(-1, 2):
        for ox in range(-1, 2):
            dot(grid, x + ox, y + oy, colour)


# --------------------------------------------------------------------------
# per-tool art
# --------------------------------------------------------------------------

def art_arrow(active):
    m = blank_mask()
    # Classic pointer: tip top-left, tail down-right, with a notch.
    add_poly(m, [(2, 2), (2, 22), (7, 17), (10, 25), (14, 23), (11, 15), (17, 15)])
    g = render(m)
    if active:
        # Pressed: solid notch highlight at the tip.
        stamp(g, add_disc(blank_mask(), 4, 5, 2), RED)
    # Hotspot is the topmost lit pixel of the tip (the black outline caps the
    # white vertex at (2,2)); (2,2) itself sits one pixel down-right of the
    # drawn tip (#690).
    return g, (2, 1)


def art_pencil(active):
    m = blank_mask()
    # Body: thick diagonal from upper-right to lower-left.
    add_seg(m, 25, 5, 11, 19, 3)
    # Ferrule band near the top.
    # Tip triangle down to the writing point.
    add_poly(m, [(12, 18), (16, 22), (7, 28), (5, 25)])
    g = render(m)
    # Wood band and graphite tip as colour accents.
    tip = blank_mask()
    add_poly(tip, [(8, 21), (12, 25), (8, 27), (5, 25)])
    stamp(g, tip, GREY)
    point = blank_mask()
    add_poly(point, [(6, 24), (8, 27), (4, 29)])
    stamp(g, point, BLACK if not active else RED)
    if active:
        # Pressed: little graphite dot at the point.
        stamp(g, add_disc(blank_mask(), 3, 28, 1), RED)
    # The idle art's writing tip is its bottom-left pixel (4,28); the active
    # art's red graphite dot extends one pixel further to (3,28). Each state
    # names an opaque tip pixel so the pointer is never beside the art (#690).
    return g, ((3, 28) if active else (4, 28))


def art_line(active):
    m = blank_mask()
    add_seg(m, 7, 24, 24, 7, 1)
    # Endpoint handles.
    add_disc(m, 6, 25, 3)
    add_disc(m, 25, 6, 3)
    g = render(m)
    if active:
        # Committed: fill the anchors with the draw colour.
        stamp(g, add_disc(blank_mask(), 6, 25, 1), RED)
        stamp(g, add_disc(blank_mask(), 25, 6, 1), RED)
    return g, (6, 25)


def art_rectangle(active):
    m = blank_mask()
    if active:
        add_rect(m, 5, 7, 26, 24)
    else:
        add_rect_outline(m, 5, 7, 26, 24, t=2)
    g = render(m)
    if active:
        # Shade the interior so it reads as "committed", keep a red corner.
        inner = blank_mask()
        add_rect(inner, 7, 9, 24, 22)
        stamp(g, inner, LGREY)
        handle(g, 5, 7, RED)
    return g, (5, 7)


def art_ellipse(active):
    m = blank_mask()
    if active:
        add_disc(m, 15, 15, 10, 7)
    else:
        add_ring(m, 15, 15, 7, 10)
    g = render(m)
    if active:
        inner = blank_mask()
        add_disc(inner, 15, 15, 8, 5)
        stamp(g, inner, LGREY)
    else:
        # Mark the centre of the hollow ring so the centre hotspot is opaque.
        dot(g, 15, 15, BLACK)
    return g, (15, 15)


def art_circle(active):
    m = blank_mask()
    if active:
        add_disc(m, 15, 15, 10)
    else:
        add_ring(m, 15, 15, 10)
    g = render(m)
    if active:
        inner = blank_mask()
        add_disc(inner, 15, 15, 8)
        stamp(g, inner, LGREY)
    else:
        # Mark the centre of the hollow ring so the centre hotspot is opaque.
        dot(g, 15, 15, BLACK)
    return g, (15, 15)


def art_fill(active):
    m = blank_mask()
    # Tilted bucket: trapezoid body.
    add_poly(m, [(8, 8), (24, 12), (20, 26), (10, 24)])
    # Handle.
    hm = blank_mask()
    add_seg(hm, 10, 9, 20, 5, 1)
    add_seg(hm, 20, 5, 24, 10, 1)
    m = union(m, hm)
    g = render(m)
    # Paint level inside the bucket.
    paint = blank_mask()
    add_poly(paint, [(11, 13), (22, 16), (20, 23), (12, 21)])
    stamp(g, paint, CYAN)
    # Spout drip.
    stamp(g, add_disc(blank_mask(), 25, 15, 2), CYAN)
    if active:
        # Spilling: a stream and a puddle.
        stamp(g, add_disc(blank_mask(), 27, 19, 2), CYAN)
        stamp(g, add_disc(blank_mask(), 28, 23, 2), CYAN)
    return g, (8, 8)


def art_eraser(active):
    m = blank_mask()
    # Tilted eraser block with a rounded nose at the lower left.
    add_poly(m, [(11, 10), (25, 17), (20, 27), (6, 20)])
    add_disc(m, 8, 20, 3)
    g = render(m)
    band = blank_mask()
    add_poly(band, [(16, 12), (22, 15), (20, 21), (14, 18)])
    stamp(g, band, MAGENTA)
    if active:
        # Erasing: crumbs flying off the nose.
        for (x, y) in ((4, 16), (3, 22), (6, 26), (1, 19)):
            stamp(g, add_disc(blank_mask(), x, y, 1), LGREY)
    return g, (5, 22)


def art_pick(active):
    m = blank_mask()
    # Eyedropper: bulb top-right, tapering to a point lower-left.
    add_seg(m, 24, 7, 9, 22, 3)
    add_disc(m, 24, 6, 4)
    add_poly(m, [(12, 19), (15, 23), (6, 29), (4, 26)])
    g = render(m)
    if active:
        # Sampled: a droplet of the picked colour.
        d = blank_mask()
        add_disc(d, 7, 28, 2)
        stamp(g, d, RED)
    return g, (4, 27)


def art_grab(active):
    m = blank_mask()
    # Four-way move arrows: cross shafts plus arrowheads.
    add_rect(m, 14, 5, 17, 26)
    add_rect(m, 5, 14, 26, 17)
    add_poly(m, [(15, 2), (12, 7), (19, 7)])
    add_poly(m, [(15, 29), (12, 24), (19, 24)])
    add_poly(m, [(2, 15), (7, 12), (7, 19)])
    add_poly(m, [(29, 15), (24, 12), (24, 19)])
    g = render(m)
    if active:
        # Grabbed: a filled centre grip.
        stamp(g, add_disc(blank_mask(), 15, 15, 3), RED)
    return g, (15, 15)


def art_magnify(active):
    m = blank_mask()
    add_ring(m, 13, 12, 9)
    add_seg(m, 20, 19, 27, 27, 2)
    g = render(m)
    # Lens tint so the icon reads as glass.
    lens = blank_mask()
    add_disc(lens, 13, 12, 7)
    stamp(g, lens, CYAN)
    if active:
        # Zoomed: a magnified plus inside the lens.
        stamp(g, add_disc(blank_mask(), 13, 12, 2), WHITE)
        plus = blank_mask()
        add_rect(plus, 9, 11, 16, 12)
        add_rect(plus, 12, 8, 13, 15)
        stamp(g, plus, WHITE)
    return g, (13, 12)


def art_airbrush(active):
    m = blank_mask()
    # Airbrush body: a pen with a nozzle to the left.
    add_poly(m, [(9, 10), (28, 13), (28, 19), (9, 22)])
    add_rect(m, 24, 9, 28, 23)
    g = render(m)
    nozzle = blank_mask()
    add_poly(nozzle, [(4, 13), (9, 12), (9, 20), (4, 19)])
    stamp(g, nozzle, GREY)
    if active:
        # Spraying: a fan of droplets from the nozzle.
        for (x, y) in ((4, 8), (2, 12), (3, 17), (1, 21), (5, 24), (7, 6), (5, 27)):
            stamp(g, add_disc(blank_mask(), x, y, 1), CYAN)
    return g, (4, 16)


def art_spray(active):
    m = blank_mask()
    # Spray can: tall body, cap and trigger.
    add_rect(m, 10, 14, 21, 28)
    add_poly(m, [(11, 14), (20, 14), (19, 9), (12, 9)])
    add_rect(m, 16, 5, 21, 9)
    add_rect(m, 22, 7, 26, 10)
    g = render(m)
    body = blank_mask()
    add_rect(body, 11, 15, 20, 22)
    stamp(g, body, RED)
    if active:
        # Spraying: cloud of dots out of the nozzle.
        for (x, y) in ((25, 3), (28, 6), (24, 9), (29, 11), (27, 13)):
            stamp(g, add_disc(blank_mask(), x, y, 1), WHITE)
    return g, (24, 8)


def art_text(active):
    m = blank_mask()
    # Capital A built from two strokes and a crossbar.
    add_seg(m, 8, 26, 15, 7, 2)
    add_seg(m, 15, 7, 22, 26, 2)
    add_rect(m, 11, 18, 19, 21)
    g = render(m)
    if active:
        # Editing: a text caret to the right of the letter.
        caret = blank_mask()
        add_rect(caret, 29, 9, 29, 27)
        add_poly(caret, [(27, 9), (31, 9), (29, 6)])
        add_poly(caret, [(27, 27), (31, 27), (29, 30)])
        stamp(g, caret, WHITE)
    return g, (8, 26)


ART = {
    "arrow": art_arrow,
    "pencil": art_pencil,
    "line": art_line,
    "rectangle": art_rectangle,
    "ellipse": art_ellipse,
    "circle": art_circle,
    "fill": art_fill,
    "eraser": art_eraser,
    "pick": art_pick,
    "grab": art_grab,
    "magnify": art_magnify,
    "airbrush": art_airbrush,
    "spray": art_spray,
    "text": art_text,
}


# --------------------------------------------------------------------------
# emit
# --------------------------------------------------------------------------

def ascii_preview(grid):
    chars = {T: ".", BLACK: "#", WHITE: "O", GREY: "g", LGREY: "-",
             RED: "R", GREEN: "G", YELLOW: "Y", CYAN: "c", BLUE: "b",
             MAGENTA: "m"}
    return "\n".join(
        "".join(chars.get(v, "?") for v in row) for row in grid
    )


def flatten(grid):
    return [grid[y][x] for y in range(H) for x in range(W)]


def emit_c(arts):
    lines = []
    lines.append("/* Generated by assets/paint/gen_cursor_art.py -- do not edit by hand.")
    lines.append(" * 32x32 tool cursors for the PAINT rewrite (#632).")
    lines.append(" * Palette indices are default VGA 256; %d is transparent." % T)
    lines.append(" */")
    lines.append('#include "paint_cursor_art.h"')
    lines.append("")
    for tool in TOOLS:
        for state in ("idle", "active"):
            data = flatten(arts[tool][state])
            lines.append("const uint8_t pca_%s_%s[PCA_CURSOR_PIXELS] = {" % (tool, state))
            for i in range(0, N, 16):
                chunk = ", ".join("%3d" % v for v in data[i:i + 16])
                lines.append("\t" + chunk + ",")
            lines.append("};")
            lines.append("")
    lines.append("const pca_sprite_t pca_sprites[PCA_TOOL_COUNT] = {")
    for tool in TOOLS:
        ihx, ihy = arts[tool]["idle_hot"]
        ahx, ahy = arts[tool]["active_hot"]
        lines.append(
            '\t{ "%s", pca_%s_idle, pca_%s_active,'
            " %d, %d, %d, %d, %d },"
            % (tool, tool, tool, ihx, ihy, ahx, ahy, T)
        )
    lines.append("};")
    lines.append("")
    return "\n".join(lines)


def emit_h():
    enum = "\n".join(
        "\tPCA_TOOL_%s = %d," % (t.upper(), i) for i, t in enumerate(TOOLS)
    )
    return """/* PAINT tool cursor art (#632).
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
#define PCA_CURSOR_TRANSPARENT %(transparent)d

typedef enum {
%(enum)s
\tPCA_TOOL_COUNT
} pca_tool_t;

/* One tool's idle and active art. Each bitmap is PCA_CURSOR_PIXELS bytes of
 * row-major default-VGA palette indices; PCA_CURSOR_TRANSPARENT marks a pixel
 * the sprite-restore runtime leaves untouched. The hotspot coordinates are in
 * sprite space (0..31), are carried per state, and always name a
 * non-transparent art pixel (#690). */
typedef struct {
\tconst char *name;
\tconst uint8_t *idle;
\tconst uint8_t *active;
\tuint8_t idle_hotspot_x;
\tuint8_t idle_hotspot_y;
\tuint8_t active_hotspot_x;
\tuint8_t active_hotspot_y;
\tuint8_t transparent;
} pca_sprite_t;

extern const pca_sprite_t pca_sprites[PCA_TOOL_COUNT];

%(arrays)s
#endif /* PAINT_CURSOR_ART_H */
""" % {
        "transparent": T,
        "enum": enum,
        "arrays": "\n".join(
            "extern const uint8_t pca_%s_%s[PCA_CURSOR_PIXELS];" % (t, s)
            for t in TOOLS for s in ("idle", "active")
        ),
    }


def check_hotspot(tool, state, grid, hot):
    """Sanity check: the hotspot must name a non-transparent art pixel (#690).

    A hotspot on a transparent pixel places the pointer beside the drawn art,
    so the requested point is not on the cursor at all.
    """
    hx, hy = hot
    if not (0 <= hx < W and 0 <= hy < H):
        raise AssertionError(
            "%s %s hotspot (%d,%d) is outside the sprite" % (tool, state, hx, hy)
        )
    if grid[hy][hx] == T:
        raise AssertionError(
            "%s %s hotspot (%d,%d) is transparent" % (tool, state, hx, hy)
        )


def build():
    arts = {}
    for tool in TOOLS:
        idle, idle_hot = ART[tool](False)
        active, active_hot = ART[tool](True)
        arts[tool] = {
            "idle": idle,
            "active": active,
            "idle_hot": idle_hot,
            "active_hot": active_hot,
        }
        check_hotspot(tool, "idle", idle, idle_hot)
        check_hotspot(tool, "active", active, active_hot)
    return arts


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--preview", action="store_true",
                    help="print ASCII previews instead of writing files")
    args = ap.parse_args(argv)

    arts = build()

    if args.preview:
        for tool in TOOLS:
            for state in ("idle", "active"):
                print("=== %s (%s) ===" % (tool, state))
                print(ascii_preview(arts[tool][state]))
                print()
        return 0

    OUT_H.write_text(emit_h())
    OUT_C.write_text(emit_c(arts))
    print("wrote %s" % OUT_H)
    print("wrote %s" % OUT_C)
    return 0


if __name__ == "__main__":
    sys.exit(main())
