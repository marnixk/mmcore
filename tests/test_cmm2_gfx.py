"""Software CMM2 graphics: IMAGE, FRAMEBUFFER, BLIT buffers, PAGE SCROLL,
BOX pixel-ops, BITMAP, and TURTLE."""

import os

import pytest

from ihelp_util import dump_topic, open_ihelp, close_ihelp, scroll_all

# Timing guards are opt-in: QEMU timing under parallel load is noisy.
PERF_ENABLED = os.environ.get("MMCORE_PERF", "").strip().lower() in (
    "1",
    "true",
    "yes",
    "on",
)


def _is_red(rgb):
    r, g, b = rgb
    return r > 150 and g < 130 and b < 130


def _is_green(rgb):
    r, g, b = rgb
    return g > 150 and r < 130 and b < 130


def _is_white(rgb):
    r, g, b = rgb
    return r > 200 and g > 200 and b > 200


def _is_black(rgb):
    return all(c < 40 for c in rgb)


def _pixel(console, x, y):
    out = console.send_line(f"PRINT PIXEL({x},{y})")
    return int(out.split()[0])


def _pixel_page(console, x, y, page):
    out = console.send_line(f"PRINT PIXEL({x},{y},{page})")
    return int(out.split()[0])


def _rgb_is_red(v):
    r = (v >> 16) & 255
    g = (v >> 8) & 255
    b = v & 255
    return r > 150 and g < 130 and b < 130


def _rgb_is_green(v):
    r = (v >> 16) & 255
    g = (v >> 8) & 255
    b = v & 255
    return g > 150 and r < 130 and b < 130


def _rgb_is_white(v):
    r = (v >> 16) & 255
    g = (v >> 8) & 255
    b = v & 255
    return r > 200 and g > 200 and b > 200


def _rgb_is_blue(v):
    r = (v >> 16) & 255
    g = (v >> 8) & 255
    b = v & 255
    return b > 150 and r < 130 and g < 130


def _rgb_is_yellow(v):
    r = (v >> 16) & 255
    g = (v >> 8) & 255
    b = v & 255
    return r > 150 and g > 150 and b < 130


def _rgb_is_cyan(v):
    r = (v >> 16) & 255
    g = (v >> 8) & 255
    b = v & 255
    return g > 150 and b > 150 and r < 130


def _rgb_is_magenta(v):
    r = (v >> 16) & 255
    g = (v >> 8) & 255
    b = v & 255
    return r > 150 and b > 150 and g < 130


def _rgb_is_black(v):
    r = (v >> 16) & 255
    g = (v >> 8) & 255
    b = v & 255
    return r < 40 and g < 40 and b < 40


def test_image_resize_fast(fresh_console):
    c = fresh_console
    assert c.send_line("CLS") == ""
    assert c.send_line("BOX 0,0,10,10,1,RGB(255,0,0),RGB(255,0,0)") == ""
    assert c.send_line("IMAGE RESIZE_FAST 0,0,10,10,100,80,40,40") == ""
    assert _rgb_is_red(_pixel(c, 120, 100))
    assert _rgb_is_black(_pixel(c, 160, 140))


def test_image_rotate_fast_90(fresh_console):
    c = fresh_console
    c.send_line("CLS")
    c.send_line("BOX 10,10,20,20,1,RGB(0,255,0),RGB(0,255,0)")
    c.send_line("PIXEL 10,10,RGB(255,0,0)")
    assert c.send_line("IMAGE ROTATE_FAST 10,10,20,20,200,100,90") == ""
    assert _rgb_is_red(_pixel(c, 219, 100))
    assert _rgb_is_green(_pixel(c, 200, 100))


def test_blit_read_write_and_copy(fresh_console):
    c = fresh_console
    c.send_line("CLS")
    c.send_line("BOX 30,30,16,16,1,RGB(255,0,0),RGB(255,0,0)")
    assert c.send_line("BLIT READ #1,30,30,16,16") == ""
    assert c.send_line("BLIT WRITE #1,200,200,0") == ""
    assert _rgb_is_red(_pixel(c, 208, 208))
    assert c.send_line("BLIT 30,30,300,40,16,16") == ""
    assert _rgb_is_red(_pixel(c, 308, 48))
    assert c.send_line("BLIT CLOSE #1") == ""


