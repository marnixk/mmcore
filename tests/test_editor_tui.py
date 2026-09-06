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
        # Status bar is the last text row (~y 464 on 640x480 / 8x16).
        # The editor pane is deep blue.
        bar = [con.screen_pixel(x, 464) for x in (8, 40, 80, 200)]
        pane = [con.screen_pixel(x, 80) for x in (40, 80, 160, 320)]
        assert any(r > 100 and g > 100 and b > 100 for r, g, b in bar), bar
        assert any(b > r + 20 and b > 40 for r, g, b in pane), pane
        # Full-height box vertical at the left of the text pane (row 3, glyph y=0).
        r, g, b = con.screen_pixel(3, 3 * 16)
        assert r > 100 and g > 100 and b > 100, (r, g, b)
        _quit(con)
    finally:
        con.stop()


def test_editor_alt_f_opens_file_menu(kernel_image):
    """USB Left-Alt+key is SOH then the letter (Circle cooked keymap drops Alt)."""
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        _edit(con, "MENU.BAS")
        opened = _keys(con, bytes([1]) + b"f")
        assert "Open" in opened or "Save" in opened or "Quit" in opened
        _keys(con, b"\x1b[B")
        closed = _keys(con, b"\x1b\x1b")
        assert "File" in closed
        _quit(con)
        assert con.send_line("PRINT 1") == "1"
    finally:
        con.stop()


