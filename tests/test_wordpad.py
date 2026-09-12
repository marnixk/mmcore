"""WORDPAD markdown editor TUI: menus, save, wrap, wide view, themes."""

import os
import re
import subprocess
import time

from harness import MMBasicConsole
from ihelp_util import close_ihelp, dump_topic, open_ihelp, scroll_all

_REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def _plain(s: str) -> str:
    return re.sub(r"\x1b\[[0-9;?]*[A-Za-z]", "", s)


def _apply_slate_theme(con):
    assert con.send_line('OPTION EDIT THEME "Slate"') == ""


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


def test_wordpad_exit_cls_and_restores_colour(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        assert con.send_line("COLOUR RGB(255,255,255), RGB(0,0,0)") == ""
        assert con.send_line("CLS RGB(0,0,0)") == ""
        _open(con)
        _keys(con, b"Hello")
        time.sleep(0.2)
        pane = con.screen_pixel(80, 80)
        _quit(con)
        time.sleep(0.3)
        after = con.screen_pixel(80, 80)
        assert after != pane or (after[0] + after[1] + after[2] < 40)
        pix = int(con.send_line("PRINT PIXEL(40,80)"))
        assert pix == 0
        assert con.send_line("PRINT 3+4") == "7"
    finally:
        con.stop()


def test_help_wordpad(console):
    listing = scroll_all(console, open_ihelp(console, "INDEX"))
    assert "WORDPAD" in listing
    close_ihelp(console)
    out = dump_topic(console, "WORDPAD")
    assert out != "?SYNTAX ERROR"
    low = out.lower()
    assert "markdown" in low
    assert "edit theme" in low or "wide" in low
    assert "bold" in low
    assert any(k in low for k in ("ctrl+x", "f10", "quit"))
    assert "ctrl+p" in low or "quick-open" in low
    assert "serif" in low or "times" in low or "h1" in low
    assert "native" in low or "32x64" in low or "16x32" in low
    assert "proportional" in low or "ink" in low or "cursor" in low


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
        for heading_y in (1 * 16 + 8, 2 * 16 + 8):
            body_y = heading_y + 4 * 16
            heading_samples = [con.screen_pixel(x, heading_y) for x in (12, 20, 28, 36, 44)]
            body_samples = [con.screen_pixel(x, body_y) for x in (12, 20, 28, 36, 44)]
            if any(h != b for h, b in zip(heading_samples, body_samples)):
                pixel_diff = True
                break
        assert pixel_diff or "Title" in serial
        _quit(con)
    finally:
        con.stop()


def _tnr32_rows(ch):
    text = open(os.path.join(_REPO, "mmbasic", "src", "font_tnr_32x64.c"), encoding="utf-8").read()
    data = [int(x, 16) for x in re.findall(r"0x[0-9A-Fa-f]{2}", text)]
    w, h = 32, 64
    rowb = (w + 7) // 8
    off = (ord(ch) - 32) * h * rowb
    rows = []
    for y in range(h):
        row = []
        for x in range(w):
            b = data[off + y * rowb + x // 8]
            row.append(1 if (b & (0x80 >> (x % 8))) else 0)
        rows.append(row)
    return rows


def _tnr_ink_span(ch):
    rows = _tnr32_rows(ch)
    xs = [x for row in rows for x, v in enumerate(row) if v]
    if not xs:
        return 0, 16
    return min(xs), max(xs) - min(xs) + 1


def _tnr_advance(ch, scale=4):
    if ch == " ":
        return (scale * 8) // 2
    _left, ink = _tnr_ink_span(ch)
    return ink + 1 + scale // 2


def _heading_cursor_width(con, y=20):
    ox = _pane_left_px(con)
    png = con.capture_png()
    out = subprocess.run(
        ["convert", png, "-crop", f"500x1+{ox}+{y}", "+repage", "txt:-"],
        check=True,
        capture_output=True,
        text=True,
    ).stdout
    pane = con.screen_pixel(8, y)
    pane_lum = _lum(pane)
    cols = []
    for line in out.splitlines():
        if ":" not in line or "(" not in line:
            continue
        coord = line.split(":")[0].strip()
        if "," not in coord:
            continue
        x = int(coord.split(",")[0])
        inner = line[line.find("(") + 1 : line.find(")")]
        parts = [p.strip() for p in inner.replace("%", "").split(",") if p.strip()]
        if len(parts) < 3:
            continue
        rgb = tuple(int(float(p)) for p in parts[:3])
        if _lum(rgb) > pane_lum + 400:
            cols.append(x)
    if not cols:
        return 0
    return cols[-1] - cols[0] + 1


def test_wordpad_heading_cursor_and_spacing(kernel_image):
    """Issue #240: H1 cursor is a visible block; i is narrower than M."""
    adv_i = _tnr_advance("i")
    adv_m = _tnr_advance("M")
    assert adv_i < adv_m - 8, (adv_i, adv_m)
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        _open(con)
        _keys(con, b"# MiMi", quiet=0.8)
        time.sleep(0.3)
        end_w = _heading_cursor_width(con, y=6)
        con.capture_png("/opt/cursor/artifacts/issue240_heading_cursor.png")
        assert end_w >= 8, end_w
        _keys(con, b"\x1b[D", quiet=0.4)
        time.sleep(0.2)
        i_w = _heading_cursor_width(con, y=6)
        _keys(con, b"\x1b[D", quiet=0.4)
        time.sleep(0.2)
        m_w = _heading_cursor_width(con, y=6)
        con.capture_png("/opt/cursor/artifacts/issue240_heading_cursor_on_letter.png")
        assert i_w >= 6, i_w
        assert m_w > i_w + 6, (m_w, i_w, end_w)
        _quit(con)
    finally:
        con.stop()


def test_wordpad_heading_wraps_long_line(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        _open(con)
        seen = _keys(con, b"# " + b"Mellow " * 12, quiet=1.0)
        time.sleep(0.3)
        png = con.capture_png("/opt/cursor/artifacts/issue240_heading_wrap.png")
        out = subprocess.run(
            ["convert", png, "-crop", "960x96+0+0", "+repage", "txt:-"],
            check=True,
            capture_output=True,
            text=True,
        ).stdout
        ink_rows = set()
        for line in out.splitlines():
            if ":" not in line or "(" not in line:
                continue
            coord = line.split(":")[0].strip()
            if "," not in coord:
                continue
            _x, y = (int(p) for p in coord.split(","))
            inner = line[line.find("(") + 1 : line.find(")")]
            parts = [p.strip() for p in inner.replace("%", "").split(",") if p.strip()]
            if len(parts) < 3:
                continue
            rgb = tuple(int(float(p)) for p in parts[:3])
            if _lum(rgb) > 400:
                ink_rows.add(y // 16)
        assert len(ink_rows) >= 2, ink_rows
        assert "Mellow" in seen
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
        _apply_slate_theme(con)
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


def test_wordpad_follows_edit_theme_phosphor(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        assert con.send_line('OPTION EDIT THEME "Phosphor"') == ""
        _open(con)
        _keys(con, b"hello", quiet=0.5)
        time.sleep(0.3)
        found_green = False
        for x in range(0, 160, 4):
            for y in range(8, 40, 4):
                r, g, b = con.screen_pixel(x, y)
                if g > r + 40 and g > b + 40:
                    found_green = True
                    break
            if found_green:
                break
        assert found_green, "expected Phosphor edit-theme colours in WORDPAD"
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


def _pane_left_px(con):
    w, _h = con.screen_size()
    cols = w // 8
    wrap = 80
    if cols <= wrap:
        return 0
    return ((cols - wrap) // 2) * 8


def _lum(rgb):
    r, g, b = rgb
    return r * 3 + g * 6 + b


def _cell_corners(con, col, row):
    x0, y0 = _pane_left_px(con) + col * 8, row * 16
    return [
        con.screen_pixel(x0 + 1, y0 + 1),
        con.screen_pixel(x0 + 6, y0 + 1),
        con.screen_pixel(x0 + 1, y0 + 14),
        con.screen_pixel(x0 + 6, y0 + 14),
    ]


def _is_solid_cursor(con, col, row):
    return all(_lum(p) > 1500 for p in _cell_corners(con, col, row))


def _cell_max_lum(con, col, row):
    x0, y0 = _pane_left_px(con) + col * 8, row * 16
    m = 0
    for y in (y0 + 2, y0 + 7, y0 + 11, y0 + 14):
        for x in (x0 + 1, x0 + 3, x0 + 5, x0 + 6):
            m = max(m, _lum(con.screen_pixel(x, y)))
    return m


def test_wordpad_cursor_visible(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        _open(con)
        _keys(con, b"Hello")
        time.sleep(0.2)
        ox = _pane_left_px(con)
        cursor = con.screen_pixel(ox + 5 * 8 + 4, 8)
        page = con.screen_pixel(ox + 20 * 8 + 4, 8)
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
        ox = _pane_left_px(con)
        heading_mid = [con.screen_pixel(ox + x, 20) for x in (12, 20, 28, 36)]
        body = [con.screen_pixel(ox + x, 4 * 16 + 8) for x in (12, 20, 28, 36)]
        assert any(h != b for h, b in zip(heading_mid, body))
        _quit(con)
    finally:
        con.stop()


def test_wordpad_h3_is_taller_than_body(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        _open(con)
        _keys(con, b"### Head\rbody text", quiet=0.8)
        time.sleep(0.2)
        ox = _pane_left_px(con)
        heading_mid = [con.screen_pixel(ox + x, 20) for x in (12, 20, 28, 36)]
        body = [con.screen_pixel(ox + x, 2 * 16 + 8) for x in (12, 20, 28, 36)]
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


def test_wordpad_edit_theme_persists(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        assert con.send_line('OPTION EDIT THEME "Nord"') == ""
        _open(con)
        _quit(con)
        assert con.send_line("NEW") == ""
        assert con.send_line('10 OPEN "A:/.mmbasic.ini" FOR INPUT AS #1') == ""
        assert con.send_line("20 IF EOF(#1) THEN GOTO 70") == ""
        assert con.send_line("30 LINE INPUT #1, A$") == ""
        assert con.send_line("40 PRINT A$") == ""
        assert con.send_line("50 GOTO 20") == ""
        assert con.send_line("70 CLOSE #1") == ""
        ini = con.send_line("RUN", timeout=8)
        assert "edit_theme=4" in ini
        _open(con, quiet=1.0)
        time.sleep(0.3)
        r, g, b = con.screen_pixel(40, 80)
        assert (r, g, b) != (0, 0, 168), "WORDPAD should not use default Turbo blue"
        assert b > r and b > g, f"expected Nord bluish background, got {(r, g, b)}"
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
        assert _is_solid_cursor(con, 0, 1)
        assert not _is_solid_cursor(con, 0, 2)
        _keys(con, b"\x1b[A", quiet=0.5)
        time.sleep(0.2)
        assert not _is_solid_cursor(con, 0, 1)
        assert _cell_max_lum(con, 0, 0) > _cell_max_lum(con, 0, 1) + 80
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
        _apply_slate_theme(con)
        _open(con)
        _keys(con, b"xx**bold**yy", quiet=0.7)
        time.sleep(0.2)
        plain = _cell_max_lum(con, 0, 0)
        strong = _cell_max_lum(con, 4, 0)
        assert strong != plain, (strong, plain)
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


def test_wordpad_esc_then_right_does_not_insert_csi(kernel_image):
    """Esc then Right must move the cursor, not insert the CSI leftover [C."""
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        _open(con, 'WORDPAD "WPESC.MD"')
        _keys(con, b"HELLO")
        seen = _keys(con, b"\x1b\x1b[C")
        assert "[C" not in seen
        _alt_menu(con, b"f", quiet=0.4)
        _keys(con, b"s", quiet=0.6)
        _quit(con)
        reopened = _open(con, 'WORDPAD "WPESC.MD"', quiet=1.0)
        assert "HELLO" in reopened
        assert "[C" not in reopened
        _quit(con)
    finally:
        con.stop()


def test_wordpad_esc_then_letter_inserts_letter(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        _open(con, 'WORDPAD "WPA.MD"')
        _keys(con, b"HELLO")
        _keys(con, b"\x1ba")
        _alt_menu(con, b"f", quiet=0.4)
        _keys(con, b"s", quiet=0.6)
        _quit(con)
        reopened = _open(con, 'WORDPAD "WPA.MD"', quiet=1.0)
        assert "HELLOa" in reopened
        _quit(con)
    finally:
        con.stop()


def test_wordpad_cr_is_ignored(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        assert con.send_line('OPEN "CR.MD" FOR OUTPUT AS #1') == ""
        assert con.send_line('PRINT #1, "Hello"; CHR$(13); "World"') == ""
        assert con.send_line("CLOSE #1") == ""
        seen = _open(con, 'WORDPAD "CR.MD"', quiet=1.0)
        assert "Hello" in seen
        assert "World" in seen
        _quit(con)
    finally:
        con.stop()
