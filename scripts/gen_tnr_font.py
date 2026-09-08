#!/usr/bin/env python3
"""Rasterize Liberation Serif into 1-bpp C arrays at 8x16, 16x32, 24x48, 32x64.

Each face uses one cell size and one shared baseline so the alphabet sits
on a line. Point size is reduced until every printable glyph's ink fits
in the cell (GH-187).
"""
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

REGULAR = "/usr/share/fonts/truetype/liberation/LiberationSerif-Regular.ttf"
BOLD = "/usr/share/fonts/truetype/liberation/LiberationSerif-Bold.ttf"
SRC = Path(__file__).resolve().parents[1] / "mmbasic" / "src"
PREVIEW = Path("/opt/cursor/artifacts/wordpad-tnr-preview.png")
PRINTABLE = list(range(32, 127))

SIZES = [
    ("8x16", 8, 16, 12, REGULAR, True),
    ("16x32", 16, 32, 26, BOLD, False),
    ("24x48", 24, 48, 40, BOLD, False),
    ("32x64", 32, 64, 54, BOLD, False),
]


def pack_glyph(im, w, h):
    pix = im.load()
    row_bytes = (w + 7) // 8
    rows = []
    for y in range(h):
        packed = [0] * row_bytes
        for x in range(w):
            if pix[x, y] > 96:
                packed[x // 8] |= 0x80 >> (x % 8)
        rows.extend(packed)
    return rows


def bbox_of(font, ch):
    try:
        box = font.getbbox(chr(ch), anchor="ls")
    except Exception:
        return (0, 0, 0, 0)
    if box is None:
        return (0, 0, 0, 0)
    return box


def face_span(font):
    min_top = 0
    max_bot = 0
    max_w = 0
    for ch in PRINTABLE:
        l, t, r, b = bbox_of(font, ch)
        min_top = min(min_top, t)
        max_bot = max(max_bot, b)
        max_w = max(max_w, max(0, r - l))
    return min_top, max_bot, max_w


def fit_pt(w, h, start_pt, ttf):
    pt = start_pt
    while pt > 4:
        font = ImageFont.truetype(ttf, pt)
        min_top, max_bot, max_w = face_span(font)
        span = max_bot - min_top
        if max_w <= w and span <= h:
            return pt, min_top, max_bot, max_w
        pt -= 1
    font = ImageFont.truetype(ttf, 4)
    min_top, max_bot, max_w = face_span(font)
    return 4, min_top, max_bot, max_w


def raster_char(ch, w, h, font, baseline_y, ttf):
    im = Image.new("L", (w, h), 0)
    extra = ttf == REGULAR and (ch == 169 or ch == 174 or ch >= 128)
    if (ch < 32 or ch > 126) and not extra:
        return pack_glyph(im, w, h)
    dr = ImageDraw.Draw(im)
    s = chr(ch)
    l, t, r, b = bbox_of(font, ch)
    gw = max(0, r - l)
    x = (w - gw) // 2 - l
    dr.text((x, baseline_y), s, font=font, fill=255, anchor="ls")
    return pack_glyph(im, w, h)


def write_c(name, w, h, glyphs, comment):
    row_bytes = (w + 7) // 8
    n = len(glyphs)
    nbytes = n * h * row_bytes
    path = SRC / f"font_tnr_{name}.c"
    lines = [
        f"/* {comment} */",
        '#include "mmb_priv.h"',
        "",
        f"const unsigned char mmb_tnr_{name}[{nbytes}] = {{",
    ]
    for bits in glyphs:
        hx = ", ".join(f"0x{b:02x}" for b in bits)
        lines.append(f"\t{hx},")
    lines.append("};")
    lines.append("")
    path.write_text("\n".join(lines))
    print("wrote", path, "bytes", path.stat().st_size)


def preview_png(layouts):
    PREVIEW.parent.mkdir(parents=True, exist_ok=True)
    samples = "Times Heading Magy"
    canvases = []
    for name, w, h, font, baseline_y, ttf in layouts:
        im = Image.new("L", (max(w * len(samples), 8), h), 0)
        x = 0
        for ch in samples:
            bits = raster_char(ord(ch), w, h, font, baseline_y, ttf)
            row_bytes = (w + 7) // 8
            cell = Image.new("L", (w, h), 0)
            px = cell.load()
            for y in range(h):
                for xb in range(row_bytes):
                    b = bits[y * row_bytes + xb]
                    for bit in range(8):
                        xx = xb * 8 + bit
                        if xx < w and (b & (0x80 >> bit)):
                            px[xx, y] = 255
            im.paste(cell, (x, 0))
            x += w
        canvases.append(im)
    total_h = sum(im.height + 4 for im in canvases)
    total_w = max(im.width for im in canvases)
    out = Image.new("RGB", (total_w, total_h), (24, 24, 24))
    y = 0
    for im in canvases:
        rgb = Image.merge("RGB", (im, im, im))
        out.paste(rgb, (0, y))
        y += im.height + 4
    out.save(PREVIEW)
    print("preview", PREVIEW)


def main():
    layouts = []
    for name, w, h, start_pt, ttf, full in SIZES:
        pt, min_top, max_bot, max_w = fit_pt(w, h, start_pt, ttf)
        span = max_bot - min_top
        baseline_y = (h - span) // 2 - min_top
        font = ImageFont.truetype(ttf, pt)
        print(
            f"{name}: pt={pt} M-fit max_w={max_w} span={span} "
            f"baseline_y={baseline_y} min_top={min_top} max_bot={max_bot}"
        )
        count = 256 if full else 95
        glyphs = []
        if full:
            for ch in range(256):
                glyphs.append(raster_char(ch, w, h, font, baseline_y, ttf))
        else:
            for ch in PRINTABLE:
                glyphs.append(raster_char(ch, w, h, font, baseline_y, ttf))
        face = "Liberation Serif Regular" if ttf == REGULAR else "Liberation Serif Bold"
        write_c(
            name,
            w,
            h,
            glyphs,
            f"{face} {w}x{h} pt {pt}, shared baseline, {count} glyphs, 1-bpp.",
        )
        layouts.append((name, w, h, font, baseline_y, ttf))
    preview_png(layouts)


if __name__ == "__main__":
    main()
