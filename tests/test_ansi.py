"""TheDraw-class ANSI art editor: .ANS round-trip and .TDF font stamping."""

import re

from harness import MMBasicConsole


def _plain(s: str) -> str:
    return re.sub(r"\x1b\[[0-9;?]*[A-Za-z]", "", s)


def _open_editor(con: MMBasicConsole, command: str) -> str:
    """Launch the ANSI editor over raw serial (send_line hangs in full-screen apps)."""
    assert con._ser is not None
    con.drain(quiet=0.15)
    con._ser.sendall(command.encode() + b"\r")
    return _plain(con.drain(quiet=0.9).decode(errors="replace"))


def _keys(con: MMBasicConsole, data: bytes, quiet: float = 0.35) -> str:
    assert con._ser is not None
    con._ser.sendall(data)
    return _plain(con.drain(quiet=quiet).decode(errors="replace"))


def _quit_editor(con: MMBasicConsole) -> str:
    return _keys(con, bytes([1]) + b"x", quiet=0.5)


def _is_colour(rgb, want, tol=70):
    r, g, b = rgb
    return (
        abs(r - want[0]) <= tol
        and abs(g - want[1]) <= tol
        and abs(b - want[2]) <= tol
        and max(rgb) > 60
    )


def test_ansi_font_info(console):
    out = _plain(console.send_line('ANSI FONT "A:/fonts/tdf/STANDARD.TDF"'))
    assert "TDF Standard type=1 spacing=1 glyphs=94" in out, out


def test_ansi_editor_stamps_tdf_text(console):
    _open_editor(
        console,
        'ANSI EDIT "A:/tests/ANSI_TDF.ANS", "A:/fonts/tdf/STANDARD.TDF"',
    )
    _keys(console, bytes([1]) + b"t")  # Alt+T: text mode
    _keys(console, b"T")
    # STANDARD.TDF "T": row 0 is " _____ " and row 1 starts with "|".
    assert _is_colour(console.screen_pixel(12, 13), (255, 255, 255)), console.screen_pixel(12, 13)
    assert _is_colour(console.screen_pixel(4, 20), (255, 255, 255)), console.screen_pixel(4, 20)
    # The glyph has a transparent top-left corner.
    assert max(console.screen_pixel(4, 4)) < 40, console.screen_pixel(4, 4)
    _quit_editor(console)


def test_ansi_editor_draw_save_reload(console):
    _open_editor(console, 'ANSI EDIT "A:/tests/ANSI_EDIT.ANS"')
    # Move to cell (2, 2), pick red (1) and paint a solid block.
    _keys(console, b"\x1b[C\x1b[C\x1b[B\x1b[B")
    _keys(console, bytes([1]) + b"1")
    _keys(console, b" ")
    assert _is_colour(console.screen_pixel(20, 40), (170, 0, 0)), console.screen_pixel(20, 40)
    _keys(console, bytes([1]) + b"s", quiet=0.7)
    _quit_editor(console)

    _open_editor(console, 'ANSI EDIT "A:/tests/ANSI_EDIT.ANS"')
    assert _is_colour(console.screen_pixel(20, 40), (170, 0, 0)), console.screen_pixel(20, 40)
    _quit_editor(console)


def test_ansi_sauce_appended_and_ignored(console):
    _open_editor(console, 'ANSI EDIT "A:/tests/ANSI_SAUCE.ANS"')
    _keys(console, b"\x1b[C\x1b[C\x1b[B\x1b[B")
    _keys(console, bytes([1]) + b"1")
    _keys(console, b" ")
    _keys(console, bytes([1]) + b"s", quiet=0.7)
    _quit_editor(console)

    assert console.send_line('ANSI SAUCE "A:/tests/ANSI_SAUCE.ANS"') == ""
    _open_editor(console, 'ANSI EDIT "A:/tests/ANSI_SAUCE.ANS"')
    assert _is_colour(console.screen_pixel(20, 40), (170, 0, 0)), console.screen_pixel(20, 40)
    _quit_editor(console)


def test_ansi_editor_loads_existing_ans(console):
    _open_editor(console, 'ANSI EDIT "A:/tests/TEST.ANS"')
    # Same red/blue blocks the FILES preview test locates.
    assert _is_colour(console.screen_pixel(20, 20), (170, 0, 0)), console.screen_pixel(20, 20)
    assert _is_colour(console.screen_pixel(20, 52), (0, 0, 170)), console.screen_pixel(20, 52)
    _quit_editor(console)


def test_ansi_editor_flood_fill(console):
    _open_editor(console, 'ANSI EDIT "A:/tests/ANSI_FILL.ANS"')
    # Paint a red block at (0, 0), then step right so the cursor is off it.
    _keys(console, bytes([1]) + b"1")
    _keys(console, b" ")
    _keys(console, b"\x1b[C")
    # Green, then flood the blank region: every cell except the red one fills.
    _keys(console, bytes([1]) + b"2")
    _keys(console, bytes([1]) + b"f")  # Alt+F: flood
    _keys(console, b"\x1b[C")  # move the cursor off the seed cell
    assert _is_colour(console.screen_pixel(20, 8), (0, 170, 0)), console.screen_pixel(20, 8)
    assert _is_colour(console.screen_pixel(4, 8), (170, 0, 0)), console.screen_pixel(4, 8)
    _quit_editor(console)
