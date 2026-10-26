"""Mid-line control flow: a DO that is not the first statement on its line
(#1329) and EXIT SUB after a same-line SUB call (#1332).

Every RUN uses a short explicit timeout: before the fixes these programs hang,
so a regression must fail fast instead of stalling the suite.
"""

RUN_TIMEOUT = 8.0


def _load(console, lines):
    assert console.send_line("NEW") == ""
    for line in lines:
        assert console.send_line(line) == "", line


def _run(console, timeout=RUN_TIMEOUT):
    out = console.send_line("RUN", timeout=timeout)
    # Return to the prompt even if the program hung, so one regression cannot
    # poison the rest of the module's shared console.
    console.send_keys(b"\x03", timeout=3.0)
    return out


def _lines(out):
    return [ln.strip() for ln in out.replace("\r", "\n").split("\n") if ln.strip()]


def test_midline_do_single_line_accumulates(console):
    """The LOOP back-edge resumes after DO, not at the start of the line."""
    _load(
        console,
        [
            "10 N = 0 : DO : N = N + 1 : LOOP UNTIL N > 3",
            '20 PRINT "n=" + STR$(N)',
        ],
    )
    assert _lines(_run(console)) == ["n=4"]


def test_midline_do_multiline_accumulates(console):
    """Statements before a mid-line DO must not run on every pass."""
    _load(
        console,
        [
            "10 M = 0 : DO : N = N + 1",
            "20 M = M + 1",
            "30 LOOP UNTIL N > 3",
            '40 PRINT "n=" + STR$(N) + ",m=" + STR$(M)',
        ],
    )
    assert _lines(_run(console)) == ["n=4,m=4"]


def test_exit_do_from_midline_do_no_loop_error(console):
    """EXIT DO finds the LOOP of a DO that does not start its line."""
    _load(
        console,
        [
            "10 N = 0 : DO : N = N + 1 : IF N = 2 THEN EXIT DO",
            "20 LOOP",
            '30 PRINT "n=" + STR$(N)',
        ],
    )
    out = _run(console)
    assert "?LOOP" not in out.upper(), out
    assert _lines(out) == ["n=2"]


def test_exit_do_midline_exact_repro_no_loop_error(console):
    """The issue's third snippet must neither hang nor report ?LOOP."""
    _load(
        console,
        [
            "10 N = 0 : M = 0 : DO : N = N + 1 : M = M + 1 : IF M > 50 THEN EXIT DO",
            "20 LOOP UNTIL N > 3",
            '30 PRINT "n=" + STR$(N) + ",m=" + STR$(M)',
        ],
    )
    out = _run(console)
    assert "?LOOP" not in out.upper(), out
    assert _lines(out) == ["n=4,m=4"]


def _exit_sub_program(call_lines):
    """A SUB G whose body calls MK and then leaves on the same line."""
    lines = [
        "10 DIM INTEGER N",
        "20 SUB MK(M)",
        "30 N = N + 1",
        "40 END SUB",
        "50 SUB G()",
    ]
    n = 60
    for stmt in call_lines:
        lines.append(f"{n} {stmt}")
        n += 10
    lines.append(f'{n} PRINT "never"')
    n += 10
    lines.append(f"{n} END SUB")
    n += 10
    lines.append(f'{n} N = 0 : G() : PRINT "back, n=" + STR$(N)')
    return lines


def test_exit_sub_after_same_line_sub_call(console):
    """The issue's exact repro prints the caller's tail once, with n=1."""
    _load(console, _exit_sub_program(["MK(1) : EXIT SUB"]))
    assert _lines(_run(console)) == ["back, n=1"]


def test_exit_sub_after_same_line_call_inside_if(console):
    _load(console, _exit_sub_program(["IF 1 THEN MK(1) : EXIT SUB"]))
    assert _lines(_run(console)) == ["back, n=1"]


def test_exit_sub_on_separate_line_control_form(console):
    _load(console, _exit_sub_program(["MK(1)", "EXIT SUB"]))
    assert _lines(_run(console)) == ["back, n=1"]