def test_blit_transparent_copy_basic_clipped_and_paged(fresh_console):
    """Mode-4 BLIT skips black source pixels, clips both rects, works from a
    non-current source page and only dirties the destination it touched."""
    c = fresh_console
    assert c.send_line("CLS") == ""
    assert c.send_line("PAGE WRITE 1") == ""
    assert c.send_line("CLS") == ""
    assert c.send_line("BOX 0,0,8,8,1,RGB(255,0,0),RGB(255,0,0)") == ""
    assert c.send_line("BOX 2,2,2,2,1,0,0") == ""
    assert c.send_line("PIXEL 5,5,RGB(0,255,0)") == ""
    assert c.send_line("PIXEL 7,7,RGB(0,255,0)") == ""
    assert c.send_line("PAGE WRITE 0") == ""
    assert c.send_line("CLS") == ""
    assert c.send_line("BOX 10,10,8,8,1,RGB(0,0,255),RGB(0,0,255)") == ""
    assert c.send_line("BLIT 0,0,10,10,8,8,1,4") == ""
    assert _rgb_is_red(_pixel(c, 10, 10))
    assert _rgb_is_blue(_pixel(c, 12, 12))  # source hole: dest kept
    assert _rgb_is_blue(_pixel(c, 13, 13))
    assert _rgb_is_green(_pixel(c, 17, 17))
    assert _rgb_is_black(_pixel(c, 9, 10))  # outside dest rect
    assert _rgb_is_black(_pixel(c, 18, 18))

    # Destination partly off the left/top edge.
    assert c.send_line("CLS") == ""
    assert c.send_line("BLIT 0,0,-2,-3,8,8,1,4") == ""
    assert _rgb_is_black(_pixel(c, 0, 0))  # src (2,3) is a hole
    assert _rgb_is_red(_pixel(c, 0, 1))  # src (2,4)
    assert _rgb_is_red(_pixel(c, 2, 0))  # src (4,3)
    assert _rgb_is_green(_pixel(c, 5, 4))  # src (7,7)
    assert _rgb_is_black(_pixel(c, 6, 0))  # still outside the rect

    # Source partly off the left/top edge: those pixels are transparent.
    assert c.send_line("CLS") == ""
    assert c.send_line("BOX 20,20,8,8,1,RGB(0,0,255),RGB(0,0,255)") == ""
    assert c.send_line("BLIT -2,-2,20,20,8,8,1,4") == ""
    assert _rgb_is_blue(_pixel(c, 20, 20))
    assert _rgb_is_blue(_pixel(c, 21, 21))
    assert _rgb_is_red(_pixel(c, 22, 22))  # src (0,0)
    assert _rgb_is_red(_pixel(c, 22, 23))  # src (0,1)
    assert _rgb_is_blue(_pixel(c, 24, 24))  # src (2,2) hole
    assert _rgb_is_green(_pixel(c, 27, 27))

    # Copy from page 2 into page 1 while page 0 is displayed.
    assert c.send_line("PAGE WRITE 2") == ""
    assert c.send_line("CLS") == ""
    assert c.send_line("BOX 0,0,4,4,1,RGB(255,255,0),RGB(255,255,0)") == ""
    assert c.send_line("PAGE WRITE 1") == ""
    assert c.send_line("CLS") == ""
    assert c.send_line("BOX 40,40,4,4,1,RGB(0,255,0),RGB(0,255,0)") == ""
    assert c.send_line("BLIT 0,0,40,40,4,4,2,4") == ""
    assert _rgb_is_yellow(_pixel_page(c, 40, 40, 1))
    assert _rgb_is_black(_pixel_page(c, 40, 40, 0))  # display page untouched
    assert c.send_line("PAGE WRITE 0") == ""


