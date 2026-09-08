#!/usr/bin/env python3
"""Rasterize Liberation Serif into an 8x16 1-bpp C array (Times-compatible)."""
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

TTF = "/usr/share/fonts/truetype/liberation/LiberationSerif-Regular.ttf"
OUT = Path(__file__).resolve().parents[1] / "mmbasic" / "src" / "font_tnr_8x16.c"

font = ImageFont.truetype(TTF, 13)
rows = []
for ch in range(256):
    im = Image.new("L", (8, 16), 0)
    dr = ImageDraw.Draw(im)
    if 32 <= ch < 127 or ch in (169, 174):
        dr.text((0, 0), chr(ch), font=font, fill=255)
    elif ch >= 128:
        dr.text((0, 0), chr(ch) if ch < 256 else "?", font=font, fill=255)
    bits = []
    pix = im.load()
    for y in range(16):
        b = 0
        for x in range(8):
            if pix[x, y] > 80:
                b |= 0x80 >> x
        bits.append(b)
    rows.append(bits)

lines = [
    "/* Liberation Serif, metric-compatible with Times New Roman, 8x16. */",
    "#include \"mmb_priv.h\"",
    "",
    "const unsigned char mmb_tnr_8x16[256 * 16] = {",
]
for ch, bits in enumerate(rows):
    hx = ", ".join(f"0x{b:02x}" for b in bits)
    lines.append(f"\t{hx},")
lines.append("};")
lines.append("")
OUT.write_text("\n".join(lines))
print("wrote", OUT, "bytes", OUT.stat().st_size)
