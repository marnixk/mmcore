"""Turbo-style MMBasic editor TUI: menus, file ops, tabs, quit."""

from harness import MMBasicConsole


def _edit(con, path: str) -> str:
    con.drain(quiet=0.1)
    con._ser.sendall(f'EDIT "{path}"\r'.encode())
    return con.drain(quiet=0.8).decode(errors="replace")


def _keys(con, data: bytes, quiet: float = 0.5) -> str:
    con._ser.sendall(data)
    return con.drain(quiet=quiet).decode(errors="replace")


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
        text = con.ocr_screen(crop="640x48+0+0", threshold=50)
        compact = "".join(ch for ch in text.upper() if ch.isalnum())
        assert "FILE" in compact or "File" in text
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
        out = _quit(con)
        assert ">" in out or out == ""
        prompt = con.send_line("PRINT 1")
        assert prompt == "1"
    finally:
        con.stop()


def test_editor_save_as_and_open(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        _edit(con, "TMP.BAS")
        _keys(con, b'PRINT 6*7')
        # Esc+F then A = Save As
        _keys(con, b"\x1bfa", quiet=0.5)
        _keys(con, b"SAVED.BAS\r", quiet=0.6)
        _quit(con)
        listing = con.send_line("DIR")
        assert "SAVED.BAS" in listing
        _edit(con, "SAVED.BAS")
        seen = con.drain(quiet=0.3).decode(errors="replace")
        # already in editor from _edit
        assert "SAVED.BAS" in seen or "PRINT" in seen or "File" in seen
        _quit(con)
        result = con.send_line('RUN "SAVED.BAS"')
        assert "42" in result
    finally:
        con.stop()


def test_editor_two_tabs_and_switch(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        _edit(con, "AAA.BAS")
        _keys(con, b'PRINT 11')
        _keys(con, bytes([15]), quiet=0.4)  # Ctrl+O save
        # Esc+F then O = Open
        _keys(con, b"\x1bfo", quiet=0.5)
        seen = _keys(con, b"BBB.BAS\r", quiet=0.6)
        assert "AAA" in seen and "BBB" in seen
        _keys(con, b'PRINT 22')
        _keys(con, bytes([15]), quiet=0.4)
        # switch back with Esc+1
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
