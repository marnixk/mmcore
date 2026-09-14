"""Boot banner: version, PicoMite copyright, HELP in bright white, two blank lines."""

import os
import subprocess
import time

from harness import MMBasicConsole

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def _mmb_version():
    return subprocess.check_output(
        ["git", "describe", "--tags", "--always"],
        cwd=REPO,
        text=True,
    ).strip()


def _pixels(png: str):
    out = subprocess.run(
        ["convert", png, "txt:-"],
        check=True,
        capture_output=True,
        text=True,
    ).stdout
    grey = []
    bright = []
    for line in out.splitlines():
        if line.startswith("#") or "#000000" in line:
            continue
        xy = line.split(":", 1)[0]
        x, y = (int(p) for p in xy.split(","))
        if "#F8FCF8" in line or "#FFFFFF" in line:
            bright.append((x, y))
        elif "#A8A8A8" in line or "#AAAAAA" in line:
            grey.append((x, y))
    return grey, bright


def test_startup_copyright_banner(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        raw = con.boot_log
        text = raw.decode(errors="replace")
        low = text.lower()
        ver = _mmb_version()
        assert f"mmbasic {ver}".lower() in low
        assert "copyright 2011-2026 geoff graham" in low
        assert "copyright 2016-2026 peter mather" in low
        assert "adapted and extended by marnix kok" in low
        assert "type " in low
        assert "get started" in low
        assert b"\x1b[97mHELP\x1b[37m" in raw
        idx = raw.find(b"get started.")
        assert idx >= 0
        after = raw[idx + len(b"get started.") :]
        prompt_at = after.find(b">")
        assert prompt_at >= 0
        gap = after[:prompt_at].replace(b"\r", b"")
        assert gap.count(b"\n") >= 3

        png = con.capture_png("/opt/cursor/artifacts/issue67_startup_banner.png")
        grey, bright = _pixels(png)
        assert grey, "copyright text should be dim white"
        assert bright, "HELP should be bright white"
        help_y = min(y for _, y in bright)
        type_luma = 0
        help_luma = 0
        for x, y in grey:
            if help_y <= y <= help_y + 16 and x < 40:
                type_luma = 168 * 3
                break
        for x, y in bright:
            if help_y <= y <= help_y + 16 and 40 <= x < 72:
                help_luma = 248 * 3
                break
        assert type_luma and help_luma, (help_y, type_luma, help_luma)
        assert help_luma > type_luma

        ocr = con.ocr_screen(crop="640x176+0+0").lower()
        assert "geoff" in ocr or "graham" in ocr or "copyright" in ocr
        assert "help" in ocr
        assert "mmbasic" in ocr or ver.lstrip("v")[:3] in ocr
        assert con.send_line("PRINT 6*7") == "42"
    finally:
        con.stop()


def _is_grey_prompt(rgb):
    r, g, b = rgb
    return (
        abs(r - g) <= 20
        and abs(g - b) <= 20
        and 120 <= r <= 200
        and r + g + b < 620
    )


def _cell_is_solid_grey(con, col, row):
    hits = 0
    for y in range(16):
        for x in range(8):
            if _is_grey_prompt(con.screen_pixel(col * 8 + x, row * 16 + y)):
                hits += 1
    return hits >= 90


def _find_prompt_cursor(con, max_row=20, max_col=24):
    for row in range(max_row):
        for col in range(max_col):
            if _cell_is_solid_grey(con, col, row):
                return col, row
    return None


def test_prompt_grey_after_boot_and_term(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        time.sleep(0.2)
        boot = con.screen_pixel(1, 4)
        assert _is_grey_prompt(boot), boot
        from test_term import _open_term, _quit

        _open_term(con, 'TERM "demo", 23', quiet=0.8, timeout=10.0)
        _quit(con)
        assert con.send_line("PRINT 1+1") == "2"
    finally:
        con.stop()


def test_prompt_block_cursor_visible(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        time.sleep(0.2)
        found = _find_prompt_cursor(con)
        png = con.capture_png("/opt/cursor/artifacts/prompt_block_cursor.png")
        assert found, "HDMI prompt cursor should be a solid grey 8x16 block"
        assert os.path.isfile(png)
        assert con.send_line("MODE 8") == ""
        time.sleep(0.2)
        found_mode = _find_prompt_cursor(con)
        con.capture_png("/opt/cursor/artifacts/prompt_block_cursor_mode8.png")
        assert found_mode, "prompt cursor must survive MODE resize"
    finally:
        con.stop()
