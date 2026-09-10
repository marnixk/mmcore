"""Live TERM replay: shuttle a real TCP host through QEMU serial.

Guest ``TERM "replay", port`` emits ``!TX <hex>`` for outbound bytes and
accepts inbound frames ``\\x1eRX<hex>\\n``. This module talks to that
session from the pytest harness without a guest NIC.
"""

from __future__ import annotations

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
        if chunk:
            with self._lock:
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

    def _to_guest(self, data: bytes) -> None:
        assert self.con._ser is not None
        hexpart = data.hex()
        self.con._ser.sendall(bytes([RS]) + b"RX" + hexpart.encode("ascii") + b"\n")

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
