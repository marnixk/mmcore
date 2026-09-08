#!/usr/bin/env python3
"""Rasterize Liberation Serif into 1-bpp C arrays at 8x16, 16x32, 24x48, 32x64."""
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

REGULAR = "/usr/share/fonts/truetype/liberation/LiberationSerif-Regular.ttf"
BOLD = "/usr/share/fonts/truetype/liberation/LiberationSerif-Bold.ttf"
SRC = Path(__file__).resolve().parents[1] / "mmbasic" / "src"
PREVIEW = Path("/opt/cursor/artifacts/wordpad-tnr-preview.png")

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


def raster_char(ch, w, h, pt, ttf):
    font = ImageFont.truetype(ttf, pt)
    im = Image.new("L", (w, h), 0)
    if ch < 32 or ch > 126:
        if not (ttf == REGULAR and (ch == 169 or ch == 174 or ch >= 128)):
            return pack_glyph(im, w, h)
    dr = ImageDraw.Draw(im)
    s = chr(ch)
    try:
        bbox = font.getbbox(s)
    except Exception:
        bbox = (0, 0, 0, 0)
    gw = max(0, bbox[2] - bbox[0])
    gh = max(0, bbox[3] - bbox[1])
    x = (w - gw) // 2 - bbox[0]
    y = (h - gh) // 2 - bbox[1]
    if y < 0:
        y = 0
    dr.text((x, y), s, font=font, fill=255)
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


def preview_png():
    PREVIEW.parent.mkdir(parents=True, exist_ok=True)
    samples = "Times Heading"
    canvases = []
    for name, w, h, pt, ttf, full in SIZES:
        font = ImageFont.truetype(ttf, pt)
        im = Image.new("L", (max(w * len(samples), 8), h), 0)
        dr = ImageDraw.Draw(im)
        x = 0
        for ch in samples:
            bits = raster_char(ord(ch), w, h, pt, ttf)
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
    for name, w, h, pt, ttf, full in SIZES:
        count = 256 if full else 95
        glyphs = []
        if full:
            for ch in range(256):
                glyphs.append(raster_char(ch, w, h, pt, ttf))
        else:
            for ch in range(32, 127):
                glyphs.append(raster_char(ch, w, h, pt, ttf))
        face = "Liberation Serif Regular" if ttf == REGULAR else "Liberation Serif Bold"
        write_c(name, w, h, glyphs, f"{face} {w}x{h}, {count} glyphs, 1-bpp.")
    preview_png()


if __name__ == "__main__":
    main()
