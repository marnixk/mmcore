#!/usr/bin/env python3
"""Hand-built pixel art for the HUB frame, tabs and progress bar.

These pieces used to be cut from the JPEG mockups, which left noise, soft
edges and stray pixels. They are drawn here pixel by pixel at 1x from a small
palette, with binary alpha only, and the 2x sheet is an exact nearest-neighbour
double. Nothing is resampled or quantized.

Writes into ramdisk/hub/:
  corner-tl/tr/bl/br   frame corners (C x C), opaque except the content cut
  edge-h / edge-hb     top / bottom rim tiles (EDGE_TILE x B)
  edge-v / edge-vr     left / right rim tiles (B x EDGE_TILE)
  tab / tab-on         inactive / active tab 9-slices
  bar-frame / bar-fill loading and install progress bar
  scroll-track / -thumb scrollbar with arrow caps and a separate thumb

The geometry constants are mirrored in mmbasic/src/hub_chrome.h
(HUB_FRAME_*, HUB_BAR_*): keep them in step.
"""

from __future__ import annotations

import math
from pathlib import Path

from PIL import Image

REPO = Path(__file__).resolve().parents[1]
OUT = REPO / "ramdisk" / "hub"

# ---- frame geometry (1x pixels) -------------------------------------------
B = 10          # rim band: frame edge to the content area
D = 10          # chamfer of the inner corner, in x+y steps
C = 24          # corner piece size
EDGE_TILE = 12  # rim tile length

# ---- palette ----------------------------------------------------------------
T = (0, 0, 0, 0)
OUTL = (6, 7, 14, 255)
SIL_HI = (236, 238, 246, 255)
SIL_1 = (184, 189, 206, 255)
SIL_2 = (138, 143, 164, 255)
SIL_SH = (78, 82, 104, 255)
GAP = (16, 19, 40, 255)
PUR_HI = (196, 156, 255, 255)
PUR = (124, 76, 226, 255)
PUR_D = (58, 34, 116, 255)
NAVY = (18, 22, 48, 255)
OR_HI = (255, 220, 150, 255)
OR = (220, 142, 56, 255)
OR_D = (150, 82, 26, 255)
SPARK = (255, 248, 226, 255)

# Rim rows by depth from the content edge (0 = innermost).
RIM = [PUR_D, PUR, GAP, GAP, OUTL, SIL_SH, SIL_2, SIL_1, SIL_HI, OUTL]
assert len(RIM) == B


def new(w, h, fill=T):
    return Image.new("RGBA", (w, h), fill)


def save_pair(im: Image.Image, name: str) -> None:
    im.save(OUT / f"{name}-1.png", optimize=True)
    im.resize((im.width * 2, im.height * 2), Image.NEAREST).save(
        OUT / f"{name}-2.png", optimize=True)


# ---- frame ------------------------------------------------------------------

def rim_depth(x: int, y: int) -> int:
    """Depth into the rim for a top-left corner pixel, -1 inside content,
    >= B outside the rim (ornament area)."""
    vx = B - x
    vy = B - y
    vd = 2 * B + D - (x + y)
    v = max(vx, vy, vd)
    if v <= 0:
        return -1
    return v - 1


def edge_h() -> Image.Image:
    im = new(EDGE_TILE, B)
    px = im.load()
    for x in range(EDGE_TILE):
        for y in range(B):
            c = RIM[B - 1 - y]
            if B - 1 - y == 1 and x % 6 == 3:
                c = PUR_HI
            px[x, y] = c
    return im


def ornament_star(px, cx, cy, r):
    """Four-point star with concave sides and a dark outline."""
    pts = {}
    for dy in range(-r, r + 1):
        for dx in range(-r, r + 1):
            ax, ay = abs(dx), abs(dy)
            if ax <= (r - ay) * 0.35 + 0.5 or ay <= (r - ax) * 0.35 + 0.5:
                pts[(dx, dy)] = 1
    for (dx, dy) in list(pts):
        for ox, oy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            q = (dx + ox, dy + oy)
            if q not in pts:
                pts[q] = 0
    for (dx, dy), ink in pts.items():
        x, y = cx + dx, cy + dy
        if ink:
            ax, ay = abs(dx), abs(dy)
            if ax + ay == 0:
                c = SPARK
            elif ax + ay == 1:
                c = OR_HI
            elif dx + dy < 0 or (dx + dy == 0 and dx < 0):
                c = OR_HI if ax == 0 or ay == 0 else OR
            else:
                c = OR if ax == 0 or ay == 0 else OR_D
            px[x, y] = c
        elif px[x, y] == NAVY:
            px[x, y] = OUTL


