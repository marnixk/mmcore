"""INKEY$ maps VT100 / CSI sequences to CMM2 single-byte codes."""

import re
import time

from harness import prompt_at_buffer_end
from ihelp_util import dump_topic


def _wait_contains(con, token: bytes, timeout: float = 5.0) -> bytes:
    deadline = time.time() + timeout
    seen = b""
    while time.time() < deadline:
        chunk = con._recv(con._ser)
        if chunk:
            seen += chunk
            if token in seen:
                return seen
        else:
            time.sleep(0.05)
    return seen


def _until_prompt(con, timeout: float = 5.0) -> str:
    deadline = time.time() + timeout
    buf = b""
    while time.time() < deadline:
        chunk = con._recv(con._ser)
        if chunk:
            buf += chunk
            if prompt_at_buffer_end(buf, con.prompt):
                break
        else:
            time.sleep(0.05)
    return buf.decode(errors="replace")


def _load_inkey_printer(con) -> None:
    con.send_line("NEW")
    con.send_line('10 PRINT "GO"')
    con.send_line("20 K$=INKEY$")
    con.send_line('30 IF K$="" THEN GOTO 20')
    con.send_line("40 PRINT ASC(K$)")
    con.send_line('50 IF K$<>"q" THEN GOTO 20')
    con.drain(quiet=0.1)


def _codes_for(con, seq: bytes, timeout: float = 6.0) -> list[int]:
    _load_inkey_printer(con)
    con._ser.sendall(b"RUN\r")
    assert b"GO" in _wait_contains(con, b"GO", timeout=8.0)
    con._ser.sendall(seq + b"q")
    out = _until_prompt(con, timeout=timeout)
    nums = [int(x) for x in re.findall(r"\b(\d{1,3})\b", out)]
    if nums and nums[-1] == 113:
        nums = nums[:-1]
    return nums


def test_inkey_arrows_cmm2(fresh_console):
    codes = _codes_for(
        fresh_console,
        b"\x1b[A\x1b[B\x1b[D\x1b[C",
    )
    assert codes == [128, 129, 130, 131]


def test_inkey_nav_and_function_keys(fresh_console):
    seq = (
        b"\x1b[H"
        b"\x1b[F"
        b"\x1b[1~"
        b"\x1b[2~"
        b"\x1b[3~"
        b"\x1b[4~"
        b"\x1b[5~"
        b"\x1b[6~"
        b"\x1bOA"
        b"\x1bOP"
        b"\x1b[[A"
        b"\x1b[[E"
        b"\x1b[17~"
        b"\x1b[24~"
        b"\x1b[1;5A"
        b"\x1b[Z"
        b"A"
        b"\x1bX"
    )
    codes = _codes_for(fresh_console, seq, timeout=8.0)
    assert codes == [
        134,
        135,
        134,
        132,
        127,
        135,
        136,
        137,
        128,
        145,
        145,
        149,
        150,
        156,
        128,
        159,
        65,
        27,
        88,
    ]


def test_inkey_chr_compare_up_arrow(fresh_console):
    c = fresh_console
    c.send_line("NEW")
    c.send_line('10 PRINT "GO"')
    c.send_line("20 K$=INKEY$")
    c.send_line('30 IF K$="" THEN GOTO 20')
    c.send_line('40 IF K$=CHR$(128) THEN PRINT "UP"')
    c.send_line("50 END")
    c.drain(quiet=0.1)
    c._ser.sendall(b"RUN\r")
    assert b"GO" in _wait_contains(c, b"GO", timeout=8.0)
    c._ser.sendall(b"\x1b[A")
    out = _until_prompt(c)
    assert "UP" in out
    assert "27" not in out.split("UP")[0]


def test_help_inkey_lists_cmm2_codes(console):
    out = dump_topic(console, "INKEY$")
    assert "128" in out
    assert "129" in out
    assert "145" in out
    assert "VT100" in out or "CMM2" in out


def _load_program(con, lines) -> None:
    con.send_line("NEW")
    for line in lines:
        con.send_line(line)
    con.drain(quiet=0.1)


def _run_and_send(con, keys: bytes, timeout: float = 8.0, delay: float = 0.0) -> str:
    con._ser.sendall(b"RUN\r")
    assert b"READY" in _wait_contains(con, b"READY", timeout=timeout)
    if delay:
        time.sleep(delay)
    con._ser.sendall(keys)
    return _until_prompt(con, timeout=timeout)


_DRAIN_SUB = [
    "200 SUB Drain",
    "210 LOCAL C$",
    "220 C$=INKEY$",
    '230 IF C$<>"" THEN PRINT "DRAIN";ASC(C$)',
    "240 END SUB",
]


def test_on_key_zero_clears_general_handler(fresh_console):
    _load_program(
        fresh_console,
        [
            "10 ON KEY Drain",
            "20 ON KEY 0",
            '30 PRINT "READY"',
            "40 PAUSE 1200",
            '50 PRINT "K";ASC(INKEY$)',
            "60 END",
            *_DRAIN_SUB,
        ],
    )
    out = _run_and_send(fresh_console, b"q")
    assert "?SYNTAX" not in out.upper()
    assert "DRAIN" not in out
    assert "K113" in out


def test_bare_on_key_clears_general_handler(fresh_console):
    _load_program(
        fresh_console,
        [
            "10 ON KEY Drain",
            "20 ON KEY",
            '30 PRINT "READY"',
            "40 PAUSE 1200",
            '50 PRINT "K";ASC(INKEY$)',
            "60 END",
            *_DRAIN_SUB,
        ],
    )
    out = _run_and_send(fresh_console, b"q")
    assert "?SYNTAX" not in out.upper()
    assert "DRAIN" not in out
    assert "K113" in out


