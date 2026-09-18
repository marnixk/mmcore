"""TCP server and ordered TERMLOG replay (R from host, T from harness).

Replay preserves log record order and per-line chunk boundaries. Millisecond
timestamps in the capture are ignored; only byte order matters.
"""

from __future__ import annotations

import socket
import threading
import time
from dataclasses import dataclass, field

from .term_log import TermLogRec, parse_termlog
from .term_replay import TermReplay


def normalize_termlog(text: str) -> str:
    """Ensure v2 captures have a ``# TERMLOG 2`` header."""
    stripped = text.lstrip()
    if stripped.startswith("# TERMLOG"):
        return text
    first = stripped.split("\n", 1)[0].split()
    if len(first) >= 4 and first[0] in ("R", "T"):
        try:
            int(first[1])
            int(first[2])
            int(first[3])
            return "# TERMLOG 2\n" + text.lstrip()
        except ValueError:
            pass
    return text


def load_termlog(text: str) -> list[TermLogRec]:
    return parse_termlog(normalize_termlog(text))


def typed_keys(recs: list[TermLogRec]) -> list[TermLogRec]:
    return [r for r in recs if r.kind == "T" and r.data[:1] != b"\xff"]


def rx_records(recs: list[TermLogRec]) -> list[TermLogRec]:
    return [r for r in recs if r.kind == "R"]


def filter_recs(
    recs: list[TermLogRec],
    *,
    start_ms: int | None = None,
    end_ms: int | None = None,
) -> list[TermLogRec]:
    out = recs
    if start_ms is not None:
        out = [r for r in out if (r.ms or 0) >= start_ms]
    if end_ms is not None:
        out = [r for r in out if (r.ms or 0) <= end_ms]
    return out


def render_lag(rec: TermLogRec) -> int:
    return rec.in_n - rec.rendered


def max_render_lag(recs: list[TermLogRec]) -> tuple[int, TermLogRec | None]:
    best = 0
    at: TermLogRec | None = None
    for r in recs:
        lag = render_lag(r)
        if lag > best:
            best = lag
            at = r
    return best, at


def iter_replay_steps(recs: list[TermLogRec]):
    """Yield (kind, data) in strict log order, one capture line at a time."""
    for rec in recs:
        if rec.kind == "R":
            yield ("R", rec.data)
        elif rec.kind == "T" and rec.data[:1] != b"\xff":
            yield ("T", rec.data)


def _drain_replay(replay: TermReplay, *, recv_tcp: bool) -> str:
    """Pump the replay session like play_termlog."""
    acc = ""
    last = time.time()
    while time.time() - last < 0.30:
        extra = replay.pump_once(recv_tcp=recv_tcp)
        if extra:
            acc += extra.decode(errors="replace")
            last = time.time()
        else:
            time.sleep(0.01)
    return acc


