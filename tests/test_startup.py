"""Boot banner: mmcore wordmark with version, the MMBasic notice, and HELP in
bright white, followed by two blank lines."""

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


def test_startup_banner(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        raw = con.boot_log
        text = raw.decode(errors="replace")
        low = text.lower()
        ver = _mmb_version()
        assert f"mmcore operating system - {ver} - 2026 (c) marnix kok" in low
        assert "mmbasic" in low
        assert "copyright" not in low
        assert "adapted and extended" not in low
        assert "type " in low
        assert "help me" in low
        assert "short introduction" in low
        assert b"\x1b[97mHELP ME\x1b[37m" in raw
        idx = raw.find(b"short introduction.")
        assert idx >= 0
        after = raw[idx + len(b"short introduction.") :]
        prompt_at = after.find(b">")
        assert prompt_at >= 0
        gap = after[:prompt_at].replace(b"\r", b"")
        assert gap.count(b"\n") >= 3

        png = con.capture_png("/opt/cursor/artifacts/issue67_startup_banner.png")
        pix = _png_rgb(png, crop="1280x256+0+0")
        bright = [(x, y) for (x, y), c in pix.items() if min(c) >= 200]
        dim = [
            (x, y)
            for (x, y), c in pix.items()
            if abs(c[0] - c[1]) <= 20
            and abs(c[1] - c[2]) <= 20
            and 120 <= c[0] <= 210
        ]
        assert dim, "banner text should be dim white"
        # The chrome wordmark is centred in the top band of the 1280-wide HDMI.
        logo_bright = [(x, y) for x, y in bright if y < 64]
        assert logo_bright, "boot logo should paint bright pixels in the top band"
        xs = [x for x, _ in logo_bright]
        assert 300 < min(xs) and max(xs) < 980, (min(xs), max(xs))
        assert abs((min(xs) + max(xs)) / 2 - 640) < 40, (min(xs), max(xs))
        assert [p for p in dim if p[1] >= 64], "banner text should sit below the logo"

        ocr = con.ocr_screen(crop="1280x256+0+0").lower()
        assert "mmcore" in ocr
        # OCR can split the short MMBasic line ("mmbas ic"); ignore spaces.
        assert "mmbasic" in ocr.replace(" ", "")
        assert "help" in ocr
        assert con.send_line("PRINT 6*7") == "42"

        # Console output must not repaint the splash logo off the top band
        # (#578/#579). The logo is stored in the console's own pixel buffer,
        # so newlines after it cannot erase it.
        con._ser.sendall(b"\r\n\r\n")
        con.drain(quiet=0.2)
        after = con.capture_png(
            "/opt/cursor/artifacts/issue578_logo_after_console_output.png"
        )
        after_pix = _png_rgb(after, crop="1280x64+0+0")
        after_logo = [p for p, c in after_pix.items() if min(c) >= 200]
        assert after_logo, "splash logo must survive console newlines"
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


def _png_rgb(png, crop=None):
    cmd = ["convert", png]
    if crop:
        cmd += ["-crop", crop, "+repage"]
    cmd += ["txt:-"]
    out = subprocess.run(cmd, check=True, capture_output=True, text=True).stdout
    pix = {}
    for line in out.splitlines():
        if line.startswith("#") or ":" not in line:
            continue
        xy, rest = line.split(":", 1)
        x, y = (int(p) for p in xy.split(","))
        if "(" not in rest:
            continue
        inner = rest[rest.find("(") + 1 : rest.find(")")]
        parts = [p.strip() for p in inner.replace("%", "").split(",") if p.strip()]
        if len(parts) == 1:
            # Grayscale screendump (for example after MODE 8): gray(v).
            v = int(float(parts[0]))
            pix[(x, y)] = (v, v, v)
        elif len(parts) >= 3:
            pix[(x, y)] = tuple(int(float(p)) for p in parts[:3])
    return pix


def _cell_is_solid_grey(pix, col, row):
    hits = 0
    for y in range(16):
        for x in range(8):
            rgb = pix.get((col * 8 + x, row * 16 + y))
            if rgb and _is_grey_prompt(rgb):
                hits += 1
    return hits >= 90


def _find_prompt_cursor(png, max_row=16, max_col=20):
    pix = _png_rgb(png, crop=f"{max_col * 8}x{max_row * 16}+0+0")
    for row in range(max_row):
        for col in range(max_col):
            if _cell_is_solid_grey(pix, col, row):
                return col, row
    return None


def test_prompt_grey_after_boot_and_term(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        time.sleep(0.2)
        png = con.capture_png("/opt/cursor/artifacts/prompt_after_boot.png")
        assert _find_prompt_cursor(png), "boot prompt should be a solid grey block"
        from test_term import _open_term, _quit

        _open_term(con, 'TERM "demo", 23', quiet=0.8, timeout=10.0)
        _quit(con)
        assert con.send_line("PRINT 1+1") == "2"
    finally:
        con.stop()


def test_boot_background_is_black_not_blue(kernel_image):
    """Console paper is black (CMM2 0), not IBM blue (1)."""
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        time.sleep(0.2)
        samples = [con.screen_pixel(x, y) for x, y in ((200, 300), (400, 280), (80, 400))]
        for rgb in samples:
            r, g, b = rgb
            assert r < 40 and g < 40 and b < 40, samples
        png = con.capture_png("/opt/cursor/artifacts/boot_black_background.png")
        assert os.path.isfile(png)
        assert con.send_line("PRINT 3") == "3"
    finally:
        con.stop()


def test_prompt_block_cursor_visible(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        time.sleep(0.2)
        png = con.capture_png("/opt/cursor/artifacts/prompt_block_cursor.png")
        found = _find_prompt_cursor(png)
        assert found, "HDMI prompt cursor should be a solid grey 8x16 block"
        assert os.path.isfile(png)
        assert con.send_line("MODE 8") == ""
        time.sleep(0.2)
        png8 = con.capture_png("/opt/cursor/artifacts/prompt_block_cursor_mode8.png")
        found_mode = _find_prompt_cursor(png8)
        assert found_mode, "prompt cursor must survive MODE resize"
    finally:
        con.stop()


def test_help_hides_prompt_at_top_left(kernel_image):
    """Starting HELP must not leave the REPL prompt on the title bar."""
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        time.sleep(0.3)
        con._ser.sendall(b"HELP\r")
        con.drain(quiet=0.8)
        png = con.capture_png("/opt/cursor/artifacts/help_no_stale_prompt.png")
        # The title bar is empty left of the centred title: any prompt-grey
        # pixel here is the leaked "A:/>" prompt.
        pix = _png_rgb(png, crop="48x16+0+0")
        leaked = sum(1 for rgb in pix.values() if _is_grey_prompt(rgb))
        assert leaked == 0, f"prompt leaked over HELP ({leaked} grey pixels)"
    finally:
        con.stop()
