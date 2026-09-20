"""ZMODEM downloads: host receiver unit test and a live TERM transfer."""

import os
import re
import socket
import subprocess
import threading
import time

from harness import MMBasicConsole, TermReplay

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

ZDLE = 0x18
ZRQINIT = 0
ZRINIT = 1
ZFILE = 4
ZSKIP = 5
ZNAK = 6
ZFIN = 8
ZRPOS = 9
ZDATA = 10
ZEOF = 11
ZCRCE = ord("h")
ZCRCG = ord("i")
ZCRCW = ord("k")


def _plain(s: str) -> str:
    return re.sub(r"\x1b\[[0-9;?]*[A-Za-z]", "", s)


def test_zmodem_host_receiver(tmp_path):
    """Compile and run the C receiver against a miniature sender."""
    src = os.path.join(REPO, "tests", "zmodem_host.c")
    impl = os.path.join(REPO, "mmbasic", "src", "zmodem.c")
    exe = os.path.join(tmp_path, "zmodem_host")
    subprocess.run(
        [
            "gcc",
            "-O0",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            os.path.join(REPO, "mmbasic", "include"),
            "-o",
            exe,
            src,
            impl,
        ],
        check=True,
        cwd=REPO,
    )
    out = subprocess.run([exe], check=True, capture_output=True, text=True)
    assert "all checks passed" in out.stdout, out.stdout + out.stderr


def _crc16(data: bytes) -> int:
    crc = 0
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if (crc & 0x8000) else (crc << 1) & 0xFFFF
    return crc


def _hex_header(htype: int, pos: int = 0) -> bytes:
    body = bytes(
        [htype, pos & 0xFF, (pos >> 8) & 0xFF, (pos >> 16) & 0xFF, (pos >> 24) & 0xFF]
    )
    crc = _crc16(body)
    body += bytes([crc >> 8, crc & 0xFF])
    return b"\x2a\x2a\x18B" + body.hex().encode("ascii") + b"\r\n\x11"


def _escape(data: bytes) -> bytes:
    out = bytearray()
    for c in data:
        if c in (ZDLE, 0x11, 0x13, 0x91, 0x93, 0x7F, 0xFF) or (c & 0x60) == 0:
            out.append(ZDLE)
            if c == 0x7F:
                out.append(ord("l"))
            elif c == 0xFF:
                out.append(ord("m"))
            else:
                out.append(c ^ 0x40)
        else:
            out.append(c)
    return bytes(out)


def _data_subpacket(data: bytes, end: int) -> bytes:
    crc = _crc16(bytes(data) + bytes([end]))
    return _escape(data) + bytes([ZDLE, end]) + _escape(bytes([crc >> 8, crc & 0xFF]))


class _HeaderReader:
    def __init__(self, sock: socket.socket) -> None:
        self.sock = sock
        self.buf = bytearray()

    def _try(self):
        i = self.buf.find(b"\x2a\x2a\x18B")
        if i < 0:
            if len(self.buf) > 4:
                del self.buf[:-4]
            return None
        if len(self.buf) < i + 18:
            return None
        try:
            raw = bytes.fromhex(self.buf[i + 4 : i + 18].decode("ascii"))
        except ValueError:
            del self.buf[: i + 4]
            return None
        del self.buf[: i + 18]
        return raw[0], int.from_bytes(raw[1:5], "little")

    def header(self, timeout: float = 4.0, types=None, resend=None):
        deadline = time.time() + timeout
        last = time.time()
        while time.time() < deadline:
            h = self._try()
            if h is not None:
                if types is None or h[0] in types:
                    return h
                continue
            if resend is not None and time.time() - last >= 1.0:
                resend()
                last = time.time()
            try:
                data = self.sock.recv(64)
            except socket.timeout:
                continue
            except OSError:
                return None
            if not data:
                return None
            self.buf.extend(data)
        return None


