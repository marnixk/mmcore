"""Backward-compatible alias for ordered direct replay."""

from __future__ import annotations

from .term_log import TermLogRec
from .term_replay import TermReplay
from .term_server import replay_termlog_ordered


def replay_termlog_direct(
    replay: TermReplay,
    text: str,
    *,
    end_ms: int | None = None,
    start_ms: int | None = None,
    pump_rounds: int = 6,
    **_ignored,
) -> tuple[str, list[TermLogRec]]:
    return replay_termlog_ordered(
        replay,
        text,
        connect=False,
        recv_tcp=False,
        start_ms=start_ms,
        end_ms=end_ms,
        pump_rounds=pump_rounds,
    )
