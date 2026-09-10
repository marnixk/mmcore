"""Blackflag termlog: Mystic login field (ESC[17D) and animation frames."""

from pathlib import Path

from harness import MMBasicConsole, TermReplay, parse_termlog
from test_term import _plain, _quit
from test_term_replay import _open_replay

REPO = Path(__file__).resolve().parents[1]
BLACKFLAG = REPO / "tests" / "term" / "blackflag-termlog"
BLACKFLAG2 = REPO / "tests" / "term" / "blackflag-termlog-2"

IAC_WILL_ECHO = bytes([255, 251, 1])
IAC_WILL_SGA = bytes([255, 251, 3])

LOGIN_FIELD = (
    b"\r\n"
    b"\x1b[27C"
    b"\x1b[1m"
    + bytes([0xB0, 0xB0])
    + b" login"
    + b"\x1b[0m:"
    + b"\x1b[1;30m"
    + b"x" * 17
    + b"\x1b[0m"
)


def test_blackflag_log_login_uses_cub_then_echo():
    recs = parse_termlog(BLACKFLAG.read_text())
    rx = b"".join(r.data for r in recs if r.kind == "R")
    assert b"login" in rx
    assert b"\x1b[17D" in rx
    i_at = None
    for r in recs:
        if r.kind == "T" and r.data == b"i":
            i_at = r.in_n
            break
    assert i_at is not None
    before = b""
    for r in recs:
        if r.kind == "R" and r.in_n <= i_at:
            before += r.data
        elif r.kind == "T" and r.data == b"i":
            break
    assert before.endswith(b"\x1b[17D")


def test_blackflag2_log_login_and_password_use_cub():
    recs = parse_termlog(BLACKFLAG2.read_text())
    rx = b"".join(r.data for r in recs if r.kind == "R")
    assert b" login" in rx
    assert b" password" in rx
    assert rx.count(b"\x1b[17D") >= 2
    for key, label in ((b"i", "username"), (b"d", "password")):
        key_at = None
        for r in recs:
            if r.kind == "T" and r.data == key:
                key_at = r.in_n
                break
        assert key_at is not None, label
        before = b""
        for r in recs:
            if r.kind == "R" and r.in_n <= key_at:
                before += r.data
            elif r.kind == "T" and r.data == key:
                break
        assert before.endswith(b"\x1b[17D"), label


def test_term_mystic_login_field_cub_before_echo(kernel_image):
    """Mystic sends the mask and ESC[17D in separate writes before echo."""
    con = MMBasicConsole(kernel_image)
    con.start()
    replay = TermReplay(con, "127.0.0.1", 1)
    try:
        seen = _open_replay(con, replay, connect=False)
        replay._to_guest(IAC_WILL_ECHO + IAC_WILL_SGA)
        replay._to_guest(LOGIN_FIELD)
        replay._to_guest(b"\x1b[17D")
        replay.send_keys(b"i")
        replay._to_guest(b"i")
        more = replay.wait_serial(lambda s: "login:i" in s, timeout=6.0)
        text = _plain(seen + more)
        assert "login:i" in text
        assert "login:xxxxxxxxxxxxxxxxxi" not in text.replace(" ", "")
        _quit(con)
        assert con.send_line("PRINT 1+1") == "2"
    finally:
        replay.stop()
        con.stop()


def test_term_mystic_login_field_echo_overwrites_mask(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    replay = TermReplay(con, "127.0.0.1", 1)
    try:
        seen = _open_replay(con, replay, connect=False)
        replay._to_guest(IAC_WILL_ECHO + IAC_WILL_SGA)
        replay._to_guest(LOGIN_FIELD)
        replay._to_guest(b"\x1b[17D")
        replay._to_guest(b"i")
        more = replay.wait_serial(lambda s: "login:i" in s, timeout=6.0)
        text = _plain(seen + more)
        assert "login:i" in text
        assert "login:xxxxxxxxxxxxxxxxxi" not in text.replace(" ", "")
        _quit(con)
        assert con.send_line("PRINT 1+1") == "2"
    finally:
        replay.stop()
        con.stop()


def test_term_animation_frames_without_keys(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    replay = TermReplay(con, "127.0.0.1", 1)
    try:
        seen = _open_replay(con, replay, connect=False)
        replay._to_guest(b"\x1b[2J\x1b[1;1H")
        for col in range(20, 36):
            replay._to_guest(f"\x1b[12;{col}H#".encode())
        replay._to_guest(b"\x1b[12;40HENDMARK")
        more = replay.wait_serial(lambda s: "ENDMARK" in s, timeout=8.0)
        text = _plain(seen + more)
        assert "ENDMARK" in text
        _quit(con)
        assert con.send_line("PRINT 2+2") == "4"
    finally:
        replay.stop()
        con.stop()
