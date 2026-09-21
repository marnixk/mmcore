#!/usr/bin/env python3
"""Generate a simple 256x256 PNG icon for the Linux AppImage.

Stdlib only (zlib + struct) so CI needs no image tooling. Draws a dark blue
tile with a white border and a white "M".
"""
import struct
import sys
import zlib

W = H = 256
BG = (30, 58, 138)
FG = (255, 255, 255)

canvas = [[BG for _ in range(W)] for _ in range(H)]


def put(x, y, color=FG):
    if 0 <= x < W and 0 <= y < H:
        canvas[y][x] = color


def rect(x0, y0, x1, y1, color=BG):
    for y in range(max(0, y0), min(H, y1 + 1)):
        for x in range(max(0, x0), min(W, x1 + 1)):
            canvas[y][x] = color


def thick_line(x0, y0, x1, y1, t, color=FG):
    steps = max(abs(x1 - x0), abs(y1 - y0), 1)
    for i in range(steps + 1):
        x = x0 + (x1 - x0) * i // steps
        y = y0 + (y1 - y0) * i // steps
        for dy in range(-t, t + 1):
            for dx in range(-t, t + 1):
                put(x + dx, y + dy, color)


# White border.
rect(0, 0, W - 1, H - 1, FG)
rect(8, 8, W - 9, H - 9, BG)

# "M": two verticals plus the middle V.
thick_line(70, 196, 70, 76, 7)
thick_line(186, 196, 186, 76, 7)
thick_line(78, 76, 128, 150, 7)
thick_line(128, 150, 178, 76, 7)

rows = []
for y in range(H):
    row = bytearray([0])
    for x in range(W):
        r, g, b = canvas[y][x]
        row += bytes((r, g, b, 255))
    rows.append(bytes(row))
raw = b"".join(rows)


def chunk(tag, data):
    return (
        struct.pack(">I", len(data))
        + tag
        + data
        + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    )


png = (
    b"\x89PNG\r\n\x1a\n"
    + chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 6, 0, 0, 0))
    + chunk(b"IDAT", zlib.compress(raw, 9))
    + chunk(b"IEND", b"")
)

out = sys.argv[1] if len(sys.argv) > 1 else "mmbasic.png"
with open(out, "wb") as f:
    f.write(png)
print("wrote %s (%d bytes)" % (out, len(png)))