def ornament_scroll(px, x0, y0):
    """Purple volute: an Archimedean curl of 1px ink with a light inner
    edge, outlined in the dark rim colour."""
    ink = set()
    t = 0.0
    while t < 3.2 * math.pi:
        r = 0.62 * t
        x = int(round(4 + r * math.cos(t + math.pi)))
        y = int(round(4 + r * math.sin(t + math.pi)))
        ink.add((x, y))
        t += 0.05
    for (x, y) in ink:
        px[x0 + x, y0 + y] = PUR
    for (x, y) in ink:
        # light on the up/left facing pixels
        if (x, y - 1) not in ink and (x - 1, y) not in ink:
            px[x0 + x, y0 + y] = PUR_HI
    for (x, y) in list(ink):
        for ox in (-1, 0, 1):
            for oy in (-1, 0, 1):
                q = (x0 + x + ox, y0 + y + oy)
                if (x + ox, y + oy) not in ink and px[q] == NAVY:
                    px[q] = OUTL


SILVER = [OUTL, SIL_HI, SIL_1, SIL_2, SIL_SH, OUTL]
BRACKET = [OUTL, OR_HI, OR, OR, OR_D, OUTL]
ARM = 15


def corner_tl(star: bool) -> Image.Image:
    im = new(C, C)
    px = im.load()
    for y in range(C):
        for x in range(C):
            o = min(x, y)
            if o < len(SILVER):
                px[x, y] = SILVER[o]
                continue
            d = rim_depth(x, y)
            if d < 0:
                continue
            if d < 4:
                c = RIM[d]
                if d == 1 and (x + y) % 6 == 3:
                    c = PUR_HI
                px[x, y] = c
            else:
                px[x, y] = NAVY
    # Orange bracket over the silver at the outer corner. Both arms end in
    # an outlined step so the bracket reads as a cap fitted on the rim.
    for y in range(C):
        for x in range(C):
            if x + y < 2:
                px[x, y] = T
                continue
            in_h = y < len(BRACKET) and x < ARM
            in_v = x < len(BRACKET) and y < ARM
            if not (in_h or in_v):
                continue
            o = min(x, y)
            c = BRACKET[o]
            if x + y == 2:
                c = OUTL
            if in_h and not in_v and x == ARM - 1:
                c = OUTL
            if in_v and not in_h and y == ARM - 1:
                c = OUTL
            px[x, y] = c
    if star:
        ornament_star(px, 10, 10, 5)
    else:
        ornament_scroll(px, 6, 6)
    return im


def frame_set():
    tl = corner_tl(True)
    bl = corner_tl(False).transpose(Image.FLIP_TOP_BOTTOM)
    # The scroll should still curl the same way after the flip; flipping the
    # whole piece keeps the rim layers on the correct side.
    out = {
        "corner-tl": tl,
        "corner-tr": tl.transpose(Image.FLIP_LEFT_RIGHT),
        "corner-bl": bl,
        "corner-br": bl.transpose(Image.FLIP_LEFT_RIGHT),
    }
    eh = edge_h()
    out["edge-h"] = eh
    out["edge-hb"] = eh.transpose(Image.FLIP_TOP_BOTTOM)
    ev = eh.transpose(Image.TRANSPOSE)
    out["edge-v"] = ev
    out["edge-vr"] = ev.transpose(Image.FLIP_LEFT_RIGHT)
    return out


# ---- tabs -------------------------------------------------------------------

TAB_W = 16
TAB_ON_H = 25   # 24 above the panel line + the line row itself
TAB_H = 20      # inactive tab, ends on the panel line
TAB_CUT = 3     # chamfer on the top corners
TAB_ON_EDGE = (204, 84, 236, 255)
TAB_ON_HI = (240, 150, 255, 255)
TAB_ON_FILL = (18, 22, 43, 255)    # HUB_PANEL, so the tab opens into it
TAB_EDGE = (66, 72, 104, 255)
TAB_FILL = (13, 16, 34, 255)
TAB_HI = (30, 35, 62, 255)


def make_tab(h, edge, fill, hi, open_bottom):
    im = new(TAB_W, h)
    px = im.load()
    for y in range(h):
        for x in range(TAB_W):
            cut = TAB_CUT - y
            if x < cut or x > TAB_W - 1 - cut:
                continue
            on_edge = (y == 0) or x == max(0, cut) or \
                x == TAB_W - 1 - max(0, cut)
            if open_bottom and y == h - 1 and 0 < x < TAB_W - 1:
                px[x, y] = fill
                continue
            if on_edge:
                px[x, y] = edge
            elif y == 1 or x == max(0, cut) + 1 and y < h - 1:
                px[x, y] = hi
            else:
                px[x, y] = fill
    return im