def test_graphics_args_fast_path_equivalence(fresh_console):
    """The #1370 fast argument path returns the same values as mmb_expr() for
    literals, scalars, array elements, constants, expressions, FRAMEBUFFER and
    malformed input."""
    c = fresh_console
    assert c.send_line("CLS") == ""
    assert c.send_line("PAGE WRITE 1") == ""
    assert c.send_line("CLS") == ""
    assert c.send_line("BOX 0,0,4,4,1,RGB(255,0,0),RGB(255,0,0)") == ""
    assert c.send_line("PAGE WRITE 0") == ""
    assert c.send_line("CLS") == ""
    assert c.send_line("DIM A(7)") == ""
    for i, v in enumerate([0, 0, 10, 10, 4, 4, 1, 0]):
        assert c.send_line(f"A({i})={v}") == ""
    for name, v in (("X1", 0), ("Y1", 0), ("X2", 20), ("Y2", 20),
                    ("W", 4), ("H", 4), ("P", 1), ("ORI", 0)):
        assert c.send_line(f"{name}={v}") == ""
    assert c.send_line("I=0") == ""
    assert c.send_line("CONST C0=0") == ""

    # Scalar array element (fast path).
    assert c.send_line("BLIT A(0),A(1),A(2),A(3),A(4),A(5),A(6),A(7)") == ""
    assert _rgb_is_red(_pixel(c, 10, 10))
    # Full expressions and an expression array index (fallback).
    assert c.send_line("BLIT X1+0,Y1+0,X2+0,Y2+0,W+0,H+0,P+0,ORI+0") == ""
    assert _rgb_is_red(_pixel(c, 20, 20))
    assert c.send_line("BLIT A(I),A(I),30,10,A(I)+4,A(I)+4,A(I)+1,A(I)") == ""
    assert _rgb_is_red(_pixel(c, 30, 10))
    # CONST (fast path).
    assert c.send_line("BLIT C0,C0,40,10,4,4,1,C0") == ""
    assert _rgb_is_red(_pixel(c, 40, 10))

    # BOX also uses the shared parser.
    for name, v in (("BX", 100), ("BY", 100), ("BW", 4), ("BH", 4)):
        assert c.send_line(f"{name}={v}") == ""
    assert c.send_line("BOX BX,BY,BW,BH,1,RGB(0,255,0),RGB(0,255,0)") == ""
    assert _rgb_is_green(_pixel(c, 101, 101))

    # FRAMEBUFFER is still a keyword in the page slot.
    assert c.send_line("FRAMEBUFFER CREATE 1280,720") == ""
    assert c.send_line("FRAMEBUFFER WRITE") == ""
    assert c.send_line("CLS") == ""
    assert c.send_line("BOX 0,0,4,4,1,RGB(0,0,255),RGB(0,0,255)") == ""
    assert c.send_line("PAGE WRITE 0") == ""
    assert c.send_line("BLIT 0,0,50,10,4,4,FRAMEBUFFER") == ""
    assert _rgb_is_blue(_pixel(c, 50, 10))
    assert c.send_line("FRAMEBUFFER CLOSE") == ""

    # Errors match the expression parser's.
    assert c.send_line('BLIT "x",0,60,10,4,4,1,0') == "?TYPE MISMATCH"
    assert c.send_line("BLIT 0,0,60,10,4") == "?SYNTAX ERROR"
    assert c.send_line("BLIT 0,0,60,10,4,4,1,0+") == "?SYNTAX ERROR"


def test_blit_transparent_rotations(fresh_console):
    """Mode-4 BLIT with the X/Y flip bits keeps the previous mapping."""
    c = fresh_console
    assert c.send_line("CLS") == ""
    assert c.send_line("PAGE WRITE 1") == ""
    assert c.send_line("CLS") == ""
    assert c.send_line("PIXEL 0,0,RGB(255,0,0)") == ""
    assert c.send_line("PIXEL 1,0,RGB(0,255,0)") == ""
    assert c.send_line("PIXEL 2,0,RGB(0,0,255)") == ""
    assert c.send_line("PIXEL 0,1,RGB(255,255,0)") == ""
    assert c.send_line("PIXEL 1,1,RGB(0,0,0)") == ""
    assert c.send_line("PIXEL 2,1,RGB(255,0,255)") == ""
    assert c.send_line("PIXEL 0,2,RGB(0,255,255)") == ""
    assert c.send_line("PIXEL 1,2,RGB(255,255,255)") == ""
    assert c.send_line("PIXEL 2,2,RGB(0,255,0)") == ""
    assert c.send_line("PAGE WRITE 0") == ""
    assert c.send_line("CLS") == ""

    # ori 5: flip X.
    assert c.send_line("PIXEL 11,11,RGB(16,16,16)") == ""
    assert c.send_line("BLIT 0,0,10,10,3,3,1,5") == ""
    assert _rgb_is_blue(_pixel(c, 10, 10))
    assert _rgb_is_green(_pixel(c, 11, 10))
    assert _rgb_is_red(_pixel(c, 12, 10))
    assert _rgb_is_magenta(_pixel(c, 10, 11))
    assert _pixel(c, 11, 11) == 0x101010  # hole skipped
    assert _rgb_is_yellow(_pixel(c, 12, 11))
    assert _rgb_is_green(_pixel(c, 10, 12))
    assert _rgb_is_white(_pixel(c, 11, 12))
    assert _rgb_is_cyan(_pixel(c, 12, 12))

    # ori 6: flip Y.
    assert c.send_line("PIXEL 21,11,RGB(16,16,16)") == ""
    assert c.send_line("BLIT 0,0,20,10,3,3,1,6") == ""
    assert _rgb_is_cyan(_pixel(c, 20, 10))
    assert _rgb_is_white(_pixel(c, 21, 10))
    assert _rgb_is_green(_pixel(c, 22, 10))
    assert _rgb_is_yellow(_pixel(c, 20, 11))
    assert _pixel(c, 21, 11) == 0x101010
    assert _rgb_is_magenta(_pixel(c, 22, 11))
    assert _rgb_is_red(_pixel(c, 20, 12))
    assert _rgb_is_green(_pixel(c, 21, 12))
    assert _rgb_is_blue(_pixel(c, 22, 12))

    # ori 7: flip both.
    assert c.send_line("PIXEL 31,11,RGB(16,16,16)") == ""
    assert c.send_line("BLIT 0,0,30,10,3,3,1,7") == ""
    assert _rgb_is_green(_pixel(c, 30, 10))
    assert _rgb_is_white(_pixel(c, 31, 10))
    assert _rgb_is_cyan(_pixel(c, 32, 10))
    assert _rgb_is_magenta(_pixel(c, 30, 11))
    assert _pixel(c, 31, 11) == 0x101010
    assert _rgb_is_yellow(_pixel(c, 32, 11))
    assert _rgb_is_blue(_pixel(c, 30, 12))
    assert _rgb_is_green(_pixel(c, 31, 12))
    assert _rgb_is_red(_pixel(c, 32, 12))


