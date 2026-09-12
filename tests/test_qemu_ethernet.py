"""Live Ethernet under QEMU usb-net (opt-in; default console has no NIC)."""

import os
import socket
import threading
import time

from harness import parse_termlog, qemu_usb_net_args
from test_term import _quit
from test_term_log import _read_termlog, _termlog_path

ARTIFACTS = "/opt/cursor/artifacts"
MARKER = b"ETHHDMI"


def test_qemu_usb_net_args():
    assert qemu_usb_net_args() == [
        "-netdev", "user,id=net0",
        "-device", "usb-net,netdev=net0",
    ]
    fwd = qemu_usb_net_args("tcp::8080-:80")
    assert fwd[1] == "user,id=net0,hostfwd=tcp::8080-:80"
    assert fwd[3] == "usb-net,netdev=net0"
    dumped = qemu_usb_net_args(dump="/tmp/net.pcap")
    assert dumped[-2:] == [
        "-object",
        "filter-dump,id=fnet0,netdev=net0,file=/tmp/net.pcap",
    ]


def test_qemu_kernel_links_cdc_ethernet(kernel_image):
    """QEMU image has Circle TCP + USB CDC Ethernet, not the CYW4343x radio."""
    map_path = os.path.join(os.path.dirname(kernel_image), "kernel8.map")
    text = open(map_path, encoding="utf-8", errors="replace").read()
    assert "CNetSubSystem" in text
    assert "CUSBCDCEthernetDevice" in text
    assert "CBcm4343Device" not in text


def _wait_dhcp(con, timeout=35):
    deadline = time.time() + timeout
    last = ""
    while time.time() < deadline:
        last = con.send_line("IPCONFIG", timeout=8)
        if "10.0.2." in last or "connected as" in last.lower():
            return last
        time.sleep(1)
    return last


def test_option_ethernet_dhcp_on_usb_net(net_console):
    con = net_console
    assert con.send_line("FACTORY_RESET") == "Factory defaults restored"
    out = con.send_line("OPTION ETHERNET ON", timeout=25)
    low = out.lower()
    assert "?syntax error" not in low
    assert "ethernet not available" not in low
    cfg = out if "10.0.2." in out else _wait_dhcp(con)
    combined = (out + "\n" + cfg).lower()
    assert "interface: ethernet" in combined or "ethernet enabled" in combined or "connected as" in combined
    assert "10.0.2." in (out + cfg) or "link is up" in combined, cfg
    listed = con.send_line("OPTION LIST")
    assert "OPTION ETHERNET ON" in listed
    cfg = con.send_line("IPCONFIG", timeout=8)
    assert "interface: ethernet" in cfg.lower()
    assert con.send_line("PRINT 2+2") == "4"


def test_qemu_ethernet_tcp_to_host(net_console):
    con = net_console
    assert con.send_line("FACTORY_RESET") == "Factory defaults restored"
    on = con.send_line("OPTION ETHERNET ON", timeout=25)
    if "10.0.2." not in on:
        cfg = _wait_dhcp(con)
        assert "10.0.2." in (on + cfg) or "link is up" in cfg.lower(), cfg

    got = []
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", 0))
    port = srv.getsockname()[1]
    srv.listen(1)
    srv.settimeout(20)

    def accept():
        try:
            conn, _ = srv.accept()
            conn.settimeout(5)
            got.append(conn.recv(64))
            conn.sendall(b"PONG\n")
            conn.close()
        except OSError:
            got.append(b"")

    th = threading.Thread(target=accept, daemon=True)
    th.start()
    try:
        open_out = con.send_line(f'OPEN "TCP:10.0.2.2:{port}" AS #1', timeout=25)
        assert "?SYNTAX ERROR" not in open_out.upper()
        low = open_out.lower()
        assert "network not available" not in low
        assert "dns failed" not in low
        assert "tcp timeout" not in low
        assert "tcp refused" not in low
        assert con.send_line('PRINT #1, "PING"') == ""
        time.sleep(0.5)
        reply = con.send_line("PRINT INPUT$(5, #1)", timeout=8)
        assert "PONG" in reply
        assert con.send_line("CLOSE #1") == ""
        th.join(timeout=5)
        assert got and b"PING" in got[0]
    finally:
        srv.close()
        con.send_line("CLOSE #1")
        assert con.send_line("PRINT 9") == "9"


