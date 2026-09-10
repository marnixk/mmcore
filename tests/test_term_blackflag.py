"""Blackflag termlog: Mystic login field (ESC[17D) and animation frames."""

import time
from pathlib import Path

from harness import (
    AnsiPane,
    MMBasicConsole,
    TermReplay,
    load_termlog,
    max_render_lag,
    parse_termlog,
    typed_keys,
)
from test_term import _plain, _quit
from test_term_replay import _open_replay

REPO = Path(__file__).resolve().parents[1]
BLACKFLAG = REPO / "tests" / "term" / "blackflag-log-v2"

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
    assert b"login" in rx.lower() or b"LOGIN" in rx
    assert b"\x1b[17D" in rx or b"\x1b[27C" in rx
    typed = typed_keys(recs)
    assert typed, "expected typed keys in capture"


def test_blackflag_v2_render_lag_is_transient_burst_not_freeze():
    """Heavy ANSI art keeps rendered one recv chunk behind, then catches up."""
    recs = load_termlog(BLACKFLAG.read_text())
    lag, _at = max_render_lag(recs)
    assert lag >= 400, "expected large transient lag during animation bursts"
    assert recs[-1].in_n == recs[-1].rendered, "capture ends fully rendered"


def test_blackflag_v2_opening_animation_completes():
    recs = load_termlog(BLACKFLAG.read_text())
    rx = b"".join(r.data for r in recs if r.kind == "R")
    assert b"Mystic BBS" in rx
    assert b"Under the Black Flag" in rx
    assert b"Connected to" in rx
    assert b"BOTCHECK" in rx


def test_blackflag_v2_session_flow_after_animation():
    recs = load_termlog(BLACKFLAG.read_text())
    rx = b"".join(r.data for r in recs if r.kind == "R")
    typed = b"".join(t.data for t in typed_keys(recs))
    assert b"Black Sails ANSi Theme" in rx
    assert b"login\x1b[0m:" in rx
    assert b"account name you entered was not located" in rx
    assert b"Apply for a new account?" in rx
    assert typed.startswith(b"\x1b\x1b1\nireal\n")


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


def test_blackflag_expected_pane_reaches_flag_after_black():
    """Wire bytes draw 'Black' first, then 'Flag' from below/right, then HQ text."""
    recs = load_termlog(BLACKFLAG.read_text())
    pane = AnsiPane()
    black_rows = None
    flag_rows = None
    hq_rows = None
    for rec in recs:
        if rec.kind != "R":
            continue
        pane.feed(rec.data)
        if rec.in_n == 12751:
            black_rows = sum(
                1 for r in pane.snapshot()[19:29] for ch in r if ch != " "
            )
        elif rec.in_n == 14197:
            flag_rows = sum(
                1 for r in pane.snapshot()[23:29] for ch in r if ch != " "
            )
        elif rec.in_n == 15952:
            text = "\n".join(pane.snapshot())
            hq_rows = "ACiD Telnet HQ" in text or "ungenannt" in text
    assert black_rows and black_rows > 150
    assert flag_rows and flag_rows > 100
    assert hq_rows


def _feed_rx_until(replay, recs, target_in_n: int) -> str:
    acc = ""
    for rec in recs:
        if rec.kind != "R":
            continue
        replay._to_guest(rec.data)
        idle = 0
        for _ in range(8):
            extra = replay.pump_once(recv_tcp=False)
            if extra:
                acc += extra.decode(errors="replace")
                idle = 0
            else:
                idle += 1
                time.sleep(0.01)
                if idle >= 2:
                    break
        if rec.in_n >= target_in_n:
            break
    for _ in range(25):
        extra = replay.pump_once(recv_tcp=False)
        if extra:
            acc += extra.decode(errors="replace")
        else:
            time.sleep(0.02)
    return acc


def test_term_blackflag_qemu_past_b_to_flag(kernel_image):
    """HDMI screenshots: Black letter, then Flag, then HQ footer."""
    import shutil

    art = Path("/opt/cursor/artifacts")
    art.mkdir(parents=True, exist_ok=True)
    recs = load_termlog(BLACKFLAG.read_text())
    con = MMBasicConsole(kernel_image)
    con.start()
    replay = TermReplay(con, "127.0.0.1", 1)
    try:
        _open_replay(con, replay, connect=False)
        seen = _feed_rx_until(replay, recs, 12751)
        con.capture_png(str(art / "qemu_black_b_animating.png"))
        seen += _feed_rx_until(
            replay, [r for r in recs if (r.in_n or 0) > 12751], 14197
        )
        con.capture_png(str(art / "qemu_flag_f_from_right.png"))
        seen += _feed_rx_until(
            replay, [r for r in recs if (r.in_n or 0) > 14197], 15952
        )
        con.capture_png(str(art / "qemu_black_flag_complete.png"))
        text = _plain(seen)
        assert "ACiD" in text or "ungenannt" in text or "blocktronics" in text, text[
            -800:
        ]
        _quit(con)
        assert con.send_line("PRINT 3+3") == "6"
    finally:
        replay.stop()
        con.stop()
    for src, dst in (
        ("expected_b_f_in12751.png", "expected_black_b_animating.png"),
        ("expected_b_f_in14039.png", "expected_flag_f_from_right.png"),
        ("expected_b_f_in15952.png", "expected_black_flag_complete.png"),
    ):
        s = art / src
        if s.exists():
            shutil.copy(s, art / dst)


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
