#!/usr/bin/env python3
"""Generate the PAINT toolbar tool icons.

The left tool column draws a 16x16 glyph per tool (`draw_tool_icon` in
`mmbasic/src/paint_tools.c`). This script authors those glyphs procedurally and
bakes them into `mmbasic/src/paint_tool_icons.h`.

Design rules
------------
* 16x16, row-major, one byte per pixel:
    0 = transparent, 1 = ink (drawn in the cell's icon colour),
    2 = cut (drawn in the cell background, so internal detail reads on both
        the selected and unselected highlight).
* The sprite order matches the PT_TOOL_* enum in `paint.h`.
* Icons are chunky and pictorial (bucket for fill, eraser block, 4-way move,
  eye-dropper, spray can) instead of abstract line glyphs.

Run from the repo root:

    python3 assets/paint/gen_tool_icons.py            # write the header
    python3 assets/paint/gen_tool_icons.py --preview  # ASCII preview only
"""

from __future__ import annotations

import argparse
from pathlib import Path

W = H = 16
N = W * H

T, INK, CUT = 0, 1, 2

REPO = Path(__file__).resolve().parents[2]
OUT_H = REPO / "mmbasic" / "src" / "paint_tool_icons.h"

TOOLS = [
    "pencil", "eraser", "line", "text", "rectangle", "rectangle_filled",
    "ellipse", "ellipse_filled", "circle", "circle_filled", "fill", "pick",
    "airbrush", "spray", "grab", "magnify", "select",
]


# --------------------------------------------------------------------------
# raster primitives, all on a 16x16 byte grid (0 = empty)
# --------------------------------------------------------------------------

def blank():
    return [[T] * W for _ in range(H)]


def put(m, x, y, v=INK):
    if 0 <= x < W and 0 <= y < H:
        m[y][x] = v


def add_disc(m, cx, cy, rx, ry=None, v=INK):
    ry = rx if ry is None else ry
    for y in range(H):
        for x in range(W):
            dx = (x - cx) / (rx + 0.35)
            dy = (y - cy) / (ry + 0.35)
            if dx * dx + dy * dy <= 1.0:
                m[y][x] = v


def add_ring(m, cx, cy, rx, ry=None, t=2, v=INK):
    ry = rx if ry is None else ry
    inner = blank()
    add_disc(inner, cx, cy, max(0.0, rx - t), max(0.0, ry - t))
    add_disc(m, cx, cy, rx, ry, v)
    for y in range(H):
        for x in range(W):
            if inner[y][x]:
                m[y][x] = T


def add_rect(m, x0, y0, x1, y1, v=INK):
    for y in range(min(y0, y1), max(y0, y1) + 1):
        for x in range(min(x0, x1), max(x0, x1) + 1):
            put(m, x, y, v)


def add_rect_outline(m, x0, y0, x1, y1, t=1, v=INK):
    add_rect(m, x0, y0, x1, y1, v)
    x0, x1 = min(x0, x1), max(x0, x1)
    y0, y1 = min(y0, y1), max(y0, y1)
    for y in range(y0 + t, y1 - t + 1):
        for x in range(x0 + t, x1 - t + 1):
            m[y][x] = T


def add_seg(m, x0, y0, x1, y1, r, v=INK):
    dx, dy = x1 - x0, y1 - y0
    steps = int(max(abs(dx), abs(dy), 1) * 3)
    for i in range(steps + 1):
        f = i / steps
        cx, cy = x0 + dx * f, y0 + dy * f
        ir = int(r) + 1
        for oy in range(-ir, ir + 1):
            for ox in range(-ir, ir + 1):
                if ox * ox + oy * oy <= (r + 0.4) ** 2:
                    put(m, round(cx) + ox, round(cy) + oy, v)


def add_poly(m, pts, v=INK):
    ys = [p[1] for p in pts]
    y0, y1 = max(0, int(min(ys))), min(H - 1, int(max(ys)))
    for y in range(y0, y1 + 1):
        xs = []
        for i in range(len(pts)):
            ax, ay = pts[i]
            bx, by = pts[(i + 1) % len(pts)]
            if ay == by:
                continue
            if (ay <= y < by) or (by <= y < ay):
                t = (y - ay) / (by - ay)
                xs.append(ax + (bx - ax) * t)
        xs.sort()
        for i in range(0, len(xs) - 1, 2):
            for x in range(round(xs[i]), round(xs[i + 1]) + 1):
                put(m, x, y, v)


