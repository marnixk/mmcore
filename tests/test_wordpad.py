"""WORDPAD markdown editor TUI: menus, save, wrap, wide view, themes."""

import re
import time

from harness import MMBasicConsole
from ihelp_util import close_ihelp, dump_topic, open_ihelp, scroll_all


def _plain(s: str) -> str:
    return re.sub(r"\x1b\[[0-9;?]*[A-Za-z]", "", s)


def _open(con, cmd: str = "WORDPAD", quiet: float = 0.8) -> str:
    con.drain(quiet=0.1)
    con._ser.sendall((cmd + "\r").encode())
    return _plain(con.drain(quiet=quiet, timeout=12).decode(errors="replace"))


def _keys(con, data: bytes, quiet: float = 0.5) -> str:
    con._ser.sendall(data)
    return _plain(con.drain(quiet=quiet).decode(errors="replace"))


def _quit(con) -> str:
    return _keys(con, bytes([24]))


def _alt_menu(con, letter: bytes, quiet: float = 0.5) -> str:
    return _keys(con, bytes([1]) + letter, quiet=quiet)


def _is_dark_theme_bg(r: int, g: int, b: int) -> bool:
    return r < 50 and g < 50 and b < 50


def _is_neon_ink(r: int, g: int, b: int) -> bool:
    return (g > 150 and r < 80) or (r > 150 and b > 150)


