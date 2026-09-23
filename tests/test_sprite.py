"""CMM2-style sprite blitstore: restore saved background, dirty only the sprite rect."""

import re


def _plain(s):
    return re.sub(r"\x1b\[[0-9;?]*[A-Za-z]", "", s)


def _open_editor(con, command):
    """Launch the sprite editor over raw serial (send_line hangs in full-screen apps)."""
    assert con._ser is not None
    con.drain(quiet=0.15)
    con._ser.sendall(command.encode() + b"\r")
    return _plain(con.drain(quiet=0.8).decode(errors="replace"))


def _keys(con, data, quiet=0.4):
    assert con._ser is not None
    con._ser.sendall(data)
    return _plain(con.drain(quiet=quiet).decode(errors="replace"))


def _quit_editor(con):
    return _keys(con, bytes([1]) + b"x", quiet=0.5)


def _pixel(console, x, y):
    out = console.send_line(f"PRINT PIXEL({x},{y})")
    return int(out.split()[0])


def _rgb_is_red(v):
    r, g, b = (v >> 16) & 255, (v >> 8) & 255, v & 255
    return r > 150 and g < 130 and b < 130


def _rgb_is_green(v):
    r, g, b = (v >> 16) & 255, (v >> 8) & 255, v & 255
    return g > 150 and r < 130 and b < 130


def _rgb_is_blue(v):
    r, g, b = (v >> 16) & 255, (v >> 8) & 255, v & 255
    return b > 150 and r < 130 and g < 130


def _rgb_is_black(v):
    r, g, b = (v >> 16) & 255, (v >> 8) & 255, v & 255
    return r < 40 and g < 40 and b < 40


def _is_red(rgb):
    r, g, b = rgb
    return r > 150 and g < 130 and b < 130


def _is_green(rgb):
    r, g, b = rgb
    return g > 150 and r < 130 and b < 130


def _is_blue(rgb):
    r, g, b = rgb
    return b > 150 and r < 130 and g < 130


def _is_black(rgb):
    return all(c < 40 for c in rgb)


def test_sprite_show_move_restores_background(fresh_console):
    c = fresh_console
    assert c.send_line("CLS") == ""
    assert c.send_line("BOX 0,0,200,200,1,RGB(0,255,0),RGB(0,255,0)") == ""
    assert c.send_line("BOX 10,10,16,16,1,RGB(255,0,0),RGB(255,0,0)") == ""
    assert c.send_line("SPRITE READ 1,10,10,16,16") == ""
    assert c.send_line("BOX 10,10,16,16,1,RGB(0,255,0),RGB(0,255,0)") == ""
    assert c.send_line("SPRITE SHOW 1,20,20,1") == ""
    assert _rgb_is_red(_pixel(c, 24, 24))
    assert _is_red(c.screen_pixel(24, 24))
    assert c.send_line("SPRITE MOVE 1,80,20") == ""
    assert _rgb_is_green(_pixel(c, 24, 24))
    assert _is_green(c.screen_pixel(24, 24))
    assert _rgb_is_red(_pixel(c, 84, 24))
    assert _is_red(c.screen_pixel(84, 24))
    assert _rgb_is_green(_pixel(c, 199, 199))
    assert _is_green(c.screen_pixel(199, 199))


def test_sprite_hide_restores_background(fresh_console):
    c = fresh_console
    c.send_line("CLS")
    c.send_line("BOX 0,0,80,80,1,RGB(0,0,255),RGB(0,0,255)")
    c.send_line("BOX 0,0,8,8,1,RGB(255,0,0),RGB(255,0,0)")
    c.send_line("SPRITE READ 1,0,0,8,8")
    c.send_line("BOX 0,0,8,8,1,RGB(0,0,255),RGB(0,0,255)")
    c.send_line("SPRITE SHOW 1,40,40,1")
    assert _rgb_is_red(_pixel(c, 43, 43))
    assert c.send_line("SPRITE HIDE 1") == ""
    assert _rgb_is_blue(_pixel(c, 43, 43))
    assert _is_blue(c.screen_pixel(43, 43))


