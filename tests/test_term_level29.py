"""Level29 BBS (bbs.fozztexx.com:23) TERM capture analysis and replay."""

import socket
import time
from pathlib import Path

from harness import (
    MMBasicConsole,
    TermReplay,
    filter_recs,
    load_termlog,
    max_render_lag,
    normalize_termlog,
    parse_termlog,
    play_termlog,
    replay_termlog_session,
    typed_keys,
)
from harness.term_server import recs_to_termlog_text
from test_term import _plain, _quit

REPO = Path(__file__).resolve().parents[1]
LEVEL29 = REPO / "tests" / "term" / "level29-termlog-v2"


def _level29_text() -> str:
    return LEVEL29.read_text()


def test_level29_v2_parses_without_header():
    raw = _level29_text()
    assert not raw.lstrip().startswith("# TERMLOG")
    recs = parse_termlog(raw)
    assert len(recs) >= 600
    assert all(r.ms is not None for r in recs)


def test_level29_v2_keystrokes_not_duplicated():
    recs = load_termlog(_level29_text())
    typed = typed_keys(recs)
    assert typed, "expected typed keys in capture"
    pairs = 0
    i = 0
    while i + 1 < len(typed):
        a, b = typed[i], typed[i + 1]
        if a.data == b.data and a.in_n == b.in_n:
            pairs += 1
            i += 2
        else:
            i += 1
    assert pairs == 0, (pairs, [t.data for t in typed[:12]])


def test_level29_v2_log_invalid_login_and_truncated_re_prompt():
    recs = load_termlog(_level29_text())
    rx = b"".join(r.data for r in recs if r.kind == "R")
    assert b"Enter your username or NEW or VISITOR" in rx
    assert b"User: ireal" in rx
    assert b"Password: ********" in rx
    assert b"Invalid user or password" in rx
    assert rx.endswith(b"Enter your ")
    typed = typed_keys(recs)
    assert b"".join(t.data for t in typed) == b"ireal\nds9space\nx"
    lag, _at = max_render_lag(recs)
    assert lag <= 40, "rendered should track in_n within one screen row"
    # Eight mask chars + invalid matches wrong 8-byte password on the wire,
    # not a display/render failure (see tests/test_term_level29_bbs.py).


def test_level29_replay_server_login_flow(kernel_image):
    """Replay login slice in log order via serial RX."""
    from harness import play_termlog
    from test_term_replay import _open_replay

    con = MMBasicConsole(kernel_image)
    con.start()
    replay = TermReplay(con, "127.0.0.1", 1)
    try:
        recs = filter_recs(load_termlog(_level29_text()), start_ms=191447, end_ms=203000)
        seen = _open_replay(con, replay, connect=False)
        played = play_termlog(replay, recs_to_termlog_text(recs), send_keys=True)
        plain = _plain(seen + played)
        assert "Enter your username" in plain or "User:" in plain
        assert "ireal" in plain
        assert "Password:" in plain
        assert "Invalid user or password" in plain
        _quit(con)
        assert con.send_line("PRINT 1+1") == "2"
    finally:
        replay.stop()
        con.stop()


def test_level29_tcp_server_replays_host_bytes():
    """TermLogServer sends host records in log order."""
    import socket

    from harness import TermLogServer

    text = (
        "# TERMLOG 2\n"
        "R 10 5 0 48656C6C6F0D0A\n"
        "T 50 5 5 58\n"
    )
    server = TermLogServer(text)
    port = server.start()
    sock = socket.create_connection(("127.0.0.1", port), timeout=5)
    sock.settimeout(5)
    try:
        server.wait_connected(timeout=5)
        server.send_all_rx()
        got = b""
        while len(got) < 7:
            chunk = sock.recv(64)
            if not chunk:
                break
            got += chunk
        assert got == b"Hello\r\n"
        sock.sendall(b"X")
        time.sleep(0.05)
        server.flush()
    finally:
        sock.close()
        server.stop()
    assert server.received == b"X"