def test_blit_transparent_overlapping_same_page(fresh_console):
    """An overlapping same-page transparent BLIT still reads the snapshot."""
    c = fresh_console
    assert c.send_line("CLS") == ""
    assert c.send_line("BOX 18,10,10,1,1,RGB(0,0,255),RGB(0,0,255)") == ""
    assert c.send_line("BOX 10,10,10,1,1,RGB(255,0,0),RGB(255,0,0)") == ""
    assert c.send_line("PIXEL 12,10,RGB(0,0,0)") == ""
    assert c.send_line("PIXEL 18,10,RGB(0,255,0)") == ""
    assert c.send_line("BLIT 10,10,18,10,10,1,0,4") == ""
    assert _rgb_is_red(_pixel(c, 18, 10))  # src (0,0)
    assert _rgb_is_red(_pixel(c, 19, 10))  # src (1,0), overlaps old dest
    assert _rgb_is_blue(_pixel(c, 20, 10))  # src (2,0) hole: dest kept
    assert _rgb_is_green(_pixel(c, 26, 10))  # src (8,0) read from snapshot
    assert _rgb_is_red(_pixel(c, 27, 10))  # src (9,0)
    assert _rgb_is_red(_pixel(c, 10, 10))  # source left edge untouched
    assert _rgb_is_black(_pixel(c, 12, 10))


def test_framebuffer_window(fresh_console):
    c = fresh_console
    assert c.send_line("FRAMEBUFFER CREATE 1280,720") == ""
    assert c.send_line("FRAMEBUFFER WRITE") == ""
    c.send_line("CLS")
    c.send_line("BOX 40,40,20,20,1,RGB(255,0,0),RGB(255,0,0)")
    assert _rgb_is_red(_pixel(c, 50, 50))
    assert c.send_line("FRAMEBUFFER WINDOW 0,0,0") == ""
    assert c.send_line("PAGE WRITE 0") == ""
    assert _rgb_is_red(_pixel(c, 50, 50))
    assert _is_red(c.screen_pixel(50, 50))
    assert c.send_line("FRAMEBUFFER CLOSE") == ""


def test_page_scroll_and_xor(fresh_console):
    c = fresh_console
    c.send_line("CLS")
    c.send_line("PIXEL 100,100,RGB(255,0,0)")
    assert c.send_line("PAGE SCROLL 0,10,0") == ""
    assert _rgb_is_red(_pixel(c, 110, 100))
    assert _rgb_is_black(_pixel(c, 100, 100))
    c.send_line("BOX 50,50,20,20,1,RGB(255,255,255),RGB(255,255,255)")
    assert c.send_line("BOX XOR_PIXELS 50,50,20,20,RGB(255,255,255)") == ""
    assert _rgb_is_black(_pixel(c, 60, 60))


