"""TERM replay host shuttles a live TCP service through QEMU serial."""

import re
import socket
import threading
import time

from harness import MMBasicConsole, TermReplay
from test_term import _plain, _quit, _pane_grey_hits


def _open_replay(con, replay, timeout=8.0, connect=True):
    con.drain(quiet=0.1)
    con._ser.sendall(b'TERM "replay", 23\r')
    deadline = time.time() + timeout
    acc = b""
    while time.time() < deadline:
        acc += replay.pump_once(recv_tcp=False)
        if b"Connected" in acc:
            break
        time.sleep(0.02)
    if connect:
        replay.connect()
    for _ in range(40):
        acc += replay.pump_once()
        time.sleep(0.03)
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
            srv.settimeout(20)
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
        seen = _open_replay(con, replay, connect=True)
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
        seen = _open_replay(con, replay, connect=False)
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


def test_term_replay_csi_split_across_frames(kernel_image):
    """ESC then CSI payload in the next UART frame must still erase, not print '['."""
    con = MMBasicConsole(kernel_image)
    con.start()
    replay = TermReplay(con, "127.0.0.1", 1)
    try:
        seen = _open_replay(con, replay, connect=False)
        replay._to_guest(b"NOISE")
        replay._to_guest(b"\x1b")
        replay._to_guest(b"[2J\x1b[1;1HSPLITOK")
        more = replay.wait_serial(lambda s: "SPLITOK" in s, timeout=6.0)
        text = _plain(seen + more)
        assert "SPLITOK" in text
        assert "[2J" not in text
        _quit(con)
        assert con.send_line("PRINT 3+3") == "6"
    finally:
        replay.stop()
        con.stop()


def test_term_replay_remote_close_reports_reason(kernel_image):
    """A hangup renders the pending text, then names the reason on its own line."""
    con = MMBasicConsole(kernel_image)
    con.start()
    replay = TermReplay(con, "127.0.0.1", 1)
    try:
        seen = _open_replay(con, replay, connect=False)
        replay._to_guest(b"Enter your ")
        replay.wait_serial(lambda s: "Enter your" in s, timeout=6.0)
        replay.close_from_host("reset by peer")
        more = replay.wait_serial(
            lambda s: "\nConnection closed: reset by peer" in _plain(s), timeout=6.0
        )
        text = _plain(seen + more)
        assert "!NET Connection closed: reset by peer" in text
        assert re.search(r"Enter your +\r?\n\r?Connection closed: reset by peer", text), text
        _quit(con)
        assert con.send_line("PRINT 6+1") == "7"
    finally:
        replay.stop()
        con.stop()


def test_term_replay_iac_split_across_frames(kernel_image):
    """IAC at end of a chunk is held; WILL ECHO in the next chunk is not a glyph."""
    con = MMBasicConsole(kernel_image)
    con.start()
    replay = TermReplay(con, "127.0.0.1", 1)
    try:
        seen = _open_replay(con, replay, connect=False)
        replay._to_guest(bytes([255]))
        replay._to_guest(bytes([251, 1]) + b"AFTERIAC")
        more = replay.wait_serial(lambda s: "AFTERIAC" in s, timeout=6.0)
        text = seen + more
        assert "AFTERIAC" in text
        _quit(con)
        assert con.send_line("PRINT 4+1") == "5"
    finally:
        replay.stop()
        con.stop()


def test_term_replay_cursor_on_then_host_hides(kernel_image):
    """Cursor is on at connect; CSI ?25l from the host turns it off."""
    con = MMBasicConsole(kernel_image)
    con.start()
    replay = TermReplay(con, "127.0.0.1", 1)
    try:
        _open_replay(con, replay, connect=False)
        on_hits = _pane_grey_hits(con, 0, 1)
        con.capture_png("/opt/cursor/artifacts/term_cursor_on_connect.png")
        assert on_hits >= 12, on_hits
        replay._to_guest(b"\x1b[?25l")
        time.sleep(0.25)
        replay.pump_once(recv_tcp=False)
        time.sleep(0.15)
        off_hits = _pane_grey_hits(con, 0, 1)
        con.capture_png("/opt/cursor/artifacts/term_cursor_hidden_by_host.png")
        assert off_hits <= 2, off_hits
        replay._to_guest(b"\x1b[?25h")
        time.sleep(0.25)
        replay.pump_once(recv_tcp=False)
        time.sleep(0.15)
        shown = _pane_grey_hits(con, 0, 1)
        con.capture_png("/opt/cursor/artifacts/term_cursor_shown_by_host.png")
        assert shown >= 12, shown
        _quit(con)
        assert con.send_line("PRINT 5") == "5"
    finally:
        replay.stop()
        con.stop()


def _is_vga_cyan(r: int, g: int, b: int) -> bool:
    return g > 80 and b > 80 and g > r + 20 and b > r + 20


def test_term_replay_newline_burst_paints_latest_line(kernel_image):
    """A burst of newlines must redraw from cells, keeping the last line on HDMI."""
    con = MMBasicConsole(kernel_image)
    con.start()
    replay = TermReplay(con, "127.0.0.1", 1)
    try:
        _open_replay(con, replay, connect=False)
        burst = b"\x1b[?25l"
        for i in range(80):
            burst += f"FILL{i:03d}\r\n".encode()
        burst += b"\x1b[1;36mCYANMARK"
        replay._to_guest(burst)
        more = replay.wait_serial(lambda s: "CYANMARK" in s, timeout=10.0)
        assert "CYANMARK" in more
        time.sleep(0.3)
        found_cyan = False
        for x in (164, 172, 180, 188, 196, 204):
            for y in range(400, 520, 8):
                if _is_vga_cyan(*con.screen_pixel(x, y)):
                    found_cyan = True
                    break
            if found_cyan:
                break
        con.capture_png("/opt/cursor/artifacts/term_newline_burst_cyanmark.png")
        assert found_cyan, "expected cyan last line after a newline burst"
        _quit(con)
        assert con.send_line("PRINT 8") == "8"
    finally:
        replay.stop()
        con.stop()
