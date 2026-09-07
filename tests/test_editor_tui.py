"""Turbo-style MMBasic editor TUI: menus, file ops, tabs, quit."""

import re
import time

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
    return _keys(con, bytes([1]) + b"x", quiet=0.5)


def test_editor_menu_labels_on_serial(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        seen = _edit(con, "HI.BAS")
        assert "File" in seen
        assert "Run" in seen
        assert "Theme" in seen
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
        closed = _keys(con, b"\x1b", quiet=0.6)
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


def test_editor_ctrl_x_does_not_quit(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        seen = _edit(con, "NOQUIT.BAS")
        assert "File" in seen
        _keys(con, bytes([24]), quiet=0.4)
        _quit(con)
        assert con.send_line("PRINT 8") == "8"
    finally:
        con.stop()


def test_editor_ctrl_c_x_v_copy_cut_paste(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        _edit(con, "CXV.BAS")
        _keys(con, b"HELLO")
        _keys(con, b"\x1b[H")
        _keys(con, b"\x1b[1;2F")
        _keys(con, bytes([3]))
        _keys(con, b"\x1b[F")
        _keys(con, bytes([22]))
        _keys(con, bytes([15]), quiet=0.4)
        _quit(con)
        assert _read_bas(con, "CXV.BAS") == "HELLOHELLO"
        _edit(con, "CXV.BAS")
        _keys(con, b"\x1b[H")
        _keys(con, b"\x1b[1;2F")
        _keys(con, bytes([24]))
        _keys(con, b"ZZ")
        _keys(con, bytes([22]))
        _keys(con, bytes([15]), quiet=0.4)
        _quit(con)
        assert _read_bas(con, "CXV.BAS") == "ZZHELLOHELLO"
    finally:
        con.stop()


def test_editor_esc_menu_open_close(kernel_image):
    """A single ESC dismisses a menu. ESC+letter is not an Alt shortcut."""
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        _edit(con, "MENU.BAS")
        leftover = _keys(con, b"\x1bf", quiet=0.6)
        assert "Open" not in leftover
        assert "Quit" not in leftover
        opened = _keys(con, bytes([1]) + b"f")
        assert "Open" in opened or "Save" in opened or "Quit" in opened
        closed = _keys(con, b"\x1b", quiet=0.6)
        assert "File" in closed
        assert "Quit" not in closed
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


def test_editor_open_dialog_has_file_and_dir_lists(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        _edit(con, "FOO.BAS")
        seen = _keys(con, bytes([1]) + b"fo", quiet=0.6)
        assert "Name" in seen
        assert "Files" in seen
        assert "Directories" in seen
        assert "*.BAS" in seen
        assert ".." in seen
        assert "[-A-]" in seen
        # White dialog chrome; green Name field (focused).
        r0, c0 = 6, 9
        wr, wg, wb = con.screen_pixel((c0 + 6) * 8 + 4, (r0 + 1) * 16 + 8)
        assert wr > 100 and wg > 100 and wb > 100, (wr, wg, wb)
        gr, gg, gb = con.screen_pixel((c0 + 4) * 8 + 4, (r0 + 2) * 16 + 8)
        assert gg > gr + 20 and gg > 40, (gr, gg, gb)
        _keys(con, b"\x1b", quiet=0.6)
        _quit(con)
    finally:
        con.stop()


def test_editor_open_types_into_folder(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        assert con.send_line('MKDIR "TPN"') == ""
        assert con.send_line('OPEN "TPN/IN.BAS" FOR OUTPUT AS #1') == ""
        assert con.send_line('PRINT #1, "PRINT 77"') == ""
        assert con.send_line("CLOSE #1") == ""
        assert "IN.BAS" in con.send_line('DIR "TPN"')
        _edit(con, "FOO.BAS")
        _keys(con, bytes([1]) + b"fo", quiet=0.5)
        seen = _keys(con, b"TPN\r", quiet=0.6)
        assert "IN.BAS" in seen
        assert "TPN" in seen
        opened = _keys(con, b"IN.BAS\r", quiet=0.6)
        assert "PRINT 77" in opened
        _quit(con)
        assert "IN.BAS" in con.send_line('DIR "TPN"')
        assert con.send_line('CHDIR "TPN"') == ""
        assert "77" in con.send_line('RUN "IN.BAS"')
    finally:
        con.stop()


def test_editor_open_navigates_dirs_with_arrows(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        assert con.send_line('MKDIR "TPN"') == ""
        assert con.send_line('OPEN "TPN/IN.BAS" FOR OUTPUT AS #1') == ""
        assert con.send_line('PRINT #1, "PRINT 88"') == ""
        assert con.send_line("CLOSE #1") == ""
        assert con.send_line('OPEN "FOO.BAS" FOR OUTPUT AS #1') == ""
        assert con.send_line('PRINT #1, "PRINT 1"') == ""
        assert con.send_line("CLOSE #1") == ""
        _edit(con, "FOO.BAS")
        _keys(con, bytes([1]) + b"fo", quiet=0.5)
        # Name -> Files -> Directories, Down from .. onto TPN/, Enter.
        seen = _keys(con, b"\t\t\x1b[B\r", quiet=0.7)
        assert "IN.BAS" in seen
        opened = _keys(con, b"\r", quiet=0.6)
        assert "PRINT 88" in opened
        _quit(con)
        assert con.send_line('CHDIR "TPN"') == ""
        assert "88" in con.send_line('RUN "IN.BAS"')
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


def test_editor_tab_indents_selection(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        _edit(con, "IND.BAS")
        _keys(con, b"AA\rBB")
        _keys(con, b"\x1b[H\x1b[1;2B\x1b[1;2B")
        _keys(con, b"\t")
        _quit(con)
        assert con.send_line('OPEN "IND.BAS" FOR INPUT AS #1') == ""
        assert con.send_line("LINE INPUT #1, A$") == ""
        assert con.send_line("LINE INPUT #1, B$") == ""
        assert con.send_line("PRINT A$") == "    AA"
        assert con.send_line("PRINT B$") == "    BB"
        assert con.send_line("CLOSE #1") == ""
    finally:
        con.stop()


def test_editor_shift_tab_outdents_line(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        _edit(con, "OUT.BAS")
        _keys(con, b"    HI")
        _keys(con, b"\x1b[Z")
        _quit(con)
        assert con.send_line('OPEN "OUT.BAS" FOR INPUT AS #1') == ""
        assert con.send_line("LINE INPUT #1, A$") == ""
        assert con.send_line("PRINT A$") == "HI"
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
        _keys(con, b"\x1b", quiet=0.6)
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


def test_editor_ctrl_p_after_theme_opens_file(kernel_image):
    """#69: quick-open must still load a file after a theme change."""
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        assert con.send_line("OPTION EDIT THEME PAPER") == ""
        _seed_switcher_tree(con)
        seen = _edit(con, "MAIN.BAS")
        assert "MAIN" in seen or "File" in seen
        themed = _keys(con, bytes([1]) + b"t", quiet=0.6)
        assert "Paper" in themed or "Snow" in themed or "Turbo" in themed
        _keys(con, b"\x1b[B\x1b[B\r", quiet=0.8)
        _keys(con, bytes([16]), quiet=0.8)
        opened = _keys(con, b"CHILD\r", quiet=1.2)
        assert "CHILD" in opened
        _quit(con)
        assert con.send_line("PRINT 3+4") == "7"
        assert con.send_line("OPTION EDIT THEME TURBO") == ""
    finally:
        con.stop()


def test_editor_ctrl_p_after_theme_opens_file(kernel_image):
    """#69: quick-open must still load a file after a theme change."""
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        assert con.send_line("OPTION EDIT THEME PAPER") == ""
        _seed_switcher_tree(con)
        seen = _edit(con, "MAIN.BAS")
        assert "MAIN" in seen or "File" in seen
        themed = _keys(con, bytes([1]) + b"t", quiet=0.6)
        assert "Paper" in themed or "Snow" in themed or "Turbo" in themed
        _keys(con, b"\x1b[B\x1b[B\r", quiet=0.8)
        _keys(con, bytes([16]), quiet=0.8)
        opened = _keys(con, b"CHILD\r", quiet=1.2)
        assert "CHILD" in opened
        _quit(con)
        assert con.send_line("PRINT 3+4") == "7"
        assert con.send_line("OPTION EDIT THEME TURBO") == ""
    finally:
        con.stop()


def test_editor_open_enter_uses_highlighted_file(kernel_image):
    """#69: File/Open Enter with an empty name opens the highlighted *.BAS."""
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        assert con.send_line("OPTION EDIT THEME SLATE") == ""
        assert con.send_line('OPEN "PICKME.BAS" FOR OUTPUT AS #1') == ""
        assert con.send_line('PRINT #1, "PRINT 77"') == ""
        assert con.send_line("CLOSE #1") == ""
        _edit(con, "UNTITLED.BAS")
        _keys(con, bytes([1]) + b"fo", quiet=0.7)
        opened = _keys(con, b"\r", quiet=1.0)
        assert "PICKME" in opened or "77" in opened
        _quit(con)
        assert con.send_line("PRINT 1+1") == "2"
        assert con.send_line("OPTION EDIT THEME TURBO") == ""
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


def test_editor_run_press_key_returns(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        _edit(con, "RUNRET.BAS")
        _keys(con, b"PRINT 6*7")
        ran = _keys(con, bytes([18]), quiet=1.2)
        assert "42" in ran
        assert "Press any key to continue" in ran
        back = _keys(con, b" ", quiet=0.8)
        assert "File" in back
        assert "Run" in back
        assert "PRINT 6*7" in back or "6*7" in back
        _quit(con)
        assert con.send_line("PRINT 1") == "1"
        assert "42" in con.send_line('RUN "RUNRET.BAS"')
    finally:
        con.stop()


def test_editor_f9_run_press_key_returns(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        _edit(con, "F9RUN.BAS")
        _keys(con, b"PRINT 8+1")
        ran = _keys(con, b"\x1b[20~", quiet=1.2)
        assert "9" in ran
        assert "Press any key to continue" in ran
        back = _keys(con, b"x", quiet=0.8)
        assert "File" in back
        _quit(con)
        assert con.send_line("PRINT 2") == "2"
    finally:
        con.stop()


def test_editor_run_restores_mode_and_page(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        _edit(con, "GFXRUN.BAS")
        _keys(con, b"MODE 7,8\rPAGE WRITE 1\rPAGE DISPLAY 1\rPRINT 99")
        ran = _keys(con, bytes([18]), quiet=1.5)
        assert "99" in ran
        assert "Press any key to continue" in ran
        back = _keys(con, b" ", quiet=1.0)
        assert "File" in back
        pane = [con.screen_pixel(x, 80) for x in (40, 80, 160, 320)]
        assert any(b > r + 20 and b > 40 for r, g, b in pane), pane
        _quit(con)
        assert con.send_line("PRINT MM.HRES") == "640"
        assert con.send_line("PRINT MM.VRES") == "480"
        assert con.send_line("PAGE WRITE 0") == ""
        assert con.send_line("CLS") == ""
        assert con.send_line("PIXEL 12,12,RGB(255,0,0)") == ""
        pix = int(con.send_line("PRINT PIXEL(12,12)"))
        assert ((pix >> 16) & 255) > 150
    finally:
        con.stop()


def test_editor_run_inkey_loop_break_restores_editor(kernel_image):
    """Issue #62: DO/INKEY$ from editor Run must not leave a dead prompt."""
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        assert con.send_line('OPEN "INK.BAS" FOR OUTPUT AS #1') == ""
        assert con.send_line('PRINT #1, "DO"') == ""
        assert con.send_line('PRINT #1, "CURRENT = ASC(INKEY$())"') == ""
        assert con.send_line('PRINT #1, "IF CURRENT THEN"') == ""
        assert con.send_line(
            'PRINT #1, "PRINT ";CHR$(34);"current input: ";CHR$(34);"; CURRENT"'
        ) == ""
        assert con.send_line('PRINT #1, "END IF"') == ""
        assert con.send_line('PRINT #1, "LOOP"') == ""
        assert con.send_line("CLOSE #1") == ""

        con.drain(quiet=0.1)
        con._ser.sendall(b'RUN "INK.BAS"\r')
        time.sleep(0.4)
        con._ser.sendall(b"B")
        seen = _plain(con.drain(quiet=0.6, timeout=2.0).decode(errors="replace"))
        assert "current input" in seen.lower()
        assert "66" in seen
        broke = con.send_keys(b"\x03", timeout=6.0)
        assert "BREAK" in broke.upper()
        assert con.send_line("PRINT 2") == "2"

        _edit(con, "INK.BAS")
        _keys(con, bytes([18]), quiet=0.5)
        con._ser.sendall(b"A")
        typed = _plain(con.drain(quiet=0.7, timeout=2.5).decode(errors="replace"))
        assert "current input" in typed.lower()
        assert "65" in typed
        con._ser.sendall(bytes([3]))
        back = _plain(con.drain(quiet=1.2, timeout=4.0).decode(errors="replace"))
        assert "File" in back
        assert "Run" in back
        _quit(con)
        time.sleep(0.4)
        assert con.send_line("PRINT 1") == "1"
    finally:
        con.stop()


def test_editor_theme_menu_lists_ten_themes(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        _edit(con, "TH.BAS")
        seen = _keys(con, bytes([1]) + b"t")
        for name in (
            "Paper",
            "Cloud",
            "Snow",
            "Night",
            "Nord",
            "Slate",
            "Forest",
            "Violet",
            "Turbo",
            "Phosphor",
        ):
            assert name in seen, name
        _keys(con, b"\x1b", quiet=0.6)
        _quit(con)
    finally:
        con.stop()


def test_editor_theme_paper_changes_pane_and_persists(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        _edit(con, "THP.BAS")
        pane = [con.screen_pixel(x, 80) for x in (40, 80, 160, 320)]
        assert any(b > r + 20 and b > 40 for r, g, b in pane), pane
        _keys(con, bytes([1]) + b"tp", quiet=0.8)
        paper = [con.screen_pixel(x, 80) for x in (40, 80, 160, 320)]
        assert any(r > 140 and g > 140 and b > 140 for r, g, b in paper), paper
        _quit(con)
        listing = con.send_line("OPTION LIST")
        assert "PAPER" in listing.upper()
        assert con.send_line("NEW") == ""
        assert con.send_line('10 OPEN "A:/.mmbasic.ini" FOR INPUT AS #1') == ""
        assert con.send_line("20 IF EOF(#1) THEN GOTO 70") == ""
        assert con.send_line("30 LINE INPUT #1, A$") == ""
        assert con.send_line("40 PRINT A$") == ""
        assert con.send_line("50 GOTO 20") == ""
        assert con.send_line("70 CLOSE #1") == ""
        ini = con.send_line("RUN", timeout=8)
        assert "edit_theme=0" in ini
        assert con.send_line("OPTION EDIT THEME TURBO") == ""
    finally:
        con.stop()


def test_option_edit_theme_phosphor(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        assert con.send_line("OPTION EDIT THEME PHOSPHOR") == ""
        _edit(con, "PHOS.BAS")
        pane = [con.screen_pixel(x, 80) for x in (40, 80, 160, 320)]
        assert all(r + g + b < 40 for r, g, b in pane), pane
        r, g, b = con.screen_pixel(3, 3 * 16)
        assert g > r + 20 and g > 40, (r, g, b)
        _quit(con)
        assert "PHOSPHOR" in con.send_line("OPTION LIST").upper()
        assert con.send_line("OPTION EDIT THEME 8") == ""
        listed = con.send_line("OPTION LIST ALL")
        assert "TURBO" in listed.upper()
    finally:
        con.stop()


def test_editor_slate_palette_and_menu_contrast(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        assert con.send_line("OPTION EDIT THEME SLATE") == ""
        _edit(con, "SLATE.BAS")
        pane = [con.screen_pixel(x, 80) for x in (40, 80, 160, 320)]
        assert all(r < 50 and g < 50 and b < 55 for r, g, b in pane), pane
        assert all(abs(r - 85) > 12 or abs(g - 85) > 12 for r, g, b in pane), pane
        assert all(b < r + 30 for r, g, b in pane), pane
        con.capture_png("/opt/cursor/artifacts/issue71_slate_pane.png")
        _keys(con, bytes([1]) + b"f", quiet=0.6)
        sel = [con.screen_pixel(x, 2 * 16 + 8) for x in range(16, 128, 8)]
        uns = [con.screen_pixel(x, 3 * 16 + 8) for x in range(16, 128, 8)]

        def dist(a, b):
            return abs(a[0] - b[0]) + abs(a[1] - b[1]) + abs(a[2] - b[2])

        assert any(dist(s, u) > 80 for s, u in zip(sel, uns)), (sel[:6], uns[:6])
        con.capture_png("/opt/cursor/artifacts/issue71_slate_menu.png")
        _keys(con, b"\x1b", quiet=0.5)
        _quit(con)
        assert con.send_line("OPTION EDIT THEME TURBO") == ""
    finally:
        con.stop()


def test_editor_help_manual_opens_ihelp_and_returns(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        seen = _edit(con, "MANUAL.BAS")
        _keys(con, b"PRINT 42")
        menu = _keys(con, bytes([1]) + b"h", quiet=0.5)
        assert "Manual" in menu
        help_seen = _keys(con, b"m", quiet=1.0)
        low = help_seen.lower()
        assert "help" in low or "basic" in low or "edit" in low
        back = _keys(con, b"\x03", quiet=0.8)
        assert "PRINT" in back or "42" in back or "MANUAL" in back
        _quit(con)
        assert con.send_line("PRINT 1+1") == "2"
    finally:
        con.stop()


def test_editor_f4_opens_include_and_reuses_tab(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        assert con.send_line('MKDIR "INC"') == ""
        assert con.send_line('CHDIR "INC"') == ""
        assert con.send_line('OPEN "CHILD.INC" FOR OUTPUT AS #1') == ""
        assert con.send_line('PRINT #1, "CONST X=22"') == ""
        assert con.send_line("CLOSE #1") == ""
        assert con.send_line('OPEN "MAIN.BAS" FOR OUTPUT AS #1') == ""
        assert con.send_line('PRINT #1, "#include ""CHILD.INC"""') == ""
        assert con.send_line("CLOSE #1") == ""
        seen = _edit(con, "MAIN.BAS")
        assert "include" in seen.lower() or "CHILD" in seen or "MAIN" in seen
        opened = _keys(con, b"\x1b[14~", quiet=0.8)
        assert "CHILD" in opened
        assert "CONST" in opened or "X=22" in opened or "22" in opened
        con.capture_png("/opt/cursor/artifacts/issue78_f4_include.png")
        again = _keys(con, b"\x1b[14~", quiet=0.6)
        assert "CHILD" in again
        _keys(con, bytes([1]) + b"1", quiet=0.5)
        back = _keys(con, b"\x1b[14~", quiet=0.6)
        assert "CHILD" in back
        _quit(con)
        assert con.send_line("PRINT 8+1") == "9"
    finally:
        con.stop()


def test_editor_f4_without_include_sets_status(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        _edit(con, "NOINC.BAS")
        _keys(con, b"PRINT 1")
        seen = _keys(con, b"\x1b[14~", quiet=0.6)
        assert "No #include" in seen or "include" in seen.lower()
        _quit(con)
        assert con.send_line("PRINT 1") == "1"
    finally:
        con.stop()


def test_editor_ctrl_w_closes_tab_and_quits_last(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        _edit(con, "W1.BAS")
        _keys(con, b"PRINT 1")
        _keys(con, bytes([1]) + b"fo", quiet=0.5)
        _keys(con, b"W2.BAS\r", quiet=0.6)
        _keys(con, b"PRINT 2")
        after = _keys(con, bytes([23]), quiet=0.5)
        assert "W1" in after or "File" in after
        _keys(con, bytes([23]), quiet=0.5)
        assert con.send_line("PRINT 5") == "5"
        listing = con.send_line("DIR")
        assert "W1.BAS" in listing
        assert "W2.BAS" in listing
    finally:
        con.stop()


def test_editor_alt_arrows_switch_tabs_no_wrap(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        _edit(con, "L.BAS")
        _keys(con, b"PRINT 1")
        _keys(con, bytes([15]), quiet=0.4)
        _keys(con, bytes([1]) + b"fo", quiet=0.5)
        seen = _keys(con, b"R.BAS\r", quiet=0.6)
        assert "L.BAS" in seen and "R.BAS" in seen
        _keys(con, b"PRINT 2")
        _keys(con, bytes([15]), quiet=0.4)
        left = _keys(con, b"\x1b[1;3D", quiet=0.5)
        assert "L.BAS" in left
        _keys(con, b"\x1b[1;3D", quiet=0.4)
        right = _keys(con, b"\x1b[1;3C", quiet=0.5)
        assert "R.BAS" in right
        _keys(con, b"\x1b[1;3C", quiet=0.4)
        still_r = _keys(con, b"\x1b[1;3D", quiet=0.5)
        assert "L.BAS" in still_r
        _quit(con)
    finally:
        con.stop()
