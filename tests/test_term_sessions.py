"""TERM session replay / diary UX: SESSIONS, speed, autolog (#529)."""

import time

from harness import MMBasicConsole
from test_term import _quit


def _write_log(con, path="A:/NIGHT.LOG"):
    assert con.send_line("NEW") == ""
    assert con.send_line(f'OPEN "{path}" FOR OUTPUT AS #1') == ""
    assert con.send_line('PRINT #1, "# TERMLOG 2"') == ""
    assert con.send_line('PRINT #1, "R 0 1 0 41"') == ""
    assert con.send_line('PRINT #1, "R 800 2 1 42"') == ""
    assert con.send_line("CLOSE #1") == ""


def _wait_replay_done(con, timeout=15.0):
    acc = b""
    deadline = time.time() + timeout
    while b"!REPLAY DONE" not in acc and time.time() < deadline:
        acc += con.drain(quiet=0.2, timeout=2)
    return acc.decode(errors="replace")


def _speed_key(con, key: bytes, timeout=4.0):
    con._ser.sendall(key)
    return con.drain(quiet=0.3, timeout=timeout).decode(errors="replace")


def test_term_sessions_list_and_replay_speed(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        _write_log(con)
        listing = con.send_line("TERM SESSIONS")
        assert "NIGHT.LOG" in listing.upper(), listing

        con.drain(quiet=0.2)
        t0 = time.time()
        con._ser.sendall(b'TERM REPLAY "A:/NIGHT.LOG", 1\r')
        text = _wait_replay_done(con)
        slow = time.time() - t0
        assert "!REPLAY SPEED 100" in text
        assert "!REPLAY DONE" in text
        assert slow >= 0.6, slow

        # Runtime speed controls.
        assert "!REPLAY SPEED 200" in _speed_key(con, b"+")
        assert "!REPLAY SPEED 100" in _speed_key(con, b"-")
        assert "PAUSED" in _speed_key(con, b" ")
        assert "!REPLAY SPEED 100" in _speed_key(con, b" ")
        _quit(con)
        assert con.send_line("PRINT 2+2") == "4"

        con.drain(quiet=0.2)
        t0 = time.time()
        con._ser.sendall(b'TERM REPLAY "A:/NIGHT.LOG"\r')
        instant = _wait_replay_done(con)
        fast = time.time() - t0
        assert "!REPLAY DONE" in instant
        assert "!REPLAY SPEED" not in instant
        assert fast + 0.3 < slow, (fast, slow)
        _quit(con)
        assert con.send_line("PRINT 3+3") == "6"
    finally:
        con.stop()


def test_option_term_autolog_starts_log(console):
    assert console.send_line("OPTION TERM AUTOLOG ON") == ""
    listed = console.send_line("OPTION LIST")
    assert "OPTION TERM AUTOLOG ON" in listed
    console.drain(quiet=0.1)
    console._ser.sendall(b'TERM "demo", 23\r')
    console.drain(quiet=0.8, timeout=12)
    _quit(console)
    out = console.send_line("TERM SESSIONS")
    assert ".termlog" in out.lower(), out
    assert console.send_line("OPTION TERM AUTOLOG OFF") == ""
    console.send_line("OPTION TERM LOG OFF")
