"""Turbo-style MMBasic editor TUI: menus, file ops, tabs, quit."""

import re

from harness import MMBasicConsole


def _plain(s: str) -> str:
    return re.sub(r"\x1b\[[0-9;?]*[A-Za-z]", "", s)


def _edit(con, path: str) -> str:
    con.drain(quiet=0.1)
    con._ser.sendall(f'EDIT "{path}"\r'.encode())
    return _plain(con.drain(quiet=0.8).decode(errors="replace"))


def _keys(con, data: bytes, quiet: float = 0.5) -> str:
    con._ser.sendall(data)
    return _plain(con.drain(quiet=quiet).decode(errors="replace"))


def _quit(con) -> str:
    return _keys(con, bytes([24]), quiet=0.5)


def test_editor_menu_labels_on_serial(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        seen = _edit(con, "HI.BAS")
        assert "File" in seen
        assert "Run" in seen
        assert "F2" in seen
        _quit(con)
    finally:
        con.stop()


def test_editor_ocr_file_label(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        _edit(con, "HI.BAS")
        # Sample the menu bar (y~8) and editor pane (y~80). QEMU dumps can
        # include a black margin, so try a few x positions.
        bar = [con.screen_pixel(x, 8) for x in (40, 80, 160, 320)]
        pane = [con.screen_pixel(x, 80) for x in (40, 80, 160, 320)]
        assert any(r > 100 and g > 100 and b > 100 for r, g, b in bar), bar
        assert any(b > r + 20 and b > 40 for r, g, b in pane), pane
        _quit(con)
    finally:
        con.stop()


def test_editor_esc_menu_open_close(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        _edit(con, "MENU.BAS")
        opened = _keys(con, b"\x1bf")
        assert "Open" in opened or "Save" in opened or "Quit" in opened
        _keys(con, b"\x1b[B")
        closed = _keys(con, b"\x1b\x1b")
        assert "File" in closed
        _quit(con)
        assert con.send_line("PRINT 1") == "1"
    finally:
        con.stop()


def test_editor_save_as_and_open(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        _edit(con, "TMP.BAS")
        _keys(con, b"PRINT 6*7")
        _keys(con, b"\x1bfa", quiet=0.5)
        # Save As prefills the current path; wipe it, then type the new name.
        _keys(con, b"\x7f\x7f\x7f\x7f\x7f\x7f\x7f\x7f\x7f\x7fSAVED.BAS\r", quiet=0.7)
        _quit(con)
        listing = con.send_line("DIR")
        assert "SAVED.BAS" in listing
        result = con.send_line('RUN "SAVED.BAS"')
        assert "42" in result
    finally:
        con.stop()


def test_editor_two_tabs_and_switch(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        _edit(con, "AAA.BAS")
        _keys(con, b"PRINT 11")
        _keys(con, bytes([15]), quiet=0.4)
        _keys(con, b"\x1bfo", quiet=0.5)
        seen = _keys(con, b"BBB.BAS\r", quiet=0.6)
        assert "AAA" in seen and "BBB" in seen
        _keys(con, b"PRINT 22")
        _keys(con, bytes([15]), quiet=0.4)
        back = _keys(con, b"\x1b1", quiet=0.5)
        assert "AAA" in back
        _quit(con)
        listing = con.send_line("DIR")
        assert "AAA.BAS" in listing
        assert "BBB.BAS" in listing
        assert "11" in con.send_line('RUN "AAA.BAS"')
        assert "22" in con.send_line('RUN "BBB.BAS"')
    finally:
        con.stop()


def test_editor_quit_returns_prompt(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        _edit(con, "Q.BAS")
        _quit(con)
        assert con.send_line("PRINT 3+4") == "7"
    finally:
        con.stop()
