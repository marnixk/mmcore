"""IBM VGA 8x16 CP437 glyphs for TERM/TUI (BBS ANSI shades and ASCII)."""

import os
import re

from harness import MMBasicConsole
from test_term import _is_creamish, _is_dark_slate, _luminance, _menu, _open_term, _quit, _apply_slate_theme

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FONT_C = os.path.join(REPO, "mmbasic", "src", "font_cp437_8x16.c")
FONT14_C = os.path.join(REPO, "mmbasic", "src", "font_cp437_8x14.c")


def _load_font(path, rows_per_glyph):
    text = open(path, encoding="utf-8").read()
    glyphs = {}
    tail = rows_per_glyph - 1
    pat = (
        r"((?:0x[0-9A-Fa-f]{2},\s*){%d}0x[0-9A-Fa-f]{2}),\s*/\* 0x([0-9A-Fa-f]{2}) \*/"
        % tail
    )
    for m in re.finditer(pat, text):
        rows = [int(x, 16) for x in re.findall(r"0x[0-9A-Fa-f]{2}", m.group(1))]
        glyphs[int(m.group(2), 16)] = bytes(rows)
    return glyphs


def load_cp437_font():
    return _load_font(FONT_C, 16)


def load_cp437_font14():
    return _load_font(FONT14_C, 14)


def test_cp437_font_has_vga_shades_and_ascii():
    glyphs = load_cp437_font()
    assert len(glyphs) == 256
    assert glyphs[176] == bytes([0x11, 0x44] * 8)
    assert glyphs[177] == bytes([0x55, 0xAA] * 8)
    assert glyphs[178] == bytes([0xDD, 0x77] * 8)
    assert glyphs[0xDB] == bytes([0xFF] * 16)
    assert glyphs[ord("?")] != glyphs[0xDB]
    t = glyphs[ord("t")]
    assert t[5] == 0xFC
    l = glyphs[ord("l")]
    assert l[2] == 0x38
    assert l[11] == 0x3C
    d = glyphs[ord("d")]
    assert d[11] == 0x76
    assert glyphs[0xB3] == bytes([0x18] * 16)


def test_ega_8x14_font_has_shades_and_box_glyphs():
    """#1155: the EGA 8x14 fallback face keeps shades, blocks and box joins."""
    glyphs = load_cp437_font14()
    assert len(glyphs) == 256
    assert glyphs[176] == bytes([0x11, 0x44] * 7)
    assert glyphs[177] == bytes([0x55, 0xAA] * 7)
    assert glyphs[178] == bytes([0xDD, 0x77] * 7)
    assert glyphs[0xDB] == bytes([0xFF] * 14)
    assert glyphs[0xB3] == bytes([0x18] * 14)
    # The horizontal line is one scanline tall, so vertical runs join it.
    assert glyphs[0xC4].count(0xFF) == 1
    assert glyphs[0xC4][7] == 0xFF
    # Corners share that scanline with the horizontal and vertical rules, so
    # box frames have no gaps.
    assert glyphs[0xDA][7] == 0x1F and glyphs[0xDA][8] == 0x18
    assert glyphs[0xBF][7] == 0xF8 and glyphs[0xBF][8] == 0x18
    assert glyphs[ord("A")] != glyphs[ord("?")]


def test_term_demo_cp437_shades_on_hdmi(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        _apply_slate_theme(con)
        seen = _open_term(con, 'TERM "demo", 23', quiet=0.8, timeout=10.0)
        assert "CP437" in seen
        _menu(con)
        w, h = con.screen_size()
        assert (w, h) == (960, 540)
        # Status bar is pinned to the last 16 scanlines (not cell-row 32).
        x0, y0 = 13 * 8, h - 16
        on = con.screen_pixel(x0 + 3, y0)
        off = con.screen_pixel(x0 + 0, y0)
        assert _is_creamish(*on), on
        assert _is_dark_slate(*off), off
        assert _luminance(*on) > _luminance(*off) + 40
        med = con.screen_pixel(x0 + 8 + 1, y0)
        assert _is_creamish(*med), med
        _quit(con)
    finally:
        con.stop()