@dataclass
class TermLogServer:
    """Accept one TCP client and send host bytes on demand."""

    text: str
    bind: str = "127.0.0.1"
    port: int = 0
    accept_timeout: float = 30.0
    recs: list[TermLogRec] = field(init=False)
    rx: list[TermLogRec] = field(init=False)
    port_actual: int = 0
    received: bytes = b""
    _srv: socket.socket | None = field(default=None, init=False, repr=False)
    _conn: socket.socket | None = field(default=None, init=False, repr=False)
    _thread: threading.Thread | None = field(default=None, init=False, repr=False)
    _ready: threading.Event = field(default_factory=threading.Event, init=False)
    _lock: threading.Lock = field(default_factory=threading.Lock, init=False)

    def __post_init__(self) -> None:
        self.recs = load_termlog(self.text)
        self.rx = rx_records(self.recs)

    def start(self) -> int:
        self._ready.clear()
        self.received = b""
        self._conn = None
        self._srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._srv.bind((self.bind, self.port))
        self._srv.listen(1)
        self.port_actual = self._srv.getsockname()[1]
        self._thread = threading.Thread(target=self._accept_loop, daemon=True)
        self._thread.start()
        return self.port_actual

    def flush(self) -> None:
        with self._lock:
            conn = self._conn
        if conn is not None:
            self._drain(conn)

    def stop(self) -> None:
        self.flush()
        with self._lock:
            if self._conn is not None:
                try:
                    self._conn.close()
                except OSError:
                    pass
                self._conn = None
            if self._srv is not None:
                try:
                    self._srv.close()
                except OSError:
                    pass
                self._srv = None
        if self._thread is not None:
            self._thread.join(timeout=3)
            self._thread = None

    def wait_connected(self, timeout: float = 15.0) -> None:
        if not self._ready.wait(timeout=timeout):
            raise TimeoutError("TermLogServer: client never connected")

    def send(self, data: bytes, *, chunk: int = 256) -> None:
        if not data:
            return
        self.wait_connected(timeout=self.accept_timeout)
        with self._lock:
            conn = self._conn
        if conn is None:
            raise OSError("TermLogServer: not connected")
        for off in range(0, len(data), chunk):
            conn.sendall(data[off : off + chunk])
            self._drain(conn)

    def send_all_rx(self) -> None:
        for rec in self.rx:
            self.send(rec.data)

    def _accept_loop(self) -> None:
        assert self._srv is not None
        try:
            self._srv.settimeout(self.accept_timeout)
            conn, _addr = self._srv.accept()
            conn.settimeout(0.1)
            with self._lock:
                self._conn = conn
            self._ready.set()
            while True:
                if not self._drain(conn):
                    break
        except OSError:
            pass
        finally:
            self._ready.set()

    def _drain(self, conn: socket.socket) -> bool:
        got = False
        while True:
            try:
                chunk = conn.recv(4096)
            except socket.timeout:
                break
            except OSError:
                return False
            if not chunk:
                return False
            self.received += chunk
            got = True
        return got


def recs_to_termlog_text(recs: list[TermLogRec]) -> str:
    lines = ["# TERMLOG 2"]
    for rec in recs:
        lines.append(
            f"{rec.kind} {rec.ms} {rec.in_n} {rec.rendered} {rec.data.hex().upper()}"
        )
    return "\n".join(lines) + "\n"


def replay_termlog_ordered(
    replay: TermReplay,
    text: str,
    *,
    connect: bool = True,
    recv_tcp: bool = True,
    start_ms: int | None = None,
    end_ms: int | None = None,
    pump_rounds: int = 6,
) -> tuple[str, list[TermLogRec]]:
    """Walk the capture in log order; R via serial RX, T via keystrokes."""
    from .term_log import play_termlog

    recs = filter_recs(load_termlog(text), start_ms=start_ms, end_ms=end_ms)
    acc = play_termlog(replay, recs_to_termlog_text(recs), send_keys=True)
    return acc, recs


def replay_termlog_session(
    replay: TermReplay,
    text: str,
    *,
    connect: bool = True,
    start_ms: int | None = None,
    end_ms: int | None = None,
    pump_rounds: int = 6,
) -> tuple[str, TermLogServer, list[TermLogRec]]:
    """Walk the capture in log order; R via TCP server, T via keystrokes."""
    server = TermLogServer(text)
    port = server.start()
    replay.host = server.bind
    replay.port = port

    seen = replay.open_session(connect=connect)
    server.wait_connected(timeout=15.0)
    acc = seen
    recs = filter_recs(server.recs, start_ms=start_ms, end_ms=end_ms)
    for kind, data in iter_replay_steps(recs):
        if kind == "R":
            server.send(data)
        else:
            replay.send_keys(data)
        acc += _drain_replay(replay, recv_tcp=True)
    for _ in range(20):
        acc += _drain_replay(replay, recv_tcp=True)
    server.stop()
    return acc, server, recs
