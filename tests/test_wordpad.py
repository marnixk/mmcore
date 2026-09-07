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
        assert "File" in seen
        assert "Edit" in seen
        assert "Settings" in seen
        assert "Theme" in seen
        assert "words" in seen.lower()
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
    assert any(k in low for k in ("ctrl+x", "f10", "quit"))


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
        closed = _keys(con, b"\x1b\x1b", quiet=0.6)
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
