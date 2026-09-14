"""TERM full-screen terminal app: syntax, help, demo UI, network failure."""

import re
import time

from harness import MMBasicConsole
from ihelp_util import close_ihelp, dump_topic, open_ihelp, scroll_all


def _plain(s: str) -> str:
    return re.sub(r"\x1b\[[0-9;?]*[A-Za-z]", "", s)


def _apply_slate_theme(con):
    assert con.send_line('OPTION EDIT THEME "Slate"') == ""


def _open_term(con, cmd: str, quiet=0.6, timeout=12.0):
    con.drain(quiet=0.1)
    con._ser.sendall((cmd + "\r").encode())
    return _plain(con.drain(quiet=quiet, timeout=timeout).decode(errors="replace"))


def _f10(con):
    con._ser.sendall(b"\x1b[21~")
    return _plain(con.drain(quiet=0.6, timeout=8).decode(errors="replace"))


def _quit(con):
    con._ser.sendall(bytes([1]) + b"x")
    return _plain(con.drain(quiet=0.8, timeout=15).decode(errors="replace"))


def _menu(con):
    con._ser.sendall(bytes([1]) + b"t")
    return _plain(con.drain(quiet=0.6, timeout=8).decode(errors="replace"))


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


def _is_black(r: int, g: int, b: int) -> bool:
    return r < 16 and g < 16 and b < 16


def _is_creamish(r: int, g: int, b: int) -> bool:
    return r > 80 and g > 75 and b > 65


def _is_vga_grey(r: int, g: int, b: int) -> bool:
    return (
        abs(r - g) <= 24
        and abs(g - b) <= 24
        and 140 <= r <= 200
        and r + g + b < 620
    )


def _line_numbers(text: str) -> list[int]:
    return [int(m) for m in re.findall(r"line\s+(\d+)", text, re.I)]


def test_term_requires_host_and_port(console):
    assert "?SYNTAX ERROR" in console.send_line('TERM "example.com"').upper()
    assert "?SYNTAX ERROR" in console.send_line("TERM 23").upper()
    assert "?SYNTAX ERROR" in console.send_line('TERM "h", 0').upper()
    assert "?SYNTAX ERROR" in console.send_line('TERM "h", 70000').upper()


def test_help_term(console):
    listing = scroll_all(console, open_ihelp(console, "INDEX"))
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
    assert "512" in low and "ring" in low
    assert "drain" in low or "starve" in low or "pane" in low
    assert "idle" in low
    assert "echo" in low
    assert "status" in low
    assert "menu" in low
    assert "backspace" in low
    assert "boxed" in low
    assert "full" in low
    assert "80x25" in low
    assert "bookmark" in low
    assert "replay" in low
    assert "term replay" in low or "replay \"file" in low or "c:/.termlog" in low
    assert "log" in low
    assert "255" in low or "iac" in low
    assert "disconnect" in low or "no argument" in low or "[host" in low
    assert "theme" in low or "editor" in low
    assert "black" in low
    assert "grey" in low or "gray" in low
    assert "restore" in low or "started" in low
    assert "capped" not in low


