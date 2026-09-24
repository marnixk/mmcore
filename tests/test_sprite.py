"""CMM2-style sprite blitstore: restore saved background, dirty only the sprite rect."""


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


def _is_red(rgb):
    r, g, b = rgb
    return r > 150 and g < 130 and b < 130


def _is_green(rgb):
    r, g, b = rgb
    return g > 150 and r < 130 and b < 130


def _is_blue(rgb):
    r, g, b = rgb
    return b > 150 and r < 130 and g < 130


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


def _read_shown_red_pair(c):
    c.send_line("CLS")
    c.send_line("BOX 0,0,80,80,1,RGB(0,255,0),RGB(0,255,0)")
    c.send_line("BOX 0,0,8,8,1,RGB(255,0,0),RGB(255,0,0)")
    c.send_line("SPRITE READ 1,0,0,8,8")
    c.send_line("SPRITE READ 2,0,0,8,8")
    c.send_line("BOX 0,0,8,8,1,RGB(0,255,0),RGB(0,255,0)")
    c.send_line("SPRITE SHOW 1,16,16,1")
    c.send_line("SPRITE SHOW 2,40,40,1")
    assert _rgb_is_red(_pixel(c, 18, 18))
    assert _rgb_is_red(_pixel(c, 42, 42))


def test_sprite_close_all_restores_every_sprite(fresh_console):
    """#587: the documented `SPRITE CLOSE ALL` frees every sprite and restores
    their backgrounds; it must also not treat ALL as a sprite number."""
    c = fresh_console
    _read_shown_red_pair(c)
    assert c.send_line("SPRITE CLOSE ALL") == ""
    assert _rgb_is_green(_pixel(c, 18, 18))
    assert _rgb_is_green(_pixel(c, 42, 42))
    assert c.send_line("PRINT 6*7") == "42"


def test_sprite_close_bare_restores_background(fresh_console):
    """A bare `SPRITE CLOSE` (reset all) restores before freeing."""
    c = fresh_console
    _read_shown_red_pair(c)
    assert c.send_line("SPRITE CLOSE") == ""
    assert _rgb_is_green(_pixel(c, 18, 18))
    assert _rgb_is_green(_pixel(c, 42, 42))


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


def test_sprite_editor_commands_removed(fresh_console):
    """#619: SPRITE EDIT/SHEET/FONT no longer open a TUI; they fail like any
    other unknown SPRITE subcommand."""
    c = fresh_console
    assert c.send_line('SPRITE EDIT "ED1.PNG"') == "?SYNTAX ERROR"
    assert c.send_line('SPRITE SHEET "ED3.PNG", 8, 4, 4') == "?SYNTAX ERROR"
    assert c.send_line('SPRITE FONT "SPRFONT"') == "?SYNTAX ERROR"
