#!/usr/bin/env python3
"""Generate the mmcore app icons from the shared branding art.

Source of truth: ``assets/branding/mmcore-app-icon.png`` (1024x1024, the
chrome M on black with red streaks). Every native package icon is derived
from it by box-filtered resizing:

    gen-appicon.py OUT.png [SIZE]   one PNG (default 256)
    gen-appicon.py --iconset DIR    the ten macOS .iconset PNGs
    gen-appicon.py --ico OUT.ico    a multi-size Windows .ico

Stdlib only (zlib + struct), so CI needs no image tooling.
"""
from __future__ import annotations

import struct
import sys
import zlib
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
BRANDING = REPO / "assets" / "branding" / "mmcore-app-icon.png"

# macOS .iconset contents: (filename, pixels).
ICONSET = [
    ("icon_16x16.png", 16),
    ("icon_16x16@2x.png", 32),
    ("icon_32x32.png", 32),
    ("icon_32x32@2x.png", 64),
    ("icon_128x128.png", 128),
    ("icon_128x128@2x.png", 256),
    ("icon_256x256.png", 256),
    ("icon_256x256@2x.png", 512),
    ("icon_512x512.png", 512),
    ("icon_512x512@2x.png", 1024),
]

# Windows .ico sizes (Vista+ reads PNG-compressed entries).
ICO_SIZES = [16, 32, 48, 64, 128, 256]


def _be32(b: bytes, o: int) -> int:
    return (b[o] << 24) | (b[o + 1] << 16) | (b[o + 2] << 8) | b[o + 3]


def _unfilter(raw: bytes, w: int, h: int, bpp: int) -> bytearray:
    rowbytes = w * bpp
    out = bytearray(rowbytes * h)
    prev = bytearray(rowbytes)
    pos = 0
    for y in range(h):
        f = raw[pos]
        pos += 1
        cur = bytearray(raw[pos : pos + rowbytes])
        pos += rowbytes
        if f == 1:  # Sub
            for x in range(bpp, rowbytes):
                cur[x] = (cur[x] + cur[x - bpp]) & 255
        elif f == 2:  # Up
            for x in range(rowbytes):
                cur[x] = (cur[x] + prev[x]) & 255
        elif f == 3:  # Average
            for x in range(rowbytes):
                a = cur[x - bpp] if x >= bpp else 0
                cur[x] = (cur[x] + ((a + prev[x]) >> 1)) & 255
        elif f == 4:  # Paeth
            for x in range(rowbytes):
                a = cur[x - bpp] if x >= bpp else 0
                b = prev[x]
                c = prev[x - bpp] if x >= bpp else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                if pa <= pb and pa <= pc:
                    pr = a
                elif pb <= pc:
                    pr = b
                else:
                    pr = c
                cur[x] = (cur[x] + pr) & 255
        elif f != 0:
            raise SystemExit("gen-appicon: unsupported PNG filter %d" % f)
        out[y * rowbytes : (y + 1) * rowbytes] = cur
        prev = cur
    return out


def decode_png(data: bytes):
    """Decode an 8-bit, non-interlaced RGB/RGBA PNG.

    Returns ``(width, height, bpp, raw)`` where ``raw`` is unfiltered
    interleaved bytes (3 or 4 per pixel).
    """
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise SystemExit("gen-appicon: not a PNG: %s" % BRANDING)
    pos = 8
    w = h = ct = 0
    idat = bytearray()
    while pos + 8 <= len(data):
        ln = _be32(data, pos)
        tag = data[pos + 4 : pos + 8]
        body = data[pos + 8 : pos + 8 + ln]
        if tag == b"IHDR":
            w, h = _be32(body, 0), _be32(body, 4)
            depth, ct, interlace = body[8], body[9], body[12]
            if depth != 8 or ct not in (2, 6) or interlace != 0:
                raise SystemExit(
                    "gen-appicon: need 8-bit non-interlaced RGB/RGBA PNG"
                )
        elif tag == b"IDAT":
            idat += body
        elif tag == b"IEND":
            break
        pos += 12 + ln
    if not w or not h or not idat:
        raise SystemExit("gen-appicon: malformed PNG: %s" % BRANDING)
    bpp = 3 if ct == 2 else 4
    return w, h, bpp, _unfilter(zlib.decompress(bytes(idat)), w, h, bpp)


