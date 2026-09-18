"""Dynamic (unbounded) strings.

Covers the areas that used to be capped at 255 characters: assignment,
concatenation, CAT, string functions, arrays, DIM ... LENGTH caps, struct
members, JSON_STRINGIFY$ and console INPUT/LINE INPUT.
"""

import time


def _prog(console, lines):
    assert console.send_line("NEW") == ""
    n = 10
    for line in lines:
        assert console.send_line(f"{n} {line}") == ""
        n += 10


def test_string_functions_beyond_255(console):
    _prog(console, [])
    assert console.send_line('PRINT LEN(STRING$(300, "X"))') == "300"
    assert console.send_line('PRINT LEN(SPACE$(1000))') == "1000"
    assert console.send_line('PRINT LEN(UCASE$("abc"))') == "3"
    assert console.send_line('PRINT UCASE$("abc")') == "ABC"
    assert console.send_line('PRINT LEN(UCASE$(STRING$(400, "a")))') == "400"
    assert console.send_line('PRINT LEN(LCASE$(STRING$(400, "A")))') == "400"
    big = 'B$ = STRING$(600, "q")'
    assert console.send_line(big) == ""
    assert console.send_line('PRINT LEN(LEFT$(B$, 500))') == "500"
    assert console.send_line('PRINT LEN(RIGHT$(B$, 500))') == "500"
    assert console.send_line('PRINT LEN(MID$(B$, 1, 500))') == "500"


def test_assignment_and_concat_beyond_255(console):
    _prog(console, [])
    assert console.send_line('A$ = STRING$(300, "y")') == ""
    assert console.send_line("PRINT LEN(A$)") == "300"
    assert console.send_line('A$ = A$ + A$') == ""
    assert console.send_line("PRINT LEN(A$)") == "600"
    assert console.send_line('A$ = STRING$(200, "a") + STRING$(200, "b")') == ""
    assert console.send_line("PRINT LEN(A$)") == "400"
    assert console.send_line('A$ = "MM"') == ""
    assert console.send_line('CAT A$, STRING$(500, "x")') == ""
    assert console.send_line("PRINT LEN(A$)") == "502"


def test_array_elements_are_dynamic(console):
    _prog(console, [])
    assert console.send_line("DIM A$(1000)") == ""
    assert console.send_line("PRINT LEN(A$(5))") == "0"
    assert console.send_line('A$(5) = STRING$(500, "q")') == ""
    assert console.send_line("PRINT LEN(A$(5))") == "500"
    assert console.send_line('A$(6) = STRING$(1200, "z")') == ""
    assert console.send_line("PRINT LEN(A$(6))") == "1200"


def test_redim_preserve_keeps_long_strings(console):
    _prog(console, [])
    assert console.send_line("DIM A$(3)") == ""
    assert console.send_line('A$(1) = STRING$(400, "r")') == ""
    assert console.send_line("REDIM PRESERVE A$(6)") == ""
    assert console.send_line("PRINT LEN(A$(1))") == "400"


def test_length_is_a_hard_cap(console):
    _prog(console, [])
    assert console.send_line("DIM S AS STRING LENGTH 10") == ""
    assert console.send_line('S = "1234567890"') == ""
    assert console.send_line("PRINT LEN(S)") == "10"
    out = console.send_line('S = "12345678901"')
    assert "OVERFLOW" in out.upper(), out
    assert "S" in out
    assert console.send_line("PRINT 1") == "1"


def test_length_does_not_imply_padding(console):
    _prog(console, [])
    assert console.send_line("DIM S AS STRING LENGTH 10") == ""
    assert console.send_line('S = "ab"') == ""
    assert console.send_line('PRINT "["; S; "]"') == "[ab]"
    assert console.send_line('LSET S = "ab"') == ""
    assert console.send_line('PRINT "["; S; "]"') == "[ab        ]"


def test_unbounded_default(console):
    _prog(console, ['S$ = STRING$(5000, "k")', "PRINT LEN(S$)"])
    assert console.send_line("RUN") == "5000"


def test_struct_string_default_is_1024(console):
    _prog(
        console,
        [
            "TYPE T",
            "a AS STRING",
            "END TYPE",
            "DIM t AS T",
            't.a = STRING$(1000, "z")',
            "PRINT LEN(t.a)",
        ],
    )
    assert console.send_line("RUN") == "1000"
    _prog(
        console,
        [
            "TYPE T",
            "a AS STRING",
            "END TYPE",
            "DIM t AS T",
            't.a = STRING$(1100, "z")',
            "PRINT 1",
        ],
    )
    out = console.send_line("RUN")
    assert "OVERFLOW" in out.upper(), out


def test_struct_member_length_cap(console):
    _prog(
        console,
        [
            "TYPE T2",
            "a AS STRING LENGTH 32",
            "END TYPE",
            "DIM t AS T2",
            't.a = STRING$(32, "z")',
            "PRINT LEN(t.a)",
        ],
    )
    assert console.send_line("RUN") == "32"
    _prog(
        console,
        [
            "TYPE T2",
            "a AS STRING LENGTH 32",
            "END TYPE",
            "DIM t AS T2",
            't.a = STRING$(33, "z")',
            "PRINT 1",
        ],
    )
    out = console.send_line("RUN")
    assert "OVERFLOW" in out.upper(), out


def test_json_stringify_beyond_255(console):
    _prog(
        console,
        [
            "TYPE Big",
            "name AS STRING",
            "END TYPE",
            "DIM w AS Big",
            'w.name = STRING$(900, "x")',
            "s$ = JSON_STRINGIFY$(w)",
            "PRINT LEN(s$)",
            'PRINT LEN(JSON$(s$, "name"))',
        ],
    )
    out = console.send_line("RUN")
    assert "911" in out, out
    assert "900" in out, out


def test_json_parse_member_overflow(console):
    _prog(
        console,
        [
            "TYPE S",
            "a AS STRING LENGTH 8",
            "END TYPE",
            "DIM s AS S",
            'j$ = "{""a"":""123456789""}"',
            "JSON_PARSE j$, s",
            "PRINT 1",
        ],
    )
    out = console.send_line("RUN")
    assert "OVERFLOW" in out.upper(), out


def _until_prompt(c, timeout=6.0):
    deadline = time.time() + timeout
    buf = b""
    while time.time() < deadline:
        chunk = c._recv(c._ser)
        if chunk:
            buf += chunk
            if buf.rstrip().endswith(b">"):
                break
        else:
            time.sleep(0.05)
    return buf.decode(errors="replace")


def test_input_long_line(fresh_console):
    c = fresh_console
    c.send_line("NEW")
    c.send_line('10 INPUT "?"; a$')
    c.send_line("20 PRINT LEN(a$)")
    c.drain(quiet=0.1)
    c._ser.sendall(b"RUN\r")
    deadline = time.time() + 5
    seen = b""
    while time.time() < deadline:
        chunk = c._recv(c._ser)
        if chunk:
            seen += chunk
            if b"?" in seen:
                break
        else:
            time.sleep(0.05)
    payload = b"a" * 400
    c._ser.sendall(payload + b"\r")
    out = _until_prompt(c)
    assert "400" in out, out
