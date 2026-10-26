"""`ON n GOTO` / `ON n GOSUB` dispatch tables (#1370).

The 1-based index selects a line number or a named label, exactly as plain
`GOTO` / `GOSUB` accept them.  Index 0 and an index past the last target fall
through to the next statement on the line; a negative or >255 index is an
error.  A GOSUB dispatch pushes the same return frame as a plain GOSUB.
"""

import time

from harness import prompt_at_buffer_end

RUN_TIMEOUT = 20.0


def _load(console, lines):
    assert console.send_line("NEW") == ""
    for line in lines:
        assert console.send_line(line) == "", line


def _run(console, timeout=RUN_TIMEOUT):
    out = console.send_line("RUN", timeout=timeout)
    console.send_keys(b"\x03", timeout=3.0)
    return out


def _lines(out):
    return [ln.strip() for ln in out.replace("\r", "\n").split("\n") if ln.strip()]


def _program(console, lines):
    _load(console, lines)
    return _lines(_run(console))


def test_on_goto_named_labels(console):
    assert _program(
        console,
        [
            "10 Q = 2",
            "20 ON Q GOTO first, second, third",
            '30 PRINT "fall"',
            "40 END",
            '100 first: PRINT "one": END',
            '110 second: PRINT "two": END',
            '120 third: PRINT "three": END',
        ],
    ) == ["two"]


def test_on_gosub_named_labels_returns(console):
    assert _program(
        console,
        [
            "10 Q = 2",
            "20 ON Q GOSUB a, b, c",
            '30 PRINT "after"',
            "40 END",
            '100 a: PRINT "a": RETURN',
            '110 b: PRINT "b": RETURN',
            '120 c: PRINT "c": RETURN',
        ],
    ) == ["b", "after"]


def test_on_zero_and_too_large_fall_through(console):
    assert _program(
        console,
        [
            "10 Q = 0",
            '20 ON Q GOTO 100, 200: PRINT "zero"',
            "30 Q = 3",
            '40 ON Q GOTO 100, 200: PRINT "large"',
            "50 END",
            '100 PRINT "one": END',
            '200 PRINT "two": END',
        ],
    ) == ["zero", "large"]


def test_on_gosub_fall_through_pushes_no_frame(console):
    assert _program(
        console,
        [
            '10 ON 0 GOSUB 100, 200: PRINT "after"',
            "20 END",
            '100 PRINT "never": RETURN',
            '200 PRINT "never2": RETURN',
        ],
    ) == ["after"]


def test_selected_branch_skips_trailing_statements(console):
    assert _program(
        console,
        [
            "10 Q = 1",
            '20 ON Q GOTO 100, 200: PRINT "after"',
            "30 END",
            '100 PRINT "one": END',
            '200 PRINT "two": END',
        ],
    ) == ["one"]


def test_on_goto_inside_sub(console):
    assert _program(
        console,
        [
            "10 SUB Dispatch(Q)",
            "20 ON Q GOTO one, two",
            '30 PRINT "fall"',
            "40 EXIT SUB",
            '50 one: PRINT "one": EXIT SUB',
            '60 two: PRINT "two": EXIT SUB',
            "70 END SUB",
            "80 Dispatch 2",
            "90 Dispatch 0",
        ],
    ) == ["two", "fall"]


def test_on_gosub_inside_sub_returns_to_sub(console):
    assert _program(
        console,
        [
            "10 SUB Dispatch(Q)",
            "20 ON Q GOSUB a, b",
            '30 PRINT "after"',
            "40 EXIT SUB",
            '50 a: PRINT "a": RETURN',
            '60 b: PRINT "b": RETURN',
            "70 END SUB",
            "80 Dispatch 1",
            "90 Dispatch 2",
        ],
    ) == ["a", "after", "b", "after"]


def test_on_goto_inside_execute(console):
    assert _program(
        console,
        [
            "10 Q = 2",
            '20 EXECUTE "ON " + STR$(Q) + " GOTO first, second"',
            '30 PRINT "after"',
            "40 END",
            '100 first: PRINT "one": END',
            '110 second: PRINT "two": END',
        ],
    ) == ["two"]


def test_on_gosub_inside_execute_returns_to_caller(console):
    assert _program(
        console,
        [
            '10 EXECUTE "ON 1 GOSUB one, two"',
            '20 PRINT "after"',
            "30 END",
            '100 one: PRINT "one": RETURN',
            '110 two: PRINT "two": RETURN',
        ],
    ) == ["one", "after"]


def test_negative_index_is_an_error(console):
    _load(
        console,
        [
            "10 ON -1 GOTO 100, 200",
            '20 PRINT "not"',
            "30 END",
            '100 PRINT "one": END',
            '200 PRINT "two": END',
        ],
    )
    out = _run(console).upper()
    assert "?INVALID" in out, out
    assert "NOT" not in out
    assert "ONE" not in out


def test_index_above_255_is_an_error(console):
    _load(
        console,
        [
            "10 ON 256 GOTO 100, 200",
            '20 PRINT "not"',
            "30 END",
            '100 PRINT "one": END',
            '200 PRINT "two": END',
        ],
    )
    out = _run(console).upper()
    assert "?INVALID" in out, out
    assert "NOT" not in out


def test_unparsable_target_is_an_error(console):
    _load(
        console,
        [
            "10 ON 1 GOTO missing",
            '20 PRINT "not"',
        ],
    )
    out = _run(console)
    assert "?LABEL NOT FOUND" in out.upper(), out
    assert "not" not in _lines(out)


def _wait_contains(con, token, timeout=8.0):
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


def _until_prompt(con, timeout=8.0):
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


def test_on_gosub_from_on_key_handler(fresh_console):
    con = fresh_console
    lines = [
        "10 ON KEY Handler",
        '20 PRINT "READY"',
        "30 PAUSE 1200",
        '40 PRINT "DONE"',
        "50 END",
        "100 SUB Handler",
        "110 ON 1 GOSUB A, B",
        '120 PRINT "handler-after"',
        "130 END SUB",
        '200 A: PRINT "handler-a": RETURN',
        '210 B: PRINT "handler-b": RETURN',
    ]
    con.send_line("NEW")
    for line in lines:
        con.send_line(line)
    con.drain(quiet=0.1)
    con._ser.sendall(b"RUN\r")
    assert b"READY" in _wait_contains(con, b"READY")
    con._ser.sendall(b"q")
    out = _until_prompt(con)
    assert "handler-a" in out, out
    assert "handler-after" in out, out
    assert "handler-b" not in out, out
    assert "DONE" in out, out