def test_two_sprites_move_restore_without_trails(fresh_console):
    c = fresh_console
    c.send_line("CLS")
    c.send_line("BOX 0,0,400,120,1,RGB(0,255,0),RGB(0,255,0)")
    c.send_line("BOX 0,0,12,12,1,RGB(255,0,0),RGB(255,0,0)")
    c.send_line("SPRITE READ 1,0,0,12,12")
    c.send_line("BOX 0,0,12,12,1,RGB(0,0,255),RGB(0,0,255)")
    c.send_line("SPRITE READ 2,0,0,12,12")
    c.send_line("BOX 0,0,12,12,1,RGB(0,255,0),RGB(0,255,0)")
    c.send_line("SPRITE SHOW 1,20,20,1")
    c.send_line("SPRITE SHOW 2,200,20,1")
    c.send_line("SPRITE NEXT 1,60,20")
    c.send_line("SPRITE NEXT 2,240,20")
    assert c.send_line("SPRITE MOVE") == ""
    assert _rgb_is_green(_pixel(c, 24, 24))
    assert _is_green(c.screen_pixel(24, 24))
    assert _rgb_is_red(_pixel(c, 64, 24))
    assert _is_red(c.screen_pixel(64, 24))
    assert _rgb_is_green(_pixel(c, 204, 24))
    assert _is_green(c.screen_pixel(204, 24))
    assert _rgb_is_blue(_pixel(c, 244, 24))
    assert _is_blue(c.screen_pixel(244, 24))
    assert _rgb_is_green(_pixel(c, 10, 10))
    assert _is_green(c.screen_pixel(10, 10))


def test_two_sprites_independent_move(fresh_console):
    c = fresh_console
    c.send_line("CLS")
    c.send_line("BOX 0,0,400,80,1,RGB(0,255,0),RGB(0,255,0)")
    c.send_line("BOX 0,0,10,10,1,RGB(255,0,0),RGB(255,0,0)")
    c.send_line("SPRITE READ 1,0,0,10,10")
    c.send_line("BOX 0,0,10,10,1,RGB(0,0,255),RGB(0,0,255)")
    c.send_line("SPRITE READ 2,0,0,10,10")
    c.send_line("BOX 0,0,10,10,1,RGB(0,255,0),RGB(0,255,0)")
    c.send_line("SPRITE SHOW 1,16,16,1")
    c.send_line("SPRITE SHOW 2,180,16,1")
    assert c.send_line("SPRITE MOVE 1,50,16") == ""
    assert c.send_line("SPRITE MOVE 2,220,16") == ""
    assert _rgb_is_green(_pixel(c, 18, 18))
    assert _is_green(c.screen_pixel(18, 18))
    assert _rgb_is_red(_pixel(c, 52, 18))
    assert _is_red(c.screen_pixel(52, 18))
    assert _rgb_is_green(_pixel(c, 182, 18))
    assert _is_green(c.screen_pixel(182, 18))
    assert _rgb_is_blue(_pixel(c, 222, 18))
    assert _is_blue(c.screen_pixel(222, 18))


def _read_red_blue_sprites(c):
    c.send_line("CLS")
    c.send_line("BOX 0,0,80,80,1,RGB(0,255,0),RGB(0,255,0)")
    c.send_line("BOX 0,0,12,12,1,RGB(255,0,0),RGB(255,0,0)")
    c.send_line("SPRITE READ 1,0,0,12,12")
    c.send_line("BOX 0,0,12,12,1,RGB(0,0,255),RGB(0,0,255)")
    c.send_line("SPRITE READ 2,0,0,12,12")
    c.send_line("BOX 0,0,80,80,1,RGB(0,255,0),RGB(0,255,0)")


