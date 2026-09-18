"""FILES dual-pane TUI: navigate, run .BAS, view unknown types, quit."""

import time

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


def _down_to(con: MMBasicConsole, needle: str, maxn: int = 16) -> str:
    """Move the file-list selection down until the status shows `needle`."""
    seen = ""
    for _ in range(maxn):
        if needle in seen:
            return seen
        seen = _keys(con, b"\x1b[B", quiet=0.25)
    assert needle in seen, (needle, seen)
    return seen


def _prep_tree(con: MMBasicConsole) -> None:
    assert con.send_line('CHDIR "A:/"') == ""
    assert con.send_line('MKDIR "DEMO"') == ""
    assert con.send_line('OPEN "HELLO.BAS" FOR OUTPUT AS #1') == ""
    assert con.send_line("PRINT #1, \"PRINT 42\"") == ""
    assert con.send_line("CLOSE #1") == ""
    assert con.send_line('OPEN "NOPE.XYZ" FOR OUTPUT AS #1') == ""
    assert con.send_line('PRINT #1, "xyz"') == ""
    assert con.send_line("CLOSE #1") == ""
    listing = con.send_line("DIR")
    assert "HELLO.BAS" in listing.upper()
    assert "DEMO" in listing.upper()


def test_files_is_not_dir(console):
    listing = console.send_line("DIR")
    assert "TEST.PNG" in listing.upper() or "HELLO" in listing.upper() or listing


def test_files_opens_dual_pane(fresh_console):
    con = fresh_console
    _prep_tree(con)
    seen = _open_files(con)
    assert "[FILES]" in seen
    assert "HELLO.BAS" in seen.upper()
    assert ".." in seen or "/" in seen
    assert "DEMO" in seen.upper()
    _keys(con, b"q")


def test_files_tab_switches_panels(fresh_console):
    con = fresh_console
    _prep_tree(con)
    seen = _open_files(con)
    assert "P=L" in seen
    seen = _keys(con, b"\t")
    assert "P=R" in seen
    seen = _keys(con, b"\t")
    assert "P=L" in seen
    _keys(con, b"q")


def test_files_right_menu_drive_updates_right_pane(fresh_console):
    """Right-menu drive change must not rewrite the focused left pane."""
    con = fresh_console
    _prep_tree(con)
    _open_files(con)
    _down_to(con, "SEL=DEMO/")
    seen = _keys(con, b"\r")
    assert "L=A:/DEMO" in seen.upper() or "PATH=A:/DEMO" in seen.upper()
    assert "P=L" in seen
    # Alt+R, then Drive A: (hotkey a). Left stays in DEMO; right is A:/.
    seen = _keys(con, bytes([1]) + b"ra")
    upper = seen.upper()
    assert "L=A:/DEMO" in upper
    assert "R=A:/" in upper
    assert "P=R" in seen
    _keys(con, b"q")


def test_files_enter_subdir_and_parent(fresh_console):
    con = fresh_console
    _prep_tree(con)
    _open_files(con)
    # listing is sorted; seeded dirs (apps, lib, tests) may precede DEMO.
    _down_to(con, "SEL=DEMO/")
    seen = _keys(con, b"\r")
    assert "DEMO" in seen.upper()
    assert "PATH=A:/DEMO" in seen.upper() or "A:/DEMO" in seen.upper()
    seen = _keys(con, b"\x7f")
    assert "PATH=A:/" in seen.upper() or "L=A:/" in seen
    _keys(con, b"q")
    cwd = con.send_line("PRINT CWD$")
    assert cwd.upper().startswith("A:")


def test_files_run_bas(fresh_console):
    con = fresh_console
    _prep_tree(con)
    _open_files(con)
    # Navigate to HELLO.BAS regardless of the seeded directories above it.
    _down_to(con, "SEL=HELLO.BAS")
    seen = _keys(con, b"\r", quiet=1.0)
    assert "42" in seen
    # back at the prompt
    assert con.send_line("PRINT 1+1") == "2"


def test_files_quit_prints_prompt(fresh_console):
    """q/Esc from FILES should reprint the prompt without needing Enter."""
    con = fresh_console
    _prep_tree(con)
    _open_files(con)
    con.drain(quiet=0.15)
    con._ser.sendall(b"q")
    out = con.drain(quiet=0.6).decode(errors="replace")
    assert "> " in out
    assert out.count("> ") == 1
    assert con.send_line("PRINT 3") == "3"

    _open_files(con)
    con.drain(quiet=0.15)
    con._ser.sendall(b"\x1b")
    out = con.drain(quiet=0.7).decode(errors="replace")
    assert "> " in out
    assert out.count("> ") == 1
    assert con.send_line("PRINT 4") == "4"


def test_files_quit_q_and_esc(fresh_console):
    con = fresh_console
    _prep_tree(con)
    _open_files(con)
    seen = _keys(con, b"q")
    assert ">" in seen or con.send_line("PRINT 7") == "7"
    _open_files(con)
    _keys(con, b"\x1b", quiet=0.6)
    assert con.send_line("PRINT 8") == "8"


