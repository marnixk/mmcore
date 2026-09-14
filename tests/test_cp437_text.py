"""Default HDMI face is IBM VGA CP437 8x16 for TEXT and PRINT (issue #288)."""

from ihelp_util import dump_topic
from test_term_cp437 import load_cp437_font


def test_help_text_cp437(console):
    out = dump_topic(console, "TEXT")
    assert "CP437" in out
    assert "CHR$" in out
    font = dump_topic(console, "FONT")
    assert "CP437" in font


def test_text_chr_219_is_cp437_block(fresh_console):
    c = fresh_console
    c.send_line("CLS")
    assert c.send_line("TEXT 0,0,CHR$(219),RGB(255,255,255)") == ""
    assert int(c.send_line("PRINT PIXEL(0,0)")) != 0
    assert int(c.send_line("PRINT PIXEL(7,15)")) != 0
    assert int(c.send_line("PRINT PIXEL(8,0)")) == 0
    assert int(c.send_line("PRINT PIXEL(0,16)")) == 0


def test_text_chr_196_is_cp437_hline(fresh_console):
    c = fresh_console
    c.send_line("CLS")
    assert c.send_line("TEXT 0,0,CHR$(196),RGB(255,255,255)") == ""
    assert int(c.send_line("PRINT PIXEL(0,7)")) != 0
    assert int(c.send_line("PRINT PIXEL(7,7)")) != 0
    assert int(c.send_line("PRINT PIXEL(0,0)")) == 0
    assert int(c.send_line("PRINT PIXEL(0,8)")) == 0


def test_text_high_byte_matches_font_bits(fresh_console):
    glyphs = load_cp437_font()
    shade = glyphs[176]
    c = fresh_console
    c.send_line("CLS")
    assert c.send_line("TEXT 0,0,CHR$(176),RGB(255,255,255)") == ""
    for x, y in ((0, 0), (3, 0), (7, 0), (1, 1), (2, 1), (5, 1)):
        on = bool(shade[y] & (0x80 >> x))
        pix = int(c.send_line(f"PRINT PIXEL({x},{y})"))
        if on:
            assert pix != 0, f"expected ink at ({x},{y})"
        else:
            assert pix == 0, f"expected paper at ({x},{y})"


def test_print_chr_219_uses_cp437_on_hdmi(fresh_console):
    c = fresh_console
    c.send_line("CLS")
    c.send_line("COLOUR RGB(255,255,255), RGB(0,0,0)")
    c.send_line("LOCATE 0,0")
    out = c.send_line("PRINT CHR$(219);")
    assert "?SYNTAX" not in out.upper()
    r, g, b = c.screen_pixel(3, 8)
    assert r > 150 and g > 150 and b > 150, (r, g, b)
    r2, g2, b2 = c.screen_pixel(3, 40)
    assert r2 < 40 and g2 < 40 and b2 < 40, (r2, g2, b2)