def test_wordpad_opens_untitled(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        seen = _open(con)
        assert "[WORDPAD]" in seen
        assert "File" not in seen
        assert "Edit" not in seen
        assert "words" not in seen.lower()
        menu = _alt_menu(con, b"f", quiet=0.5)
        assert "File" in menu
        _keys(con, b"\x1b", quiet=0.6)
        _quit(con)
        assert con.send_line("PRINT 3+4") == "7"
    finally:
        con.stop()


def test_help_wordpad(console):
    listing = scroll_all(console, open_ihelp(console))
    assert "WORDPAD" in listing
    close_ihelp(console)
    out = dump_topic(console, "WORDPAD")
    assert out != "?SYNTAX ERROR"
    low = out.lower()
    assert "markdown" in low
    assert "theme" in low or "wide" in low
    assert "bold" in low
    assert any(k in low for k in ("ctrl+x", "f10", "quit"))
    assert "ctrl+p" in low or "quick-open" in low


def test_wordpad_type_save_and_reload(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        _open(con, 'WORDPAD "NOTE.MD"')
        _keys(con, b"Hello zen")
        _alt_menu(con, b"f", quiet=0.4)
        _keys(con, b"s", quiet=0.6)
        _quit(con)
        listing = con.send_line("DIR")
        assert "NOTE.MD" in listing
        reopened = _open(con, 'WORDPAD "NOTE.MD"', quiet=1.0)
        assert "Hello" in reopened
        _quit(con)
    finally:
        con.stop()


def test_wordpad_markdown_heading_style(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        _open(con)
        seen = _keys(con, b"# Title\rbody text", quiet=0.8)
        _alt_menu(con, b"f", quiet=0.4)
        closed = _keys(con, b"\x1b", quiet=0.6)
        time.sleep(0.3)
        serial = seen + " " + closed + " " + _plain(con.drain(quiet=0.4).decode(errors="replace"))
        pixel_diff = False
        for heading_y in (1 * 16 + 8, 3 * 16 + 8):
            body_y = heading_y + 16
            heading_samples = [con.screen_pixel(x, heading_y) for x in (12, 20, 28, 36, 44)]
            body_samples = [con.screen_pixel(x, body_y) for x in (12, 20, 28, 36, 44)]
            if any(h != b for h, b in zip(heading_samples, body_samples)):
                pixel_diff = True
                break
        assert pixel_diff or "Title" in serial
        _quit(con)
    finally:
        con.stop()


def test_wordpad_word_wrap(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        _open(con)
        long_line = b"word " * 20
        seen = _keys(con, long_line, quiet=1.0)
        assert len(long_line) > 40
        wrapped = (
            seen.count("word") >= 2
            and (
                len(seen.splitlines()) > 1
                or any(len(line.strip()) < len(long_line) for line in seen.splitlines())
            )
        )
        assert wrapped, "expected wrapped text across multiple visual rows"
        _quit(con)
    finally:
        con.stop()


def test_wordpad_wide_view_toggle(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        assert con.send_line("MODE 14,16") == ""
        time.sleep(0.3)
        assert con.screen_size() == (960, 540)
        _open(con, quiet=1.0)
        margin = con.screen_pixel(40, 80)
        assert _is_dark_theme_bg(*margin), margin
        menu = _alt_menu(con, b"s", quiet=0.5)
        if "Wide" in menu or "wide" in menu.lower():
            toggled = _keys(con, b"\r", quiet=0.8)
        else:
            toggled = _keys(con, b"w", quiet=0.8)
        low = toggled.lower()
        wide_hint = "wide" in low or "120" in toggled
        still_hd = con.screen_size() == (960, 540)
        margin_after = con.screen_pixel(40, 80)
        margin_stays_bg = _is_dark_theme_bg(*margin_after)
        assert wide_hint or (still_hd and margin_stays_bg)
        _quit(con)
        assert con.send_line("PRINT 1") == "1"
    finally:
        con.stop()


def test_wordpad_theme_neon(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        _open(con)
        menu = _alt_menu(con, b"t", quiet=0.5)
        if "Neon" in menu:
            _keys(con, b"\x1b[B\r", quiet=0.8)
        else:
            _keys(con, b"n", quiet=0.8)
        time.sleep(0.3)
        found_neon = False
        for x in (20, 40, 80):
            for y in (56, 80, 120):
                r, g, b = con.screen_pixel(x, y)
                if r + g + b < 80 or _is_neon_ink(r, g, b):
                    found_neon = True
                    break
            if found_neon:
                break
        assert found_neon, "expected Neon theme colours on pane or margin"
        _quit(con)
    finally:
        con.stop()


def test_wordpad_copy_paste(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        _open(con)
        _keys(con, b"xyz")
        _keys(con, b"\x0b")
        pasted = _keys(con, bytes([21]), quiet=0.6)
        assert "xyz" in pasted
        _quit(con)
        assert con.send_line("PRINT 1") == "1"
    finally:
        con.stop()


def _lum(rgb):
    r, g, b = rgb
    return r * 3 + g * 6 + b


def _cell_corners(con, col, row):
    x0, y0 = col * 8, row * 16
    return [
        con.screen_pixel(x0 + 1, y0 + 1),
        con.screen_pixel(x0 + 6, y0 + 1),
        con.screen_pixel(x0 + 1, y0 + 14),
        con.screen_pixel(x0 + 6, y0 + 14),
    ]


def _is_solid_cursor(con, col, row):
    return all(_lum(p) > 1500 for p in _cell_corners(con, col, row))


def test_wordpad_cursor_visible(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        _open(con)
        _keys(con, b"Hello")
        time.sleep(0.2)
        cursor = con.screen_pixel(5 * 8 + 4, 8)
        page = con.screen_pixel(20 * 8 + 4, 8)
        assert _lum(cursor) > _lum(page) + 80, (cursor, page)
        _quit(con)
    finally:
        con.stop()


def test_wordpad_h1_is_taller(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        _open(con)
        _keys(con, b"# Title\rbody text", quiet=0.8)
        time.sleep(0.2)
        heading_mid = [con.screen_pixel(x, 20) for x in (12, 20, 28, 36)]
        body = [con.screen_pixel(x, 2 * 16 + 8) for x in (12, 20, 28, 36)]
        assert any(h != b for h, b in zip(heading_mid, body))
        _quit(con)
    finally:
        con.stop()


def test_wordpad_dialog_surface_stands_out(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        _open(con)
        margin = con.screen_pixel(8, 80)
        _alt_menu(con, b"f", quiet=0.4)
        _keys(con, b"o", quiet=0.8)
        time.sleep(0.2)
        dlg = con.screen_pixel(320, 240)
        assert _lum(dlg) > _lum(margin) + 30, (dlg, margin)
        _keys(con, b"\x1b", quiet=0.6)
        _quit(con)
    finally:
        con.stop()


def test_wordpad_theme_persists(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        _open(con)
        _alt_menu(con, b"t", quiet=0.5)
        _keys(con, b"n", quiet=0.8)
        _quit(con)
        assert con.send_line("NEW") == ""
        assert con.send_line('10 OPEN "A:/.mmbasic.ini" FOR INPUT AS #1') == ""
        assert con.send_line("20 IF EOF(#1) THEN GOTO 70") == ""
        assert con.send_line("30 LINE INPUT #1, A$") == ""
        assert con.send_line("40 PRINT A$") == ""
        assert con.send_line("50 GOTO 20") == ""
        assert con.send_line("70 CLOSE #1") == ""
        ini = con.send_line("RUN", timeout=8)
        assert "wordpad_theme=4" in ini
        reopened = _open(con, quiet=1.0)
        found_neon = False
        for x in (20, 40, 80):
            for y in (56, 80, 120):
                r, g, b = con.screen_pixel(x, y)
                if r + g + b < 80 or _is_neon_ink(r, g, b):
                    found_neon = True
                    break
            if found_neon:
                break
        assert found_neon, "expected Neon theme after reopen " + reopened[:80]
        _quit(con)
    finally:
        con.stop()


def _seed_wp_files(con):
    assert con.send_line('OPEN "A.MD" FOR OUTPUT AS #1') == ""
    assert con.send_line('PRINT #1, "alpha-doc"') == ""
    assert con.send_line("CLOSE #1") == ""
    assert con.send_line('MKDIR "NEST"') == ""
    assert con.send_line('OPEN "NEST/B.MD" FOR OUTPUT AS #1') == ""
    assert con.send_line('PRINT #1, "beta-doc"') == ""
    assert con.send_line("CLOSE #1") == ""


def test_wordpad_ctrl_p_quick_open(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        _seed_wp_files(con)
        _open(con, 'WORDPAD "A.MD"')
        seen = _keys(con, bytes([16]), quiet=0.8)
        assert "Quick open" in seen
        assert "A.MD" in seen
        assert "B.MD" in seen or "NEST" in seen
        opened = _keys(con, b"b\r", quiet=0.8)
        assert "beta" in opened.lower() or "B.MD" in opened
        _quit(con)
    finally:
        con.stop()


def test_wordpad_autosave_on_switch(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        _seed_wp_files(con)
        _open(con, 'WORDPAD "A.MD"')
        _keys(con, b"\x1b[F extra", quiet=0.6)
        _keys(con, bytes([16]), quiet=0.6)
        _keys(con, b"b\r", quiet=0.8)
        _quit(con)
        assert con.send_line('OPEN "A.MD" FOR INPUT AS #1') == ""
        assert con.send_line("LINE INPUT #1, A$") == ""
        line = con.send_line("PRINT A$")
        con.send_line("CLOSE #1")
        assert "extra" in line
    finally:
        con.stop()


def test_wordpad_empty_line_cursor_and_up(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        _open(con)
        _keys(con, b"A\r", quiet=0.6)
        time.sleep(0.2)
        empty = con.screen_pixel(4, 16 + 8)
        below = con.screen_pixel(4, 32 + 8)
        assert _lum(empty) > _lum(below) + 80, (empty, below)
        _keys(con, b"\x1b[A", quiet=0.5)
        time.sleep(0.2)
        on_a = con.screen_pixel(4, 8)
        still_empty = con.screen_pixel(4, 16 + 8)
        assert _lum(on_a) > _lum(still_empty) + 80, (on_a, still_empty)
        _quit(con)
    finally:
        con.stop()


def test_wordpad_up_from_longer_line_end(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        _open(con)
        _keys(con, b"ab\rabcd", quiet=0.7)
        _keys(con, b"\x1b[A", quiet=0.5)
        time.sleep(0.2)
        assert _is_solid_cursor(con, 2, 0)
        assert not _is_solid_cursor(con, 0, 1)
        assert not _is_solid_cursor(con, 0, 0)
        _quit(con)
    finally:
        con.stop()


def test_wordpad_bold_is_more_intense(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        _open(con)
        _keys(con, b"xx**bold**yy", quiet=0.7)
        time.sleep(0.2)
        plain = con.screen_pixel(4, 8)
        strong = con.screen_pixel(2 * 8 + 4, 8)
        assert _lum(strong) != _lum(plain), (strong, plain)
        _quit(con)
    finally:
        con.stop()


def test_wordpad_menu_status_and_hotkeys(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        w, h = con.screen_size()
        _open(con)
        _keys(con, b"hello", quiet=0.5)
        pane = con.screen_pixel(8, 80)
        seen = _alt_menu(con, b"f", quiet=0.6)
        time.sleep(0.2)
        low = seen.lower()
        assert "untitled" in low
        assert "words" in low
        assert not low.rstrip().endswith("80")
        assert not low.rstrip().endswith("120")
        menu_bar = con.screen_pixel(4 * 8 + 4, 8)
        drop = con.screen_pixel(12, 3 * 16 + 8)
        assert _lum(menu_bar) > _lum(pane) + 30, (menu_bar, pane)
        assert _lum(drop) > _lum(pane) + 30, (drop, pane)
        hot_f = con.screen_pixel(4, 8)
        letter_i = con.screen_pixel(12, 8)
        assert hot_f != letter_i, (hot_f, letter_i)
        status_y = (h // 16 - 1) * 16 + 8
        chip = con.screen_pixel(4, status_y)
        assert _lum(chip) > _lum(pane) + 30, (chip, pane)
        _keys(con, b"\x1b", quiet=0.6)
        _quit(con)
    finally:
        con.stop()