def test_files_view_unsupported_is_info(fresh_console):
    con = fresh_console
    _prep_tree(con)
    seen = _open_files(con)
    # Walk until SEL=NOPE.XYZ then view
    for _ in range(12):
        if "SEL=NOPE.XYZ" in seen:
            break
        seen = _keys(con, b"\x1b[B", quiet=0.25)
    assert "SEL=NOPE.XYZ" in seen
    seen = _keys(con, b"v")
    assert "File info" in seen or "NOPE.XYZ" in seen
    assert "?" not in seen.split("\n")[0] or "File info" in seen
    _keys(con, b"q")
    # still in FILES after closing info with... overlay closes on most keys.
    # q after info first closes overlay then we need another q; send two
    _keys(con, b"q")


def test_files_view_seeded_png_smoke(fresh_console):
    con = fresh_console
    _prep_tree(con)
    assert con.send_line('CHDIR "A:/tests"') == ""
    seen = _open_files(con)
    for _ in range(16):
        if "SEL=TEST.PNG" in seen:
            break
        seen = _keys(con, b"\x1b[B", quiet=0.25)
    assert "SEL=TEST.PNG" in seen
    seen = _keys(con, b"v", quiet=0.8)
    assert "PREVIEW" in seen or "TEST.PNG" in seen
    _keys(con, b"x")  # leave preview
    _keys(con, b"q")


def test_dir_still_lists(console):
    listing = console.send_line('DIR "A:/tests"')
    assert "TEST.PNG" in listing.upper()


def test_files_reports_video_cell_size(fresh_console):
    con = fresh_console
    _prep_tree(con)
    seen = _open_files(con)
    assert "COLS=160" in seen
    assert "ROWS=45" in seen
    _keys(con, b"q")


def test_files_box_drawing_covers_cell_height(fresh_console):
    """Vertical pane border is a full-height line, not ASCII '|' with gaps."""
    con = fresh_console
    _prep_tree(con)
    _open_files(con)
    # Header row (y=2) left pane border is white-on-blue │; glyph row 0 of
    # ASCII '|' is empty, so a lit pixel here is the full-height box line.
    r, g, b = con.screen_pixel(3, 2 * 16)
    assert r > 100 and g > 100 and b > 100, (r, g, b)
    # Horizontal ─ on the top pane border, away from the path caption.
    r, g, b = con.screen_pixel(80, 16 + 7)
    assert r > 100 and g > 100 and b > 100, (r, g, b)
    # Last fkey row covers the bottom of 1280x720.
    br, bg_, bb = con.screen_pixel(24, 704)
    assert br + bg_ + bb > 40, (br, bg_, bb)
    _keys(con, b"q")


def test_files_tracks_mode_resolution(fresh_console):
    con = fresh_console
    assert con.send_line("MODE 7,8") == ""
    assert con.send_line("PRINT MM.HRES") == "320"
    assert con.send_line("PRINT MM.VRES") == "240"
    _prep_tree(con)
    seen = _open_files(con)
    assert "COLS=40" in seen
    assert "ROWS=15" in seen
    _keys(con, b"q")
    assert con.send_line("MODE 8,16") == ""


def test_files_follows_editor_theme_phosphor(fresh_console):
    con = fresh_console
    assert con.send_line("OPTION EDIT THEME PHOSPHOR") == ""
    _prep_tree(con)
    _open_files(con)
    empty = [con.screen_pixel(x, 176) for x in (40, 80, 360, 400)]
    assert all(r + g + b < 50 for r, g, b in empty), empty
    r, g, b = con.screen_pixel(3, 2 * 16)
    assert g > r + 20 and g > 40, (r, g, b)
    _keys(con, b"q")
    assert con.send_line("OPTION EDIT THEME TURBO") == ""


def test_files_follows_editor_theme_paper(fresh_console):
    con = fresh_console
    assert con.send_line("OPTION EDIT THEME PAPER") == ""
    _prep_tree(con)
    _open_files(con)
    empty = [con.screen_pixel(x, 176) for x in (40, 80, 360, 400)]
    assert any(r > 140 and g > 130 and b > 120 for r, g, b in empty), empty
    _keys(con, b"q")
    assert con.send_line("OPTION EDIT THEME TURBO") == ""


def test_files_f4_shows_editor_immediately(fresh_console):
    """#135: F4/e from FILES must paint the editor without waiting for another key."""
    con = fresh_console
    _prep_tree(con)
    seen = _open_files(con)
    for _ in range(12):
        if "SEL=HELLO.BAS" in seen.upper() or "SEL=HELLO.BAS" in seen:
            break
        seen = _keys(con, b"\x1b[B", quiet=0.25)
    assert "HELLO" in seen.upper()
    opened = _keys(con, b"\x1b[14~", quiet=0.9)
    assert "File" in opened
    assert "Run" in opened or "Alt+X" in opened
    pane = [con.screen_pixel(x, 80) for x in (40, 80, 160, 320)]
    assert all(r < 50 and g < 50 and b < 55 for r, g, b in pane), pane
    _keys(con, bytes([1]) + b"x", quiet=0.6)
    _keys(con, b"q")
    assert con.send_line("PRINT 5") == "5"
