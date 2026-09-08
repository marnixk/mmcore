"""WORDPAD Times faces are native bitmaps, not nearest-neighbour 8x16."""

import os
import re

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def _load_bytes(name):
    text = open(os.path.join(REPO, "mmbasic", "src", name), encoding="utf-8").read()
    return [int(x, 16) for x in re.findall(r"0x[0-9A-Fa-f]{2}", text)]


def _unpack(data, index, w, h):
    rowb = (w + 7) // 8
    off = index * h * rowb
    rows = []
    for y in range(h):
        row = []
        for x in range(w):
            b = data[off + y * rowb + x // 8]
            row.append(1 if (b & (0x80 >> (x % 8))) else 0)
        rows.append(row)
    return rows


def _is_nearest(small, large, n):
    for y, row in enumerate(large):
        for x, pix in enumerate(row):
            if pix != small[y // n][x // n]:
                return False
    return True


def test_tnr_heading_fonts_are_not_nearest_neighbour():
    body = _load_bytes("font_tnr_8x16.c")
    h3 = _load_bytes("font_tnr_16x32.c")
    h2 = _load_bytes("font_tnr_24x48.c")
    h1 = _load_bytes("font_tnr_32x64.c")
    assert len(body) == 256 * 16
    assert len(h3) == 95 * 32 * 2
    assert len(h2) == 95 * 48 * 3
    assert len(h1) == 95 * 64 * 4
    small = _unpack(body, ord("A"), 8, 16)
    assert not _is_nearest(small, _unpack(h3, ord("A") - 32, 16, 32), 2)
    assert not _is_nearest(small, _unpack(h2, ord("A") - 32, 24, 48), 3)
    assert not _is_nearest(small, _unpack(h1, ord("A") - 32, 32, 64), 4)
    t = _unpack(h1, ord("T") - 32, 32, 64)
    mixed = 0
    for y in range(0, 64, 4):
        for x in range(0, 32, 4):
            block = {t[y + dy][x + dx] for dy in range(4) for dx in range(4)}
            if len(block) > 1:
                mixed += 1
    assert mixed >= 4
