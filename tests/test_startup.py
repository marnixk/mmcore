"""Boot banner: PicoMite copyright, HELP in bright white, two blank lines."""

from harness import MMBasicConsole


def test_startup_copyright_banner(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        raw = con.boot_log
        text = raw.decode(errors="replace")
        low = text.lower()
        assert "copyright 2011-2026 geoff graham" in low
        assert "copyright 2016-2026 peter mather" in low
        assert "adapted and extended by marnix kok" in low
        assert "type " in low
        assert "get started" in low
        assert b"\x1b[97mHELP\x1b[37m" in raw
        idx = raw.find(b"get started.")
        assert idx >= 0
        after = raw[idx + len(b"get started.") :]
        assert after.startswith(b"\r\n\r\n\r\n") or after.startswith(b"\n\n\n")
        assert con.send_line("PRINT 6*7") == "42"

        con.capture_png("/opt/cursor/artifacts/issue67_startup_banner.png")

        y = 4 * 16 + 8
        type_px = con.screen_pixel(4, y)
        help_px = con.screen_pixel(40 + 4, y)
        type_luma = sum(type_px)
        help_luma = sum(help_px)
        assert help_luma > type_luma + 40, (type_px, help_px)
    finally:
        con.stop()
