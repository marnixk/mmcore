"""Graphics tests: verify CMM2-style drawing commands by reading the emulated
framebuffer back.

The Colour Maximite 2 is the compatibility target, and graphics equivalence is
the priority. These tests exercise the verification approach the graphics port
will rely on: draw with the console's PIXEL/LINE/BOX/CIRCLE commands, then read
individual pixels and compare the whole frame against a golden image.
"""

import os

import pytest

GOLDEN_DIR = os.path.join(os.path.dirname(__file__), "golden")

# A fixed, deterministic scene reused by the golden-image test and to
# (re)generate the golden itself. See scripts/gen_golden.py.
SCENE = [
    "CLS",
    "BOX 60,120,200,150,GREEN",
    "LINE 60,120,260,270,RED",
    "CIRCLE 440,200,90,CYAN",
    "LINE 340,120,540,120,YELLOW",
    "PIXEL 500,300,MAGENTA",
]


def _is_red(rgb):
    r, g, b = rgb
    return r > 150 and g < 130 and b < 130


def _is_green(rgb):
    r, g, b = rgb
    return g > 150 and r < 130 and b < 130


def _is_blue(rgb):
    r, g, b = rgb
    return b > 150 and r < 130 and g < 130


def _is_yellow(rgb):
    r, g, b = rgb
    return r > 150 and g > 150 and b < 130


def _is_black(rgb):
    return all(c < 40 for c in rgb)


def test_hdmi_follows_mode(fresh_console):
    """MODE must retune the HDMI framebuffer, not only MM.HRES/MM.VRES."""
    c = fresh_console
    assert c.screen_size() == (640, 480)
    assert c.send_line("MODE 7,16") == ""
    assert c.send_line("PRINT MM.HRES") == "320"
    assert c.send_line("PRINT MM.VRES") == "240"
    assert c.screen_size() == (320, 240)
    c.send_line("CLS")
    c.send_line("PIXEL 319,239,RED")
    assert _is_red(c.screen_pixel(319, 239))
    assert c.send_line("MODE 8,16") == ""
    assert c.send_line("PRINT MM.HRES") == "640"
    assert c.screen_size() == (640, 480)


def test_pixel_sets_color(fresh_console):
    fresh_console.send_line("CLS")
    fresh_console.send_line("PIXEL 300,300,RED")
    assert _is_red(fresh_console.screen_pixel(300, 300))
    assert _is_black(fresh_console.screen_pixel(300, 340))


def test_horizontal_line(fresh_console):
    fresh_console.send_line("CLS")
    fresh_console.send_line("LINE 50,200,300,200,GREEN")
    assert _is_green(fresh_console.screen_pixel(175, 200))
    assert _is_black(fresh_console.screen_pixel(175, 260))


def test_box_outline(fresh_console):
    fresh_console.send_line("CLS")
    fresh_console.send_line("BOX 100,120,200,150,BLUE")
    assert _is_blue(fresh_console.screen_pixel(200, 120))   # top edge
    assert _is_blue(fresh_console.screen_pixel(100, 195))   # left edge
    assert _is_black(fresh_console.screen_pixel(200, 195))  # interior (outline only)


def test_circle_outline(fresh_console):
    fresh_console.send_line("CLS")
    fresh_console.send_line("CIRCLE 320,240,100,YELLOW")
    assert _is_yellow(fresh_console.screen_pixel(420, 240))  # rightmost point
    assert _is_black(fresh_console.screen_pixel(320, 240))   # centre is empty


def test_circle_fill_centre_and_rim(fresh_console):
    fresh_console.send_line("CLS")
    fresh_console.send_line("CIRCLE 200,180,60,1,RGB(255,0,0),RGB(0,255,0)")
    assert _is_green(fresh_console.screen_pixel(200, 180))   # filled centre
    assert _is_green(fresh_console.screen_pixel(230, 180))   # interior
    assert _is_red(fresh_console.screen_pixel(260, 180))     # rim
    assert _is_black(fresh_console.screen_pixel(270, 180))   # outside
    assert _is_red(fresh_console.screen_pixel(200, 120))     # top rim
    assert _is_black(fresh_console.screen_pixel(200, 110))   # above


def test_circle_fill_solid_disk(fresh_console):
    fresh_console.send_line("CLS")
    fresh_console.send_line("CIRCLE 100,100,40,1,RGB(0,0,255),RGB(0,0,255)")
    assert _is_blue(fresh_console.screen_pixel(100, 100))
    assert _is_blue(fresh_console.screen_pixel(100, 130))
    assert _is_blue(fresh_console.screen_pixel(128, 128))
    assert _is_black(fresh_console.screen_pixel(100, 145))


def test_circle_thick_stroke_no_background_leak(fresh_console):
    """#137: thick stroke + different fill must not leave CLS gaps in the rim."""
    c = fresh_console
    c.send_line("CLS")
    c.send_line("CIRCLE 200,180,60,4,RGB(255,0,0),RGB(0,255,0)")
    assert _is_green(c.screen_pixel(200, 180))
    assert _is_green(c.screen_pixel(248, 180))
    for x, y in ((258, 180), (200, 122), (142, 180), (200, 238), (242, 222)):
        pix = c.screen_pixel(x, y)
        assert _is_red(pix) or _is_green(pix), (x, y, pix)
    assert _is_black(c.screen_pixel(270, 180))
    v = int(c.send_line("PRINT PIXEL(258,180)").split()[0])
    r, g, b = (v >> 16) & 255, (v >> 8) & 255, v & 255
    assert r > 150 or g > 150, (v, r, g, b)
    assert not (r < 40 and g < 40 and b < 40)


def test_bad_arg_count_is_syntax_error(fresh_console):
    assert fresh_console.send_line("BOX 1,2,3") == "?SYNTAX ERROR"


def test_filled_box_interior(fresh_console):
    fresh_console.send_line("CLS")
    fresh_console.send_line("BOX 80,80,40,40,1,RGB(0,255,0),RGB(0,255,0)")
    assert _is_green(fresh_console.screen_pixel(100, 100))


def test_scene_matches_golden(fresh_console):
    golden = os.path.join(GOLDEN_DIR, "scene.png")
    assert os.path.isfile(golden), "run scripts/gen_golden.py to create the golden"
    for cmd in SCENE:
        fresh_console.send_line(cmd)
    ratio = fresh_console.image_diff_ratio(golden)
    assert ratio < 0.02, f"scene differs from golden by {ratio:.4%}"
