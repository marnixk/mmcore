"""Software CMM2 graphics: IMAGE, FRAMEBUFFER, BLIT buffers, PAGE SCROLL,
BOX pixel-ops, BITMAP, and TURTLE."""

from ihelp_util import dump_topic, open_ihelp, close_ihelp, scroll_all


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


def test_framebuffer_window(fresh_console):
    c = fresh_console
    assert c.send_line("FRAMEBUFFER CREATE 640,480") == ""
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


def test_turtle_and_bitmap(fresh_console):
    c = fresh_console
    assert c.send_line("TURTLE RESET") == ""
    assert c.send_line("TURTLE HEADING 90") == ""
    assert c.send_line("TURTLE FORWARD 40") == ""
    assert _rgb_is_white(_pixel(c, 340, 240))
    c.send_line("CLS")
    assert c.send_line("BITMAP 10,10,&HFF,8,1,1,RGB(255,0,0)") == ""
    assert _rgb_is_red(_pixel(c, 10, 10))
    assert _rgb_is_red(_pixel(c, 17, 10))
    assert _rgb_is_black(_pixel(c, 10, 11))


def test_help_cmm2_gfx_topics(console):
    listing = scroll_all(console, open_ihelp(console))
    for name in ("IMAGE", "FRAMEBUFFER", "TURTLE"):
        assert name in listing, name
    assert "GUI" not in listing
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
