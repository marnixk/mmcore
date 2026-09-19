"""FILES FTP server: command-menu wiring, modal behaviour, live transfer."""

import ftplib
import io
import socket
import time

import pytest

from harness import MMBasicConsole, qemu_usb_net_args
from net_util import host_tcp_available, live_net_enabled
from test_files_ui import _keys, _open_files, _prep_tree
from test_qemu_ethernet import _wait_dhcp

# Guest control port is 21 and passive data is control + 1000 (see cmd_ftp.c).
# QEMU hostfwd maps a host port to each. The data host port must equal the
# advertised guest port, because clients connect to the control host IP.
FTP_DATA_PORT = 1021


def _free_port() -> int:
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def _enable_ethernet(con: MMBasicConsole) -> None:
    assert con.send_line("FACTORY_RESET") == "Factory defaults restored"
    out = con.send_line("OPTION ETHERNET ON", timeout=25)
    if "10.0.2." not in out:
        out = out + "\n" + _wait_dhcp(con)
    assert "10.0.2." in out, out


def _wait_serial(con: MMBasicConsole, needle: str, timeout: float = 15.0) -> str:
    buf = ""
    deadline = time.time() + timeout
    while time.time() < deadline:
        buf += con.drain(quiet=0.25).decode(errors="replace")
        if needle in buf:
            return buf
    return buf


def test_files_command_menu_offers_ftp_server(fresh_console):
    con = fresh_console
    _prep_tree(con)
    _open_files(con)
    seen = _keys(con, bytes([1]) + b"c", quiet=0.5)
    assert "FTP server" in seen
    # No NIC in the default image: starting must fail fast and stay in FILES.
    seen = _keys(con, b"s", quiet=0.8)
    assert "[FTP] LISTEN" not in seen
    _keys(con, b"q")
    assert con.send_line("PRINT 1") == "1"


def test_files_ftp_modal_start_stop(net_console):
    """Start on a live NIC, prove the modal blocks input, then Esc stops it."""
    con = net_console
    _enable_ethernet(con)
    assert con.send_line('MKDIR "A:/FTPDIR"') == ""

    con.drain(quiet=0.2)
    con._ser.sendall(b'FILES "A:/FTPDIR"\r')
    seen = con.drain(quiet=1.0).decode(errors="replace")
    assert "[FILES]" in seen

    con._ser.sendall(bytes([1]) + b"c")
    con.drain(quiet=0.3)
    con._ser.sendall(b"s")
    started = _wait_serial(con, "[FTP] LISTEN")
    assert "[FTP] LISTEN" in started, started[-500:]
    assert "ROOT A:/FTPDIR" in started.upper(), started[-500:]
    assert "FTP SERVER" in started.upper(), started[-500:]

    # The modal must swallow other keys: q must not quit FILES (which would
    # stop the server and emit [FTP] STOP), and letters do nothing.
    con._ser.sendall(b"qZv")
    ignored = _wait_serial(con, "[FTP] LISTEN", timeout=1.0)
    assert "[FTP] STOP" not in ignored, ignored[-400:]

    con._ser.sendall(b"\x1b")
    stopped = _wait_serial(con, "[FTP] STOP")
    assert "[FTP] STOP" in stopped, stopped[-400:]

    con._ser.sendall(b"q")
    con.drain(quiet=0.5)
    assert con.send_line("PRINT 5") == "5"


@pytest.fixture(scope="module")
def ftp_console(kernel_image):
    if not live_net_enabled():
        pytest.skip("set MMCORE_LIVE_NET=1 to run live FTP tests")
    ctrl = _free_port()
    forward = [f"tcp::{ctrl}-:21", f"tcp::{FTP_DATA_PORT}-:{FTP_DATA_PORT}"]
    con = MMBasicConsole(
        kernel_image,
        extra_qemu=qemu_usb_net_args(forward),
        boot_timeout=40.0,
    )
    con.ctrl_host_port = ctrl
    con.start()
    yield con
    con.stop()


def test_files_ftp_roundtrip(ftp_console):
    con = ftp_console
    _enable_ethernet(con)
    # Host->guest TCP needs the same SLIRP unicast path the existing live tests
    # probe; skip where that is unavailable (see #389).
    if not host_tcp_available(con):
        pytest.skip("QEMU SLIRP TCP unavailable in this environment")

    assert con.send_line('MKDIR "A:/FTPDIR"') == ""
    assert con.send_line('OPEN "A:/FTPDIR/HELLO.TXT" FOR OUTPUT AS #1') == ""
    assert con.send_line('PRINT #1, "hello ftp"') == ""
    assert con.send_line("CLOSE #1") == ""

    con.drain(quiet=0.2)
    con._ser.sendall(b'FILES "A:/FTPDIR"\r')
    seen = con.drain(quiet=1.0).decode(errors="replace")
    assert "[FILES]" in seen
    assert "HELLO.TXT" in seen.upper()

    con._ser.sendall(bytes([1]) + b"c")
    con.drain(quiet=0.3)
    con._ser.sendall(b"s")
    started = _wait_serial(con, "[FTP] LISTEN", timeout=20)
    assert "[FTP] LISTEN" in started, started[-600:]

    ctrl = con.ctrl_host_port
    ftp = ftplib.FTP()
    ftp.connect("127.0.0.1", ctrl, timeout=20)
    ftp.trust_server_pasv_ipv4_address = True
    ftp.login("anonymous", "x")
    names = ftp.nlst()
    assert any("HELLO.TXT" in n.upper() for n in names), names

    data = bytearray()
    ftp.retrbinary("RETR HELLO.TXT", data.extend)
    assert b"hello ftp" in bytes(data)

    ftp.storbinary("STOR UPLOAD.TXT", io.BytesIO(b"uploaded!"))
    assert "UPLOAD.TXT" in [n.upper() for n in ftp.nlst()]
    ftp.quit()

    con._ser.sendall(b"\x1b")
    stopped = _wait_serial(con, "[FTP] STOP")
    assert "[FTP] STOP" in stopped, stopped[-400:]

    con._ser.sendall(b"q")
    con.drain(quiet=0.5)
    listing = con.send_line('DIR "A:/FTPDIR"')
    assert "UPLOAD.TXT" in listing.upper()
    assert con.send_line("PRINT 6") == "6"