def test_sprite_layer_beats_show_order(fresh_console):
    c = fresh_console
    _read_red_blue_sprites(c)
    c.send_line("SPRITE SHOW 1,20,20,8")
    c.send_line("SPRITE SHOW 2,24,24,1")
    assert _rgb_is_red(_pixel(c, 26, 26))
    assert _rgb_is_blue(_pixel(c, 33, 33))


def test_sprite_lower_layer_behind_higher(fresh_console):
    c = fresh_console
    _read_red_blue_sprites(c)
    c.send_line("SPRITE SHOW 1,20,20,0")
    c.send_line("SPRITE SHOW 2,24,24,1")
    assert _rgb_is_blue(_pixel(c, 26, 26))
    assert _rgb_is_red(_pixel(c, 21, 21))


def test_sprite_equal_layer_is_stable_by_show_order(fresh_console):
    c = fresh_console
    _read_red_blue_sprites(c)
    c.send_line("SPRITE SHOW 1,20,20,1")
    c.send_line("SPRITE SHOW 2,24,24,1")
    assert _rgb_is_blue(_pixel(c, 26, 26))
    # Re-showing the first sprite must not reorder equal layers.
    c.send_line("SPRITE SHOW 1,20,20,1")
    assert _rgb_is_blue(_pixel(c, 26, 26))
    assert _rgb_is_red(_pixel(c, 21, 21))


def test_sprite_layer_change_redraws_without_stale(fresh_console):
    c = fresh_console
    _read_red_blue_sprites(c)
    c.send_line("SPRITE SHOW 1,20,20,8")
    c.send_line("SPRITE SHOW 2,24,24,1")
    assert _rgb_is_red(_pixel(c, 26, 26))
    c.send_line("SPRITE SHOW 1,20,20,0")
    assert _rgb_is_blue(_pixel(c, 26, 26))
    assert _rgb_is_red(_pixel(c, 21, 21))
    assert _rgb_is_green(_pixel(c, 40, 40))
    assert _rgb_is_blue(_pixel(c, 31, 31))


def test_sprite_hide_and_close_keep_order_consistent(fresh_console):
    c = fresh_console
    _read_red_blue_sprites(c)
    c.send_line("SPRITE SHOW 1,20,20,2")
    c.send_line("SPRITE SHOW 2,24,24,1")
    assert _rgb_is_red(_pixel(c, 26, 26))
    c.send_line("SPRITE HIDE 1")
    assert _rgb_is_blue(_pixel(c, 26, 26))
    assert _rgb_is_green(_pixel(c, 21, 21))
    c.send_line("SPRITE CLOSE 2")
    assert _rgb_is_green(_pixel(c, 26, 26))
    assert _rgb_is_green(_pixel(c, 30, 30))


def test_sprite_transparent_pixels_keep_background(fresh_console):
    c = fresh_console
    c.send_line("CLS")
    c.send_line("BOX 0,0,40,40,1,RGB(0,255,0),RGB(0,255,0)")
    c.send_line("BOX 0,0,8,8,1,RGB(255,0,0),RGB(255,0,0)")
    c.send_line("PIXEL 0,0,RGB(0,0,0)")
    c.send_line("SPRITE READ 1,0,0,8,8")
    c.send_line("BOX 0,0,8,8,1,RGB(0,255,0),RGB(0,255,0)")
    c.send_line("SPRITE SHOW 1,16,16,1")
    assert _rgb_is_green(_pixel(c, 16, 16))
    assert _is_green(c.screen_pixel(16, 16))
    assert _rgb_is_red(_pixel(c, 17, 16))
    assert _is_red(c.screen_pixel(17, 16))


def test_sprite_editor_draws_and_saves_png(fresh_console):
    c = fresh_console
    seen = _open_editor(c, 'SPRITE EDIT "ED1.PNG"')
    assert "SPRITE EDIT" in seen.upper()
    assert "ED1.PNG" in seen.upper()
    # Colour 4 (red) then set the pixel under the cursor (top-left).
    _keys(c, b"4 ")
    _keys(c, b"s", quiet=0.5)
    _quit_editor(c)
    assert c.send_line('SPRITE LOADPNG 1, "ED1.PNG"') == ""
    assert c.send_line("SPRITE SHOW 1, 0, 0, 1") == ""
    assert _rgb_is_red(_pixel(c, 0, 0))
    assert _is_red(c.screen_pixel(0, 0))
    c.send_line("SPRITE CLOSE 1")


