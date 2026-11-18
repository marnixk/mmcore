#!/usr/bin/env python3
"""Cut HUB chrome PNGs from the approved mockups into ramdisk/hub/.

Each piece is written at 2x (the 1280 mockup's own pixels) and at 1x (nearest
neighbour half). Backgrounds that touch the crop edge are cleared to alpha.
The title face is a generated italic chrome/purple sheet, not CP437.
"""

from __future__ import annotations

import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter

REPO = Path(__file__).resolve().parents[1]
MOCK = Path("/tmp/hub-mockups")
OUT = REPO / "ramdisk" / "hub"

# 7-wide glyphs, '#' is ink. Rows are top to bottom.
GLYPHS = {
    "A": ["0111110", "1100011", "1100011", "1111111", "1100011", "1100011", "1100011"],
    "B": ["1111110", "1100011", "1100011", "1111110", "1100011", "1100011", "1111110"],
    "C": ["0111111", "1100000", "1100000", "1100000", "1100000", "1100000", "0111111"],
    "D": ["1111110", "1100011", "1100011", "1100011", "1100011", "1100011", "1111110"],
    "E": ["1111111", "1100000", "1100000", "1111110", "1100000", "1100000", "1111111"],
    "F": ["1111111", "1100000", "1100000", "1111110", "1100000", "1100000", "1100000"],
    "G": ["0111111", "1100000", "1100000", "1101111", "1100011", "1100011", "0111111"],
    "H": ["1100011", "1100011", "1100011", "1111111", "1100011", "1100011", "1100011"],
    "I": ["1111111", "0011100", "0011100", "0011100", "0011100", "0011100", "1111111"],
    "J": ["0001111", "0000110", "0000110", "0000110", "1100110", "1100110", "0111100"],
    "K": ["1100011", "1100110", "1101100", "1111000", "1101100", "1100110", "1100011"],
    "L": ["1100000", "1100000", "1100000", "1100000", "1100000", "1100000", "1111111"],
    "M": ["1100011", "1110111", "1111111", "1101011", "1100011", "1100011", "1100011"],
    "N": ["1100011", "1110011", "1111011", "1101111", "1100111", "1100011", "1100011"],
    "O": ["0111110", "1100011", "1100011", "1100011", "1100011", "1100011", "0111110"],
    "P": ["1111110", "1100011", "1100011", "1111110", "1100000", "1100000", "1100000"],
    "Q": ["0111110", "1100011", "1100011", "1100011", "1101011", "1100110", "0111101"],
    "R": ["1111110", "1100011", "1100011", "1111110", "1101100", "1100110", "1100011"],
    "S": ["0111111", "1100000", "1100000", "0111110", "0000011", "0000011", "1111110"],
    "T": ["1111111", "0011100", "0011100", "0011100", "0011100", "0011100", "0011100"],
    "U": ["1100011", "1100011", "1100011", "1100011", "1100011", "1100011", "0111110"],
    "V": ["1100011", "1100011", "1100011", "1100011", "0110110", "0110110", "0011100"],
    "W": ["1100011", "1100011", "1100011", "1101011", "1111111", "1110111", "1100011"],
    "X": ["1100011", "0110110", "0011100", "0011100", "0011100", "0110110", "1100011"],
    "Y": ["1100011", "1100011", "0110110", "0011100", "0011100", "0011100", "0011100"],
    "Z": ["1111111", "0000011", "0000110", "0001100", "0011000", "0110000", "1111111"],
    "0": ["0111110", "1100011", "1100111", "1101011", "1110011", "1100011", "0111110"],
    "1": ["0011100", "0111100", "0011100", "0011100", "0011100", "0011100", "1111111"],
    "2": ["0111110", "1100011", "0000011", "0001110", "0110000", "1100000", "1111111"],
    "3": ["0111110", "1100011", "0000011", "0011110", "0000011", "1100011", "0111110"],
    "4": ["0001110", "0011110", "0110110", "1100110", "1111111", "0000110", "0000110"],
    "5": ["1111111", "1100000", "1111110", "0000011", "0000011", "1100011", "0111110"],
    "6": ["0111110", "1100000", "1100000", "1111110", "1100011", "1100011", "0111110"],
    "7": ["1111111", "0000011", "0000110", "0001100", "0011000", "0011000", "0011000"],
    "8": ["0111110", "1100011", "1100011", "0111110", "1100011", "1100011", "0111110"],
    "9": ["0111110", "1100011", "1100011", "0111111", "0000011", "0000011", "0111110"],
    " ": ["0000000"] * 7,
    ".": ["0000000", "0000000", "0000000", "0000000", "0000000", "0110000", "0110000"],
    ",": ["0000000", "0000000", "0000000", "0000000", "0011000", "0011000", "0110000"],
    "'": ["0110000", "0110000", "0010000", "0000000", "0000000", "0000000", "0000000"],
    "!": ["0011000", "0011000", "0011000", "0011000", "0000000", "0011000", "0000000"],
    "?": ["0111100", "1100110", "0000110", "0011000", "0011000", "0000000", "0011000"],
    ":": ["0000000", "0011000", "0000000", "0000000", "0011000", "0000000", "0000000"],
    "-": ["0000000", "0000000", "0000000", "1111110", "0000000", "0000000", "0000000"],
    "+": ["0000000", "0011000", "0011000", "1111110", "0011000", "0011000", "0000000"],
    "/": ["0000011", "0000110", "0001100", "0011000", "0110000", "1100000", "0000000"],
    "(": ["0001100", "0011000", "0110000", "0110000", "0110000", "0011000", "0001100"],
    ")": ["0110000", "0011000", "0001100", "0001100", "0001100", "0011000", "0110000"],
}

FONT_ORDER = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 .,'!?:-+/()"