def resize(src: bytes, sw: int, sh: int, bpp: int, nw: int, nh: int) -> bytes:
    """Box-filter ``src`` down to ``nw`` x ``nh`` and return RGBA bytes."""
    out = bytearray(nw * nh * 4)
    o = 0
    for dy in range(nh):
        y0 = dy * sh // nh
        y1 = max(y0 + 1, (dy + 1) * sh // nh)
        for dx in range(nw):
            x0 = dx * sw // nw
            x1 = max(x0 + 1, (dx + 1) * sw // nw)
            r = g = b = a = 0
            count = 0
            for sy in range(y0, y1):
                base = sy * sw * bpp
                for sx in range(x0, x1):
                    p = base + sx * bpp
                    r += src[p]
                    g += src[p + 1]
                    b += src[p + 2]
                    if bpp == 4:
                        a += src[p + 3]
                    count += 1
            if bpp == 3:
                a = 255 * count
            out[o] = r // count
            out[o + 1] = g // count
            out[o + 2] = b // count
            out[o + 3] = a // count
            o += 4
    return bytes(out)


def _chunk(tag: bytes, data: bytes) -> bytes:
    return (
        struct.pack(">I", len(data))
        + tag
        + data
        + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    )


def encode_png(w: int, h: int, rgba: bytes) -> bytes:
    stride = w * 4
    scan = bytearray()
    for y in range(h):
        scan.append(0)
        scan += rgba[y * stride : (y + 1) * stride]
    return (
        b"\x89PNG\r\n\x1a\n"
        + _chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
        + _chunk(b"IDAT", zlib.compress(bytes(scan), 9))
        + _chunk(b"IEND", b"")
    )


def write_png(path, w: int, h: int, rgba: bytes) -> None:
    Path(path).write_bytes(encode_png(w, h, rgba))


def write_ico(path, images) -> None:
    """Write a Windows .ico whose entries are PNG-compressed ``images``."""
    header = struct.pack("<HHH", 0, 1, len(images))
    offset = 6 + 16 * len(images)
    entries = bytearray()
    blobs = bytearray()
    for size, blob in images:
        dim = 0 if size >= 256 else size
        entries += struct.pack(
            "<BBBBHHII", dim, dim, 0, 0, 1, 32, len(blob), offset
        )
        offset += len(blob)
        blobs += blob
    Path(path).write_bytes(header + bytes(entries) + bytes(blobs))


def load_branding():
    w, h, bpp, src = decode_png(BRANDING.read_bytes())
    return w, h, bpp, src


def main(argv) -> int:
    if not argv:
        print(__doc__.strip().splitlines()[0])
        return 2

    if argv[0] == "--iconset":
        if len(argv) < 2:
            raise SystemExit("gen-appicon: --iconset needs a directory")
        outdir = Path(argv[1])
        outdir.mkdir(parents=True, exist_ok=True)
        w, h, bpp, src = load_branding()
        for name, size in ICONSET:
            write_png(outdir / name, size, size, resize(src, w, h, bpp, size, size))
        print("wrote %s (%d files)" % (outdir, len(ICONSET)))
        return 0

    if argv[0] == "--ico":
        if len(argv) < 2:
            raise SystemExit("gen-appicon: --ico needs an output path")
        w, h, bpp, src = load_branding()
        images = [
            (size, encode_png(size, size, resize(src, w, h, bpp, size, size)))
            for size in ICO_SIZES
        ]
        write_ico(argv[1], images)
        print("wrote %s (%d sizes)" % (argv[1], len(images)))
        return 0

    out = argv[0]
    size = int(argv[1]) if len(argv) > 1 else 256
    w, h, bpp, src = load_branding()
    write_png(out, size, size, resize(src, w, h, bpp, size, size))
    print("wrote %s (%dx%d)" % (out, size, size))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
