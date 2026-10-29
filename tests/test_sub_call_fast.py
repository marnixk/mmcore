"""#1420: user FUNCTION/SUB fast path (resolved entry + single param lookup).

These pin the cases where a cached SUB index or a cached parameter binding
could go wrong: recursion, a caller LOCAL shadowing the parameter name,
omitted/typed arguments, the return variable, ON ERROR, CALL vs bare calls,
and redefining a SUB across NEW/RUN.
"""


def _load(console, program):
    assert console.send_line("NEW") == ""
    n = 10
    for line in program:
        assert console.send_line(f"{n} {line}") == ""
        n += 10


def _run(console, program, timeout=60.0):
    _load(console, program)
    out = console.send_line("RUN", timeout=timeout)
    return [ln.strip() for ln in out.replace("\r", "\n").split("\n") if ln.strip()]


def test_recursive_function_and_sub(console):
    lines = _run(console, [
        "FUNCTION FACT(N)",
        "  IF N <= 1 THEN",
        "    FACT = 1",
        "    EXIT FUNCTION",
        "  END IF",
        "  FACT = N * FACT(N - 1)",
        "END FUNCTION",
        "SUB WALK(N)",
        "  IF N <= 0 THEN EXIT SUB",
        "  WALK N - 1",
        '  PRINT "W";N',
        "END SUB",
        'PRINT "F";FACT(6)',
        "WALK 3",
    ])
    assert lines == ["F720", "W1", "W2", "W3"], lines


def test_caller_local_shadows_parameter_name(console):
    """The parameter save/restore must use the caller's LOCAL binding."""
    lines = _run(console, [
        "SUB OUTER()",
        "  LOCAL X",
        "  X = 7",
        "  INNER X",
        '  PRINT "O";X',
        "END SUB",
        "SUB INNER(X)",
        '  PRINT "I";X',
        "  X = 99",
        "END SUB",
        "OUTER",
        'PRINT "G";X',
    ])
    assert lines == ["I7", "O7", "G0"], lines


def test_omitted_and_typed_arguments(console):
    lines = _run(console, [
        "SUB P(A AS INTEGER, B AS STRING, C)",
        '  PRINT "P[";A;"][";B;"][";C;"]"',
        "END SUB",
        "P",
        'P 5,"s",3',
    ])
    assert lines == ["P[0][][0]", "P[5][s][3]"], lines


def test_string_function_return_variable(console):
    lines = _run(console, [
        "FUNCTION G$(N)",
        '  G$ = MID$("abcdef",N,1)',
        "END FUNCTION",
        'PRINT "G";G$(3)',
        'PRINT "G";G$(1)',
    ])
    assert lines == ["Gc", "Ga"], lines


def test_call_and_bare_sub_forms(console):
    lines = _run(console, [
        "SUB SHOW(N)",
        '  PRINT "S";N',
        "END SUB",
        "CALL SHOW(1)",
        "SHOW 2",
    ])
    assert lines == ["S1", "S2"], lines


def test_on_error_unwinds_a_fast_call(console):
    lines = _run(console, [
        "DIM AR(3)",
        "SUB BAD()",
        "  ON ERROR GOTO CATCH",
        "  X = AR(99)",
        '  PRINT "NO"',
        "  EXIT SUB",
        'CATCH: PRINT "CAUGHT"',
        "END SUB",
        "BAD",
    ])
    assert "CAUGHT" in lines, lines
    assert "NO" not in lines, lines


def test_redefine_across_new_and_run(console):
    lines = _run(console, [
        "SUB M(X)",
        '  PRINT "A";X',
        "END SUB",
        "M 1",
    ])
    assert lines == ["A1"], lines
    out = console.send_line("RUN")
    assert [ln.strip() for ln in out.replace("\r", "\n").split("\n") if ln.strip()] == ["A1"]
    _load(console, [
        "SUB M(X$)",
        '  PRINT "B";X$',
        "END SUB",
        'M "2"',
    ])
    out = console.send_line("RUN")
    lines = [ln.strip() for ln in out.replace("\r", "\n").split("\n") if ln.strip()]
    assert lines == ["B2"], lines
