"""TERM scrollback: history paging and search (#528)."""

import time

from harness import MMBasicConsole
from test_term import _plain, _quit


def _open_demo(con, host="demoburst", settle=1.8):
    con.drain(quiet=0.1)
    con._ser.sendall(f'TERM "{host}", 23\r'.encode())
    acc = con.drain(quiet=0.4, timeout=20)
    time.sleep(settle)
    acc += con.drain(quiet=0.4, timeout=10)
    return _plain(acc.decode(errors="replace"))


def _key(con, data, quiet=0.7, timeout=8.0):
    con._ser.sendall(data)
    return _plain(con.drain(quiet=quiet, timeout=timeout).decode(errors="replace"))


def test_option_term_scrollback_roundtrip(console):
    assert console.send_line("OPTION TERM SCROLLBACK 32") == ""
    listed = console.send_line("OPTION LIST")
    assert "OPTION TERM SCROLLBACK 32" in listed
    assert console.send_line("OPTION TERM SCROLLBACK 9999") == ""
    listed = console.send_line("OPTION LIST")
    assert "OPTION TERM SCROLLBACK 256" in listed
    assert console.send_line("OPTION TERM SCROLLBACK 200") == ""
    listed = console.send_line("OPTION LIST")
    assert "OPTION TERM SCROLLBACK" not in listed


def test_term_scrollback_pages_history(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        seen = _open_demo(con)
        assert "line 40" in seen
        con.drain(quiet=0.3, timeout=3)
        up = _key(con, b"\x1b[5~")
        assert "SCROLL -" in up
        assert "TERM demo" in up
        assert "line 01" in up
        down = _key(con, b"\x1b[6~")
        assert "SCROLL -" not in down
        assert "line 40" in down
        _quit(con)
        assert con.send_line("PRINT 2+2") == "4"
    finally:
        con.stop()


def test_term_scrollback_arrows_do_not_scroll(kernel_image):
    """#592: Up/Down are cursor keys, not scrollback controls."""
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        assert "line 40" in _open_demo(con)
        con.drain(quiet=0.3, timeout=3)
        up = _key(con, b"\x1b[A")
        assert "SCROLL -" not in up
        con.drain(quiet=0.3, timeout=3)
        pgup = _key(con, b"\x1b[5~")
        assert "SCROLL -" in pgup
        scrolled = _key(con, b"\x1b[B")
        assert "SCROLL -" not in scrolled
        still = _key(con, b"\x1b[A")
        assert "SCROLL -" not in still
        _quit(con)
        assert con.send_line("PRINT 3+3") == "6"
    finally:
        con.stop()


def test_term_scrollback_page_and_home(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        assert "line 40" in _open_demo(con)
        con.drain(quiet=0.3, timeout=3)
        pgup = _key(con, b"\x1b[5~")
        assert "SCROLL -" in pgup
        home = _key(con, b"\x1b[1~")
        assert "SCROLL -" in home
        assert "TERM demo" in home
        live = _key(con, b"q")
        assert "SCROLL -" not in live
        _quit(con)
        assert con.send_line("PRINT 5+5") == "10"
    finally:
        con.stop()


def test_term_scrollback_search(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        assert "line 40" in _open_demo(con)
        con.drain(quiet=0.3, timeout=3)
        _key(con, b"\x1b[5~")
        prompt = _key(con, b"/")
        assert "Search:" in prompt
        found = _key(con, b"line 05\r")
        assert "MATCH line 05" in found
        _quit(con)
        assert con.send_line("PRINT 4+4") == "8"
    finally:
        con.stop()