def _zmodem_send(sock: socket.socket, name: str, data: bytes) -> bool:
    """Minimal ZMODEM sender: hex headers plus CRC-16 data subpackets."""
    reader = _HeaderReader(sock)
    sock.settimeout(0.2)

    def send_zrqinit():
        sock.sendall(b"rz\r" + _hex_header(ZRQINIT))

    send_zrqinit()
    h = reader.header(timeout=12.0, types={ZRINIT}, resend=send_zrqinit)
    assert h is not None and h[0] == ZRINIT, f"no ZRINIT: {h}"

    payload = name.encode() + b"\x00" + str(len(data)).encode()

    def send_zfile():
        sock.sendall(_hex_header(ZFILE) + _data_subpacket(payload, ZCRCW))

    send_zfile()
    h = reader.header(timeout=12.0, types={ZRPOS, ZSKIP, ZNAK}, resend=send_zfile)
    if h is not None and h[0] == ZSKIP:
        return False
    assert h is not None and h[0] == ZRPOS, f"no ZRPOS: {h}"
    assert h[1] == 0, h

    sock.sendall(_hex_header(ZDATA, 0))
    chunk = 1024
    i = 0
    while i < len(data):
        piece = data[i : i + chunk]
        end = ZCRCE if i + len(piece) >= len(data) else ZCRCG
        sock.sendall(_data_subpacket(piece, end))
        i += len(piece)
    if not data:
        sock.sendall(_data_subpacket(b"", ZCRCE))

    sock.sendall(_hex_header(ZEOF, len(data)))
    h = reader.header(timeout=12.0, types={ZRINIT, ZRPOS})
    assert h is not None and h[0] == ZRINIT, f"no ZRINIT after ZEOF: {h}"

    sock.sendall(_hex_header(ZFIN))
    reader.header(timeout=5.0, types={ZFIN})
    return True


def _wait_extra(extras: list, needle: bytes, timeout: float = 10.0) -> bool:
    deadline = time.time() + timeout
    while time.time() < deadline:
        if needle in b"".join(extras):
            return True
        time.sleep(0.05)
    return False


def _wait_prompt(con: MMBasicConsole, timeout: float = 20.0) -> None:
    deadline = time.time() + timeout
    buf = b""
    while time.time() < deadline:
        chunk = con._recv(con._ser)
        if chunk:
            buf += chunk
        if buf.rstrip().endswith(b">"):
            return
    return


def test_term_zmodem_download(kernel_image):
    """A live ZMODEM sender drives a download through TERM "replay"."""
    con = MMBasicConsole(kernel_image)
    con.start()
    replay = TermReplay(con, "127.0.0.1", 0)
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", 0))
    srv.listen(1)
    srv.settimeout(15.0)
    conn = None
    try:
        assert con.send_line('CHDIR "A:/"') == ""
        replay.host = "127.0.0.1"
        replay.port = srv.getsockname()[1]
        text = replay.open_session(timeout=10.0, connect=True)
        assert "Connected" in text, text
        extras: list = []
        stop = [False]

        def pump_loop():
            while not stop[0]:
                extras.append(replay.pump_once())
                time.sleep(0.01)

        pump = threading.Thread(target=pump_loop, daemon=True)
        pump.start()
        conn, _ = srv.accept()
        assert _zmodem_send(conn, "E2E.TXT", b"zmodem e2e payload\n") is True
        assert _wait_extra(extras, b"!ZMODEM done", timeout=10.0), b"".join(extras)
        stop[0] = True
        pump.join(timeout=2)
        con._ser.sendall(bytes([1]) + b"x")
        _wait_prompt(con)
        assert con.send_line('OPEN "A:/E2E.TXT" FOR INPUT AS #1') == ""
        assert con.send_line("LINE INPUT #1, A$") == ""
        assert con.send_line("PRINT A$") == "zmodem e2e payload"
    finally:
        if conn is not None:
            try:
                conn.close()
            except OSError:
                pass
        srv.close()
        replay.stop()
        con.stop()


def test_term_download_folder_dialog(kernel_image):
    """Terminal menu opens a folder browser dialog."""
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        con.drain(quiet=0.1)
        con._ser.sendall(b"TERM\r")
        con.drain(quiet=0.6, timeout=10.0)
        con._ser.sendall(bytes([1]) + b"t")
        con.drain(quiet=0.4, timeout=8.0)
        con._ser.sendall(b"\x1b[B\x1b[B\x1b[B\r")
        seen = _plain(con.drain(quiet=0.6, timeout=8.0).decode(errors="replace"))
        assert "Download folder" in seen, seen
        assert "Use" in seen and "Cancel" in seen, seen
        con._ser.sendall(b"\x1b")
        con.drain(quiet=0.4, timeout=5.0)
        con._ser.sendall(bytes([1]) + b"x")
        _plain(con.drain(quiet=0.8, timeout=15.0).decode(errors="replace"))
        assert con.send_line("PRINT 1+1") == "2"
    finally:
        con.stop()