def union(*ms):
    out = blank()
    for m in ms:
        for y in range(H):
            for x in range(W):
                if m[y][x] != T:
                    out[y][x] = m[y][x]
    return out


def repaint(m, mask, v):
    for y in range(H):
        for x in range(W):
            if mask[y][x] == INK:
                m[y][x] = v


# --------------------------------------------------------------------------
# icon designs
# --------------------------------------------------------------------------

def art_pencil():
    m = blank()
    add_poly(m, [(4, 13), (13, 4), (10, 1), (1, 10)])   # shaft
    add_poly(m, [(1, 10), (4, 13), (1, 14)])            # sharpened tip
    cut = blank()
    add_seg(cut, 7, 3, 11, 7, 0.5)                      # facet band
    repaint(m, cut, CUT)
    return m


def art_line():
    m = blank()
    add_seg(m, 4, 11, 11, 4, 0.5)
    add_rect(m, 2, 11, 4, 13)              # endpoint handles
    add_rect(m, 11, 2, 13, 4)
    return m


def art_rectangle():
    m = blank()
    add_rect_outline(m, 2, 3, 13, 12, 2)
    return m


def art_rectangle_filled():
    m = blank()
    add_rect(m, 2, 3, 13, 12)
    return m


def art_ellipse():
    m = blank()
    add_ring(m, 7.5, 7.5, 6.2, 4.6, 2)
    return m


def art_ellipse_filled():
    m = blank()
    add_disc(m, 7.5, 7.5, 6.2, 4.6)
    return m


def art_circle():
    m = blank()
    add_ring(m, 7.5, 7.5, 6.2, 6.2, 2)
    return m


def art_circle_filled():
    m = blank()
    add_disc(m, 7.5, 7.5, 6.2, 6.2)
    return m


def art_fill():
    m = blank()
    add_poly(m, [(3, 6), (12, 6), (10, 13), (5, 13)])   # bucket body
    add_rect(m, 2, 5, 13, 6)                            # rim
    add_rect(m, 4, 1, 11, 2)                            # handle bar
    add_rect(m, 4, 2, 5, 5)                             # handle posts
    add_rect(m, 10, 2, 11, 5)
    add_disc(m, 14, 8, 1.2)                             # pour drop
    cut = blank()
    add_rect(cut, 5, 7, 10, 7)                          # paint level
    repaint(m, cut, CUT)
    return m


def art_eraser():
    m = blank()
    add_rect(m, 3, 4, 12, 12)                           # block
    m[4][3] = T                                         # round the corners
    m[4][12] = T
    m[12][3] = T
    m[12][12] = T
    cut = blank()
    add_seg(cut, 4, 9, 12, 6, 0.5)                      # sleeve edge
    repaint(m, cut, CUT)
    return m


def art_pick():
    m = blank()
    add_disc(m, 12, 3, 1.8)                # bulb
    add_seg(m, 11, 4, 5, 10, 0.7)          # tube
    add_poly(m, [(4, 10), (6, 11), (1, 14)])  # tip
    return m


def art_grab():
    m = blank()
    # Dashed marquee: the tool drags out a region and keeps it as a brush.
    for x in (1, 2, 4, 5, 7, 8, 10, 11, 13, 14):
        put(m, x, 2)
        put(m, x, 13)
    for y in (4, 5, 7, 8, 10, 11):
        put(m, 1, y)
        put(m, 14, y)
    return m


def art_magnify():
    m = blank()
    add_ring(m, 6.5, 6.5, 5.2, 5.2, 2)
    add_seg(m, 9.5, 9.5, 13, 13, 1.2)
    return m


def art_airbrush():
    m = blank()
    add_rect_outline(m, 3, 5, 8, 14, 1)    # can
    add_rect(m, 5, 2, 6, 5)                # nozzle
    for (x, y) in ((10, 3), (11, 5), (10, 7), (12, 4), (11, 9)):
        put(m, x, y)
    return m


