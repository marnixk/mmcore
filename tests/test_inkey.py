"""INKEY$ maps VT100 / CSI sequences to CMM2 single-byte codes."""

import re
import time

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
            if buf.rstrip().endswith(b">"):
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
