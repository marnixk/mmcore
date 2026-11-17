#!/usr/bin/env python3
"""Cut HUB chrome PNGs from the approved mockups into ramdisk/hub/.

Each piece is written at 2x (the 1280 mockup's own pixels) and at 1x (nearest
neighbour half). Backgrounds that touch the crop edge are cleared to alpha.
The title face is a generated italic chrome/purple sheet, not CP437.
"""

from __future__ import annotations

import sys
from pathlib import Path

from PIL import Image

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


def make_font() -> Image.Image:
    """Two rows: chrome, then purple. One cell per character."""
    scale = 7
    gw, gh = 7 * scale, 7 * scale
    italic = scale  # extra width so the lean stays inside the cell
    pad = 3
    cell_w = gw + italic + pad * 2
    cell_h = gh + pad * 2
    n = len(FONT_ORDER)
    sheet = Image.new("RGBA", (cell_w * n, cell_h * 2), (0, 0, 0, 0))
    sp = sheet.load()

    def grad(row, t):
        # t is 0 at the top of the glyph, 1 at the bottom.
        if row == 0:
            r = int(255 - 70 * t)
            g = int(250 - 90 * t)
            b = int(255 - 40 * t)
        else:
            r = int(240 - 140 * t)
            g = int(180 - 140 * t)
            b = int(255 - 40 * t)
        return r, g, b

    for index, ch in enumerate(FONT_ORDER):
        rows = GLYPHS.get(ch)
        if not rows:
            continue
        for line in (0, 1):
            ox = index * cell_w
            oy = line * cell_h
            ink = []
            for gy, bits in enumerate(rows):
                for gx, bit in enumerate(bits):
                    if bit != "1":
                        continue
                    for sy in range(scale):
                        for sx in range(scale):
                            y = pad + gy * scale + sy
                            lean = (cell_h - (pad + gy * scale + sy)) // 5
                            x = pad + gx * scale + sx + lean
                            ink.append((x, y, gy / max(1, len(rows) - 1)))
            solid = set((x, y) for x, y, _ in ink)
            # Dark outline, then the gradient core.
            for x, y, _ in ink:
                for dx in range(-2, 3):
                    for dy in range(-2, 3):
                        px, py = ox + x + dx, oy + y + dy
                        if 0 <= px < sheet.size[0] and 0 <= py < sheet.size[1]:
                            if (x + dx, y + dy) not in solid:
                                sp[px, py] = (12, 6, 22, 255)
            for x, y, t in ink:
                sp[ox + x, oy + y] = (*grad(line, t), 255)
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


def corners_and_edges(listing: Image.Image):
    """Ornaments from the listing preview card."""
    px = listing.load()
    w, h = listing.size
    xs, ys = [], []
    for y in range(110, h - 8):
        for x in range(790, w - 4):
            r, g, b = px[x, y][:3]
            if r > 150 and 90 < g < 200 and b < 120 and r > g + 10:
                xs.append(x)
                ys.append(y)
    x0, x1 = min(xs), max(xs) + 1
    y0, y1 = min(ys), max(ys) + 1
    side = 70
    boxes = {
        "corner-tl": (x0 - 8, y0 - 8, x0 + side, y0 + side),
        "corner-tr": (x1 - side, y0 - 8, x1 + 8, y0 + side),
        "corner-bl": (x0 - 8, y1 - side, x0 + side, y1 + 8),
        "corner-br": (x1 - side, y1 - side, x1 + 8, y1 + 8),
    }
    out = {name: keep_frame(listing.crop(box)) for name, box in boxes.items()}
    mid = (x0 + x1) // 2
    out["edge-h"] = keep_frame(listing.crop((mid - 16, y0 - 6, mid + 16, y0 + 18)))
    out["edge-v"] = keep_frame(listing.crop((x0 - 6, (y0 + y1) // 2 - 16,
                                             x0 + 18, (y0 + y1) // 2 + 16)))
    return out


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    listing = load("2-listing-wide.jpg")
    detail = load("3-detail-wide.jpg")
    loading = load("1-loading.jpg")
    install = load("6-install-dialog.jpg")

    save_pair(crop_knock(loading, (400, 20, 900, 530), 14, 0), "loading-cube", 24)
    # Include the dotted leaders. The box stops above the progress bar so a
    # segment does not survive as a stray dot after the word HUB.
    save_pair(crop_knock(loading, (268, 530, 1014, 566), 12, 0), "load-title")
    frame, fill = split_bar(loading.crop((260, 580, 1030, 670)))
    save_pair(frame, "bar-frame")
    save_pair(fill, "bar-fill")

    save_pair(crop_knock(listing, (16, 6, 74, 70), 20, 0), "header-cube")
    save_pair(crop_knock(listing, (78, 10, 320, 58), 20, 0), "wordmark")
    # Same cube is the fallback app icon.
    save_pair(crop_knock(listing, (16, 6, 74, 70), 20, 0), "icon-cube")

    save_pair(crop_knock(listing, (648, 154, 782, 206), 14, 1), "badge-installed")
    save_pair(crop_knock(listing, (648, 248, 782, 304), 14, 1), "badge-update")
    save_pair(crop_knock(listing, (644, 526, 782, 582), 14, 1), "badge-needs")

    # Active tab (GAMES) and a quiet tab (DEMOS). Text is cleared so the
    # label is drawn with the UI font on top of a 9-slice.
    tab_on = clear_interior(trim(knock(listing.crop((118, 72, 246, 118)), 14), 0), 8)
    tab = clear_interior(trim(knock(listing.crop((250, 72, 390, 118)), 14), 0), 8)
    save_pair(tab_on, "tab-on")
    save_pair(tab, "tab")

    button = trim(knock(detail.crop((780, 590, 1160, 700)), 16), 1)
    button = clear_interior(button, 16)
    save_pair(button, "button")

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
        save_pair(stripe, "bar-stripe")

    for name, im in corners_and_edges(listing).items():
        save_pair(im, name)

    track = detail.crop((1246, 248, 1258, 688))
    save_pair(trim(knock(track, 20), 0), "scroll-track")
    thumb = detail.crop((1246, 250, 1258, 268))
    save_pair(trim(knock(thumb, 20), 0), "scroll-thumb")

    save_pair(make_font(), "title-font")
    print("font order", FONT_ORDER)


if __name__ == "__main__":
    sys.exit(main())
