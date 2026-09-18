"""AFK Mystify-style screensaver: starts, draws, and restores the mode."""

import subprocess
import time

from harness import MMBasicConsole


def _lit_fraction(con) -> float:
    """Fraction of the framebuffer above a low brightness threshold."""
    png = con.capture_png()
    out = subprocess.run(
        [
            "convert", png, "-colorspace", "gray", "-threshold", "8%",
            "-format", "%[fx:mean]", "info:",
        ],
        check=True, capture_output=True, text=True,
    ).stdout.strip()
    return float(out)


def _peak_lit(con, samples: int = 10, gap: float = 0.35) -> float:
    """Peak lit fraction across several captures.

    QEMU repaints the whole frame through a slow emulated clear, so a single
    capture can land in the black phase; sample until a drawn frame is seen.
    """
    best = 0.0
    for _ in range(samples):
        best = max(best, _lit_fraction(con))
        if best > 0.0005:
            break
        time.sleep(gap)
    return best


def test_afk_draws_and_enter_exits(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        before = con.send_line("PRINT MM.INFO(MODE)")
        con.drain(quiet=0.2)
        con._ser.sendall(b"AFK\r")
        con.drain(quiet=0.8, timeout=8)
        assert _peak_lit(con) > 0.0005
        # Enter exits and restores the previous mode and prompt.
        con._ser.sendall(b"\r")
        con.drain(quiet=0.6, timeout=8)
        assert con.send_line("PRINT MM.INFO(MODE)") == before
        assert con.send_line("PRINT 1+1") == "2"
    finally:
        con.stop()


def test_afk_ctrl_c_exits(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        con.drain(quiet=0.2)
        con._ser.sendall(b"AFK\r")
        con.drain(quiet=0.8, timeout=8)
        assert _peak_lit(con) > 0.0005
        con._ser.sendall(bytes([3]))
        con.drain(quiet=0.6, timeout=8)
        assert con.send_line("PRINT 2+2") == "4"
    finally:
        con.stop()


def test_afk_follows_turbo_theme(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        assert con.send_line("OPTION EDIT THEME TURBO") == ""
        con.drain(quiet=0.2)
        con._ser.sendall(b"AFK\r")
        con.drain(quiet=0.8, timeout=8)
        assert _peak_lit(con) > 0.0005
        con._ser.sendall(b"\r")
        con.drain(quiet=0.6, timeout=8)
        assert con.send_line("OPTION EDIT THEME SLATE") == ""
    finally:
        con.stop()


def test_afk_not_available_in_run(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        assert con.send_line('OPEN "AFK.BAS" FOR OUTPUT AS #1') == ""
        assert con.send_line('PRINT #1, "AFK"') == ""
        assert con.send_line("CLOSE #1") == ""
        out = con.send_line('RUN "AFK.BAS"').upper()
        assert "NOT AVAILABLE IN RUN" in out, out
    finally:
        con.stop()
