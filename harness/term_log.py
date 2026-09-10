"""Parse and play OPTION TERM LOG captures (C:/.termlog / A:/.termlog).

Guest format, one record per line::

    # TERMLOG 1
    R <in_total> <rendered> <hex>   incoming host bytes
    T <in_total> <rendered> <hex>   outbound (typed / telnet)

``in_total`` is incoming bytes seen after that record's payload.
``rendered`` is the last incoming count that drew a pane cell.
``T`` lines after a freeze keep ``rendered`` stuck while ``in_n`` grows.
"""

from __future__ import annotations

import time
from dataclasses import dataclass

from .term_replay import TermReplay


@dataclass(frozen=True)
class TermLogRec:
    kind: str
    in_n: int
    rendered: int
    data: bytes


def parse_termlog(text: str) -> list[TermLogRec]:
    recs: list[TermLogRec] = []
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split(" ", 3)
        if len(parts) < 3:
            continue
        kind = parts[0]
        if kind not in ("R", "T"):
            continue
        hexpart = parts[3] if len(parts) > 3 else ""
        try:
            in_n = int(parts[1])
            rendered = int(parts[2])
            data = bytes.fromhex(hexpart) if hexpart else b""
        except ValueError:
            continue
        recs.append(TermLogRec(kind, in_n, rendered, data))
    return recs


def play_termlog(replay: TermReplay, text: str, *, send_keys: bool = True) -> str:
    """Feed a capture into an open ``TERM "replay"`` session."""
    acc = ""
    for rec in parse_termlog(text):
        if rec.kind == "R":
            replay._to_guest(rec.data)
        elif rec.kind == "T" and send_keys:
            if rec.data[:1] == b"\xff":
                continue
            replay.send_keys(rec.data)
        else:
            continue
        idle = 0
        for _ in range(12):
            extra = replay.pump_once(recv_tcp=False)
            if extra:
                acc += extra.decode(errors="replace")
                idle = 0
            else:
                idle += 1
                time.sleep(0.01)
                if idle >= 3:
                    break
    return acc
