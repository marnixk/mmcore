"""FILES ANSI viewer: colours, scrolling, and the 80x25 toggle.

The seeded ``A:/tests/TEST.ANS`` paints eight-cell blocks at 1-based rows 2,
4, 31 and 33 (0-based 1, 3, 30, 32) with red, blue, magenta and green
backgrounds so the tests can locate them as pixels.
"""

from harness import MMBasicConsole


def _open_files(con: MMBasicConsole) -> str:
    assert con._ser is not None
    con.drain(quiet=0.15)
    con._ser.sendall(b"FILES\r")
    return con.drain(quiet=0.8).decode(errors="replace")


def _keys(con: MMBasicConsole, data: bytes, quiet: float = 0.45) -> str:
    assert con._ser is not None
    con._ser.sendall(data)
    return con.drain(quiet=quiet).decode(errors="replace")


def _open_ansi(con: MMBasicConsole) -> str:
    assert con.send_line('CHDIR "A:/tests"') == ""
    seen = _open_files(con)
    for _ in range(16):
        if "SEL=TEST.ANS" in seen:
            break
        seen = _keys(con, b"\x1b[B", quiet=0.25)
    assert "SEL=TEST.ANS" in seen
    seen = _keys(con, b"\r", quiet=1.0)
    assert "[FILES] ANSI TEST.ANS" in seen, seen
    return seen


def _is_colour(rgb, want, tol=60):
    r, g, b = rgb
    return (
        abs(r - want[0]) <= tol
        and abs(g - want[1]) <= tol
        and abs(b - want[2]) <= tol
        and max(r, g, b) > 60
    )


def test_files_ansi_preview_colours_and_exit(fresh_console):
    con = fresh_console
    _open_ansi(con)
    # Red block (ESC[41m) at 0-based row 1, col 2 -> cell (2, 1).
    assert _is_colour(con.screen_pixel(20, 20), (170, 0, 0)), con.screen_pixel(20, 20)
    # Blue block (ESC[44m) at 0-based row 3, col 2.
    assert _is_colour(con.screen_pixel(20, 52), (0, 0, 170)), con.screen_pixel(20, 52)
    # Far corner stays blank black.
    w, h = con.screen_size()
    corner = con.screen_pixel(w - 2, h - 2)
    assert max(corner) < 40, corner
    # Esc returns to the file manager.
    seen = _keys(con, b"\x1b", quiet=0.8)
    assert "SEL=" in seen, seen
    _keys(con, b"q")


def test_files_ansi_toggle_80x25(fresh_console):
    con = fresh_console
    _open_ansi(con)
    assert con.screen_size() == (1280, 720)
    _keys(con, b"f", quiet=1.0)
    assert con.screen_size() == (640, 400)
    # 80x25: red block still at row 1, col 2 (x=20, y=20).
    assert _is_colour(con.screen_pixel(20, 20), (170, 0, 0)), con.screen_pixel(20, 20)
    _keys(con, b"f", quiet=1.0)
    assert con.screen_size() == (1280, 720)
    _keys(con, b"\x1b", quiet=0.8)
    _keys(con, b"q")


def test_files_ansi_scroll_in_80x25(fresh_console):
    con = fresh_console
    _open_ansi(con)
    _keys(con, b"f", quiet=1.0)
    assert con.screen_size() == (640, 400)
    # Row 33 (0-based 32) is below the 25-row screen and must not be visible.
    assert not _is_colour(con.screen_pixel(20, 388), (0, 170, 0)), con.screen_pixel(20, 388)
    # Scroll to the bottom (max top = 33 - 25 = 8) to bring it into view.
    for _ in range(10):
        _keys(con, b"\x1b[B", quiet=0.2)
    assert _is_colour(con.screen_pixel(20, 388), (0, 170, 0)), con.screen_pixel(20, 388)
    assert _is_colour(con.screen_pixel(20, 356), (170, 0, 170)), con.screen_pixel(20, 356)
    _keys(con, b"\x1b", quiet=0.8)
    _keys(con, b"q")
