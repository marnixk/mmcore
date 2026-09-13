"""TERM REPLAY \"file$\" plays a termlog through the RX ring (not UART)."""

import os
import shutil
import subprocess
import tempfile
import time

import pytest

from harness import MMBasicConsole
from test_term import _plain, _quit

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BLACKFLAG = os.path.join(REPO, "tests", "term", "blackflag-log-v2")
ARTIFACTS = "/opt/cursor/artifacts"


def _fat_with_log():
    fd, img = tempfile.mkstemp(suffix=".img")
    os.close(fd)
    subprocess.run(
        ["dd", "if=/dev/zero", f"of={img}", "bs=1M", "count=64"],
        check=True,
        capture_output=True,
    )
    subprocess.run(
        ["mkfs.vfat", "-F", "32", "-n", "MMBASIC", img],
        check=True,
        capture_output=True,
    )
    subprocess.run(
        ["mcopy", "-i", img, BLACKFLAG, "::BF.LOG"],
        check=True,
        capture_output=True,
    )
    os.sync()
    return img


def _wait_serial(con, needle: bytes, timeout: float) -> bytes:
    deadline = time.time() + timeout
    acc = b""
    while time.time() < deadline:
        acc += con.drain(quiet=0.05, timeout=0.4)
        if needle in acc:
            return acc
        time.sleep(0.05)
    raise AssertionError(f"timeout waiting for {needle!r} in {acc[-400]!r}")


def test_term_replay_missing_file(console):
    out = console.send_line('TERM REPLAY "A:/NOPE.LOG"')
    assert "?FILE" in out.upper()


# Typed T pauses in tests/term/blackflag-log-v2:
#   1 ESC botcheck, 2 ESC, 3 theme "1", 4 Enter, 5-10 "ireal"+Enter,
#   11 YES/NO after the post-login ANSI animation.
_SHOTS = {
    1: "hdmi_blackflag_botcheck.png",
    3: "hdmi_blackflag_theme_select.png",
    5: "hdmi_blackflag_login_screen.png",
    11: "hdmi_blackflag_after_login_animation.png",
}


@pytest.mark.skipif(shutil.which("mkfs.vfat") is None, reason="mkfs.vfat not installed")
@pytest.mark.skipif(shutil.which("mcopy") is None, reason="mcopy not installed")
def test_blackflag_file_replay_hdmi_past_animation(kernel_image):
    img = _fat_with_log()
    os.makedirs(ARTIFACTS, exist_ok=True)
    con = MMBasicConsole(
        kernel_image,
        extra_qemu=["-drive", f"file={img},if=sd,format=raw"],
        boot_timeout=30,
    )
    con.start()
    try:
        drv = con.send_line("DRIVE")
        assert "C: SD" in drv
        assert "no media" not in drv.lower(), drv
        assert con.send_line('CHDIR "C:"') == ""
        listing = con.send_line("DIR")
        assert "BF.LOG" in listing.upper(), listing
        con.drain(quiet=0.1)
        con._ser.sendall(b'TERM REPLAY "C:/BF.LOG"\r')
        wait_n = 0
        target = max(_SHOTS)
        while wait_n < target:
            if wait_n == 0:
                seen = _wait_serial(con, b"!REPLAY WAIT", timeout=25.0)
                assert b"!REPLAY WAIT" in seen
            else:
                con._ser.sendall(b"x")
                _wait_serial(con, b"!REPLAY WAIT", timeout=25.0)
            wait_n += 1
            name = _SHOTS.get(wait_n)
            if name:
                path = os.path.join(ARTIFACTS, name)
                con.capture_png(path)
                assert os.path.getsize(path) > 1000, path
        w, h = con.screen_size()
        assert w >= 640 and h >= 400
        ocr = con.ocr_screen()
        low = ocr.lower()
        assert "apply" in low or "ungenannt" in low or "account" in low, ocr
        _quit(con)
        assert con.send_line("PRINT 1+1") == "2"
    finally:
        con.stop()
        os.remove(img)
