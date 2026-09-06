"""Console INPUT reads a typed line into variables."""

import time


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


def test_input_console_string(fresh_console):
    c = fresh_console
    c.send_line("NEW")
    c.send_line("10 INPUT a$")
    c.send_line('20 PRINT "[" + a$ + "]"')
    c.drain(quiet=0.1)
    c._ser.sendall(b"RUN\r")
    assert b"?" in _wait_contains(c, b"?")
    c._ser.sendall(b"hello\r")
    out = _until_prompt(c)
    assert "[hello]" in out


def test_input_console_prompt_and_csv(fresh_console):
    c = fresh_console
    c.send_line("NEW")
    c.send_line('10 INPUT "Name"; n$')
    c.send_line("20 PRINT n$")
    c.drain(quiet=0.1)
    c._ser.sendall(b"RUN\r")
    first = _wait_contains(c, b"Name?")
    assert b"Name?" in first
    c._ser.sendall(b"Ada\r")
    out = _until_prompt(c)
    assert "Ada" in out

    c.send_line("NEW")
    c.send_line("10 INPUT a, b$")
    c.send_line('20 PRINT STR$(a) + "/" + b$')
    c.drain(quiet=0.1)
    c._ser.sendall(b"RUN\r")
    assert b"?" in _wait_contains(c, b"?")
    c._ser.sendall(b"3,xyz\r")
    out = _until_prompt(c)
    assert "3/xyz" in out


def test_print_semicolon_then_input_shows_print_first(fresh_console):
    c = fresh_console
    c.send_line("NEW")
    c.send_line('10 PRINT "what is happening?";')
    c.send_line("20 INPUT a$")
    c.send_line('30 PRINT "Oh interesting, I never thought of"; a$')
    c.drain(quiet=0.1)
    c._ser.sendall(b"RUN\r")
    first = _wait_contains(c, b"what is happening?", timeout=8.0)
    assert b"what is happening?" in first, first
    c._ser.sendall(b"bananas\r")
    out = _until_prompt(c, timeout=8.0)
    assert "Oh interesting, I never thought ofbananas" in out.replace("\r", "")


def test_print_then_line_input_shows_print_first(fresh_console):
    c = fresh_console
    c.send_line("NEW")
    c.send_line('10 PRINT "ahead";')
    c.send_line('20 LINE INPUT "Go"; s$')
    c.send_line("30 PRINT s$")
    c.drain(quiet=0.1)
    c._ser.sendall(b"RUN\r")
    first = _wait_contains(c, b"aheadGo", timeout=8.0)
    assert b"aheadGo" in first, first
    assert first.find(b"ahead") <= first.find(b"Go")
    c._ser.sendall(b"xyz\r")
    out = _until_prompt(c, timeout=8.0)
    assert "xyz" in out


def test_line_input_console(fresh_console):
    c = fresh_console
    c.send_line("NEW")
    c.send_line('10 LINE INPUT "Go"; s$')
    c.send_line("20 PRINT s$")
    c.drain(quiet=0.1)
    c._ser.sendall(b"RUN\r")
    assert b"Go" in _wait_contains(c, b"Go")
    c._ser.sendall(b"abc def\r")
    out = _until_prompt(c)
    assert "abc def" in out


from ihelp_util import dump_topic


def test_help_input_describes_console(console):
    out = dump_topic(console, "INPUT")
    assert "not implemented" not in out.lower()
    assert "prompt" in out.lower()
    assert "INPUT" in out