def test_sprite_editor_flip_horizontal(fresh_console):
    c = fresh_console
    _open_editor(c, 'SPRITE EDIT "ED2.PNG"')
    _keys(c, b"4 ")
    _keys(c, b"f")
    _keys(c, b"s", quiet=0.5)
    _quit_editor(c)
    assert c.send_line('SPRITE LOADPNG 1, "ED2.PNG"') == ""
    assert c.send_line("SPRITE SHOW 1, 0, 0, 1") == ""
    assert _rgb_is_black(_pixel(c, 0, 0))
    assert _rgb_is_red(_pixel(c, 7, 0))
    c.send_line("SPRITE CLOSE 1")


def test_sprite_editor_ctrl_z_undo(fresh_console):
    """#532: Ctrl+Z undoes the last SPRITE EDIT pixel change."""
    c = fresh_console
    _open_editor(c, 'SPRITE EDIT "ED_UNDO.PNG"')
    _keys(c, b"4 ")          # red at the cursor (top-left)
    _keys(c, b"\x1a")        # Ctrl+Z restores transparent
    _keys(c, b"s", quiet=0.5)
    _quit_editor(c)
    assert c.send_line('SPRITE LOADPNG 1, "ED_UNDO.PNG"') == ""
    assert c.send_line("SPRITE SHOW 1, 0, 0, 1") == ""
    assert _rgb_is_black(_pixel(c, 0, 0))
    c.send_line("SPRITE CLOSE 1")


def test_sprite_sheet_scrubs_frames(fresh_console):
    c = fresh_console
    _open_editor(c, 'SPRITE SHEET "ED3.PNG", 8, 4, 4')
    _keys(c, b"n")  # scrub to frame 1 -> sheet offset (8,0)
    _keys(c, b"4 ")
    _keys(c, b"s", quiet=0.5)
    _quit_editor(c)
    assert c.send_line('SPRITE LOADPNG 1, "ED3.PNG"') == ""
    assert c.send_line("SPRITE SHOW 1, 0, 0, 1") == ""
    assert _rgb_is_black(_pixel(c, 0, 0))
    assert _rgb_is_red(_pixel(c, 8, 0))
    c.send_line("SPRITE CLOSE 1")


def _write_bas(console, path, lines):
    assert console.send_line(f'OPEN "{path}" FOR OUTPUT AS #1') == ""
    for line in lines:
        esc = line.replace('"', '""')
        assert console.send_line(f'PRINT #1, "{esc}"') == ""
    assert console.send_line("CLOSE #1") == ""


def test_sprite_font_editor_saves_loadable_json(fresh_console):
    c = fresh_console
    seen = _open_editor(c, 'SPRITE FONT "SPRFONT"')
    assert "SPRITE FONT" in seen.upper()
    _keys(c, b"4 ")
    _keys(c, b"s", quiet=0.5)
    _quit_editor(c)
    _write_bas(
        c,
        "FONTEDIT.BAS",
        [
            '#INCLUDE "A:/fonts/fonts.inc"',
            "DIM f AS FontDescription",
            'f = fontLoad("SPRFONT")',
            "PRINT f.source",
            "PRINT f.charWidth",
            "PRINT f.charHeight",
            "PRINT f.charsPerRow",
            "PRINT LEN(f.charset)",
        ],
    )
    out = c.send_line('RUN "FONTEDIT.BAS"')
    lines = [ln.strip() for ln in out.replace("\r", "\n").split("\n") if ln.strip()]
    assert lines == ["SPRFONT.png", "8", "8", "40", "59"], out