def load(name: str) -> Image.Image:
    path = MOCK / name
    if not path.is_file():
        raise SystemExit(f"missing mockup {path}")
    return Image.open(path).convert("RGBA")


def knock(im: Image.Image, thresh: int = 16) -> Image.Image:
    """Clear near-black pixels that connect to the crop edge."""
    im = im.convert("RGBA")
    w, h = im.size
    pix = im.load()
    seen = bytearray(w * h)
    stack = []
    for x in range(w):
        stack.append((x, 0))
        stack.append((x, h - 1))
    for y in range(h):
        stack.append((0, y))
        stack.append((w - 1, y))
    while stack:
        x, y = stack.pop()
        if x < 0 or y < 0 or x >= w or y >= h:
            continue
        i = y * w + x
        if seen[i]:
            continue
        r, g, b, a = pix[x, y]
        if max(r, g, b) > thresh:
            continue
        seen[i] = 1
        pix[x, y] = (r, g, b, 0)
        stack.extend(((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)))
    return im


def trim(im: Image.Image, pad: int = 0) -> Image.Image:
    w, h = im.size
    pix = im.load()
    xs, ys = [], []
    for y in range(h):
        for x in range(w):
            if pix[x, y][3] > 8:
                xs.append(x)
                ys.append(y)
    if not xs:
        return im
    x0 = max(0, min(xs) - pad)
    y0 = max(0, min(ys) - pad)
    x1 = min(w, max(xs) + 1 + pad)
    y1 = min(h, max(ys) + 1 + pad)
    return im.crop((x0, y0, x1, y1))


def crunch(im: Image.Image, colors: int = 48) -> Image.Image:
    """Posterise so JPEG noise does not bloat the ramdisk."""
    im = im.convert("RGBA")
    alpha = im.getchannel("A")
    rgb = im.convert("RGB").quantize(
        colors=colors, method=Image.Quantize.FASTOCTREE, dither=Image.Dither.NONE
    ).convert("RGB")
    rgb.putalpha(alpha)
    return rgb


