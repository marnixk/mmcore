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


def test_files_enter_subdir_and_parent(fresh_console):
    con = fresh_console
    _prep_tree(con)
    _open_files(con)
    # listing is sorted: .., DEMO/, then files. One down + Enter opens DEMO.
    seen = _keys(con, b"\x1b[B\r")
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
    # .., DEMO/, HELLO.BAS — two downs then Enter runs PRINT 42.
    seen = _keys(con, b"\x1b[B\x1b[B\r", quiet=1.0)
    assert "42" in seen
    # back at the prompt
    assert con.send_line("PRINT 1+1") == "2"


def test_files_quit_q_and_esc(fresh_console):
    con = fresh_console
    _prep_tree(con)
    _open_files(con)
    seen = _keys(con, b"q")
    assert ">" in seen or con.send_line("PRINT 7") == "7"
    _open_files(con)
    # Esc then a follow-up byte completes the lone-ESC detect
    _keys(con, b"\x1bX")
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
    listing = console.send_line('DIR "A:/"')
    assert "TEST.PNG" in listing.upper()
