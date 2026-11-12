"""TERM REPLAY \"file$\" plays a termlog through the RX ring (not UART)."""

import os
import re
import shutil
import subprocess
import tempfile
import time

import pytest

from harness import MMBasicConsole, parse_termlog
from test_term import _plain, _quit
from test_tui_prompt_cmm2 import _upload_binary_file

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BLACKFLAG = os.path.join(REPO, "tests", "term", "blackflag-log-v2")
JPEG_REFRESH = os.path.join(REPO, "tests", "term", "jpeg-refresh-termlog")
from artifacts_util import ARTIFACTS


def _fat_with_log(src=BLACKFLAG, dest="BF.LOG"):
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
        ["mcopy", "-i", img, src, f"::{dest}"],
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


def _wait_any(con, needles, timeout: float) -> bytes:
    deadline = time.time() + timeout
    acc = b""
    while time.time() < deadline:
        acc += con.drain(quiet=0.05, timeout=0.4)
        if any(n in acc for n in needles):
            return acc
        time.sleep(0.05)
    raise AssertionError(f"timeout waiting for {needles!r} in {acc[-400]!r}")


def _record_at_tx_offset(recs, offset: int) -> int:
    pos = 0
    for i, rec in enumerate(recs):
        if rec.kind == "T":
            if pos <= offset < pos + len(rec.data):
                return i
            pos += len(rec.data)
    raise AssertionError(f"no T record at tx offset {offset}")


def _replay_wait_index(recs, rec_index: int) -> int:
    """The 1-based ``!REPLAY WAIT`` that pauses on that T record.

    File replay pauses only for T records with data that does not start with
    IAC, so the wait number is that record's position in the filtered list.
    """
    eligible = [
        i
        for i, rec in enumerate(recs)
        if rec.kind == "T" and rec.data and rec.data[0] != 0xFF
    ]
    return eligible.index(rec_index) + 1


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
        cyan = False
        for x, y in ((240, 40), (400, 48), (560, 40), (480, 64), (320, 80), (640, 56)):
            r, g, b = con.screen_pixel(x, y)
            if g > 80 and b > 80 and g > r:
                cyan = True
                break
        assert cyan, "expected cyan ANSI sky after Blackflag file replay"
        _quit(con)
        assert con.send_line("PRINT 1+1") == "2"
    finally:
        con.stop()
        os.remove(img)


def test_jpeg_refresh_partial_redraw_updates_wall(kernel_image):
    """#1488: Up redraws update the sixel wall without a Ctrl+L full redraw.

    The capture has two Up presses after the wall is drawn.  Each answer is a
    sixel image; the second one is a short strip at the bottom of the wall and
    used to be clipped away because its raster height was measured from the
    pane top instead of the image origin.
    """
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        _upload_binary_file(con, JPEG_REFRESH, "A:/J.LOG")
        assert "J.LOG" in con.send_line('DIR "A:/"').upper()

        recs = parse_termlog(open(JPEG_REFRESH, errors="replace").read())
        tx = b"".join(r.data for r in recs if r.kind == "T")
        ups = [m.start() for m in re.finditer(re.escape(b"\x1b[A"), tx)]
        assert len(ups) >= 2, ups
        ctrl_l = tx.index(b"\x0c")
        wait_up2 = _replay_wait_index(
            recs, _record_at_tx_offset(recs, ups[1] + 2)
        )
        wait_ctrl = _replay_wait_index(recs, _record_at_tx_offset(recs, ctrl_l))
        assert wait_up2 < wait_ctrl

        # DCS3 is the strip the second Up adds at the bottom of the wall.
        coords = [(x, y) for y in range(304, 332, 2) for x in range(162, 800, 8)]
        states = {}
        wait = 0
        con.drain(quiet=0.1)
        con._ser.sendall(b'TERM REPLAY "A:/J.LOG"\r')
        while wait < wait_ctrl:
            _wait_serial(con, b"!REPLAY WAIT", timeout=30.0)
            wait += 1
            if wait in (wait_up2, wait_ctrl):
                time.sleep(0.4)
                states[wait] = con.screen_pixels(coords)
            con._ser.sendall(b"x")
        assert set(states) == {wait_up2, wait_ctrl}, states.keys()
        changed = sum(
            1
            for a, b in zip(states[wait_up2], states[wait_ctrl])
            if a != b
        )
        assert changed > 10, (
            f"wall bottom did not change on the second Up (waits "
            f"{wait_up2}/{wait_ctrl}, {changed} pixels)"
        )

        for _ in range(6):
            acc = _wait_any(
                con, (b"!REPLAY WAIT", b"!REPLAY DONE"), timeout=20.0
            )
            if b"!REPLAY DONE" in acc:
                break
            con._ser.sendall(b"x")
        _quit(con)
        assert con.send_line("PRINT 2+3") == "5"
    finally:
        con.stop()