def art_spray():
    m = blank()
    add_rect(m, 2, 4, 6, 6)                # nozzle head
    add_rect(m, 4, 6, 5, 14)               # can stem
    for (x, y) in ((8, 3), (9, 5), (8, 7), (10, 2), (10, 4), (10, 6),
                   (11, 7), (12, 3), (12, 5), (9, 9), (11, 9)):
        put(m, x, y)
    return m


def art_text():
    m = blank()
    add_rect(m, 2, 3, 13, 4)               # top bar
    add_rect(m, 7, 3, 8, 13)               # stem
    add_rect(m, 4, 12, 11, 13)             # serif foot
    return m


def art_select():
    m = blank()
    # Marching-ants marquee: dashed rectangle with solid corner handles.
    for x in range(2, 14):
        if x % 2 == 0:
            put(m, x, 2)
            put(m, x, 13)
    for y in range(2, 14):
        if y % 2 == 0:
            put(m, 2, y)
            put(m, 13, y)
    for cx, cy in ((2, 2), (13, 2), (2, 13), (13, 13)):
        put(m, cx, cy)
    # Centre move grip.
    add_rect(m, 7, 4, 8, 11)
    add_rect(m, 4, 7, 11, 8)
    return m


BUILDERS = [
    art_pencil, art_eraser, art_line, art_text, art_rectangle,
    art_rectangle_filled, art_ellipse, art_ellipse_filled, art_circle,
    art_circle_filled, art_fill, art_pick, art_airbrush, art_spray,
    art_grab, art_magnify, art_select,
]


def build():
    return [fn() for fn in BUILDERS]


# --------------------------------------------------------------------------
# emit / preview
# --------------------------------------------------------------------------

def emit_h(arts=None):
    arts = build() if arts is None else arts
    lines = []
    lines.append("/* Generated by assets/paint/gen_tool_icons.py -- do not edit by hand.")
    lines.append(" * 16x16 PAINT toolbar icons. Values: 0 transparent, 1 ink, 2 cut")
    lines.append(" * (background) detail. Order matches the PT_TOOL_* enum in paint.h. */")
    lines.append("#ifndef PAINT_TOOL_ICONS_H")
    lines.append("#define PAINT_TOOL_ICONS_H")
    lines.append("")
    lines.append("#include <stdint.h>")
    lines.append("")
    lines.append("#define PTI_ICON_W 16")
    lines.append("#define PTI_ICON_H 16")
    lines.append("#define PTI_ICON_PIXELS (PTI_ICON_W * PTI_ICON_H)")
    lines.append("#define PTI_ICON_TRANSPARENT 0")
    lines.append("#define PTI_ICON_INK 1")
    lines.append("#define PTI_ICON_CUT 2")
    lines.append("")
    lines.append("typedef enum {")
    for i, name in enumerate(TOOLS):
        lines.append("\tPTI_TOOL_%s = %d," % (name.upper(), i))
    lines.append("\tPTI_TOOL_COUNT")
    lines.append("} pti_tool_t;")
    lines.append("")
    lines.append("static const uint8_t pti_icons[PTI_TOOL_COUNT][PTI_ICON_PIXELS] = {")
    for name, art in zip(TOOLS, arts):
        lines.append("\t/* %s */" % name)
        lines.append("\t{")
        for y in range(H):
            row = ", ".join("%d" % art[y][x] for x in range(W))
            lines.append("\t\t%s," % row)
        lines.append("\t},")
    lines.append("};")
    lines.append("")
    lines.append("#endif /* PAINT_TOOL_ICONS_H */")
    return "\n".join(lines) + "\n"


def preview(arts):
    for name, art in zip(TOOLS, arts):
        print("==== %s" % name)
        for y in range(H):
            print("".join(".o#"[art[y][x]] for x in range(W)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--preview", action="store_true")
    args = ap.parse_args()
    arts = build()
    if args.preview:
        preview(arts)
        return
    OUT_H.write_text(emit_h(arts), encoding="utf-8")
    print("wrote %s" % OUT_H)


if __name__ == "__main__":
    main()
