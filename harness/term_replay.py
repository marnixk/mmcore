"""Live TERM replay: shuttle a real TCP host through QEMU serial.

Guest ``TERM "replay", port`` emits ``!TX <hex>`` for outbound bytes and
accepts inbound frames ``\\x1eRX<hex>\\n``; ``\\x1eRC <reason>\\n`` makes
the guest treat the connection as closed by the remote host. This module
talks to that session from the pytest harness without a guest NIC.
"""

from __future__ import annotations

import re
import re
import socket
import threading
import time
from typing import Callable

from .qemu_harness import MMBasicConsole

RS = 0x1E
_TX_RE = re.compile(rb"!TX ([0-9A-Fa-f]+)\r?\n")


class TermReplay:
    def __init__(self, con: MMBasicConsole, host: str, port: int) -> None:
        self.con = con
        self.host = host
        self.port = port
        self.sock: socket.socket | None = None
        self.log: list[tuple[str, float, bytes]] = []
        self.serial_buf = b""
        self.pending_out = b""
        self._stop = False
        self._thread: threading.Thread | None = None
        self._lock = threading.Lock()

    def connect(self, timeout: float = 15.0) -> None:
        self.sock = socket.create_connection((self.host, self.port), timeout=timeout)
        self.sock.settimeout(0.05)
        if self.pending_out:
            try:
                self.sock.sendall(self.pending_out)
            except OSError:
                pass
            self.pending_out = b""

    def start_pump(self) -> None:
        self._stop = False
        self._thread = threading.Thread(target=self._pump, daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._stop = True
        if self._thread is not None:
            self._thread.join(timeout=2)
            self._thread = None
        if self.sock is not None:
            try:
                self.sock.close()
            except OSError:
                pass
            self.sock = None

    def pump_once(self, recv_tcp: bool = True) -> bytes:
        """One shuttle step; returns extra serial text (pane dumps, !MON)."""
        extra = b""
        if recv_tcp and self.sock is not None:
            try:
                data = self.sock.recv(256)
            except socket.timeout:
                data = b""
            except OSError:
                data = b""
            if data:
                self.log.append(("in", time.time(), data))
                self._to_guest(data)
        chunk = self.con._recv(self.con._ser)
        with self._lock:
            if chunk:
                self.serial_buf += chunk
            extra, self.serial_buf = self._consume_tx(self.serial_buf)
        return extra

    def wait_serial(self, pred: Callable[[str], bool], timeout: float = 8.0) -> str:
        deadline = time.time() + timeout
        acc = ""
        while time.time() < deadline:
            extra = self.pump_once()
            if extra:
                acc += extra.decode(errors="replace")
                if pred(acc):
                    return acc
            else:
                time.sleep(0.02)
        return acc

    def send_keys(self, data: bytes) -> None:
        assert self.con._ser is not None
        self.con._ser.sendall(data)

    def open_session(self, timeout: float = 8.0, connect: bool = True) -> str:
        """Open ``TERM "replay", port`` and optionally connect TCP."""
        self.con.drain(quiet=0.1)
        self.con._ser.sendall(
            f'TERM "replay", {self.port}\r'.encode()
        )
        deadline = time.time() + timeout
        acc = b""
        while time.time() < deadline:
            acc += self.pump_once(recv_tcp=False)
            if b"Connected" in acc:
                break
            time.sleep(0.02)
        if connect:
            self.connect()
        old_timeout = self.con._ser.gettimeout() if self.con._ser else None
        if self.con._ser is not None:
            self.con._ser.settimeout(2.0)
        for _ in range(40):
            acc += self.pump_once()
            time.sleep(0.03)
        if self.con._ser is not None and old_timeout is not None:
            self.con._ser.settimeout(old_timeout)
        text = acc.decode(errors="replace")
        return re.sub(r"\x1b\[[0-9;?]*[A-Za-z]", "", text)

    def close_from_host(self, reason: str = "") -> None:
        """Tell the guest the remote host hung up (``RS RC [reason] NL``)."""
        assert self.con._ser is not None
        text = reason.encode("ascii", errors="replace")
        self.con._ser.sendall(bytes([RS]) + b"RC " + text + b"\n")

    def _to_guest(self, data: bytes) -> None:
        assert self.con._ser is not None
        old_timeout = self.con._ser.gettimeout()
        self.con._ser.settimeout(0.4)
        try:
            off = 0
            while off < len(data):
                piece = data[off : off + 32]
                off += len(piece)
                hexpart = piece.hex()
                frame = bytes([RS]) + b"RX" + hexpart.encode("ascii") + b"\n"
                for _attempt in range(40):
                    try:
                        self.con._ser.sendall(frame)
                        break
                    except socket.timeout:
                        extra = self.con._recv(self.con._ser)
                        if extra:
                            with self._lock:
                                self.serial_buf += extra
                        time.sleep(0.01)
                else:
                    self.con._ser.sendall(frame)
                extra = self.con._recv(self.con._ser)
                if extra:
                    with self._lock:
                        self.serial_buf += extra
        finally:
            self.con._ser.settimeout(old_timeout)

    def _consume_tx(self, buf: bytes) -> tuple[bytes, bytes]:
        extra = b""
        while True:
            m = _TX_RE.search(buf)
            if not m:
                keep = buf.rfind(b"!TX ")
                if keep < 0:
                    extra += buf
                    buf = b""
                else:
                    extra += buf[:keep]
                    buf = buf[keep:]
                break
            extra += buf[: m.start()]
            payload = bytes.fromhex(m.group(1).decode("ascii"))
            self.log.append(("out", time.time(), payload))
            if payload:
                if self.sock is not None:
                    try:
                        self.sock.sendall(payload)
                    except OSError:
                        pass
                else:
                    self.pending_out += payload
            buf = buf[m.end() :]
        return extra, buf

    def _pump(self) -> None:
        while not self._stop:
            self.pump_once()
            time.sleep(0.01)
