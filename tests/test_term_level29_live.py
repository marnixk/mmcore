"""Live Level29 login over QEMU usb-net.

Guest DNS/SYN to the public BBS is flaky, so QEMU guestfwd maps
10.0.2.100:23 to the BBS. TERM talks to that address (CR, not CRLF).
Proof is OPTION TERM LOG, not HDMI OCR.
"""

import os
import socket
import time

import pytest

from harness import MMBasicConsole, parse_termlog, qemu_usb_net_args
from net_util import live_net_enabled
from test_pcap_send_holes import client_seq_holes
from test_qemu_ethernet import _wait_dhcp
from test_term import _quit
from test_term_log import _read_termlog, _termlog_path

BBS = ("bbs.fozztexx.com", 23)
GUEST_BBS = "10.0.2.100"
USER = b"ireal"
PASS = b"ds9space"
from artifacts_util import ARTIFACTS

# Live external BBS login is opt-in: the emulated guest path is only
# transiently usable and fixed sleeps flake under load (#389).
pytestmark = pytest.mark.skipif(
    not live_net_enabled(),
    reason="live Level29 BBS test is opt-in; set MMCORE_LIVE_NET=1",
)


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


@pytest.mark.skipif(not _bbs_reachable(), reason="Level29 BBS not reachable")
def test_level29_live_term_login(kernel_image):
    guest_pcap = os.path.join(ARTIFACTS, "level29_guest_usbnic.pcap")
    bbs_ip = socket.getaddrinfo(BBS[0], BBS[1], socket.AF_INET)[0][4][0]
    con = MMBasicConsole(
        kernel_image,
        extra_qemu=qemu_usb_net_args(
            guestfwd=f"tcp:{GUEST_BBS}:23-tcp:{bbs_ip}:23",
            dump=guest_pcap,
        ),
        boot_timeout=40.0,
    )
    con.start()
    try:
        assert con.send_line("FACTORY_RESET") == "Factory defaults restored"
        on = con.send_line("OPTION ETHERNET ON", timeout=25)
        cfg = on if "10.0.2." in on else _wait_dhcp(con, timeout=40)
        assert "10.0.2." in (on + cfg), cfg
        assert ".termlog" in con.send_line("OPTION TERM LOG ON")

        con.drain(quiet=0.1, timeout=0.4)
        con._ser.sendall(f'TERM "{GUEST_BBS}", 23\r'.encode())
        serial = _wait_serial(con, b"!NET connected", timeout=25.0)
        if b"!NET connected" not in serial:
            # The host can reach the BBS, but the emulated guestfwd path can
            # time out in sandboxed QEMU setups; only run the live login when
            # the guest actually established the connection.
            pytest.skip(
                "guest could not reach the live BBS through guestfwd: "
                + serial.decode(errors="replace")[-200:]
            )

        # Poll for the prompts instead of fixed sleeps: under parallel load
        # the banner can take far longer than 8s to arrive (#389).
        con.wait_ocr("login", timeout=30.0)
        con._ser.sendall(USER + b"\r")
        con.wait_ocr("password", timeout=20.0)
        con._ser.sendall(PASS + b"\r")
        con.wait_ocr("welcome", timeout=15.0)
        con.capture_png(os.path.join(ARTIFACTS, "level29_ireal_logged_in.png"))

        _quit(con)
        off = con.send_line("OPTION TERM LOG OFF")
        assert "in=" in off
        path = _termlog_path(con)
        assert path, "expected A:/.termlog or C:/.termlog"
        body = _read_termlog(con, path)
        rx = b"".join(r.data for r in parse_termlog(body) if r.kind == "R")
        open(os.path.join(ARTIFACTS, "level29_ireal_termlog.txt"), "w").write(body)
        holes = client_seq_holes(guest_pcap, dport=23)
        assert holes == [], holes
        assert b"Welcome ireal" in rx or b"Terminal size" in rx, rx[-400:]
        assert con.send_line("PRINT 1+1") == "2"
    finally:
        try:
            _quit(con)
        except Exception:
            pass
        con.stop()