def tabs():
    return {
        "tab-on": make_tab(TAB_ON_H, TAB_ON_EDGE, TAB_ON_FILL, TAB_ON_HI, True),
        "tab": make_tab(TAB_H, TAB_EDGE, TAB_FILL, TAB_HI, False),
    }


# ---- progress bar -----------------------------------------------------------

BAR_W = 370
BAR_H = 26
BAR_INSET = 5       # frame edge to the first block, all sides
BAR_SEG = 18        # block width
BAR_GAP = 2
BAR_CUT = 3


def lerp(a, b, t):
    return tuple(int(round(a[i] + (b[i] - a[i]) * t)) for i in range(3)) + (255,)


def bar_frame():
    im = new(BAR_W, BAR_H)
    px = im.load()
    for y in range(BAR_H):
        for x in range(BAR_W):
            # distance to the outer edge, with chamfered corners
            dx = min(x, BAR_W - 1 - x)
            dy = min(y, BAR_H - 1 - y)
            if dx + dy < BAR_CUT:
                continue
            k = min(dx, dy, dx + dy - BAR_CUT)
            if k == 0:
                c = OUTL
            elif k == 1:
                c = SIL_HI if (y < BAR_H // 2) else SIL_1
            elif k == 2:
                c = SIL_2
            elif k == 3:
                c = OUTL
            else:
                c = (8, 10, 18, 255)
            px[x, y] = c
    return im


def bar_fill():
    inner_w = BAR_W - 2 * BAR_INSET
    inner_h = BAR_H - 2 * BAR_INSET
    n = (inner_w + BAR_GAP) // (BAR_SEG + BAR_GAP)
    im = new(n * (BAR_SEG + BAR_GAP) - BAR_GAP, inner_h)
    px = im.load()
    a, b = (224, 0, 200), (0, 228, 236)
    for i in range(n):
        base = lerp(a, b, i / max(1, n - 1))
        hi = lerp(base[:3], (255, 255, 255), 0.45)
        sh = lerp(base[:3], (0, 0, 0), 0.35)
        x0 = i * (BAR_SEG + BAR_GAP)
        for y in range(inner_h):
            for x in range(BAR_SEG):
                c = base
                if y == 0:
                    c = hi
                elif y == inner_h - 1:
                    c = sh
                px[x0 + x, y] = c
    return im, n


# ---- scrollbar ----------------------------------------------------------------

SCROLL_W = 8
SCROLL_CAP = 9      # arrow button plus a 1px gap; the thumb stays clear of it
SCROLL_MID = 8
GROOVE = (14, 17, 34, 255)
BTN = (44, 48, 76, 255)


def scroll_track():
    h = 2 * SCROLL_CAP + SCROLL_MID
    im = new(SCROLL_W, h)
    px = im.load()

    def button(y0, up):
        for y in range(8):
            for x in range(SCROLL_W):
                edge = y in (0, 7) or x in (0, 7)
                corner = (y in (0, 7)) and (x in (0, 7))
                if corner:
                    continue
                px[x, y0 + y] = OUTL if edge else BTN
        rows = [(3, 4), (2, 5), (1, 6)]
        for i, (a, b) in enumerate(rows):
            yy = y0 + (2 + i if up else 5 - i)
            for x in range(a, b + 1):
                px[x, yy] = SIL_HI if (up and i == 0) or (not up and i == 2) \
                    else SIL_1
    button(0, True)
    button(h - 8, False)
    for y in range(SCROLL_CAP, h - SCROLL_CAP):
        px[2, y] = OUTL
        px[5, y] = OUTL
        px[3, y] = GROOVE
        px[4, y] = GROOVE
    return im


def scroll_thumb():
    im = new(6, 8)
    px = im.load()
    cols = [OUTL, SIL_HI, SIL_1, SIL_2, SIL_SH, OUTL]
    for y in range(8):
        for x in range(6):
            if y in (0, 7):
                if 0 < x < 5:
                    px[x, y] = OUTL
                continue
            px[x, y] = cols[x]
    return im


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    for name, im in frame_set().items():
        save_pair(im, name)
    for name, im in tabs().items():
        save_pair(im, name)
    save_pair(bar_frame(), "bar-frame")
    fill, n = bar_fill()
    save_pair(fill, "bar-fill")
    save_pair(scroll_track(), "scroll-track")
    save_pair(scroll_thumb(), "scroll-thumb")
    print("wrote frame, tabs and bar (%d blocks)" % n)


if __name__ == "__main__":
    main()
