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
        seen = _wait_serial(con, b"!REPLAY WAIT", timeout=25.0)
        assert b"!REPLAY START" in seen or b"Replay" in seen or b"!REPLAY WAIT" in seen
        before = os.path.join(ARTIFACTS, "hdmi_blackflag_before_first_key.png")
        con.capture_png(before)
        assert os.path.getsize(before) > 1000
        con._ser.sendall(b"x")
        _wait_serial(con, b"!REPLAY WAIT", timeout=15.0)
        after = os.path.join(ARTIFACTS, "hdmi_blackflag_after_first_key.png")
        con.capture_png(after)
        assert os.path.getsize(after) > 1000
        w, h = con.screen_size()
        assert w >= 640 and h >= 400
        px = con.screen_pixel(w // 2, h // 2)
        assert px != (0, 0, 0)
        _quit(con)
        assert con.send_line("PRINT 1+1") == "2"
    finally:
        con.stop()
        os.remove(img)
