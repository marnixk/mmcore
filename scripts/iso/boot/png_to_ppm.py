#!/usr/bin/env python3
"""Convert a PNG to a binary P6 PPM for the mmcore boot splash (#902).

`mmcore-splash` reads a plain P6 PPM so the helper has no image-library
dependency; this converter runs in the ISO builder (which already has Python)
and turns the committed branding `splash.png` into that PPM. Only the subset
the branding tools emit is supported: non-interlaced 8-bit grayscale, RGB or
RGBA. It uses only the standard library, so the builder needs no image toolkit.

Usage: png_to_ppm.py INPUT.png OUTPUT.ppm
"""

import struct
import sys
import zlib

PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"

COLOR_CHANNELS = {
    0: 1,  # grayscale
    2: 3,  # RGB
    4: 2,  # grayscale + alpha
    6: 4,  # RGBA
}


class PngError(Exception):
    pass


def _paeth(a, b, c):
    p = a + b - c
    pa = abs(p - a)
    pb = abs(p - b)
    pc = abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    if pb <= pc:
        return b
    return c


def _unfilter(raw, width, height, channels):
    """Reverse the per-scanline PNG filters. Returns width*height*channels."""
    stride = width * channels
    out = bytearray(height * stride)
    pos = 0
    prev = bytearray(stride)
    for y in range(height):
        if pos >= len(raw):
            raise PngError("truncated image data")
        ftype = raw[pos]
        pos += 1
        line = bytearray(raw[pos:pos + stride])
        if len(line) != stride:
            raise PngError("truncated scanline")
        pos += stride
        for x in range(stride):
            left = line[x - channels] if x >= channels else 0
            up = prev[x]
            up_left = prev[x - channels] if x >= channels else 0
            if ftype == 0:
                pass
            elif ftype == 1:
                line[x] = (line[x] + left) & 0xFF
            elif ftype == 2:
                line[x] = (line[x] + up) & 0xFF
            elif ftype == 3:
                line[x] = (line[x] + ((left + up) >> 1)) & 0xFF
            elif ftype == 4:
                line[x] = (line[x] + _paeth(left, up, up_left)) & 0xFF
            else:
                raise PngError(f"unknown filter type {ftype}")
        out[y * stride:(y + 1) * stride] = line
        prev = line
    return out


def decode_png(data):
    """Return (width, height, rgb_bytes) for a supported PNG."""
    if data[:8] != PNG_SIGNATURE:
        raise PngError("not a PNG file")
    pos = 8
    width = height = None
    bit_depth = color_type = interlace = None
    idat = bytearray()
    while pos + 8 <= len(data):
        length, ctype = struct.unpack(">I4s", data[pos:pos + 8])
        pos += 8
        chunk = data[pos:pos + length]
        pos += length + 4  # skip the chunk CRC
        if ctype == b"IHDR":
            width, height, bit_depth, color_type, _, _, interlace = struct.unpack(
                ">IIBBBBB", chunk
            )
        elif ctype == b"IDAT":
            idat += chunk
        elif ctype == b"IEND":
            break
    if width is None:
        raise PngError("missing IHDR")
    if bit_depth != 8:
        raise PngError(f"unsupported bit depth {bit_depth}")
    if interlace != 0:
        raise PngError("interlaced PNGs are not supported")
    channels = COLOR_CHANNELS.get(color_type)
    if channels is None:
        raise PngError(f"unsupported colour type {color_type}")
    raw = zlib.decompress(bytes(idat))
    pixels = _unfilter(raw, width, height, channels)
    rgb = bytearray(width * height * 3)
    for i in range(width * height):
        src = i * channels
        dst = i * 3
        if channels == 1:
            r = g = b = pixels[src]
        elif channels == 2:
            r = g = b = pixels[src]
        else:
            r, g, b = pixels[src], pixels[src + 1], pixels[src + 2]
        rgb[dst] = r
        rgb[dst + 1] = g
        rgb[dst + 2] = b
    return width, height, bytes(rgb)


def convert(src, dst):
    with open(src, "rb") as fh:
        data = fh.read()
    width, height, rgb = decode_png(data)
    with open(dst, "wb") as fh:
        fh.write(f"P6\n{width} {height}\n255\n".encode("ascii"))
        fh.write(rgb)
    return width, height


def main(argv):
    if len(argv) != 3:
        sys.stderr.write("usage: png_to_ppm.py INPUT.png OUTPUT.ppm\n")
        return 2
    convert(argv[1], argv[2])
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
