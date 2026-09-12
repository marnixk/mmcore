"""Live Level29 BBS over QEMU usb-net (slow drip). Skip if the host cannot reach it.

TERMLOG matching HDMI cannot show whether Circle still had data. This session
keeps HDMI as the screen, serial !TCP as socket Receive/GetStatus, and a SLIRP
pcap as the wire.
"""

import os
import socket
import time

import pytest

from harness import MMBasicConsole, qemu_usb_net_args
from test_qemu_ethernet import _wait_dhcp
from test_term import _quit

BBS = ("bbs.fozztexx.com", 23)
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
        if needle in acc:
            return acc
        time.sleep(0.05)
    return acc


def _tcp_lines(serial: bytes) -> list[str]:
    return [
        ln.strip()
        for ln in serial.decode(errors="replace").splitlines()
        if ln.startswith("!TCP ")
    ]


@pytest.mark.skipif(not _bbs_reachable(), reason="Level29 BBS not reachable")
def test_level29_live_term_drip_over_ethernet(kernel_image):
    os.makedirs(ARTIFACTS, exist_ok=True)
    pcap = os.path.join(ARTIFACTS, "level29_live.pcap")
    serial_log = os.path.join(ARTIFACTS, "level29_live_serial.txt")
    con = MMBasicConsole(
        kernel_image,
        extra_qemu=qemu_usb_net_args(dump=pcap),
        boot_timeout=40.0,
    )
    con.start()
    serial = b""
    try:
        assert con.send_line("FACTORY_RESET") == "Factory defaults restored"
        on = con.send_line("OPTION ETHERNET ON", timeout=25)
        if "10.0.2." not in on:
            cfg = _wait_dhcp(con)
            assert "10.0.2." in (on + cfg) or "link is up" in cfg.lower(), cfg
        assert "?SYNTAX ERROR" not in con.send_line("OPTION WIFI DEBUG ON").upper()
        assert ".termlog" in con.send_line("OPTION TERM LOG ON")

        con.drain(quiet=0.1, timeout=0.4)
        con._ser.sendall(b'TERM "bbs.fozztexx.com", 23\r')
        serial = _wait_serial(con, b"!NET connected", timeout=30.0)
        assert b"!NET connected" in serial, serial.decode(errors="replace")[-800:]

        ocr = con.wait_ocr("username", timeout=25.0)
        banner = os.path.join(ARTIFACTS, "level29_live_banner.png")
        con.capture_png(banner)
        serial += con.drain(quiet=0.3, timeout=2.0)
        tcp = _tcp_lines(serial)
        assert tcp, "expected !TCP snapshots after WIFI DEBUG ON"
        last = tcp[-1]
        assert "bytes=" in last and "zero=" in last and "rxrdy=" in last

        saw_prompt = "username" in ocr.lower() or "enter your" in ocr.lower()
        if saw_prompt:
            con._ser.sendall(USER + b"\r")
            time.sleep(1.0)
            serial += con.drain(quiet=0.2, timeout=2.0)
            pw = con.wait_ocr("Password", timeout=20.0)
            con.capture_png(os.path.join(ARTIFACTS, "level29_live_password.png"))
            if "password" in pw.lower():
                con._ser.sendall(PASS + b"\r")
                time.sleep(2.0)
                serial += _wait_serial(con, b"!TCP ", timeout=12.0)
                after = os.path.join(ARTIFACTS, "level29_live_after_login.png")
                con.capture_png(after)
                con.wait_ocr("Welcome", timeout=8.0)

        serial += con.drain(quiet=0.3, timeout=2.0)
        open(serial_log, "w", encoding="utf-8").write(serial.decode(errors="replace"))
        tcp = _tcp_lines(serial)
        assert os.path.isfile(pcap) and os.path.getsize(pcap) > 64, pcap
        assert saw_prompt, (
            "Level29 drip did not reach the username prompt on HDMI; "
            f"ocr={ocr!r} tcp={tcp[-3:]!r} pcap={os.path.getsize(pcap)} bytes"
        )
        _quit(con)
        assert con.send_line("PRINT 1+1") == "2"
    finally:
        try:
            open(serial_log, "a", encoding="utf-8").write(
                "\n" + serial.decode(errors="replace")
            )
        except Exception:
            pass
        try:
            _quit(con)
        except Exception:
            pass
        con.stop()
