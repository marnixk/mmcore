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
