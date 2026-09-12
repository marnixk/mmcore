"""Live Level29 login over QEMU usb-net via CONNECT.

CONNECT echoes the BBS to serial, so there is no HDMI OCR. Guest DNS/SYN
to the public host is flaky; QEMU guestfwd maps 10.0.2.100:23 to the BBS.
Level29 needs CR (CONNECT character mode after IAC WILL SGA).
"""

import os
import socket
import time

import pytest

from harness import MMBasicConsole, qemu_usb_net_args
from test_qemu_ethernet import _wait_dhcp

BBS = ("bbs.fozztexx.com", 23)
GUEST_BBS = "10.0.2.100"
USER = b"ireal"
PASS = b"ds9space"
ARTIFACTS = "/opt/cursor/artifacts"


def _bbs_reachable() -> bool:
    try:
        s = socket.create_connection(BBS, timeout=8)
        s.close()
        return True
    except OSError:
        return False


def _wait_serial(con, needle: bytes, timeout: float) -> bytes:
    deadline = time.time() + timeout
    acc = b""
    while time.time() < deadline:
        acc += con.drain(quiet=0.05, timeout=0.3)
        if needle.lower() in acc.lower():
            return acc
        time.sleep(0.05)
    return acc


def _quit_connect(con) -> None:
    con._ser.sendall(bytes([0x1D]))
    con.drain(quiet=0.4, timeout=8.0)


@pytest.mark.skipif(not _bbs_reachable(), reason="Level29 BBS not reachable")
def test_level29_live_connect_login(kernel_image):
    os.makedirs(ARTIFACTS, exist_ok=True)
    log_path = os.path.join(ARTIFACTS, "level29_ireal_serial.txt")
    bbs_ip = socket.getaddrinfo(BBS[0], BBS[1], socket.AF_INET)[0][4][0]
    con = MMBasicConsole(
        kernel_image,
        extra_qemu=qemu_usb_net_args(
            guestfwd=f"tcp:{GUEST_BBS}:23-tcp:{bbs_ip}:23"
        ),
        boot_timeout=40.0,
    )
    con.start()
    serial = b""
    try:
        assert con.send_line("FACTORY_RESET") == "Factory defaults restored"
        on = con.send_line("OPTION ETHERNET ON", timeout=25)
        cfg = on if "10.0.2." in on else _wait_dhcp(con, timeout=40)
        assert "10.0.2." in (on + cfg), cfg

        con.drain(quiet=0.1, timeout=0.4)
        con._ser.sendall(f'CONNECT "{GUEST_BBS}", 23\r'.encode())
        serial = _wait_serial(con, b"User:", timeout=25.0)
        assert b"User:" in serial or b"username" in serial.lower(), serial[-800:]

        con._ser.sendall(USER + b"\r")
        serial += _wait_serial(con, b"Password", timeout=12.0)
        assert b"assword" in serial.lower(), serial[-400:]

        con._ser.sendall(PASS + b"\r")
        serial += _wait_serial(con, b"Welcome ireal", timeout=12.0)
        if b"Welcome ireal" not in serial:
            serial += _wait_serial(con, b"Terminal size", timeout=8.0)
        con.capture_png(os.path.join(ARTIFACTS, "level29_ireal_logged_in.png"))
        open(log_path, "wb").write(serial)
        assert b"Welcome ireal" in serial or b"Terminal size" in serial, serial[-400:]

        _quit_connect(con)
        assert con.send_line("PRINT 1+1") == "2"
    finally:
        try:
            open(log_path, "ab").write(b"\n" + serial)
        except Exception:
            pass
        try:
            _quit_connect(con)
        except Exception:
            pass
        con.stop()
