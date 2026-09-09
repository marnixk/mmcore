"""TERM replay host shuttles a live TCP service through QEMU serial."""

import socket
import threading
import time

from harness import MMBasicConsole, TermReplay
from test_term import _plain, _quit


def _open_replay(con, replay, timeout=8.0):
    con.drain(quiet=0.1)
    con._ser.sendall(b'TERM "replay", 23\r')
    deadline = time.time() + timeout
    acc = b""
    while time.time() < deadline:
        acc += replay.pump_once()
        if b"replay" in acc.lower() or b"Connected" in acc or b"TERM" in acc:
            for _ in range(20):
                acc += replay.pump_once()
                time.sleep(0.03)
            return _plain(acc.decode(errors="replace"))
        time.sleep(0.02)
    return _plain(acc.decode(errors="replace"))


def _echo_server(banner: bytes):
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", 0))
    srv.listen(1)
    port = srv.getsockname()[1]
    held = {"sock": None}

    def run():
        try:
            srv.settimeout(8)
            c, _ = srv.accept()
            held["sock"] = c
            c.settimeout(0.2)
            c.sendall(banner)
            while True:
                try:
                    n = c.recv(64)
                except socket.timeout:
                    continue
                except OSError:
                    break
                if not n:
                    break
                c.sendall(n)
        finally:
            srv.close()

    th = threading.Thread(target=run, daemon=True)
    th.start()
    return port, held, th


def test_term_replay_local_banner_and_echo(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    replay = TermReplay(con, "127.0.0.1", 0)
    try:
        port, held, _th = _echo_server(b"HELLO-REPLAY\r\n")
        replay.port = port
        replay.host = "127.0.0.1"
        replay.connect()
        seen = _open_replay(con, replay)
        more = replay.wait_serial(lambda s: "HELLO-REPLAY" in s, timeout=6.0)
        text = seen + more
        assert "HELLO-REPLAY" in text
        replay.send_keys(b"Z")
        echoed = replay.wait_serial(lambda s: "Z" in s, timeout=4.0)
        assert "Z" in echoed
        _quit(con)
        assert con.send_line("PRINT 2+2") == "4"
    finally:
        replay.stop()
        if held.get("sock"):
            try:
                held["sock"].close()
            except OSError:
                pass
        con.stop()


def test_term_replay_charset_esc_b_not_printed(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    replay = TermReplay(con, "127.0.0.1", 1)
    try:
        seen = _open_replay(con, replay)
        replay._to_guest(b"\x1b(BKEEP")
        time.sleep(0.2)
        more = replay.wait_serial(lambda s: "KEEP" in s, timeout=4.0)
        text = seen + more
        assert "KEEP" in text
        assert "BKEEP" not in text
        _quit(con)
        assert con.send_line("PRINT 1") == "1"
    finally:
        replay.stop()
        con.stop()
