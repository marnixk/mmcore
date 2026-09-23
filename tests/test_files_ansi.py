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


def _open_ansi(con: MMBasicConsole, name: str = "TEST.ANS") -> str:
    assert con.send_line('CHDIR "A:/tests"') == ""
    seen = _open_files(con)
    for _ in range(16):
        if f"SEL={name}" in seen:
            break
        seen = _keys(con, b"\x1b[B", quiet=0.25)
    assert f"SEL={name}" in seen
    seen = _keys(con, b"\r", quiet=1.0)
    assert f"[FILES] ANSI {name}" in seen, seen
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


def test_files_ansi_page_scroll(fresh_console):
    """PgDn/PgUp jump a page rather than a single row."""
    con = fresh_console
    _open_ansi(con)
    _keys(con, b"f", quiet=1.0)
    assert con.screen_size() == (640, 400)
    assert not _is_colour(con.screen_pixel(20, 388), (0, 170, 0))
    _keys(con, b"\x1b[6~", quiet=0.4)  # PageDown clamps to the last page.
    assert _is_colour(con.screen_pixel(20, 388), (0, 170, 0)), con.screen_pixel(20, 388)
    _keys(con, b"\x1b[5~", quiet=0.4)  # PageUp back to the top.
    assert not _is_colour(con.screen_pixel(20, 388), (0, 170, 0)), con.screen_pixel(20, 388)
    _keys(con, b"\x1b", quiet=0.8)
    _keys(con, b"q")


def test_files_ansi_wraps_at_80_columns(fresh_console):
    """A long line with no newline must autowrap at 80 cols, not the MODE width."""
    con = fresh_console
    _open_ansi(con, "WRAP.ANS")
    # Cyan run. Row 0 starts cyan and row 1 also starts cyan because the
    # 81st-85th cells wrapped down to the next row.
    assert _is_colour(con.screen_pixel(20, 4), (0, 170, 170)), con.screen_pixel(20, 4)
    assert _is_colour(con.screen_pixel(20, 20), (0, 170, 170)), con.screen_pixel(20, 20)
    # Past the 80-column edge on row 0 is blank, even at 1280x720 (160 cols).
    assert max(con.screen_pixel(660, 4)) < 40, con.screen_pixel(660, 4)
    _keys(con, b"\x1b", quiet=0.8)
    _keys(con, b"q")


def _editor(con: MMBasicConsole, data: bytes, quiet: float = 0.4) -> str:
    assert con._ser is not None
    con._ser.sendall(data)
    return con.drain(quiet=quiet).decode(errors="replace")


def test_editor_saved_ans_renders_in_files_preview(fresh_console):
    """Art saved by the ANSI editor is valid ANSI the FILES viewer can page."""
    con = fresh_console
    con.drain(quiet=0.15)
    con._ser.sendall(b'ANSI EDIT "A:/tests/EDITOR.ANS"\r')
    con.drain(quiet=0.9)
    # Red block at 0-based row 1, col 2 (the cell TEST.ANS uses).
    _editor(con, b"\x1b[C\x1b[C\x1b[B")
    _editor(con, bytes([1]) + b"1")
    _editor(con, b" ")
    _editor(con, bytes([1]) + b"s", quiet=0.7)
    _editor(con, bytes([1]) + b"x", quiet=0.6)

    _open_ansi(con, "EDITOR.ANS")
    assert _is_colour(con.screen_pixel(20, 20), (170, 0, 0)), con.screen_pixel(20, 20)
    _keys(con, b"\x1b", quiet=0.8)
    _keys(con, b"q")