def test_term_no_args_disconnected(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        _apply_slate_theme(con)
        seen = _open_term(con, "TERM", quiet=0.8, timeout=10.0)
        assert "Disconnected" in seen
        assert "?SYNTAX ERROR" not in seen.upper()
        con._ser.sendall(bytes([1]) + b"t")
        menu = _plain(con.drain(quiet=0.6, timeout=8).decode(errors="replace"))
        assert "Bookmarks" in menu
        con._ser.sendall(b"k")
        listing = _plain(con.drain(quiet=0.6, timeout=8).decode(errors="replace"))
        assert "Bookmarks" in listing
        _quit(con)
        assert con.send_line("PRINT 9*9") == "81"
    finally:
        con.stop()


def test_term_demo_mode14_slate_and_f10(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        _apply_slate_theme(con)
        seen = _open_term(con, 'TERM "demo", 23', quiet=0.8, timeout=10.0)
        time.sleep(0.4)
        assert con.screen_size() == (960, 540)
        r, g, b = con.screen_pixel(40, 200)
        assert _is_black(r, g, b), (r, g, b)
        assert not re.search(r"\.{8,}", seen), seen[:200]
        assert "TERM demo" in seen or "term demo" in seen.lower()
        assert "Luxurious terminal" in seen or "luxurious terminal" in seen.lower()
        assert "Alt-X" in seen or "alt-x" in seen.lower()
        _quit(con)
        assert con.send_line("PRINT 6*7") == "42"
        assert con.screen_size() == (1280, 720)
    finally:
        con.stop()


def test_term_demo_centered_80col_and_grey_text(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        _apply_slate_theme(con)
        _open_term(con, 'TERM "demo", 23', quiet=0.8, timeout=10.0)
        time.sleep(1.2)
        margin = con.screen_pixel(20, 200)
        assert _is_black(*margin), margin
        found_grey = False
        for x in (164, 168, 172, 180, 188):
            for y in (8, 24, 40, 200, 248):
                rgb = con.screen_pixel(x, y)
                if _luminance(*rgb) > _luminance(*margin) + 30 and _is_vga_grey(*rgb):
                    found_grey = True
                    break
            if found_grey:
                break
        assert found_grey, "expected VGA grey text inside 80-col pane, not theme cream"
        _quit(con)
    finally:
        con.stop()


def test_term_pane_background_black_across_themes(kernel_image):
    """Issue #265: pane and letterbox stay black; menu and status keep the theme."""
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        shots = (
            ("Paper", "/opt/cursor/artifacts/issue265_term_black_bg_paper.png"),
            ("Nord", "/opt/cursor/artifacts/issue265_term_black_bg_nord.png"),
            ("Slate", "/opt/cursor/artifacts/issue265_term_black_bg_slate.png"),
        )
        for theme, snap in shots:
            assert con.send_line(f'OPTION EDIT THEME "{theme}"') == ""
            _open_term(con, 'TERM "demo", 23', quiet=0.8, timeout=10.0)
            time.sleep(0.4)
            pane = con.screen_pixel(20, 200)
            assert _is_black(*pane), (theme, pane)
            _menu(con)
            bar = con.screen_pixel(400, 4)
            status = con.screen_pixel(400, 538)
            con.capture_png(snap)
            assert not _is_black(*bar), (theme, bar)
            assert not _is_black(*status), (theme, status)
            assert _luminance(*bar) > _luminance(*pane) + 15, (theme, bar, pane)
            _quit(con)
            assert con.send_line("PRINT 1") == "1"
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
        _quit(con)
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
        _quit(con)
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
        _quit(con)
        assert con.send_line("PRINT 9") == "9"
    finally:
        con.stop()


def test_term_demoburst_hdmi_keeps_scrolled_text(kernel_image):
    """A burst of newlines must paint glyphs before pixel-scroll (issue #190)."""
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        seen = _open_term(con, 'TERM "demoburst", 23', quiet=0.8, timeout=10.0)
        time.sleep(0.5)
        nums = _line_numbers(seen)
        assert nums and max(nums) >= 30, seen[-400:]
        found_grey = False
        for x in (164, 168, 172, 180, 188):
            for y in (200, 248, 320, 360, 400):
                rgb = con.screen_pixel(x, y)
                if _is_vga_grey(*rgb):
                    found_grey = True
                    break
            if found_grey:
                break
        assert found_grey, "expected grey text in the pane after a burst scroll"
        _quit(con)
        assert con.send_line("PRINT 5+6") == "11"
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


def test_term_alt_t_terminal_menu_then_x_exits(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        _open_term(con, 'TERM "demo", 23', quiet=0.8, timeout=10.0)
        menu = _menu(con)
        assert "Terminal" in menu
        assert "File" not in menu
        assert "Exit" in menu
        assert "Echo ON" in menu
        assert "Boxed" in menu
        assert "Bookmarks" in menu
        con._ser.sendall(b"x")
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
        _quit(con)
        assert con.send_line("PRINT 8+1") == "9"
        assert con.screen_size() == (1280, 720)
    finally:
        con.stop()


def test_term_f10_opens_menu_does_not_exit(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        _open_term(con, 'TERM "demo", 23', quiet=0.8, timeout=10.0)
        menu = _f10(con)
        assert "Terminal" in menu
        assert "Exit" in menu
        assert con.screen_size() == (960, 540)
        _quit(con)
        assert con.send_line("PRINT 8+1") == "9"
    finally:
        con.stop()


def test_term_menu_bar_full_width_when_open(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        assert con.send_line('OPTION EDIT THEME "Nord"') == ""
        _open_term(con, 'TERM "demo", 23', quiet=0.8, timeout=10.0)
        time.sleep(0.3)
        closed = con.screen_pixel(8, 4)
        _menu(con)
        title = con.screen_pixel(20, 2)
        empty = con.screen_pixel(400, 4)
        far = con.screen_pixel(940, 4)
        pad_l = _cell_top(con, 0, 0)
        pad_r = _cell_top(con, 9, 0)
        assert _luminance(*empty) > _luminance(*closed) + 20, (empty, closed)
        assert abs(empty[0] - far[0]) < 40
        assert abs(empty[1] - far[1]) < 40
        assert abs(empty[2] - far[2]) < 40
        assert abs(_luminance(*title) - _luminance(*empty)) > 20, (title, empty)
        assert _rgb_dist(pad_l, pad_r) < 40, (pad_l, pad_r)
        assert _rgb_dist(pad_l, empty) > 40, (pad_l, empty)
        drop = con.screen_pixel(24, 24)
        letterbox = con.screen_pixel(8, 200)
        assert _luminance(*drop) > _luminance(*letterbox) + 10, (drop, letterbox)
        stroke = con.screen_pixel(16, 16 + 7)
        pad = con.screen_pixel(16, 16 + 2)
        assert abs(_luminance(*stroke) - _luminance(*pad)) > 30, (stroke, pad)
        _quit(con)
        assert con.send_line("PRINT 1+1") == "2"
    finally:
        con.stop()


def test_term_menu_clears_letterbox_when_closed(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        assert con.send_line('OPTION EDIT THEME "Nord"') == ""
        _open_term(con, 'TERM "demo", 23', quiet=0.8, timeout=10.0)
        time.sleep(0.3)
        letterbox = con.screen_pixel(8, 200)
        closed_bar = con.screen_pixel(940, 4)
        closed_drop = con.screen_pixel(24, 24)
        menu = _menu(con)
        assert "Terminal" in menu
        assert "Exit" in menu
        bar = con.screen_pixel(940, 4)
        drop = con.screen_pixel(24, 24)
        con.capture_png("/opt/cursor/artifacts/term_menu_letterbox_open.png")
        assert _luminance(*bar) > _luminance(*closed_bar) + 20, (bar, closed_bar)
        assert _luminance(*drop) > _luminance(*letterbox) + 10, (drop, letterbox)
        con._ser.sendall(b"\x1b\x1b")
        _plain(con.drain(quiet=0.6, timeout=8).decode(errors="replace"))
        after_bar = con.screen_pixel(940, 4)
        after_drop = con.screen_pixel(24, 24)
        after_letterbox = con.screen_pixel(8, 200)
        con.capture_png("/opt/cursor/artifacts/term_menu_letterbox_closed.png")
        assert _rgb_dist(after_bar, closed_bar) < 40, (after_bar, closed_bar)
        assert _rgb_dist(after_drop, closed_drop) < 40, (after_drop, closed_drop)
        assert _rgb_dist(after_letterbox, letterbox) < 40, (after_letterbox, letterbox)
        assert _luminance(*after_bar) < _luminance(*bar) - 20, (after_bar, bar)
        assert _luminance(*after_drop) < _luminance(*drop) - 10, (after_drop, drop)
        _quit(con)
        assert con.send_line("PRINT 3+5") == "8"
    finally:
        con.stop()


def _cell_top(con, col, row):
    return con.screen_pixel(col * 8 + 4, row * 16 + 1)


def _rgb_dist(a, b):
    return abs(a[0] - b[0]) + abs(a[1] - b[1]) + abs(a[2] - b[2])


def test_term_menu_colours_match_edit(kernel_image):
    """TERM menus use the same theme slots as EDIT (menu/sel/dlg/hot)."""
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        assert con.send_line('OPTION EDIT THEME "Slate"') == ""
        con.drain(quiet=0.1)
        con._ser.sendall(b'EDIT "MENU.BAS"\r')
        _plain(con.drain(quiet=0.8, timeout=10).decode(errors="replace"))
        con._ser.sendall(bytes([1]) + b"f")
        _plain(con.drain(quiet=0.6, timeout=8).decode(errors="replace"))
        ed_bar = _cell_top(con, 40, 0)
        ed_title = _cell_top(con, 2, 0)
        ed_sel = _cell_top(con, 2, 2)
        ed_uns = _cell_top(con, 2, 3)
        con.capture_png("/opt/cursor/artifacts/edit_file_menu_slate.png")
        con._ser.sendall(bytes([1]) + b"x")
        _plain(con.drain(quiet=0.8, timeout=15).decode(errors="replace"))
        assert con.send_line("PRINT 1") == "1"

        _open_term(con, 'TERM "demo", 23', quiet=0.8, timeout=10.0)
        _menu(con)
        tm_bar = _cell_top(con, 40, 0)
        tm_title = _cell_top(con, 2, 0)
        tm_sel = _cell_top(con, 2, 2)
        tm_uns = _cell_top(con, 2, 3)
        con.capture_png("/opt/cursor/artifacts/term_terminal_menu_slate.png")
        assert _rgb_dist(ed_bar, tm_bar) < 40, (ed_bar, tm_bar)
        assert _rgb_dist(ed_title, tm_title) < 40, (ed_title, tm_title)
        assert _rgb_dist(ed_sel, tm_sel) < 40, (ed_sel, tm_sel)
        assert _rgb_dist(ed_uns, tm_uns) < 40, (ed_uns, tm_uns)
        assert _rgb_dist(tm_sel, tm_uns) > 80, (tm_sel, tm_uns)
        assert _rgb_dist(tm_title, tm_bar) > 80, (tm_title, tm_bar)
        uns_row = [con.screen_pixel(x, 3 * 16 + 8) for x in range(16, 96, 4)]
        uns_luma = [_luminance(*p) for p in uns_row]
        assert max(uns_luma) - min(uns_luma) > 70, (uns_row[:6], uns_luma[:6])
        _quit(con)
        assert con.send_line("PRINT 2") == "2"
    finally:
        con.stop()


def test_term_slate_menu_bar_survives_8bit_mode(kernel_image):
    """8-bit RGB332 crushes Slate greys to black; TERM must use 16-bit."""
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        assert con.send_line('OPTION EDIT THEME "Slate"') == ""
        assert con.send_line("MODE 11,8") == ""
        con.drain(quiet=0.1)
        con._ser.sendall(b'EDIT "MENU.BAS"\r')
        _plain(con.drain(quiet=0.8, timeout=10).decode(errors="replace"))
        con._ser.sendall(bytes([1]) + b"f")
        _plain(con.drain(quiet=0.6, timeout=8).decode(errors="replace"))
        ed_bar = _cell_top(con, 40, 0)
        con.capture_png("/opt/cursor/artifacts/edit_slate_menu_8bit.png")
        con._ser.sendall(bytes([1]) + b"x")
        _plain(con.drain(quiet=0.8, timeout=15).decode(errors="replace"))
        assert con.send_line("PRINT 1") == "1"
        _open_term(con, 'TERM "demo", 23', quiet=0.8, timeout=10.0)
        _menu(con)
        tm_bar = _cell_top(con, 40, 0)
        con.capture_png("/opt/cursor/artifacts/term_slate_menu_8bit.png")
        assert tm_bar[0] + tm_bar[1] + tm_bar[2] > 30, tm_bar
        assert _rgb_dist(ed_bar, tm_bar) < 50, (ed_bar, tm_bar)
        _quit(con)
        assert con.send_line("PRINT 2") == "2"
    finally:
        con.stop()


def test_term_status_bar_on_last_scanline(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        assert con.send_line('OPTION EDIT THEME "Nord"') == ""
        _open_term(con, 'TERM "demo", 23', quiet=0.8, timeout=10.0)
        menu = _menu(con)
        assert "Terminal" in menu
        w, h = con.screen_size()
        assert (w, h) == (960, 540)
        bottom = con.screen_pixel(400, h - 2)
        letterbox = con.screen_pixel(8, 200)
        con.capture_png("/opt/cursor/artifacts/term_status_last_line.png")
        assert _luminance(*bottom) > _luminance(*letterbox) + 20, (bottom, letterbox)
        _quit(con)
        assert con.send_line("PRINT 1+1") == "2"
    finally:
        con.stop()


def test_term_esc_idle_then_quit(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        _open_term(con, 'TERM "demo", 23', quiet=0.8, timeout=10.0)
        con._ser.sendall(b"\x1b")
        time.sleep(0.15)
        _quit(con)
        assert con.send_line("PRINT 2+2") == "4"
    finally:
        con.stop()


def _max_dump_width(text: str) -> int:
    return max((len(ln.replace("\r", "")) for ln in text.split("\n")), default=0)


def test_term_file_menu_boxed_full_toggle(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        _apply_slate_theme(con)
        assert con.send_line("MODE 8,16") == ""
        opened = _open_term(con, 'TERM "demo", 23', quiet=0.8, timeout=10.0)
        time.sleep(0.4)
        assert _max_dump_width(opened) == 80
        margin = con.screen_pixel(20, 200)
        assert _is_black(*margin), margin
        menu = _menu(con)
        assert "Boxed" in menu
        con._ser.sendall(b"w")
        full = _plain(con.drain(quiet=0.6, timeout=8).decode(errors="replace"))
        assert "Full" in full
        assert con.screen_size() == (640, 480)
        assert _max_dump_width(full) == 80
        found_grey = False
        for x in (4, 8, 12, 16, 24, 32):
            for y in (4, 8, 12, 20, 24, 40):
                rgb = con.screen_pixel(x, y)
                if _is_vga_grey(*rgb):
                    found_grey = True
                    break
            if found_grey:
                break
        assert found_grey, "expected grey glyphs at the left edge in full-width mode"
        con._ser.sendall(b"w")
        boxed = _plain(con.drain(quiet=0.6, timeout=8).decode(errors="replace"))
        assert "Boxed" in boxed
        assert _max_dump_width(boxed) == 80
        assert con.screen_size() == (960, 540)
        _quit(con)
        assert con.send_line("PRINT 1+2") == "3"
    finally:
        con.stop()


def test_term_full_keeps_mode14_if_started_there(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        assert con.send_line("MODE 14,16") == ""
        assert con.screen_size() == (960, 540)
        _open_term(con, 'TERM "demo", 23', quiet=0.8, timeout=10.0)
        time.sleep(0.3)
        assert con.screen_size() == (960, 540)
        con._ser.sendall(bytes([1]) + b"t")
        _plain(con.drain(quiet=0.5, timeout=8).decode(errors="replace"))
        con._ser.sendall(b"w")
        full = _plain(con.drain(quiet=0.6, timeout=8).decode(errors="replace"))
        assert "Full" in full
        assert con.screen_size() == (960, 540)
        assert _max_dump_width(full) == 120
        _quit(con)
        assert con.send_line("PRINT 9+1") == "10"
    finally:
        con.stop()


def test_term_full_uses_wide_start_mode(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        assert con.send_line("MODE 9,16") == ""
        assert con.screen_size() == (1024, 768)
        _open_term(con, 'TERM "demo", 23', quiet=0.8, timeout=10.0)
        time.sleep(0.3)
        con._ser.sendall(bytes([1]) + b"t")
        _plain(con.drain(quiet=0.5, timeout=8).decode(errors="replace"))
        con._ser.sendall(b"w")
        full = _plain(con.drain(quiet=0.6, timeout=8).decode(errors="replace"))
        assert "Full" in full
        assert con.screen_size() == (1024, 768)
        assert _max_dump_width(full) == 128
        _quit(con)
        assert con.send_line("PRINT 8+2") == "10"
    finally:
        con.stop()


def test_term_file_menu_echo_toggle(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        _open_term(con, 'TERM "demo", 23', quiet=0.8, timeout=10.0)
        con._ser.sendall(bytes([1]) + b"t")
        menu = _plain(con.drain(quiet=0.6, timeout=8).decode(errors="replace"))
        assert "Echo ON" in menu
        con._ser.sendall(b"e")
        toggled = _plain(con.drain(quiet=0.6, timeout=8).decode(errors="replace"))
        assert "Echo OFF" in toggled
        con._ser.sendall(b"e")
        again = _plain(con.drain(quiet=0.6, timeout=8).decode(errors="replace"))
        assert "Echo ON" in again
        _quit(con)
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
        con._ser.sendall(b"\x08" * 20)
        rubbed = _plain(con.drain(quiet=0.6, timeout=8).decode(errors="replace"))
        assert "ECHOTEST99" not in rubbed
        assert "line " in rubbed.lower()
        con._ser.sendall(bytes([1]) + b"t")
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
        _quit(con)
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
        _quit(con)
        assert con.send_line("PRINT 5+5") == "10"
    finally:
        con.stop()


def _termconfig_path(con):
    for path in ("A:/.termconfig", "C:/.termconfig"):
        out = con.send_line(f'OPEN "{path}" FOR INPUT AS #1')
        con.send_line("CLOSE #1")
        if out == "":
            return path
    return None


def _read_termconfig(con, path):
    assert con.send_line("NEW") == ""
    assert con.send_line(f'10 OPEN "{path}" FOR INPUT AS #1') == ""
    assert con.send_line("20 IF EOF(#1) THEN GOTO 70") == ""
    assert con.send_line("30 LINE INPUT #1, A$") == ""
    assert con.send_line("40 PRINT A$") == ""
    assert con.send_line("50 GOTO 20") == ""
    assert con.send_line("70 CLOSE #1") == ""
    return con.send_line("RUN", timeout=8)


def test_term_bookmarks_new_persist_delete(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        _open_term(con, 'TERM "demo", 23', quiet=0.8, timeout=10.0)
        con._ser.sendall(bytes([1]) + b"t")
        menu = _plain(con.drain(quiet=0.6, timeout=8).decode(errors="replace"))
        assert "Bookmarks" in menu
        con._ser.sendall(b"k")
        listing = _plain(con.drain(quiet=0.6, timeout=8).decode(errors="replace"))
        assert "Bookmarks" in listing
        assert "New" in listing
        assert "Connect" in listing
        con._ser.sendall(b"\x1b[D\x1b[D\x1b[D")
        _plain(con.drain(quiet=0.35, timeout=6).decode(errors="replace"))
        con._ser.sendall(b"\r")
        form = _plain(con.drain(quiet=0.6, timeout=8).decode(errors="replace"))
        assert "Name" in form
        assert "Save" in form
        con._ser.sendall(b"Alpha")
        con._ser.sendall(b"\x1b[B")
        _plain(con.drain(quiet=0.3, timeout=4).decode(errors="replace"))
        con._ser.sendall(b"demo")
        con._ser.sendall(bytes([1]) + b"s")
        saved = _plain(con.drain(quiet=0.6, timeout=8).decode(errors="replace"))
        assert "Alpha" in saved
        con._ser.sendall(b"\x1b[D")
        _plain(con.drain(quiet=0.3, timeout=4).decode(errors="replace"))
        con._ser.sendall(b"\r")
        confirm = _plain(con.drain(quiet=0.6, timeout=8).decode(errors="replace"))
        assert "Delete" in confirm
        con._ser.sendall(b"\r")
        gone = _plain(con.drain(quiet=0.6, timeout=8).decode(errors="replace"))
        assert "Alpha" not in gone
        con._ser.sendall(bytes([1]) + b"t")
        _plain(con.drain(quiet=0.4, timeout=6).decode(errors="replace"))
        con._ser.sendall(b"k")
        _plain(con.drain(quiet=0.5, timeout=6).decode(errors="replace"))
        con._ser.sendall(b"\x1b[D\x1b[D\x1b[D")
        _plain(con.drain(quiet=0.3, timeout=4).decode(errors="replace"))
        con._ser.sendall(b"\r")
        _plain(con.drain(quiet=0.5, timeout=6).decode(errors="replace"))
        con._ser.sendall(b"Beta")
        con._ser.sendall(b"\x1b[B")
        _plain(con.drain(quiet=0.25, timeout=4).decode(errors="replace"))
        con._ser.sendall(b"demo")
        con._ser.sendall(bytes([1]) + b"s")
        saved2 = _plain(con.drain(quiet=0.6, timeout=8).decode(errors="replace"))
        assert "Beta" in saved2
        _quit(con)
        path = _termconfig_path(con)
        assert path, "expected A:/.termconfig or C:/.termconfig"
        ini = _read_termconfig(con, path)
        assert "Beta" in ini
        assert "demo" in ini
        assert "letterboxed" in ini.lower() or "letterboxed=" in ini.lower()
        _open_term(con, 'TERM "demo", 23', quiet=0.8, timeout=10.0)
        con._ser.sendall(bytes([1]) + b"t")
        _plain(con.drain(quiet=0.5, timeout=6).decode(errors="replace"))
        con._ser.sendall(b"k")
        again = _plain(con.drain(quiet=0.6, timeout=8).decode(errors="replace"))
        assert "Beta" in again
        _quit(con)
        assert con.send_line("PRINT 7+8") == "15"
    finally:
        con.stop()


def test_term_bookmark_80x25_mode(kernel_image):
    """Issue #287: bookmark 80x25 uses MODE 2 with a full 80x25 pane."""
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        _open_term(con, 'TERM "demo", 23', quiet=0.8, timeout=10.0)
        con._ser.sendall(bytes([1]) + b"t")
        _plain(con.drain(quiet=0.5, timeout=6).decode(errors="replace"))
        con._ser.sendall(b"k")
        _plain(con.drain(quiet=0.5, timeout=6).decode(errors="replace"))
        con._ser.sendall(b"\x1b[D\x1b[D\x1b[D")
        _plain(con.drain(quiet=0.3, timeout=4).decode(errors="replace"))
        con._ser.sendall(b"\r")
        form = _plain(con.drain(quiet=0.6, timeout=8).decode(errors="replace"))
        assert "Use 80x25 mode" in form
        con._ser.sendall(b"Eighty")
        con._ser.sendall(b"\x1b[B")
        _plain(con.drain(quiet=0.25, timeout=4).decode(errors="replace"))
        con._ser.sendall(b"demo")
        for _ in range(4):
            con._ser.sendall(b"\x1b[B")
            _plain(con.drain(quiet=0.15, timeout=3).decode(errors="replace"))
        con._ser.sendall(b" ")
        checked = _plain(con.drain(quiet=0.4, timeout=6).decode(errors="replace"))
        assert "[X] Use 80x25 mode" in checked
        con._ser.sendall(bytes([1]) + b"s")
        saved = _plain(con.drain(quiet=0.6, timeout=8).decode(errors="replace"))
        assert "Eighty" in saved
        con._ser.sendall(b"\r")
        connected = _plain(con.drain(quiet=1.0, timeout=12).decode(errors="replace"))
        assert con.screen_size() == (640, 400)
        rows_80 = [
            ln
            for ln in connected.replace("\r", "").split("\n")
            if len(ln) == 80
        ]
        assert len(rows_80) >= 25, len(rows_80)
        con._ser.sendall(bytes([1]) + b"t")
        menu = _plain(con.drain(quiet=0.5, timeout=6).decode(errors="replace"))
        assert "80x25" in menu
        con.capture_png("/opt/cursor/artifacts/term_80x25_mode.png")
        _quit(con)
        path = _termconfig_path(con)
        assert path
        ini = _read_termconfig(con, path)
        assert "mode80x25=1" in ini.replace(" ", "")
        assert con.send_line("PRINT 2+2") == "4"
    finally:
        con.stop()


def test_term_demoiac_glyphs_and_commands(kernel_image):
    """0xFF is IAC only after a command peek; CP437 0xFF must not freeze the pane."""
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        con.drain(quiet=0.1)
        con._ser.sendall(b'TERM "demoiac", 23\r')
        raw = b""
        t0 = time.monotonic()
        while time.monotonic() - t0 < 10.0:
            raw += con.drain(quiet=0.35, timeout=2)
            if b"KEEP" in raw and b"GO" in raw and b"TY" in raw:
                break
        assert b"KEEP" in raw
        assert b"G1:" in raw
        assert b"OK" in raw
        assert b"G3:" in raw and b"X" in raw
        assert b"TY" in raw
        assert b"GO" in raw
        assert b"\xfa" in raw
        assert b"\xf0" in raw
        assert b"\x80" in raw
        _quit(con)
        assert con.send_line("PRINT 1+2") == "3"
    finally:
        con.stop()


def _pane_grey_hits(con, col, row, pane_left=20):
    hits = 0
    x0 = (pane_left + col) * 8
    y0 = row * 16
    for y in range(2, 15, 2):
        for x in range(1, 7, 2):
            if _is_vga_grey(*con.screen_pixel(x0 + x, y0 + y)):
                hits += 1
    return hits


def test_term_pane_grey_independent_of_phosphor_theme(kernel_image):
    """Incoming pane text stays VGA grey even when the editor theme is green."""
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        assert con.send_line('OPTION EDIT THEME "Phosphor"') == ""
        _open_term(con, 'TERM "demo", 23', quiet=0.8, timeout=10.0)
        time.sleep(0.5)
        found_grey = False
        found_green = False
        for x in (164, 168, 172, 180, 188):
            for y in (8, 24, 40):
                rgb = con.screen_pixel(x, y)
                if _is_vga_grey(*rgb):
                    found_grey = True
                r, g, b = rgb
                if g > r + 40 and g > b + 40 and g > 80:
                    found_green = True
        con.capture_png("/opt/cursor/artifacts/term_pane_grey_phosphor.png")
        assert found_grey, "expected VGA grey incoming text under Phosphor"
        assert not found_green, "pane text must not use Phosphor green"
        _menu(con)
        bar = con.screen_pixel(400, 4)
        assert not _is_black(*bar), bar
        _quit(con)
        assert con.send_line("PRINT 1") == "1"
    finally:
        con.stop()


def test_term_block_cursor_on_when_disconnected(kernel_image):
    """Block cursor is on by default (host may later send CSI ?25l)."""
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        _apply_slate_theme(con)
        seen = _open_term(con, "TERM", quiet=0.8, timeout=10.0)
        assert "Disconnected" in seen
        hits = _pane_grey_hits(con, 0, 1)
        con.capture_png("/opt/cursor/artifacts/term_default_cursor_on.png")
        assert hits >= 12, hits
        _quit(con)
        assert con.send_line("PRINT 2") == "2"
    finally:
        con.stop()