def test_level29_replay_tcp_session_login_flow(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    replay = TermReplay(con, "127.0.0.1", 1)
    server = None
    try:
        text, server, _recs = replay_termlog_session(
            replay,
            _level29_text(),
            start_ms=191447,
            end_ms=203000,
        )
        plain = _plain(text)
        assert "Enter your username" in plain or "User:" in plain
        assert "ireal" in plain
        assert "Invalid user or password" in plain
        _quit(con)
        assert con.send_line("PRINT 1+1") == "2"
    finally:
        if server is not None:
            server.stop()
        replay.stop()
        con.stop()


def test_level29_replay_second_prompt_visible_before_x(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    replay = TermReplay(con, "127.0.0.1", 1)
    server = None
    try:
        text, server, _recs = replay_termlog_session(
            replay,
            _level29_text(),
            start_ms=202600,
            end_ms=214200,
        )
        plain = _plain(text)
        assert "Enter your" in plain
        assert "x" in plain
        _quit(con)
        assert con.send_line("PRINT 2+2") == "4"
    finally:
        if server is not None:
            server.stop()
        replay.stop()
        con.stop()


def test_term_log_char_mode_key_logged_once(kernel_image):
    from test_term_log import _read_termlog, _termlog_path

    con = MMBasicConsole(kernel_image)
    con.start()
    replay = TermReplay(con, "127.0.0.1", 1)
    try:
        on = con.send_line("OPTION TERM LOG ON")
        assert ".termlog" in on
        seen = replay.open_session(connect=False)
        assert "Connected" in seen
        replay._to_guest(bytes([255, 251, 3]))
        replay.send_keys(b"i")
        more = replay.wait_serial(lambda s: "i" in s, timeout=6.0)
        assert "i" in more
        _quit(con)
        con.send_line("OPTION TERM LOG OFF")
        path = _termlog_path(con)
        assert path, "expected A:/.termlog or C:/.termlog"
        body = _read_termlog(con, path)
        recs = parse_termlog(body)
        hits = [r for r in recs if r.kind == "T" and r.data == b"i"]
        assert len(hits) == 1, [r for r in recs if r.kind == "T"]
        assert con.send_line("PRINT 3+4") == "7"
        replay.stop()
    finally:
        replay.stop()
        con.stop()


def test_term_log_usb_enter_is_cr_not_lf(kernel_image):
    """USB Enter is LF locally; TCP and the TERM log must record CR only."""
    from test_term_log import _read_termlog, _termlog_path

    con = MMBasicConsole(kernel_image)
    con.start()
    replay = TermReplay(con, "127.0.0.1", 1)
    try:
        on = con.send_line("OPTION TERM LOG ON")
        assert ".termlog" in on
        seen = replay.open_session(connect=False)
        assert "Connected" in seen
        replay._to_guest(bytes([255, 251, 3]))
        replay.send_keys(b"ab\n")
        replay.wait_serial(lambda s: "ab" in s, timeout=6.0)
        _quit(con)
        con.send_line("OPTION TERM LOG OFF")
        path = _termlog_path(con)
        assert path, "expected A:/.termlog or C:/.termlog"
        body = _read_termlog(con, path)
        recs = parse_termlog(body)
        typed = b"".join(r.data for r in recs if r.kind == "T")
        assert b"ab" in typed, typed
        assert b"\r" in typed, typed
        assert b"\n" not in typed, typed
        assert con.send_line("PRINT 2+2") == "4"
        replay.stop()
    finally:
        replay.stop()
        con.stop()


def test_term_level29_mid_username_negotiation(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    replay = TermReplay(con, "127.0.0.1", 1)
    try:
        seen = replay.open_session(connect=False)
        replay._to_guest(b"User: ")
        replay._to_guest(
            b"i\xff\xfd\x18\xff\xfd \xff\xfb\x00\xff\xfd\x00\xff\xfd\x17"
        )
        more = replay.wait_serial(lambda s: "User: i" in s, timeout=6.0)
        text = _plain(seen + more)
        assert "User: i" in text
        outs = b"".join(d for kind, _, d in replay.log if kind == "out")
        assert bytes([255, 251, 24]) in outs
        assert bytes([255, 252, 32]) in outs
        _quit(con)
        assert con.send_line("PRINT 1+1") == "2"
    finally:
        replay.stop()
        con.stop()