def _wait_serial(con, needle: bytes, timeout: float) -> bytes:
    deadline = time.time() + timeout
    acc = b""
    while time.time() < deadline:
        acc += con.drain(quiet=0.05, timeout=0.3)
        if needle in acc:
            return acc
        time.sleep(0.05)
    return acc


def _luminance(r: int, g: int, b: int) -> float:
    return 0.299 * r + 0.587 * g + 0.114 * b


def test_qemu_ethernet_term_hdmi_not_serial_pane(net_console):
    """Live Circle TCP: HDMI shows the session; serial is !NET counters, not pane dumps."""
    con = net_console
    os.makedirs(ARTIFACTS, exist_ok=True)
    assert con.send_line("FACTORY_RESET") == "Factory defaults restored"
    on = con.send_line("OPTION ETHERNET ON", timeout=25)
    if "10.0.2." not in on:
        cfg = _wait_dhcp(con)
        assert "10.0.2." in (on + cfg) or "link is up" in cfg.lower(), cfg
    dbg = con.send_line("OPTION WIFI DEBUG ON")
    assert "?SYNTAX ERROR" not in dbg.upper()
    log_on = con.send_line("OPTION TERM LOG ON")
    assert ".termlog" in log_on

    received = []
    hold = threading.Event()
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", 0))
    port = srv.getsockname()[1]
    srv.listen(1)
    srv.settimeout(25)

    def accept():
        conn = None
        try:
            conn, _ = srv.accept()
            conn.settimeout(8)
            try:
                received.append(conn.recv(64))
            except OSError:
                received.append(b"")
            conn.sendall(MARKER + b"\r\nlogin:\r\n")
            try:
                conn.settimeout(20)
                received.append(conn.recv(64))
            except OSError:
                received.append(b"")
            hold.wait(25)
        except OSError:
            received.append(b"")
        finally:
            if conn is not None:
                try:
                    conn.close()
                except OSError:
                    pass

    th = threading.Thread(target=accept, daemon=True)
    th.start()
    try:
        con.drain(quiet=0.1, timeout=0.4)
        con._ser.sendall(f'TERM "10.0.2.2", {port}\r'.encode())
        serial = _wait_serial(con, b"!NET connected", timeout=25.0)
        serial_text = serial.decode(errors="replace")
        assert b"!NET connected" in serial, serial_text[-800:]
        assert MARKER not in serial, serial_text[-800:]

        ocr = con.wait_ocr("ETHHDMI", timeout=18.0)
        png = con.capture_png(os.path.join(ARTIFACTS, "qemu_ethernet_term_hdmi.png"))
        serial += con.drain(quiet=0.2, timeout=1.0)
        serial_text = serial.decode(errors="replace")
        assert MARKER not in serial, serial_text[-1200:]
        assert "!NET " in serial_text

        pane = con.screen_pixel(164, 24)
        cream = _luminance(*pane) > 80
        ocr_hit = "ethhdmi" in ocr.lower() or "login" in ocr.lower()
        assert cream or ocr_hit, (
            f"expected HDMI banner (ocr={ocr!r} pixel={pane} png={png})"
        )
        con._ser.sendall(b"guest\r")
        time.sleep(0.4)
        _quit(con)
        assert con.send_line("PRINT 3+4") == "7"
        off = con.send_line("OPTION TERM LOG OFF")
        assert "in=" in off
        path = _termlog_path(con)
        assert path, "expected A:/.termlog or C:/.termlog"
        body = _read_termlog(con, path)
        recs = parse_termlog(body)
        rx = b"".join(r.data for r in recs if r.kind == "R")
        assert MARKER in rx, body[-800:]
        th.join(timeout=5)
    finally:
        hold.set()
        srv.close()
        try:
            _quit(con)
        except Exception:
            pass
        con.send_line("PRINT 1")