def test_page_scroll_all_directions_and_wrap(fresh_console):
    """#1317: positive x moves content right, positive y moves it up (the
    documented CMM2/PAGE semantics), and both axes wrap at the page edge."""
    c = fresh_console
    assert c.send_line("MODE 8,16") == ""
    assert c.send_line("PAGE WRITE 1") == ""

    c.send_line("CLS")
    c.send_line("PIXEL 100,100,RGB(255,0,0)")
    assert c.send_line("PAGE SCROLL 1,10,0") == ""
    assert _rgb_is_red(_pixel_page(c, 110, 100, 1))
    assert _rgb_is_black(_pixel_page(c, 100, 100, 1))

    c.send_line("CLS")
    c.send_line("PIXEL 100,100,RGB(255,0,0)")
    assert c.send_line("PAGE SCROLL 1,-10,0") == ""
    assert _rgb_is_red(_pixel_page(c, 90, 100, 1))

    c.send_line("CLS")
    c.send_line("PIXEL 100,100,RGB(255,0,0)")
    assert c.send_line("PAGE SCROLL 1,0,10") == ""
    assert _rgb_is_red(_pixel_page(c, 100, 90, 1))

    c.send_line("CLS")
    c.send_line("PIXEL 100,100,RGB(255,0,0)")
    assert c.send_line("PAGE SCROLL 1,0,-10") == ""
    assert _rgb_is_red(_pixel_page(c, 100, 110, 1))

    # Wrap across the right and both vertical edges.
    c.send_line("CLS")
    c.send_line("PIXEL 630,100,RGB(255,0,0)")
    assert c.send_line("PAGE SCROLL 1,10,0") == ""
    assert _rgb_is_red(_pixel_page(c, 0, 100, 1))

    c.send_line("CLS")
    c.send_line("PIXEL 100,5,RGB(255,0,0)")
    assert c.send_line("PAGE SCROLL 1,0,10") == ""
    assert _rgb_is_red(_pixel_page(c, 100, 475, 1))

    c.send_line("CLS")
    c.send_line("PIXEL 100,475,RGB(255,0,0)")
    assert c.send_line("PAGE SCROLL 1,0,-10") == ""
    assert _rgb_is_red(_pixel_page(c, 100, 5, 1))

    # Both axes move the same pixel.
    c.send_line("CLS")
    c.send_line("PIXEL 100,100,RGB(255,0,0)")
    assert c.send_line("PAGE SCROLL 1,5,3") == ""
    assert _rgb_is_red(_pixel_page(c, 105, 97, 1))
    assert _rgb_is_black(_pixel_page(c, 100, 100, 1))
    assert c.send_line("PAGE WRITE 0") == ""


def test_page_scroll_fill_and_gap(fresh_console):
    """#1317: an explicit colour fills only the uncovered strip, -1 leaves the
    old pixels, and omitting the colour wraps."""
    c = fresh_console
    assert c.send_line("MODE 8,16") == ""
    assert c.send_line("PAGE WRITE 1") == ""

    c.send_line("CLS")
    c.send_line("PIXEL 100,100,RGB(255,0,0)")
    assert c.send_line("PAGE SCROLL 1,10,0,RGB(0,0,255)") == ""
    assert _rgb_is_red(_pixel_page(c, 110, 100, 1))
    assert _rgb_is_blue(_pixel_page(c, 5, 100, 1))
    assert _rgb_is_black(_pixel_page(c, 100, 100, 1))

    c.send_line("CLS")
    c.send_line("PIXEL 100,100,RGB(255,0,0)")
    assert c.send_line("PAGE SCROLL 1,0,10,RGB(0,255,0)") == ""
    assert _rgb_is_red(_pixel_page(c, 100, 90, 1))
    assert _rgb_is_green(_pixel_page(c, 100, 475, 1))

    c.send_line("CLS")
    c.send_line("PIXEL 5,100,RGB(0,0,255)")
    c.send_line("PIXEL 100,100,RGB(255,0,0)")
    assert c.send_line("PAGE SCROLL 1,10,0,-1") == ""
    assert _rgb_is_blue(_pixel_page(c, 5, 100, 1))
    assert _rgb_is_red(_pixel_page(c, 110, 100, 1))
    assert c.send_line("PAGE WRITE 0") == ""


