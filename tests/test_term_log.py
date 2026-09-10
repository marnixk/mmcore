"""OPTION TERM LOG captures RX/TX for later QEMU replay."""

from harness import MMBasicConsole, TermReplay, parse_termlog, play_termlog
from test_term import _plain, _quit
from test_term_replay import _open_replay


def test_parse_termlog_records():
    text = (
        "# TERMLOG 1\n"
        "R 5 0 4142430D0A\n"
        "T 5 5 5A\n"
        "# ignore\n"
        "X 1 0 00\n"
    )
    recs = parse_termlog(text)
    assert len(recs) == 2
    assert recs[0].kind == "R"
    assert recs[0].in_n == 5
    assert recs[0].rendered == 0
    assert recs[0].data == b"ABC\r\n"
    assert recs[1].kind == "T"
    assert recs[1].rendered == 5
    assert recs[1].data == b"Z"


def _termlog_path(con):
    for path in ("A:/.termlog", "C:/.termlog"):
        out = con.send_line(f'OPEN "{path}" FOR INPUT AS #1')
        con.send_line("CLOSE #1")
        if out == "":
            return path
    return None


def _read_termlog(con, path):
    assert con.send_line("NEW") == ""
    assert con.send_line(f'10 OPEN "{path}" FOR INPUT AS #1') == ""
    assert con.send_line("20 IF EOF(#1) THEN GOTO 70") == ""
    assert con.send_line("30 LINE INPUT #1, A$") == ""
    assert con.send_line("40 PRINT A$") == ""
    assert con.send_line("50 GOTO 20") == ""
    assert con.send_line("70 CLOSE #1") == ""
    return con.send_line("RUN", timeout=8)


def test_option_term_log_on_off_file(console):
    console.send_line("OPTION TERM LOG OFF")
    listed = console.send_line("OPTION LIST ALL")
    assert "OPTION TERM LOG OFF" in listed
    out = console.send_line("OPTION TERM LOG ON")
    assert "TERM log" in out
    assert ".termlog" in out
    listed = console.send_line("OPTION LIST")
    assert "OPTION TERM LOG ON" in listed
    path = "C:/.termlog" if "C:/.termlog" in out else "A:/.termlog"
    body = _read_termlog(console, path)
    assert "# TERMLOG 1" in body
    off = console.send_line("OPTION TERM LOG OFF")
    assert "in=0" in off
    assert "rendered=0" in off
    listed = console.send_line("OPTION LIST")
    assert "TERM LOG" not in listed


def test_term_log_roundtrip_replay(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    replay = TermReplay(con, "127.0.0.1", 1)
    try:
        on = con.send_line("OPTION TERM LOG ON")
        assert ".termlog" in on
        seen = _open_replay(con, replay, connect=False)
        replay._to_guest(b"HELLO-LOG\r\n")
        more = replay.wait_serial(lambda s: "HELLO-LOG" in s, timeout=6.0)
        text = seen + more
        assert "HELLO-LOG" in text
        replay.send_keys(b"Z")
        echoed = replay.wait_serial(lambda s: "Z" in s, timeout=4.0)
        assert "Z" in echoed
        _quit(con)
        off = con.send_line("OPTION TERM LOG OFF")
        assert "in=" in off
        path = _termlog_path(con)
        assert path, "expected A:/.termlog or C:/.termlog"
        body = _read_termlog(con, path)
        recs = parse_termlog(body)
        rx = b"".join(r.data for r in recs if r.kind == "R")
        tx = b"".join(r.data for r in recs if r.kind == "T" and r.data[:1] != b"\xff")
        assert b"HELLO-LOG" in rx
        assert b"Z" in tx
        typed = [r for r in recs if r.kind == "T" and r.data == b"Z"]
        assert typed
        assert typed[0].rendered >= 1

        replay2 = TermReplay(con, "127.0.0.1", 1)
        seen2 = _open_replay(con, replay2, connect=False)
        played = play_termlog(replay2, body)
        more2 = replay2.wait_serial(
            lambda s: "HELLO-LOG" in s and "Z" in s, timeout=8.0
        )
        text2 = _plain(seen2 + played + more2)
        assert "HELLO-LOG" in text2
        assert "Z" in text2
        _quit(con)
        assert con.send_line("PRINT 3+4") == "7"
        replay2.stop()
    finally:
        replay.stop()
        con.stop()
