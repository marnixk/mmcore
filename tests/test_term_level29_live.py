"""Live Level29 BBS over QEMU usb-net (slow drip). Skip if the host cannot reach it.

TERMLOG matching HDMI cannot show whether Circle still had data. This session
keeps HDMI as the screen and serial !TCP as socket Receive/GetStatus.
"""

import os
import select
import socket
import threading
import time

import pytest

from harness import MMBasicConsole, qemu_usb_net_args
from test_qemu_ethernet import _wait_dhcp
from test_term import _quit

BBS = ("bbs.fozztexx.com", 23)
USER = b"ireal"
PASS = b"ds9space"
ARTIFACTS = "/opt/cursor/artifacts"
PANE = "640x480+160+0"


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


def _proxy_pipe(a: socket.socket, b: socket.socket) -> None:
    try:
        while True:
            ready, _, _ = select.select([a, b], [], [], 45.0)
            if not ready:
                continue
            for src, dst in ((a, b), (b, a)):
                if src not in ready:
                    continue
                data = src.recv(4096)
                if not data:
                    return
                dst.sendall(data)
    except OSError:
        pass
    finally:
        for s in (a, b):
            try:
                s.close()
            except OSError:
                pass


def _start_bbs_proxy() -> tuple[socket.socket, int]:
    """Guest SLIRP SYNs to the public BBS often time out; 10.0.2.2 does not.

    Byte-pipe the BBS onto the host so TERM still sees Level29's slow drip.
    """
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", 0))
    port = srv.getsockname()[1]
    srv.listen(4)
    srv.settimeout(90.0)

    def accept_loop() -> None:
        while True:
            try:
                client, _ = srv.accept()
            except OSError:
                return
            try:
                remote = socket.create_connection(BBS, timeout=15)
            except OSError:
                try:
                    client.close()
                except OSError:
                    pass
                continue
            client.settimeout(None)
            remote.settimeout(None)
            threading.Thread(
                target=_proxy_pipe, args=(client, remote), daemon=True
            ).start()

    threading.Thread(target=accept_loop, daemon=True).start()
    return srv, port


@pytest.mark.skipif(not _bbs_reachable(), reason="Level29 BBS not reachable")
def test_level29_live_term_drip_over_ethernet(kernel_image):
    os.makedirs(ARTIFACTS, exist_ok=True)
    serial_log = os.path.join(ARTIFACTS, "level29_live_serial.txt")
    con = MMBasicConsole(
        kernel_image,
        extra_qemu=qemu_usb_net_args(),
        boot_timeout=40.0,
    )
    con.start()
    serial = b""
    proxy = None
    try:
        proxy, proxy_port = _start_bbs_proxy()
        assert con.send_line("FACTORY_RESET") == "Factory defaults restored"
        on = con.send_line("OPTION ETHERNET ON", timeout=25)
        cfg = on if "10.0.2." in on else _wait_dhcp(con, timeout=40)
        assert "10.0.2." in (on + cfg), cfg
        time.sleep(2.0)
        assert "?SYNTAX ERROR" not in con.send_line("OPTION WIFI DEBUG ON").upper()
        assert ".termlog" in con.send_line("OPTION TERM LOG ON")

        serial = b""
        connected = False
        for _attempt in range(3):
            con.drain(quiet=0.1, timeout=0.4)
            con._ser.sendall(f'TERM "10.0.2.2", {proxy_port}\r'.encode())
            serial += _wait_serial(con, b"!NET connected", timeout=25.0)
            if b"!NET connected" in serial:
                connected = True
                break
            _quit(con)
            time.sleep(1.5)
        assert connected, serial.decode(errors="replace")[-800:]
        time.sleep(0.4)

        ocr = con.wait_ocr("User:", timeout=40.0, crop=PANE)
        banner = os.path.join(ARTIFACTS, "level29_session_username.png")
        con.capture_png(banner)
        serial += con.drain(quiet=0.3, timeout=2.0)
        tcp = _tcp_lines(serial)
        assert tcp, "expected !TCP snapshots after WIFI DEBUG ON"
        last = tcp[-1]
        assert "bytes=" in last and "zero=" in last and "rxrdy=" in last

        saw_prompt = "user:" in ocr.lower() or "username" in ocr.lower()
        assert saw_prompt, (
            "Level29 drip did not reach the username prompt on HDMI; "
            f"ocr={ocr!r} tcp={tcp[-3:]!r}"
        )
        con._ser.sendall(USER + b"\r")
        serial += _wait_serial(con, b"!TCP ", timeout=8.0)
        pw = con.wait_ocr("Password", timeout=20.0, crop=PANE)
        con.capture_png(os.path.join(ARTIFACTS, "level29_session_password.png"))
        saw_password = "password" in pw.lower()
        assert saw_password, (
            "sent ireal but HDMI never showed Password; "
            f"ocr={pw!r} tcp={tcp[-3:]!r}"
        )
        con._ser.sendall(PASS + b"\r")
        serial += _wait_serial(con, b"!TCP ", timeout=8.0)
        after_ocr = con.wait_ocr("Welcome ireal", timeout=20.0, crop=PANE)
        if "welcome ireal" not in after_ocr.lower():
            after_ocr = con.wait_ocr("Terminal size", timeout=8.0, crop=PANE) or after_ocr
        after = os.path.join(ARTIFACTS, "level29_session_welcome.png")
        con.capture_png(after)

        serial += con.drain(quiet=0.3, timeout=2.0)
        open(serial_log, "w", encoding="utf-8").write(serial.decode(errors="replace"))
        tcp = _tcp_lines(serial)
        logged_in = (
            "welcome ireal" in after_ocr.lower() or "terminal size" in after_ocr.lower()
        )
        assert logged_in, (
            "sent ireal/ds9space but HDMI did not show Welcome ireal; "
            f"ocr={after_ocr!r} tcp={tcp[-4:]!r}"
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
        if proxy is not None:
            try:
                proxy.close()
            except OSError:
                pass
        con.stop()