def test_on_key_ascii_consumes_key_without_general(fresh_console):
    _load_program(
        fresh_console,
        [
            "10 ON KEY 128, ArrowUp",
            '20 PRINT "READY"',
            "30 PAUSE 1200",
            "40 PAUSE 200",
            '50 PRINT "LEFT[";INKEY$;"]"',
            "60 END",
            "100 SUB ArrowUp",
            '110 PRINT "UP"',
            "120 END SUB",
        ],
    )
    out = _run_and_send(fresh_console, b"\x1b[A")
    assert "UP" in out
    assert "LEFT[]" in out


def test_on_key_ascii_and_general_are_independent(fresh_console):
    _load_program(
        fresh_console,
        [
            "10 ON KEY 128, ArrowUp",
            "20 ON KEY Drain",
            '30 PRINT "READY"',
            "40 PAUSE 1200",
            "50 PAUSE 200",
            '60 PRINT "END"',
            "70 END",
            "100 SUB ArrowUp",
            '110 PRINT "UP"',
            "120 END SUB",
            *_DRAIN_SUB,
        ],
    )
    out = _run_and_send(fresh_console, b"\x1b[Az")
    assert "UP" in out
    assert "DRAIN128" not in out
    assert "DRAIN122" in out


def test_on_key_ascii_zero_disables_handler(fresh_console):
    _load_program(
        fresh_console,
        [
            "10 ON KEY 128, ArrowUp",
            "20 ON KEY 128, 0",
            "30 ON KEY Drain",
            '40 PRINT "READY"',
            "50 PAUSE 1200",
            "60 PAUSE 200",
            '70 PRINT "END"',
            "80 END",
            "100 SUB ArrowUp",
            '110 PRINT "UP"',
            "120 END SUB",
            *_DRAIN_SUB,
        ],
    )
    out = _run_and_send(fresh_console, b"\x1b[A")
    assert "UP" not in out
    assert "DRAIN128" in out


def test_on_key_ascii_fires_during_pause(fresh_console):
    # #1289: a key sent while PAUSE runs must reach the ON KEY handler at the
    # following statement boundary, without an extra PAUSE to pump input.
    _load_program(
        fresh_console,
        [
            "10 ON KEY 128, ArrowUp",
            '20 PRINT "READY"',
            "30 PAUSE 1500",
            '40 PRINT "LEFT[";INKEY$;"]"',
            "50 END",
            "100 SUB ArrowUp",
            '110 PRINT "UP"',
            "120 END SUB",
        ],
    )
    out = _run_and_send(fresh_console, b"\x1b[A", delay=0.4)
    assert "UP" in out
    assert "LEFT[]" in out


def test_general_on_key_fires_during_pause(fresh_console):
    # #1289: the general ON KEY handler wins over an INKEY$ on the statement
    # immediately after PAUSE, and still consumes the key it reads.
    _load_program(
        fresh_console,
        [
            "10 ON KEY Drain",
            '20 PRINT "READY"',
            "30 PAUSE 1500",
            '40 PRINT "AFTER[";INKEY$;"]"',
            "50 END",
            *_DRAIN_SUB,
        ],
    )
    out = _run_and_send(fresh_console, b"q", delay=0.4)
    assert "DRAIN113" in out
    assert "AFTER[]" in out


def test_general_on_key_does_not_reenter_unread_key(fresh_console):
    # #1293: the general handler leaves the key in the buffer for INKEY$, so
    # a level-triggered dispatch re-ran it on every PAUSE spin (tens of
    # thousands of calls for one key). It must fire once per key arrival.
    _load_program(
        fresh_console,
        [
            "10 ON KEY Nop",
            "20 CNT=0",
            '30 PRINT "READY"',
            "40 PAUSE 500",
            '50 PRINT "CNT";CNT',
            '60 PRINT "K";ASC(INKEY$)',
            "70 END",
            "100 SUB Nop",
            "110 CNT=CNT+1",
            "120 END SUB",
        ],
    )
    out = _run_and_send(fresh_console, b"q", delay=0.1)
    assert "?SYNTAX" not in out.upper()
    match = re.search(r"CNT(\d+)", out)
    assert match, out
    assert int(match.group(1)) <= 3, out
    assert "K113" in out


def test_general_on_key_refires_for_next_key(fresh_console):
    # #1293: an unread key must not re-fire the handler, but the next key to
    # arrive must still dispatch it (with the earlier key still queued).
    con = fresh_console
    _load_program(
        con,
        [
            "10 ON KEY Nop",
            "20 CNT=0",
            '30 PRINT "READY"',
            "40 PAUSE 400",
            "50 PAUSE 400",
            '60 PRINT "CNT";CNT',
            '70 PRINT "K";ASC(INKEY$);"-";ASC(INKEY$)',
            "80 END",
            "100 SUB Nop",
            "110 CNT=CNT+1",
            "120 END SUB",
        ],
    )
    con._ser.sendall(b"RUN\r")
    assert b"READY" in _wait_contains(con, b"READY", timeout=8.0)
    time.sleep(0.1)
    con._ser.sendall(b"q")
    time.sleep(0.5)
    con._ser.sendall(b"w")
    out = _until_prompt(con, timeout=8.0)
    assert "?SYNTAX" not in out.upper()
    match = re.search(r"CNT(\d+)", out)
    assert match, out
    assert int(match.group(1)) == 2, out
    assert "K113-119" in out