def test_page_scroll_non_visible_page(fresh_console):
    """#1317: scrolling an off-screen page must not touch the display page."""
    c = fresh_console
    assert c.send_line("MODE 8,16") == ""
    assert c.send_line("PAGE WRITE 1") == ""
    c.send_line("CLS")
    c.send_line("PIXEL 100,100,RGB(255,0,0)")
    assert c.send_line("PAGE SCROLL 1,10,0") == ""
    assert _rgb_is_red(_pixel_page(c, 110, 100, 1))
    assert _rgb_is_black(_pixel_page(c, 110, 100, 0))
    assert c.send_line("PAGE WRITE 0") == ""


def test_page_scroll_visible_page_presents(fresh_console):
    """#1317: a visible-page scroll presents the scrolled pixels."""
    c = fresh_console
    assert c.send_line("MODE 8,16") == ""
    assert c.send_line("PAGE WRITE 0") == ""
    assert c.send_line("PAGE DISPLAY 0") == ""
    assert c.send_line("CLS") == ""
    assert c.send_line("PIXEL 100,100,RGB(255,0,0)") == ""
    assert c.send_line("PAGE SCROLL 0,10,0") == ""
    assert _rgb_is_red(_pixel(c, 110, 100))
    assert _rgb_is_black(_pixel(c, 100, 100))
    r, g, b = c.screen_pixel(110, 100)
    assert r > 150 and g < 130 and b < 130, (r, g, b)
    r, g, b = c.screen_pixel(100, 100)
    assert r < 40 and g < 40 and b < 40, (r, g, b)


def test_page_scroll_all_bit_depths(fresh_console):
    """#1317: the memmove scroll keeps working at every supported depth."""
    c = fresh_console
    for bits in (8, 12, 16, 32):
        assert c.send_line(f"MODE 8,{bits}") == ""
        assert c.send_line("PAGE WRITE 1") == ""
        assert c.send_line("CLS") == ""
        assert c.send_line("PIXEL 50,50,RGB(255,255,255)") == ""
        assert c.send_line("PAGE SCROLL 1,5,3") == ""
        assert _rgb_is_white(_pixel_page(c, 55, 47, 1))
        assert _rgb_is_black(_pixel_page(c, 50, 50, 1))

        # Fill the right and top gaps for a left/down move.
        assert c.send_line("CLS") == ""
        assert c.send_line("PIXEL 50,50,RGB(255,255,255)") == ""
        assert c.send_line("PAGE SCROLL 1,-5,-3,RGB(255,0,0)") == ""
        assert _rgb_is_white(_pixel_page(c, 45, 53, 1))
        assert _rgb_is_red(_pixel_page(c, 639, 300, 1))
        assert _rgb_is_red(_pixel_page(c, 300, 0, 1))
    assert c.send_line("PAGE WRITE 0") == ""


@pytest.mark.skipif(
    not PERF_ENABLED, reason="set MMCORE_PERF=1 to run timing guards"
)
def test_page_scroll_perf_offscreen_wrap(fresh_console):
    """#1317 opt-in guard: a wrapped off-screen scroll stays far inside the
    50 Hz frame budget (20 ms) at both full-screen modes."""
    c = fresh_console
    for mode, limit_ms in ((19, 10.0), (16, 20.0)):
        assert c.send_line(f"MODE {mode},16", timeout=30) == ""
        assert c.send_line("PAGE WRITE 1") == ""
        assert c.send_line("CLS", timeout=60) == ""
        assert c.send_line("NEW") == ""
        lines = [
            "N=100",
            "T=TIMER",
            "FOR I=1 TO N",
            "PAGE SCROLL 1,1,0",
            "NEXT I",
            "PRINT TIMER-T",
        ]
        for i, line in enumerate(lines, 1):
            assert c.send_line(f"{i * 10} {line}") == ""
        out = c.send_line("RUN", timeout=120)
        per_ms = int(out.split()[0]) / 100
        assert per_ms < limit_ms, (mode, per_ms)
    assert c.send_line("PAGE WRITE 0") == ""



