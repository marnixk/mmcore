"""Level29 BBS (bbs.fozztexx.com:23) TERM capture analysis.

The attached ``tests/term/level29-termlog`` shows two failure modes:

- the first login is rejected even with correct credentials;
- the second login prompt stalls at ``Enter your `` and the trace ends
  with a lone ``T ... 78`` (``x``) that has no network duplicate,
  proving the TCP session had already dropped (``T.tcp == 0`` logs the
  raw key only, while connected keys log once as typed).

The old capture also logs every char-mode keystroke twice: the raw key
in ``mmb_term_key`` plus the identical wire byte in ``term_net_send``
(same ``in_n``, ~16ms apart), with Enter as an ``0A``/``0D`` pair.
``play_termlog`` replays every non-IAC ``T`` as a keystroke, so the
duplicates double-type ``ireal`` as ``iireal`` etc. and replayed logins
always fail. The fix logs typed keys once in ``mmb_term_key`` and only
IAC negotiation in ``term_net_send``, buffers TX log writes instead of
flushing SD on every key, and draws once per poll so HDMI never starves
the 1-byte TCP drain.
"""

from pathlib import Path

from harness import MMBasicConsole, TermReplay, parse_termlog
from test_term import _plain, _quit
from test_term_log import _read_termlog, _termlog_path
from test_term_replay import _open_replay

REPO = Path(__file__).resolve().parents[1]
LEVEL29 = REPO / "tests" / "term" / "level29-termlog"


def test_level29_log_keystrokes_are_paired_raw_plus_net():
    recs = parse_termlog(LEVEL29.read_text())
    typed = [r for r in recs if r.kind == "T" and r.data[:1] != b"\xff"]
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
    assert pairs >= 10, (pairs, [t.data for t in typed[:12]])
    enters = [t for t in typed if t.data in (b"\n", b"\r")]
    assert len(enters) >= 2
    assert enters[0].data == b"\n" and enters[1].data == b"\r"
    assert enters[0].in_n == enters[1].in_n


def test_level29_log_tail_proves_tcp_drop():
    recs = parse_termlog(LEVEL29.read_text())
    typed = [r for r in recs if r.kind == "T" and r.data[:1] != b"\xff"]
    assert typed[-1].data == b"x"
    assert len([t for t in typed if t.data == b"x"]) == 1
    rx_tail = b"".join(r.data for r in recs if r.kind == "R")[-64:]
    assert b"Enter your " in rx_tail


def test_term_log_char_mode_key_logged_once(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    replay = TermReplay(con, "127.0.0.1", 1)
    try:
        on = con.send_line("OPTION TERM LOG ON")
        assert ".termlog" in on
        seen = _open_replay(con, replay, connect=False)
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


def test_term_level29_mid_username_negotiation(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    replay = TermReplay(con, "127.0.0.1", 1)
    try:
        seen = _open_replay(con, replay, connect=False)
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
