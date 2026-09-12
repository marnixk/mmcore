"""Live Ethernet under QEMU usb-net (opt-in; default console has no NIC)."""

import os
import socket
import threading
import time

from harness import qemu_usb_net_args


def test_qemu_usb_net_args():
    assert qemu_usb_net_args() == [
        "-netdev", "user,id=net0",
        "-device", "usb-net,netdev=net0",
    ]
    fwd = qemu_usb_net_args("tcp::8080-:80")
    assert fwd[1] == "user,id=net0,hostfwd=tcp::8080-:80"
    assert fwd[3] == "usb-net,netdev=net0"


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