def test_turtle_and_bitmap(fresh_console):
    c = fresh_console
    assert c.send_line("TURTLE RESET") == ""
    assert c.send_line("TURTLE HEADING 90") == ""
    assert c.send_line("TURTLE FORWARD 40") == ""
    w = int(c.send_line("PRINT MM.HRES").split()[0])
    h = int(c.send_line("PRINT MM.VRES").split()[0])
    assert _rgb_is_white(_pixel(c, w // 2 + 20, h // 2))
    c.send_line("CLS")
    assert c.send_line("BITMAP 10,10,&HFF,8,1,1,RGB(255,0,0)") == ""
    assert _rgb_is_red(_pixel(c, 10, 10))
    assert _rgb_is_red(_pixel(c, 17, 10))
    assert _rgb_is_black(_pixel(c, 10, 11))


def test_help_cmm2_gfx_topics(console):
    listing = scroll_all(console, open_ihelp(console, "INDEX"))
    for name in ("IMAGE", "FRAMEBUFFER", "TURTLE", "GUI"):
        assert name in listing, name
    assert "CAMERA" not in listing
    close_ihelp(console)
    img = dump_topic(console, "IMAGE")
    assert "RESIZE_FAST" in img
    assert "ROTATE" in img
    fb = dump_topic(console, "FRAMEBUFFER")
    assert "CREATE" in fb
    assert "WINDOW" in fb
    bl = dump_topic(console, "BLIT")
    assert "READ" in bl
    tu = dump_topic(console, "TURTLE")
    assert "FORWARD" in tu
    cmm2 = dump_topic(console, "CMM2")
    assert "IMAGE" in cmm2
    assert "FRAMEBUFFER" in cmm2
    assert "TURTLE" in cmm2
    sprite = dump_topic(console, "SPRITE")
    assert "SPRITE SCROLL" in sprite


def test_rgb_alpha_and_page_copy_b(fresh_console):
    """RGB(...,t) still packs, but t no longer affects the screen; PAGE COPY B
    skip-black onto page 1 is a plain copy (#1051)."""
    c = fresh_console
    assert c.send_line("PRINT HEX$(RGB(RED))") == "AA0000"
    packed = int(c.send_line("PRINT RGB(1,2,3,15)"))
    assert (packed >> 24) & 0xFF == 0x1F
    assert packed & 0xFFFFFF == 0x010203
    assert c.send_line("MODE 7,12") == ""
    assert c.send_line("PAGE WRITE 2") == ""
    assert c.send_line("CLS") == ""
    assert c.send_line("BOX 40,40,40,40,1,RGB(220,0,0),RGB(220,0,0)") == ""
    assert c.send_line("PAGE WRITE 1") == ""
    assert c.send_line("CLS") == ""
    # Alpha 15 is ignored: the black pixel is stored, not flagged transparent.
    assert c.send_line("PIXEL 10,10,RGB(0,0,0,15)") == ""
    assert int(c.send_line("PRINT PIXEL(10,10,1)")) == 0
    assert c.send_line("PAGE COPY 2, 1, B") == ""
    red = int(c.send_line("PRINT PIXEL(50,50,1)").split()[0])
    gap = int(c.send_line("PRINT PIXEL(10,40,1)").split()[0])
    assert ((red >> 16) & 255) > 150
    assert (gap & 0xFFFFFF) == 0
    assert c.send_line("PAGE WRITE 0") == ""


def test_span_fast_path_visible_matches_hidden(fresh_console):
    """#1082: 1px spans drawn to the visible page match the hidden page.

    LINE, BOX fill and CIRCLE fill take the resolved-span fast path. On the
    visible page that also dirty-tracks and set_pixels; a hidden page only
    writes the page buffer. Both must store byte-identical pixels.
    """
    c = fresh_console
    assert c.send_line("MODE 8,16") == ""
    scene = [
        "LINE 20,30,300,30,RGB(255,0,0)",
        "LINE 40,50,220,180,RGB(0,255,0)",
        "BOX 60,200,120,60,1,RGB(0,0,255),RGB(255,255,0)",
        "CIRCLE 450,150,50,1,RGB(255,0,255),RGB(0,255,255)",
    ]
    samples = [
        (20, 30), (160, 30), (300, 30),
        (40, 50), (220, 180),
        (60, 200), (179, 259), (100, 220),
        (450, 150), (500, 150), (400, 400),
    ]
    values = {}
    for page in (0, 1):
        c.send_line(f"PAGE WRITE {page}")
        c.send_line("CLS")
        for line in scene:
            assert c.send_line(line) == ""
        values[page] = [
            int(c.send_line(f"PRINT PIXEL({x},{y},{page})").split()[0])
            for x, y in samples
        ]
    c.send_line("PAGE WRITE 0")
    assert values[0] == values[1], (values[0], values[1])
    assert sum(1 for v in values[0] if v & 0xFFFFFF) >= 8, values[0]


def test_page1_is_not_composited_over_display(fresh_console):
    """#1051: page-1 pixels, with or without AFLAG, never appear over page 0."""
    c = fresh_console
    assert c.send_line("MODE 8,16") == ""
    assert c.send_line("PAGE WRITE 0") == ""
    assert c.send_line("PAGE DISPLAY 0") == ""
    assert c.send_line("CLS RGB(0,0,80)") == ""
    assert c.send_line("PAGE WRITE 1") == ""
    assert c.send_line("CLS") == ""
    assert c.send_line("PIXEL 100,80,RGB(255,200,0)") == ""
    assert c.send_line("PAGE WRITE 0") == ""
    assert c.send_line("PAGE DISPLAY 0") == ""
    # Page 0 keeps its own pixels; the page-1 orange does not composite over it.
    r, g, b = c.screen_pixel(100, 80)
    assert r < 40 and g < 40 and b > 60, (r, g, b)

    # Displaying page 1 shows the real pixel instead.
    assert c.send_line("PAGE DISPLAY 1") == ""
    r, g, b = c.screen_pixel(100, 80)
    assert r > 200 and g > 150 and b < 80, (r, g, b)

    # A partial-alpha colour is stored opaque (t is ignored).
    assert c.send_line("PAGE WRITE 1") == ""
    assert c.send_line("CLS") == ""
    assert c.send_line("PIXEL 50,50,RGB(255,0,0,8)") == ""
    assert int(c.send_line("PRINT PIXEL(50,50,1)")) & 0xFFFFFF == 0xFF0000
    assert c.send_line("PAGE DISPLAY 1") == ""
    r, g, b = c.screen_pixel(50, 50)
    assert r > 200 and g < 80 and b < 80, (r, g, b)
    assert c.send_line("PAGE DISPLAY 0") == ""
    assert c.send_line("PAGE WRITE 0") == ""


@pytest.mark.skipif(
    not PERF_ENABLED, reason="set MMCORE_PERF=1 to run timing guards"
)
def test_blit_fast_path_perf(fresh_console):
    """#1370 opt-in guard: variable arguments stay near literal cost and a
    mode-4 transparent 32x32 BLIT stays near the opaque one."""
    c = fresh_console
    assert c.send_line("PAGE WRITE 1") == ""
    assert c.send_line("CLS") == ""
    assert c.send_line("BOX 0,0,32,32,1,RGB(255,0,0),RGB(255,0,0)") == ""
    assert c.send_line("BOX 4,4,8,8,1,0,0") == ""
    # Copy into a hidden page so the benchmark measures the copy, not the
    # per-frame present caused by blitting onto the display page.
    assert c.send_line("PAGE WRITE 2") == ""
    assert c.send_line("CLS") == ""
    assert c.send_line("NEW") == ""
    lines = [
        "X1=0",
        "Y1=0",
        "X2=100",
        "Y2=100",
        "W=32",
        "H=32",
        "P=1",
        "ORI=0",
        "N=10000",
        "T=TIMER",
        "FOR I=1 TO N",
        "BLIT X1,Y1,X2,Y2,W,H,P,ORI",
        "NEXT I",
        "A=(TIMER-T)*1000/N",
        "T=TIMER",
        "FOR I=1 TO N",
        "BLIT 0,0,100,100,32,32,1,0",
        "NEXT I",
        "B=(TIMER-T)*1000/N",
        "ORI=4",
        "T=TIMER",
        "FOR I=1 TO N",
        "BLIT X1,Y1,X2,Y2,W,H,P,ORI",
        "NEXT I",
        "C=(TIMER-T)*1000/N",
        "T=TIMER",
        "FOR I=1 TO N",
        "NEXT I",
        "D=(TIMER-T)*1000/N",
        "PRINT A;\" \";B;\" \";C;\" \";D",
    ]
    for i, line in enumerate(lines, 10):
        assert c.send_line(f"{i * 10} {line}") == ""
    out = c.send_line("RUN", timeout=180)
    var_us, lit_us, trans_us, empty_us = (float(v) for v in out.split()[:4])
    var_us -= empty_us
    lit_us -= empty_us
    trans_us -= empty_us
    print(
        f"\nBLIT 32x32 us: var={var_us} literal={lit_us} "
        f"transparent={trans_us} (empty loop {empty_us})"
    )
    assert var_us < lit_us * 3 + 0.5, (var_us, lit_us)
    assert trans_us < lit_us * 4 + 0.5, (trans_us, lit_us)