def test_editor_alt_x_quits(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        _edit(con, "ALTX.BAS")
        _keys(con, bytes([1]) + b"x")
        assert con.send_line("PRINT 9") == "9"
    finally:
        con.stop()


def test_editor_esc_menu_open_close(kernel_image):
    """Serial Meta fallback: ESC+letter still opens the same menus."""
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
        _keys(con, bytes([1]) + b"fa", quiet=0.5)
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
        _keys(con, bytes([1]) + b"fo", quiet=0.5)
        seen = _keys(con, b"BBB.BAS\r", quiet=0.6)
        assert "AAA" in seen and "BBB" in seen
        _keys(con, b"PRINT 22")
        _keys(con, bytes([15]), quiet=0.4)
        back = _keys(con, bytes([1]) + b"1", quiet=0.5)
        assert "AAA" in back
        _quit(con)
        listing = con.send_line("DIR")
        assert "AAA.BAS" in listing
        assert "BBB.BAS" in listing
        assert "11" in con.send_line('RUN "AAA.BAS"')
        assert "22" in con.send_line('RUN "BBB.BAS"')
    finally:
        con.stop()


def test_editor_up_from_shorter_line_clamps_column(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        _edit(con, "CLAMP.BAS")
        _keys(con, b"HI\rHELLO!!!")
        _keys(con, b"\x1b[A")
        _keys(con, b"X")
        _keys(con, bytes([15]), quiet=0.4)
        _quit(con)
        assert con.send_line('OPEN "CLAMP.BAS" FOR INPUT AS #1') == ""
        assert con.send_line("LINE INPUT #1, A$") == ""
        assert con.send_line("PRINT A$") == "HIX"
        assert con.send_line("LINE INPUT #1, B$") == ""
        assert con.send_line("PRINT B$") == "HELLO!!!"
        assert con.send_line("CLOSE #1") == ""
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


def test_editor_enter_autoindents(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        _edit(con, "IND.BAS")
        _keys(con, b"    PRINT 1\rPRINT 2")
        _keys(con, bytes([15]), quiet=0.4)
        _quit(con)
        assert con.send_line('OPEN "IND.BAS" FOR INPUT AS #1') == ""
        assert con.send_line("LINE INPUT #1, A$") == ""
        assert con.send_line("LINE INPUT #1, B$") == ""
        assert con.send_line("CLOSE #1") == ""
        # send_line() strips leading spaces; LEN/ASC check the indent.
        assert con.send_line("PRINT LEN(A$)") == "11"
        assert con.send_line("PRINT ASC(A$)") == "32"
        assert con.send_line("PRINT LEN(B$)") == "11"
        assert con.send_line("PRINT ASC(B$)") == "32"
        assert "PRINT 1" in con.send_line("PRINT MID$(A$,5)")
        assert "PRINT 2" in con.send_line("PRINT MID$(B$,5)")
    finally:
        con.stop()


def test_editor_enter_without_indent(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        _edit(con, "NOIND.BAS")
        _keys(con, b"PRINT 1\rPRINT 2")
        _keys(con, bytes([15]), quiet=0.4)
        _quit(con)
        assert con.send_line('OPEN "NOIND.BAS" FOR INPUT AS #1') == ""
        assert con.send_line("LINE INPUT #1, A$") == ""
        assert con.send_line("PRINT A$") == "PRINT 1"
        assert con.send_line("LINE INPUT #1, B$") == ""
        assert con.send_line("PRINT B$") == "PRINT 2"
        assert con.send_line("CLOSE #1") == ""
    finally:
        con.stop()


def test_editor_tab_inserts_four_spaces(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        _edit(con, "TABIN.BAS")
        _keys(con, b"A\tB")
        _quit(con)
        assert con.send_line('OPEN "TABIN.BAS" FOR INPUT AS #1') == ""
        assert con.send_line("LINE INPUT #1, A$") == ""
        assert con.send_line("PRINT LEN(A$)") == "6"
        assert con.send_line("PRINT A$") == "A    B"
        assert con.send_line("PRINT INSTR(A$, CHR$(9))") == "0"
        assert con.send_line("CLOSE #1") == ""
    finally:
        con.stop()


def test_editor_tab_char_does_not_shift_border(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        assert con.send_line('OPEN "TABSHOW.BAS" FOR OUTPUT AS #1') == ""
        assert con.send_line('PRINT #1, "X" + CHR$(9) + "Y"') == ""
        assert con.send_line("CLOSE #1") == ""
        _edit(con, "TABSHOW.BAS")
        # Right pane border stays in the last character column (79 * 8 + 3).
        r, g, b = con.screen_pixel(79 * 8 + 3, 3 * 16)
        assert r > 100 and g > 100 and b > 100, (r, g, b)
        _quit(con)
    finally:
        con.stop()


def _cell_samples(con, col, row):
    x0, y0 = col * 8, row * 16
    return [
        con.screen_pixel(x0 + 1, y0 + 1),
        con.screen_pixel(x0 + 6, y0 + 1),
        con.screen_pixel(x0 + 1, y0 + 14),
        con.screen_pixel(x0 + 6, y0 + 14),
    ]


def _is_edit_blue(rgb):
    r, g, b = rgb
    return b > r + 20 and b > 40 and r < 80


def _is_sel_light(rgb):
    r, g, b = rgb
    return r > 100 and g > 100 and b > 100


def _read_bas(con, path):
    assert con.send_line(f'OPEN "{path}" FOR INPUT AS #1') == ""
    assert con.send_line("LINE INPUT #1, A$") == ""
    text = con.send_line("PRINT A$")
    con.send_line("CLOSE #1")
    return text


def test_editor_shift_arrows_highlight_selection(kernel_image):
    """Shift+Left/Right paints selected glyphs with inverted fg/bg (not editor blue)."""
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        _edit(con, "SELHL.BAS")
        _keys(con, b"HELLO")
        _keys(con, b"\x1b[H")
        _keys(con, b"\x1b[1;2C\x1b[1;2C\x1b[1;2C")
        marked = _cell_samples(con, 1, 3)
        rest = _cell_samples(con, 5, 3)
        assert any(_is_sel_light(p) for p in marked), marked
        assert not all(_is_edit_blue(p) for p in marked), marked
        assert any(_is_edit_blue(p) for p in rest), rest
        _quit(con)
    finally:
        con.stop()


def test_editor_shift_del_cut_and_shift_ins_paste(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        _edit(con, "SELCUT.BAS")
        _keys(con, b"HELLO")
        _keys(con, b"\x1b[H")
        _keys(con, b"\x1b[1;2F")
        _keys(con, b"\x1b[3;2~")
        _keys(con, b"ZZ")
        _keys(con, b"\x1b[2;2~")
        _keys(con, bytes([15]), quiet=0.4)
        _quit(con)
        assert _read_bas(con, "SELCUT.BAS") == "ZZHELLO"
    finally:
        con.stop()


def test_editor_ctrl_ins_copies_selection(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        _edit(con, "SELCP.BAS")
        _keys(con, b"AB")
        _keys(con, b"\x1b[H")
        _keys(con, b"\x1b[1;2F")
        _keys(con, b"\x1b[2;5~")
        _keys(con, b"\x1b[F")
        _keys(con, b"\x1b[2;2~")
        _keys(con, bytes([15]), quiet=0.4)
        _quit(con)
        assert _read_bas(con, "SELCP.BAS") == "ABAB"
    finally:
        con.stop()


def test_editor_del_erases_selection_without_clipboard(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        _edit(con, "SELDEL.BAS")
        _keys(con, b"HELLO")
        _keys(con, b"\x1b[H")
        _keys(con, b"\x1b[1;2C\x1b[1;2C")
        _keys(con, b"\x1b[3~")
        _keys(con, bytes([15]), quiet=0.4)
        _quit(con)
        assert _read_bas(con, "SELDEL.BAS") == "LLO"
    finally:
        con.stop()


def test_editor_unshifted_arrow_clears_selection(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        _edit(con, "SELCLR.BAS")
        _keys(con, b"HELLO")
        _keys(con, b"\x1b[H")
        _keys(con, b"\x1b[1;2F")
        _keys(con, b"\x1b[D")
        _keys(con, b"\x1b[3~")
        _keys(con, bytes([15]), quiet=0.4)
        _quit(con)
        text = _read_bas(con, "SELCLR.BAS")
        assert text == "HELL"
        assert text != ""
    finally:
        con.stop()


def test_editor_shift_down_selects_current_line(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        _edit(con, "SELLN.BAS")
        _keys(con, b"AAA\rBBB")
        _keys(con, b"\x1b[1;5H")
        _keys(con, b"\x1b[1;2B")
        _keys(con, b"\x1b[3~")
        _keys(con, bytes([15]), quiet=0.4)
        _quit(con)
        assert _read_bas(con, "SELLN.BAS") == "BBB"
    finally:
        con.stop()


def test_editor_typing_replaces_selection(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        _edit(con, "SELTYP.BAS")
        _keys(con, b"HELLO")
        _keys(con, b"\x1b[H")
        _keys(con, b"\x1b[1;2C\x1b[1;2C")
        _keys(con, b"X")
        _keys(con, bytes([15]), quiet=0.4)
        _quit(con)
        assert _read_bas(con, "SELTYP.BAS") == "XLLO"
    finally:
        con.stop()


def _seed_switcher_tree(con):
    assert con.send_line('MKDIR "SWP"') == ""
    assert con.send_line('CHDIR "SWP"') == ""
    assert con.send_line('OPEN "MAIN.BAS" FOR OUTPUT AS #1') == ""
    assert con.send_line('PRINT #1, "PRINT 11"') == ""
    assert con.send_line("CLOSE #1") == ""
    assert con.send_line('MKDIR "NEST"') == ""
    assert con.send_line('OPEN "NEST/CHILD.BAS" FOR OUTPUT AS #1') == ""
    assert con.send_line('PRINT #1, "PRINT 22"') == ""
    assert con.send_line("CLOSE #1") == ""


def test_editor_ctrl_p_lists_recursive_files(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        _seed_switcher_tree(con)
        _edit(con, "MAIN.BAS")
        seen = _keys(con, bytes([16]))
        assert "Quick open" in seen
        assert "MAIN.BAS" in seen
        assert "CHILD.BAS" in seen or "NEST/CHILD" in seen
        _keys(con, b"\x1b\x1b")
        _quit(con)
    finally:
        con.stop()


def test_editor_ctrl_p_enter_opens_nested_file(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        _seed_switcher_tree(con)
        seen = _edit(con, "MAIN.BAS")
        assert "MAIN" in seen
        _keys(con, bytes([16]))
        opened = _keys(con, b"\x1b[B\r")
        assert "MAIN" in opened
        assert "CHILD" in opened
        _quit(con)
        listing = con.send_line('DIR "A:/SWP"')
        assert "MAIN.BAS" in listing
        assert "22" in con.send_line('RUN "A:/SWP/NEST/CHILD.BAS"')
    finally:
        con.stop()


def test_editor_ctrl_p_reuses_existing_tab(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        _seed_switcher_tree(con)
        _edit(con, "MAIN.BAS")
        _keys(con, bytes([16]))
        _keys(con, b"\x1b[B\r")
        both = _keys(con, bytes([16]))
        assert "MAIN.BAS" in both and ("CHILD" in both or "NEST" in both)
        back = _keys(con, b"\r")
        assert "MAIN" in back
        assert "CHILD" in back
        _quit(con)
    finally:
        con.stop()


def test_editor_ctrl_p_filter_then_enter(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        _seed_switcher_tree(con)
        _edit(con, "MAIN.BAS")
        _keys(con, bytes([16]))
        opened = _keys(con, b"CHILD\r")
        assert "CHILD" in opened
        _quit(con)
        assert "22" in con.send_line('RUN "A:/SWP/NEST/CHILD.BAS"')
    finally:
        con.stop()
