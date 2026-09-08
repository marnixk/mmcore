"""TERM full-screen terminal app: syntax, help, demo UI, network failure."""

import re
import time

from harness import MMBasicConsole
from ihelp_util import close_ihelp, dump_topic, open_ihelp, scroll_all


def _plain(s: str) -> str:
    return re.sub(r"\x1b\[[0-9;?]*[A-Za-z]", "", s)


def _open_term(con, cmd: str, quiet=0.6, timeout=12.0):
    con.drain(quiet=0.1)
    con._ser.sendall((cmd + "\r").encode())
    return _plain(con.drain(quiet=quiet, timeout=timeout).decode(errors="replace"))


def _f10(con):
    con._ser.sendall(b"\x1b[21~")
    return _plain(con.drain(quiet=0.8, timeout=15).decode(errors="replace"))


def _luminance(r: int, g: int, b: int) -> float:
    return 0.299 * r + 0.587 * g + 0.114 * b


def _is_dark_slate(r: int, g: int, b: int) -> bool:
    return (
        r < 50
        and g < 50
        and b < 50
        and abs(r - 18) <= 25
        and abs(g - 22) <= 25
        and abs(b - 28) <= 25
    )


def _is_creamish(r: int, g: int, b: int) -> bool:
    return r > 80 and g > 75 and b > 65


def _line_numbers(text: str) -> list[int]:
    return [int(m) for m in re.findall(r"line\s+(\d+)", text, re.I)]


def test_term_requires_host_and_port(console):
    assert "?SYNTAX ERROR" in console.send_line("TERM").upper()
    assert "?SYNTAX ERROR" in console.send_line('TERM "example.com"').upper()
    assert "?SYNTAX ERROR" in console.send_line("TERM 23").upper()
    assert "?SYNTAX ERROR" in console.send_line('TERM "h", 0').upper()
    assert "?SYNTAX ERROR" in console.send_line('TERM "h", 70000').upper()


def test_help_term(console):
    listing = scroll_all(console, open_ihelp(console))
    assert "TERM" in listing
    close_ihelp(console)
    out = dump_topic(console, "TERM")
    assert out != "?SYNTAX ERROR"
    low = out.lower()
    assert "host" in low
    assert "f10" in low
    assert "alt" in low
    assert "esc" in low
    assert "14" in low and "mode" in low
    assert "demo" in low
    assert "ansi" in low
    assert "cp437" in low or "437" in low
    assert "vga" in low
    assert "page" in low
    assert "fade out" not in low
    assert any(k in low for k in ("scroll", "slate"))
    assert "dns" in low or "tcp" in low
    assert "character mode" in low or "sga" in low
    assert "cr only" in low or "enter sends cr" in low
    assert "drain" in low or "starve" in low or "pane" in low
    assert "echo" in low
    assert "boxed" in low
    assert "full" in low
    assert "120" in low