def save_pair(im: Image.Image, name: str, colors: int = 48) -> None:
    im = crunch(im, colors)
    # Even size so the 1x nearest half lands on pixel boundaries.
    w, h = im.size
    if w % 2:
        im = im.crop((0, 0, w - 1, h))
        w -= 1
    if h % 2:
        im = im.crop((0, 0, w, h - 1))
        h -= 1
    p2 = OUT / f"{name}-2.png"
    p1 = OUT / f"{name}-1.png"
    im.save(p2, optimize=True)
    small = im.resize((w // 2, h // 2), Image.Resampling.NEAREST)
    small.save(p1, optimize=True)
    print(f"{name:16} 2x {w:4}x{h:<4} {p2.stat().st_size:6}  "
          f"1x {small.size[0]:4}x{small.size[1]:<4} {p1.stat().st_size:6}")


def crop_knock(src: Image.Image, box, thresh=16, pad=1) -> Image.Image:
    return trim(knock(src.crop(box), thresh), pad)


def tint_hub(im: Image.Image) -> Image.Image:
    """Keep 'mmcore' white and paint 'HUB' magenta.

    The JPEG washes the second word out to grey. The gap between the two
    words is the split.
    """
    im = im.convert("RGBA")
    pix = im.load()
    w, h = im.size
    ink = [any(pix[x, y][3] > 20 for y in range(h)) for x in range(w)]
    best = None
    x = 0
    while x < w:
        if ink[x]:
            x += 1
            continue
        x0 = x
        while x < w and not ink[x]:
            x += 1
        if x0 > w // 4 and x < w * 4 // 5:
            if best is None or (x - x0) > (best[1] - best[0]):
                best = (x0, x)
    if not best:
        return im
    cut = best[1]
    for y in range(h):
        for x in range(cut, w):
            r, g, b, a = pix[x, y]
            if a < 8:
                continue
            lum = max(r, g, b) / 255.0
            pix[x, y] = (int(255 * lum), int(42 * lum), int(214 * lum), a)
    return im


def make_pill(border: tuple, fill: tuple) -> Image.Image:
    """9-slice badge background. The centre is the fill, so any label fits."""
    w, h = 56, 32
    im = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    pix = im.load()
    rad = 8

    def inside(x, y):
        cx = min(max(x, rad), w - 1 - rad)
        cy = min(max(y, rad), h - 1 - rad)
        return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= rad * rad

    for y in range(h):
        for x in range(w):
            if not inside(x, y):
                continue
            edge = (
                not inside(x - 2, y)
                or not inside(x + 2, y)
                or not inside(x, y - 2)
                or not inside(x, y + 2)
            )
            pix[x, y] = border if edge else fill
    return im


# 5x7 UI face. The 2x sheet doubles every pixel, so the strokes are a clean
# 2px at 720p instead of the thin CP437 face.
UI_ORDER = (
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
    "0123456789 .,'!?:-+/()%&#\""
)
UI_GLYPHS = {
    "A": ["01110", "10001", "10001", "11111", "10001", "10001", "10001"],
    "B": ["11110", "10001", "10001", "11110", "10001", "10001", "11110"],
    "C": ["01111", "10000", "10000", "10000", "10000", "10000", "01111"],
    "D": ["11110", "10001", "10001", "10001", "10001", "10001", "11110"],
    "E": ["11111", "10000", "10000", "11110", "10000", "10000", "11111"],
    "F": ["11111", "10000", "10000", "11110", "10000", "10000", "10000"],
    "G": ["01111", "10000", "10000", "10111", "10001", "10001", "01111"],
    "H": ["10001", "10001", "10001", "11111", "10001", "10001", "10001"],
    "I": ["11111", "00100", "00100", "00100", "00100", "00100", "11111"],
    "J": ["00111", "00010", "00010", "00010", "10010", "10010", "01100"],
    "K": ["10001", "10010", "10100", "11000", "10100", "10010", "10001"],
    "L": ["10000", "10000", "10000", "10000", "10000", "10000", "11111"],
    "M": ["10001", "11011", "10101", "10101", "10001", "10001", "10001"],
    "N": ["10001", "11001", "10101", "10011", "10001", "10001", "10001"],
    "O": ["01110", "10001", "10001", "10001", "10001", "10001", "01110"],
    "P": ["11110", "10001", "10001", "11110", "10000", "10000", "10000"],
    "Q": ["01110", "10001", "10001", "10001", "10101", "10010", "01101"],
    "R": ["11110", "10001", "10001", "11110", "10100", "10010", "10001"],
    "S": ["01111", "10000", "10000", "01110", "00001", "00001", "11110"],
    "T": ["11111", "00100", "00100", "00100", "00100", "00100", "00100"],
    "U": ["10001", "10001", "10001", "10001", "10001", "10001", "01110"],
    "V": ["10001", "10001", "10001", "10001", "01010", "01010", "00100"],
    "W": ["10001", "10001", "10001", "10101", "10101", "10101", "01010"],
    "X": ["10001", "01010", "00100", "00100", "00100", "01010", "10001"],
    "Y": ["10001", "10001", "01010", "00100", "00100", "00100", "00100"],
    "Z": ["11111", "00001", "00010", "00100", "01000", "10000", "11111"],
    "a": ["00000", "00000", "01110", "00001", "01111", "10001", "01111"],
    "b": ["10000", "10000", "11110", "10001", "10001", "10001", "11110"],
    "c": ["00000", "00000", "01111", "10000", "10000", "10000", "01111"],
    "d": ["00001", "00001", "01111", "10001", "10001", "10001", "01111"],
    "e": ["00000", "00000", "01110", "10001", "11111", "10000", "01110"],
    "f": ["00110", "01001", "01000", "11100", "01000", "01000", "01000"],
    "g": ["00000", "01111", "10001", "10001", "01111", "00001", "01110"],
    "h": ["10000", "10000", "11110", "10001", "10001", "10001", "10001"],
    "i": ["00100", "00000", "01100", "00100", "00100", "00100", "01110"],
    "j": ["00010", "00000", "00110", "00010", "00010", "10010", "01100"],
    "k": ["10000", "10000", "10010", "10100", "11000", "10100", "10010"],
    "l": ["01100", "00100", "00100", "00100", "00100", "00100", "01110"],
    "m": ["00000", "00000", "11010", "10101", "10101", "10001", "10001"],
    "n": ["00000", "00000", "11110", "10001", "10001", "10001", "10001"],
    "o": ["00000", "00000", "01110", "10001", "10001", "10001", "01110"],
    "p": ["00000", "11110", "10001", "10001", "11110", "10000", "10000"],
    "q": ["00000", "01111", "10001", "10001", "01111", "00001", "00001"],
    "r": ["00000", "00000", "10110", "11001", "10000", "10000", "10000"],
    "s": ["00000", "00000", "01111", "10000", "01110", "00001", "11110"],
    "t": ["01000", "01000", "11100", "01000", "01000", "01001", "00110"],
    "u": ["00000", "00000", "10001", "10001", "10001", "10001", "01111"],
    "v": ["00000", "00000", "10001", "10001", "10001", "01010", "00100"],
    "w": ["00000", "00000", "10001", "10001", "10101", "10101", "01010"],
    "x": ["00000", "00000", "10001", "01010", "00100", "01010", "10001"],
    "y": ["00000", "10001", "10001", "01111", "00001", "00001", "01110"],
    "z": ["00000", "00000", "11111", "00010", "00100", "01000", "11111"],
    "0": ["01110", "10001", "10011", "10101", "11001", "10001", "01110"],
    "1": ["00100", "01100", "00100", "00100", "00100", "00100", "01110"],
    "2": ["01110", "10001", "00001", "00010", "00100", "01000", "11111"],
    "3": ["11110", "00001", "00001", "01110", "00001", "00001", "11110"],
    "4": ["00010", "00110", "01010", "10010", "11111", "00010", "00010"],
    "5": ["11111", "10000", "11110", "00001", "00001", "10001", "01110"],
    "6": ["00110", "01000", "10000", "11110", "10001", "10001", "01110"],
    "7": ["11111", "00001", "00010", "00100", "01000", "01000", "01000"],
    "8": ["01110", "10001", "10001", "01110", "10001", "10001", "01110"],
    "9": ["01110", "10001", "10001", "01111", "00001", "00010", "01100"],
    " ": ["00000", "00000", "00000", "00000", "00000", "00000", "00000"],
    ".": ["00000", "00000", "00000", "00000", "00000", "01100", "01100"],
    ",": ["00000", "00000", "00000", "00000", "00110", "00110", "01100"],
    "'": ["00100", "00100", "01000", "00000", "00000", "00000", "00000"],
    "!": ["00100", "00100", "00100", "00100", "00000", "00100", "00000"],
    "?": ["01110", "10001", "00010", "00100", "00000", "00100", "00000"],
    ":": ["00000", "01100", "01100", "00000", "01100", "01100", "00000"],
    "-": ["00000", "00000", "00000", "11111", "00000", "00000", "00000"],
    "+": ["00000", "00100", "00100", "11111", "00100", "00100", "00000"],
    "/": ["00001", "00010", "00100", "00100", "01000", "10000", "00000"],
    "(": ["00010", "00100", "01000", "01000", "01000", "00100", "00010"],
    ")": ["01000", "00100", "00010", "00010", "00010", "00100", "01000"],
    "%": ["11001", "11010", "00010", "00100", "01000", "01011", "10011"],
    "&": ["01100", "10010", "10100", "01000", "10101", "10010", "01101"],
    "#": ["01010", "01010", "11111", "01010", "11111", "01010", "01010"],
    '"': ["01010", "01010", "01010", "00000", "00000", "00000", "00000"],
}


def make_ui_font() -> Image.Image:
    """1x sheet. Callers double it for the 2x file so the scale stays integer."""
    gw, gh = 5, 7
    cell_w, cell_h = 6, 9
    n = len(UI_ORDER)
    sheet = Image.new("RGBA", (cell_w * n, cell_h), (0, 0, 0, 0))
    pix = sheet.load()
    for index, ch in enumerate(UI_ORDER):
        rows = UI_GLYPHS.get(ch)
        if not rows:
            continue
        ox = index * cell_w
        for gy, bits in enumerate(rows):
            for gx, bit in enumerate(bits):
                if bit == "1":
                    pix[ox + gx, 1 + gy] = (255, 255, 255, 255)
    return sheet


def save_exact_double(im1: Image.Image, name: str) -> None:
    w, h = im1.size
    im2 = im1.resize((w * 2, h * 2), Image.Resampling.NEAREST)
    p2 = OUT / f"{name}-2.png"
    p1 = OUT / f"{name}-1.png"
    im2.save(p2, optimize=True)
    im1.save(p1, optimize=True)
    print(f"{name:16} 2x {im2.size[0]:4}x{im2.size[1]:<4} {p2.stat().st_size:6}  "
          f"1x {w:4}x{h:<4} {p1.stat().st_size:6}")


# Title letters as strokes in a 24 x 40 box. Drawn thick, sheared, then given
# a vertical gradient so they read as italic chrome rather than a scaled 7x7.
TITLE_STROKES = {
    "A": [((2, 38), (12, 2), (22, 38)), ((6, 24), (18, 24))],
    "B": [((3, 2), (3, 38)), ((3, 2), (16, 2), (18, 8), (16, 18), (3, 18)),
          ((3, 18), (17, 18), (20, 26), (17, 38), (3, 38))],
    "C": [((20, 8), (14, 2), (6, 4), (2, 16), (2, 26), (6, 36), (14, 38), (20, 32))],
    "D": [((3, 2), (3, 38)), ((3, 2), (14, 2), (20, 10), (20, 30), (14, 38), (3, 38))],
    "E": [((20, 2), (3, 2), (3, 38), (20, 38)), ((3, 20), (15, 20))],
    "F": [((20, 2), (3, 2), (3, 38)), ((3, 20), (15, 20))],
    "G": [((20, 8), (14, 2), (6, 4), (2, 18), (2, 28), (8, 38), (18, 36), (18, 22), (12, 22))],
    "H": [((3, 2), (3, 38)), ((20, 2), (20, 38)), ((3, 20), (20, 20))],
    "I": [((6, 2), (18, 2)), ((12, 2), (12, 38)), ((6, 38), (18, 38))],
    "J": [((8, 2), (20, 2)), ((16, 2), (16, 30), (12, 38), (6, 36), (4, 28))],
    "K": [((3, 2), (3, 38)), ((20, 2), (3, 22)), ((10, 20), (20, 38))],
    "L": [((3, 2), (3, 38), (20, 38))],
    "M": [((2, 38), (2, 4), (12, 20), (22, 4), (22, 38))],
    "N": [((3, 38), (3, 2), (20, 38), (20, 2))],
    "O": [((12, 2), (4, 6), (2, 18), (2, 26), (6, 38), (18, 38), (22, 26), (22, 14), (16, 2), (12, 2))],
    "P": [((3, 38), (3, 2), (16, 2), (20, 8), (16, 18), (3, 18))],
    "Q": [((12, 2), (4, 8), (2, 20), (6, 36), (16, 36), (22, 24), (22, 12), (16, 2), (12, 2)),
          ((14, 28), (22, 40))],
    "R": [((3, 38), (3, 2), (16, 2), (20, 8), (16, 18), (3, 18)), ((12, 18), (20, 38))],
    "S": [((18, 6), (12, 2), (5, 6), (4, 14), (10, 20), (18, 26), (18, 34), (12, 38), (4, 34))],
    "T": [((2, 2), (22, 2)), ((12, 2), (12, 38))],
    "U": [((3, 2), (3, 30), (8, 38), (16, 38), (21, 30), (21, 2))],
    "V": [((2, 2), (12, 38), (22, 2))],
    "W": [((1, 2), (6, 38), (12, 18), (18, 38), (23, 2))],
    "X": [((2, 2), (22, 38)), ((22, 2), (2, 38))],
    "Y": [((2, 2), (12, 18), (22, 2)), ((12, 18), (12, 38))],
    "Z": [((2, 2), (22, 2), (2, 38), (22, 38))],
    "0": [((12, 2), (4, 8), (2, 20), (6, 36), (18, 36), (22, 22), (18, 4), (12, 2)),
          ((8, 32), (16, 8))],
    "1": [((8, 8), (12, 2), (12, 38)), ((6, 38), (18, 38))],
    "2": [((4, 10), (8, 2), (16, 2), (20, 8), (4, 38), (22, 38))],
    "3": [((4, 4), (16, 2), (20, 10), (14, 18), (20, 28), (16, 38), (4, 36))],
    "4": [((16, 2), (4, 26), (22, 26)), ((16, 2), (16, 38))],
    "5": [((20, 2), (4, 2), (4, 16), (16, 16), (20, 26), (14, 38), (4, 34))],
    "6": [((18, 6), (10, 2), (4, 12), (4, 28), (10, 38), (18, 34), (20, 24), (12, 18), (4, 22))],
    "7": [((2, 2), (22, 2), (8, 38))],
    "8": [((8, 2), (16, 2), (20, 8), (8, 18), (16, 18), (20, 28), (14, 38), (6, 38), (2, 30), (8, 20), (2, 8), (8, 2))],
    "9": [((6, 36), (14, 38), (20, 28), (20, 12), (14, 2), (6, 6), (4, 16), (12, 22), (20, 18))],
    " ": [],
    ".": [((10, 34), (10, 38))],
    ",": [((10, 32), (10, 36), (6, 40))],
    "'": [((10, 2), (8, 10))],
    "!": [((12, 2), (12, 26)), ((12, 34), (12, 38))],
    "?": [((4, 8), (10, 2), (18, 6), (16, 16), (12, 22), (12, 26)), ((12, 34), (12, 38))],
    ":": [((12, 12), (12, 16)), ((12, 28), (12, 32))],
    "-": [((4, 20), (20, 20))],
    "+": [((4, 20), (20, 20)), ((12, 12), (12, 28))],
    "/": [((18, 2), (6, 38))],
    "(": [((16, 2), (8, 10), (8, 30), (16, 38))],
    ")": [((8, 2), (16, 10), (16, 30), (8, 38))],
}


def _stroke(draw: ImageDraw.ImageDraw, pts, width: int, scale: float,
            shear: float, ox: int, oy: int, box_h: float) -> None:
    scaled = []
    for x, y in pts:
        sx = ox + (x + (box_h - y) * shear) * scale
        sy = oy + y * scale
        scaled.append((sx, sy))
    if len(scaled) == 1:
        r = width / 2
        x, y = scaled[0]
        draw.ellipse((x - r, y - r, x + r, y + r), fill=(255, 255, 255, 255))
        return
    draw.line(scaled, fill=(255, 255, 255, 255), width=width, joint="curve")
    r = width / 2
    for x, y in scaled:
        draw.ellipse((x - r, y - r, x + r, y + r), fill=(255, 255, 255, 255))


def make_font() -> Image.Image:
    """Italic chrome face. Glyphs are about 44px tall on the 2x sheet."""
    scale = 1.22
    shear = 0.22
    stroke_w = 8
    cell_w, cell_h = 56, 68
    n = len(FONT_ORDER)
    sheet = Image.new("RGBA", (cell_w * n, cell_h * 2), (0, 0, 0, 0))

    def grad(row: int, t: float):
        if row == 0:
            return (int(255 - 80 * t), int(248 - 100 * t), int(255 - 30 * t))
        return (int(236 - 120 * t), int(170 - 130 * t), int(255 - 50 * t))

    for index, ch in enumerate(FONT_ORDER):
        strokes = TITLE_STROKES.get(ch, [])
        # Drawn at the 2x cell. No downsample: a 3x-then-scale step smears
        # the strokes the same way a fractional chrome scale does.
        small = Image.new("RGBA", (cell_w, cell_h), (0, 0, 0, 0))
        draw = ImageDraw.Draw(small)
        for pts in strokes:
            _stroke(draw, pts, stroke_w, scale, shear, 8, 6, 40)
        mask = small.getchannel("A")
        # A couple of pixels of dark outline around the smooth core.
        outline = mask.filter(ImageFilter.MaxFilter(5))
        core = mask.load()
        ring = outline.load()
        src = small.load()
        for line in (0, 1):
            ox = index * cell_w
            oy = line * cell_h
            dest = sheet.load()
            for y in range(cell_h):
                t = y / max(1, cell_h - 1)
                col = grad(line, t)
                for x in range(cell_w):
                    if ring[x, y] < 16:
                        continue
                    if core[x, y] > 40:
                        a = min(255, int(src[x, y][3]))
                        dest[ox + x, oy + y] = (*col, a)
                    else:
                        dest[ox + x, oy + y] = (16, 8, 28, min(255, ring[x, y]))
    return sheet


def split_bar(bar: Image.Image):
    """Separate the grey rounded frame from the coloured segments."""
    bar = knock(bar, 12)
    w, h = bar.size
    pix = bar.load()
    frame = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    fill = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    fp, gp = frame.load(), fill.load()
    for y in range(h):
        for x in range(w):
            r, g, b, a = pix[x, y]
            if a < 8:
                continue
            if abs(r - g) < 28 and abs(g - b) < 28 and r > 70:
                fp[x, y] = (r, g, b, 255)
            elif max(r, g, b) > 40:
                gp[x, y] = (r, g, b, 255)
    return trim(frame, 0), trim(fill, 0)


def keep_frame(im: Image.Image) -> Image.Image:
    """Keep gold, purple and metal pixels; drop the photo and the panel."""
    im = im.convert("RGBA")
    pix = im.load()
    w, h = im.size
    for y in range(h):
        for x in range(w):
            r, g, b, a = pix[x, y]
            gold = r > 120 and g > 55 and b < 170 and r > b + 15
            purple = b > 45 and r > 25 and b > g + 6
            metal = min(r, g, b) > 80 and abs(r - b) < 50
            if max(r, g, b) < 36 or not (gold or purple or metal):
                pix[x, y] = (0, 0, 0, 0)
    return trim(im, 0)


def save_raw_pair(im: Image.Image, name: str) -> None:
    """Write 2x and a nearest 1x half without posterising.

    The action button's gradient is a few units per row. Quantising it to a
    handful of colours turns the face into a flat fill.
    """
    im = im.convert("RGBA")
    w, h = im.size
    if w % 2:
        im = im.crop((0, 0, w - 1, h))
        w -= 1
    if h % 2:
        im = im.crop((0, 0, w, h - 1))
        h -= 1
    p2 = OUT / f"{name}-2.png"
    p1 = OUT / f"{name}-1.png"
    im.save(p2, optimize=True)
    small = im.resize((w // 2, h // 2), Image.Resampling.NEAREST)
    small.save(p1, optimize=True)
    print(f"{name:16} 2x {w:4}x{h:<4} {p2.stat().st_size:6}  "
          f"1x {small.size[0]:4}x{small.size[1]:<4} {p1.stat().st_size:6}")


def cut_action_button(detail: Image.Image) -> Image.Image:
    """The mockup UPDATE control, with the baked word filled from the face.

    Panel pixels are cleared only where they touch the crop edge, so the
    copper rim, the stepped corners and the outer shadow stay. The word is
    replaced by the gradient on either side of it. Nothing is recoloured.
    """
    crop = detail.crop((768, 576, 1102, 704)).convert("RGBA")
    pix = crop.load()
    w, h = crop.size
    seen = bytearray(w * h)
    stack = [(x, 0) for x in range(w)] + [(x, h - 1) for x in range(w)]
    stack += [(0, y) for y in range(h)] + [(w - 1, y) for y in range(h)]

    def is_panel(r: int, g: int, b: int) -> bool:
        return b + 8 >= r and max(r, g, b) < 55 and r < 40

    while stack:
        x, y = stack.pop()
        if x < 0 or y < 0 or x >= w or y >= h:
            continue
        i = y * w + x
        if seen[i]:
            continue
        r, g, b, _ = pix[x, y]
        if not is_panel(r, g, b):
            continue
        seen[i] = 1
        pix[x, y] = (0, 0, 0, 0)
        stack.extend(((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)))
    xs, ys = [], []
    for y in range(h):
        for x in range(w):
            if pix[x, y][3] > 10:
                xs.append(x)
                ys.append(y)
    btn = crop.crop((min(xs), min(ys), max(xs) + 1, max(ys) + 1))
    bp = btn.load()
    bw, bh = btn.size
    wx, wy = [], []
    for y in range(bh):
        for x in range(bw):
            r, g, b, a = bp[x, y]
            if a > 200 and min(r, g, b) > 170 and abs(r - b) < 40:
                wx.append(x)
                wy.append(y)
    if not wx:
        return btn
    x0, x1 = min(wx) - 6, max(wx) + 7
    y0, y1 = min(wy) - 5, max(wy) + 6
    lx = max(0, x0 - 14)
    mask = [[0] * bw for _ in range(bh)]
    for y in range(max(0, y0), min(bh, y1)):
        sample = bp[lx, y]
        for x in range(max(0, x0), min(bw, x1)):
            r, g, b, _ = bp[x, y]
            dist = abs(r - sample[0]) + abs(g - sample[1]) + abs(b - sample[2])
            if dist > 26 or min(r, g, b) > 160:
                mask[y][x] = 1
    dil = [row[:] for row in mask]
    for y in range(bh):
        for x in range(bw):
            if not mask[y][x]:
                continue
            for dy in range(-2, 3):
                for dx in range(-2, 3):
                    yy, xx = y + dy, x + dx
                    if 0 <= yy < bh and 0 <= xx < bw:
                        dil[yy][xx] = 1
    for y in range(bh):
        for x in range(bw):
            if not dil[y][x]:
                continue
            xl = x
            while xl > 0 and dil[y][xl]:
                xl -= 1
            xr = x
            while xr < bw - 1 and dil[y][xr]:
                xr += 1
            if dil[y][xl]:
                bp[x, y] = bp[min(bw - 1, xr), y]
                continue
            if dil[y][xr]:
                bp[x, y] = bp[xl, y]
                continue
            span = max(1, xr - xl)
            t = (x - xl) / span
            left, right = bp[xl, y], bp[xr, y]
            bp[x, y] = (
                int(left[0] * (1 - t) + right[0] * t),
                int(left[1] * (1 - t) + right[1] * t),
                int(left[2] * (1 - t) + right[2] * t),
                255,
            )
    return btn


def tint_button_gold(im: Image.Image) -> Image.Image:
    """Same bevel, face shifted toward gold so UPDATE reads apart from INSTALL."""
    im = im.copy()
    pix = im.load()
    w, h = im.size
    for y in range(h):
        for x in range(w):
            r, g, b, a = pix[x, y]
            if a < 20:
                continue
            if r > 200 and g > 130:
                pix[x, y] = (min(255, r + 8), min(255, g + 30), min(70, b), a)
            elif r > 60 and r > b + 15:
                pix[x, y] = (
                    min(255, int(r * 1.04) + 16),
                    min(255, int(g * 1.6) + 18),
                    max(0, b // 3),
                    a,
                )
    return im


def tint_button_grey(im: Image.Image) -> Image.Image:
    """Desaturated face for the disabled NEEDS control."""
    im = im.copy()
    pix = im.load()
    w, h = im.size
    for y in range(h):
        for x in range(w):
            r, g, b, a = pix[x, y]
            if a < 16:
                continue
            yv = (r * 3 + g * 6 + b) // 10
            yv = yv * 7 // 10
            pix[x, y] = (yv, yv, yv + min(8, yv // 8), a)
    return im


def clear_interior(im: Image.Image, inset: int) -> Image.Image:
    """Fill the inside of a control with its own body colour, dropping baked text."""
    im = im.convert("RGBA")
    w, h = im.size
    if w < inset * 3 or h < inset * 3:
        return im
    pix = im.load()
    r, g, b, _ = pix[inset + 2, h // 2]
    for y in range(inset, h - inset):
        for x in range(inset, w - inset):
            if pix[x, y][3]:
                pix[x, y] = (r, g, b, 255)
    return im


def _ornament(r: int, g: int, b: int) -> bool:
    """Copper, purple and the silver highlight. Not the photo, not the navy."""
    if r > 155 and r > b + 45 and 55 < g < 210 and r > g:
        return True
    if b > 85 and b > g + 18 and b > r - 10 and max(r, g, b) > 90:
        return True
    return False


def _photo(r: int, g: int, b: int) -> bool:
    return g > r + 4 and g > 32


def _keep_component(mask, min_px: int) -> None:
    """Drop specks that are not part of a real ornament."""
    h = len(mask)
    w = len(mask[0])
    seen = bytearray(w * h)
    for y in range(h):
        for x in range(w):
            i = y * w + x
            if seen[i] or not mask[y][x]:
                continue
            stack = [(x, y)]
            cells = []
            seen[i] = 1
            while stack:
                cx, cy = stack.pop()
                cells.append((cx, cy))
                for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                    xx, yy = cx + dx, cy + dy
                    if xx < 0 or yy < 0 or xx >= w or yy >= h:
                        continue
                    j = yy * w + xx
                    if seen[j] or not mask[yy][xx]:
                        continue
                    seen[j] = 1
                    stack.append((xx, yy))
            if len(cells) < min_px:
                for cx, cy in cells:
                    mask[cy][cx] = 0


def _cut_corner(src: Image.Image, box, bands, interior) -> Image.Image:
    """Native crop. `bands` are source rectangles of the straight rim to keep
    whole, including the dark groove, so the edge tile can continue them.
    """
    x0, y0, x1, y1 = box
    w, h = x1 - x0, y1 - y0
    sp = src.load()
    pix = [[sp[x0 + x, y0 + y][:3] for x in range(w)] for y in range(h)]
    orn = [[_ornament(*pix[y][x]) for x in range(w)] for y in range(h)]
    # The star's white sparkle sits inside the copper, not on the photo.
    for y in range(h):
        for x in range(w):
            r, g, b = pix[y][x]
            if min(r, g, b) < 190 or not (r > 140 and r > b + 20):
                # sparkle is bright; the second clause is the copper neighbour
                pass
            if min(r, g, b) > 200 and any(
                0 <= y + dy < h and 0 <= x + dx < w and
                pix[y + dy][x + dx][0] > 170 and
                pix[y + dy][x + dx][2] < 120 and
                pix[y + dy][x + dx][0] > pix[y + dy][x + dx][2] + 50
                for dy in range(-2, 3) for dx in range(-2, 3)
            ):
                orn[y][x] = True
    grow = [row[:] for row in orn]
    for _ in range(2):
        nxt = [row[:] for row in grow]
        for y in range(h):
            for x in range(w):
                if grow[y][x]:
                    continue
                r, g, b = pix[y][x]
                if _photo(r, g, b) or max(r, g, b) > 60:
                    continue
                if any(
                    0 <= y + dy < h and 0 <= x + dx < w and grow[y + dy][x + dx]
                    for dy in (-1, 0, 1) for dx in (-1, 0, 1)
                ):
                    nxt[y][x] = True
        grow = nxt
    for y in range(h):
        for x in range(w):
            sx, sy = x0 + x, y0 + y
            for bx0, by0, bx1, by1 in bands:
                if bx0 <= sx < bx1 and by0 <= sy < by1:
                    grow[y][x] = True
    # The photo sits inside the rim. Keep ornament pixels and their fringe.
    ix0, iy0, ix1, iy1 = interior
    for y in range(h):
        for x in range(w):
            sx, sy = x0 + x, y0 + y
            in_band = any(bx0 <= sx < bx1 and by0 <= sy < by1
                          for bx0, by0, bx1, by1 in bands)
            if in_band or orn[y][x]:
                continue
            if not (ix0 <= sx < ix1 and iy0 <= sy < iy1):
                continue
            near = any(
                0 <= y + dy < h and 0 <= x + dx < w and orn[y + dy][x + dx]
                for dy in range(-2, 3) for dx in range(-2, 3)
            )
            r, g, b = pix[y][x]
            if _photo(r, g, b) or max(r, g, b) > 70 or not near:
                grow[y][x] = False
    _keep_component(grow, 12)
    out = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    op = out.load()
    for y in range(h):
        for x in range(w):
            if grow[y][x]:
                r, g, b = pix[y][x]
                op[x, y] = (r, g, b, 255)
    return out


def _edge_from_corner(src: Image.Image, corner_box, band, along: str,
                      span: int) -> Image.Image:
    """A strip whose pixels are the source rows or columns just past the
    corner, in the same band the corner kept. Row 0 of a horizontal edge is
    the corner's top source row, so the two bitmaps share a y origin.
    """
    x0, y0, x1, y1 = corner_box
    sp = src.load()
    if along == "h":
        im = Image.new("RGBA", (span, y1 - y0), (0, 0, 0, 0))
        ip = im.load()
        by0, by1 = band[1], band[3]
        for y in range(y1 - y0):
            sy = y0 + y
            if not (by0 <= sy < by1):
                continue
            for x in range(span):
                r, g, b = sp[x1 + x, sy][:3]
                ip[x, y] = (r, g, b, 255)
        return im
    im = Image.new("RGBA", (x1 - x0, span), (0, 0, 0, 0))
    ip = im.load()
    bx0, bx1 = band[0], band[2]
    for y in range(span):
        for x in range(x1 - x0):
            sx = x0 + x
            if not (bx0 <= sx < bx1):
                continue
            r, g, b = sp[sx, y1 + y][:3]
            ip[x, y] = (r, g, b, 255)
    return im


def content_frame(detail: Image.Image):
    """Screenshot-frame corners and edges from the detail mockup.

    Top corners share a y crop and bottom corners share a y crop, so a
    horizontal edge cut from the same rows meets both rims. Left corners
    share an x crop, and so do the right corners.
    """
    # Straight rim bands, measured on 3-detail-wide.jpg.
    top = (96, 29, 520, 54)
    bot = (160, 458, 520, 484)
    left = (30, 176, 57, 360)
    right = (646, 176, 674, 360)
    # Crops include the full star (top) or scroll (bottom) plus a run of
    # straight rim on the two inner sides. Even sizes, join side uncropped.
    tl = (8, 8, 148, 176)
    tr = (496, 8, 688, 176)
    bl = (8, 360, 148, 504)
    br = (496, 360, 688, 504)
    # Bands the corner itself must keep (the rim run inside the crop).
    tl_bands = [(96, 29, 148, 54), (30, 70, 48, 176)]
    tr_bands = [(496, 29, 646, 54), (646, 70, 674, 176)]
    bl_bands = [(30, 360, 48, 458), (96, 458, 148, 484)]
    br_bands = [(646, 360, 674, 458), (496, 458, 646, 484)]
    # Interior is the photograph. Anything bright in there that is not the
    # ornament is a leftover pixel from the mockup picture.
    out = {
        "corner-tl": _cut_corner(detail, tl, tl_bands, (64, 56, 148, 176)),
        "corner-tr": _cut_corner(detail, tr, tr_bands, (496, 56, 640, 176)),
        "corner-bl": _cut_corner(detail, bl, bl_bands, (64, 360, 148, 450)),
        "corner-br": _cut_corner(detail, br, br_bands, (496, 360, 640, 450)),
    }
    # Edges are the next source pixels after the corner, same origin, so the
    # rim line is the same row or column on both sides of the join.
    out["edge-h"] = _edge_from_corner(detail, tl, top, "h", 64)
    out["edge-hb"] = _edge_from_corner(detail, bl, bot, "h", 64)
    out["edge-v"] = _edge_from_corner(detail, tl, left, "v", 48)
    out["edge-vr"] = _edge_from_corner(detail, tr, right, "v", 48)
    return out


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    listing = load("2-listing-wide.jpg")
    detail = load("3-detail-wide.jpg")
    loading = load("1-loading.jpg")
    install = load("6-install-dialog.jpg")

    # Mockup pixels, no palette quantize. The cube crop includes the sparkle.
    save_raw_pair(crop_knock(loading, (400, 20, 900, 530), 14, 0), "loading-cube")
    # Include the dotted leaders. The box stops above the progress bar so a
    # segment does not survive as a stray dot after the word HUB.
    save_raw_pair(crop_knock(loading, (268, 530, 1014, 566), 12, 0), "load-title")
    frame, fill = split_bar(loading.crop((260, 580, 1030, 670)))
    save_raw_pair(frame, "bar-frame")
    save_raw_pair(fill, "bar-fill")

    save_raw_pair(crop_knock(listing, (16, 6, 74, 70), 20, 0), "header-cube")
    save_raw_pair(tint_hub(crop_knock(listing, (78, 10, 320, 58), 20, 0)), "wordmark")
    # Same cube is the fallback app icon.
    save_raw_pair(crop_knock(listing, (16, 6, 74, 70), 20, 0), "icon-cube")

    save_raw_pair(make_pill((40, 220, 70, 255), (8, 28, 16, 255)), "badge-installed")
    save_raw_pair(make_pill((230, 120, 30, 255), (48, 22, 8, 255)), "badge-update")
    save_raw_pair(make_pill((220, 48, 48, 255), (40, 10, 14, 255)), "badge-needs")

    # Active tab (GAMES) and a quiet tab (DEMOS). Text is cleared so the
    # label is drawn with the UI font on top of a 9-slice.
    tab_on = clear_interior(trim(knock(listing.crop((118, 72, 246, 118)), 14), 0), 8)
    tab = clear_interior(trim(knock(listing.crop((250, 72, 390, 118)), 14), 0), 8)
    # The mockup draws a colon between brackets. It sits in the inactive
    # tab's right 9-slice cap and is copied between every tab. The mark is
    # JPEG-gray, not pure white, so clear the cap and keep the frame edge.
    tp = tab.load()
    tw, th = tab.size
    cap = 14
    for y in range(6, th - 8):
        for x in range(tw - cap, tw):
            tp[x, y] = (0, 0, 0, 0)
    save_raw_pair(tab_on, "tab-on")
    save_raw_pair(tab, "tab")

    # Mockup pixels, label filled from the face. INSTALL and RUN share it.
    # UPDATE is the same shape pushed toward gold. NEEDS is the grey form.
    button = cut_action_button(detail)
    save_raw_pair(button, "button")
    save_raw_pair(tint_button_gold(button), "button-update")
    save_raw_pair(tint_button_grey(button), "button-needs")

    px = install.load()

    # Green striped install bar.
    xs, ys = [], []
    for y in range(200, 520):
        for x in range(300, 1000):
            r, g, b, _ = px[x, y]
            if g > 120 and g > r + 30 and g > b + 20:
                xs.append(x)
                ys.append(y)
    if xs:
        # The word DOWNLOADING sits above the bar; keep the dense lower band.
        y_bar = max(ys) - 36
        box = (min(xs) - 4, y_bar, max(xs) + 5, max(ys) + 6)
        stripe = trim(knock(install.crop(box), 16), 0)
        save_raw_pair(stripe, "bar-stripe")

    for name, im in content_frame(detail).items():
        save_raw_pair(im, name)

    track = detail.crop((1240, 240, 1264, 690))
    save_raw_pair(trim(knock(track, 18), 0), "scroll-track")
    thumb = detail.crop((1240, 236, 1264, 268))
    save_raw_pair(trim(knock(thumb, 18), 0), "scroll-thumb")

    save_hi = make_font()
    w, h = save_hi.size
    if w % 2:
        save_hi = save_hi.crop((0, 0, w - 1, h))
        w -= 1
    if h % 2:
        save_hi = save_hi.crop((0, 0, w, h - 1))
        h -= 1
    save_hi.save(OUT / "title-font-2.png", optimize=True)
    save_hi.resize((w // 2, h // 2), Image.Resampling.NEAREST).save(
        OUT / "title-font-1.png", optimize=True)
    print(f"{'title-font':16} 2x {w:4}x{h:<4} "
          f"{(OUT / 'title-font-2.png').stat().st_size:6}")
    save_exact_double(make_ui_font(), "ui-font")
    print("ui order", UI_ORDER)


if __name__ == "__main__":
    sys.exit(main())