def test_term_demo_mode14_slate_and_f10(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        seen = _open_term(con, 'TERM "demo", 23', quiet=0.8, timeout=10.0)
        time.sleep(0.4)
        assert con.screen_size() == (960, 540)
        r, g, b = con.screen_pixel(40, 200)
        assert _is_dark_slate(r, g, b), (r, g, b)
        assert not re.search(r"\.{8,}", seen), seen[:200]
        assert "TERM demo" in seen or "term demo" in seen.lower()
        assert "Luxurious terminal" in seen or "luxurious terminal" in seen.lower()
        assert "F10" in seen
        _f10(con)
        assert con.send_line("PRINT 6*7") == "42"
        assert con.screen_size() == (640, 480)
    finally:
        con.stop()


def test_term_demo_centered_80col_and_cream_text(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        _open_term(con, 'TERM "demo", 23', quiet=0.8, timeout=10.0)
        time.sleep(1.2)
        margin = con.screen_pixel(20, 200)
        assert _is_dark_slate(*margin), margin
        found_cream = False
        for x in (164, 168, 172, 180, 188):
            for y in (8, 24, 40, 200, 248):
                rgb = con.screen_pixel(x, y)
                if _luminance(*rgb) > _luminance(*margin) + 30 and _is_creamish(*rgb):
                    found_cream = True
                    break
            if found_cream:
                break
        assert found_cream, "expected cream text lighter than left margin inside 80-col pane"
        _f10(con)
    finally:
        con.stop()


def test_term_demo_new_text_and_scroll(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        early = _open_term(con, 'TERM "demo", 23', quiet=0.25, timeout=3.5)
        early_nums = _line_numbers(early)
        time.sleep(4.0)
        later = _plain(con.drain(quiet=0.6, timeout=8).decode(errors="replace"))
        later_nums = _line_numbers(later)
        early_max = max(early_nums) if early_nums else 0
        later_max = max(later_nums) if later_nums else 0
        later_count = len(re.findall(r"line\s+\d+", later, re.I))
        assert later_max > early_max or later_count >= 3, (
            f"expected new scrolled lines (early_max={early_max}, later_max={later_max}, "
            f"later_count={later_count})"
        )
        _f10(con)
    finally:
        con.stop()


def test_term_network_host_stays_in_ui_until_f10(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        seen = _open_term(con, 'TERM "127.0.0.1", 23', quiet=0.8, timeout=10.0)
        low = seen.lower()
        assert (
            "network not available" in low
            or "connect failed" in low
            or "dns failed" in low
            or "tcp timeout" in low
            or "tcp refused" in low
        )
        _f10(con)
        assert con.send_line("PRINT 1+1") == "2"
    finally:
        con.stop()


def test_term_wrong_port_does_not_hang(kernel_image):
    """Wrong host/port must enter the TUI quickly so F10 can quit."""
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        t0 = time.monotonic()
        seen = _open_term(con, 'TERM "20forbeers.com", 137', quiet=0.8, timeout=6.0)
        elapsed = time.monotonic() - t0
        assert elapsed < 5.0, elapsed
        low = seen.lower()
        assert (
            "network not available" in low
            or "connect failed" in low
            or "connecting" in low
            or "dns failed" in low
            or "tcp timeout" in low
            or "tcp refused" in low
            or "cancelling" in low
        )
        _f10(con)
        assert con.send_line("PRINT 9") == "9"
    finally:
        con.stop()


def test_term_alt_x_exits(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        _open_term(con, 'TERM "demo", 23', quiet=0.8, timeout=10.0)
        con._ser.sendall(bytes([1]) + b"x")
        _plain(con.drain(quiet=0.8, timeout=15).decode(errors="replace"))
        assert con.send_line("PRINT 6*7") == "42"
    finally:
        con.stop()


def test_term_alt_f_file_menu_then_enter_exits(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        _open_term(con, 'TERM "demo", 23', quiet=0.8, timeout=10.0)
        con._ser.sendall(bytes([1]) + b"f")
        menu = _plain(con.drain(quiet=0.6, timeout=8).decode(errors="replace"))
        assert "File" in menu
        assert "Exit" in menu
        assert "Echo ON" in menu
        assert "Boxed" in menu
        con._ser.sendall(b"\r")
        _plain(con.drain(quiet=0.8, timeout=15).decode(errors="replace"))
        assert con.send_line("PRINT 3+4") == "7"
    finally:
        con.stop()


def test_term_f1_does_not_exit(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        _open_term(con, 'TERM "demo", 23', quiet=0.8, timeout=10.0)
        con._ser.sendall(b"\x1bOP")
        time.sleep(0.4)
        _plain(con.drain(quiet=0.3, timeout=4).decode(errors="replace"))
        assert con.screen_size() == (960, 540)
        con._ser.sendall(b"\x1b[[A")
        time.sleep(0.3)
        assert con.screen_size() == (960, 540)
        _f10(con)
        assert con.send_line("PRINT 8+1") == "9"
        assert con.screen_size() == (640, 480)
    finally:
        con.stop()


def test_term_esc_idle_then_f10(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        _open_term(con, 'TERM "demo", 23', quiet=0.8, timeout=10.0)
        con._ser.sendall(b"\x1b")
        time.sleep(0.15)
        _f10(con)
        assert con.send_line("PRINT 2+2") == "4"
    finally:
        con.stop()


def _max_dump_width(text: str) -> int:
    return max((len(ln.rstrip("\r")) for ln in text.split("\n")), default=0)


def test_term_file_menu_boxed_full_toggle(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        opened = _open_term(con, 'TERM "demo", 23', quiet=0.8, timeout=10.0)
        time.sleep(0.4)
        assert _max_dump_width(opened) == 80
        margin = con.screen_pixel(20, 200)
        assert _is_dark_slate(*margin), margin
        con._ser.sendall(bytes([1]) + b"f")
        menu = _plain(con.drain(quiet=0.6, timeout=8).decode(errors="replace"))
        assert "Boxed" in menu
        con._ser.sendall(b"b")
        full = _plain(con.drain(quiet=0.6, timeout=8).decode(errors="replace"))
        assert "Full" in full
        assert _max_dump_width(full) == 120
        found_cream = False
        for x in (4, 8, 12, 16, 24, 32):
            for y in (4, 8, 12, 20, 24, 40):
                rgb = con.screen_pixel(x, y)
                if _is_creamish(*rgb):
                    found_cream = True
                    break
            if found_cream:
                break
        assert found_cream, "expected cream glyphs at the left edge in full-width mode"
        con._ser.sendall(b"b")
        boxed = _plain(con.drain(quiet=0.6, timeout=8).decode(errors="replace"))
        assert "Boxed" in boxed
        assert _max_dump_width(boxed) == 80
        _f10(con)
        assert con.send_line("PRINT 1+2") == "3"
    finally:
        con.stop()


def test_term_file_menu_echo_toggle(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        _open_term(con, 'TERM "demo", 23', quiet=0.8, timeout=10.0)
        con._ser.sendall(bytes([1]) + b"f")
        menu = _plain(con.drain(quiet=0.6, timeout=8).decode(errors="replace"))
        assert "Echo ON" in menu
        con._ser.sendall(b"e")
        toggled = _plain(con.drain(quiet=0.6, timeout=8).decode(errors="replace"))
        assert "Echo OFF" in toggled
        con._ser.sendall(b"e")
        again = _plain(con.drain(quiet=0.6, timeout=8).decode(errors="replace"))
        assert "Echo ON" in again
        _f10(con)
        assert con.send_line("PRINT 2+3") == "5"
    finally:
        con.stop()


def _wait_demo_idle(con, timeout=12.0):
    t0 = time.monotonic()
    seen = ""
    while time.monotonic() - t0 < timeout:
        seen += _plain(con.drain(quiet=0.35, timeout=2).decode(errors="replace"))
        nums = _line_numbers(seen)
        if nums and max(nums) >= 39:
            time.sleep(0.8)
            seen += _plain(con.drain(quiet=0.4, timeout=2).decode(errors="replace"))
            return seen
    return seen


def test_term_demo_local_echo_and_hide(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        _open_term(con, 'TERM "demo", 23', quiet=0.8, timeout=10.0)
        _wait_demo_idle(con)
        con._ser.sendall(b"ECHOTEST99")
        shown = _plain(con.drain(quiet=0.6, timeout=8).decode(errors="replace"))
        assert "ECHOTEST99" in shown
        con._ser.sendall(bytes([1]) + b"f")
        _plain(con.drain(quiet=0.5, timeout=6).decode(errors="replace"))
        con._ser.sendall(b"e")
        off = _plain(con.drain(quiet=0.5, timeout=6).decode(errors="replace"))
        assert "Echo OFF" in off
        con._ser.sendall(b"\x1b")
        time.sleep(0.12)
        _plain(con.drain(quiet=0.3, timeout=4).decode(errors="replace"))
        con._ser.sendall(b"ECHOHIDE77")
        hidden = _plain(con.drain(quiet=0.6, timeout=8).decode(errors="replace"))
        assert "ECHOHIDE77" not in hidden
        _f10(con)
        assert con.send_line("PRINT 4+4") == "8"
    finally:
        con.stop()


def test_term_double_esc_then_f10(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        _open_term(con, 'TERM "demo", 23', quiet=0.8, timeout=10.0)
        con._ser.sendall(b"\x1b\x1b")
        time.sleep(0.1)
        _f10(con)
        assert con.send_line("PRINT 5+5") == "10"
    finally:
        con.stop()
