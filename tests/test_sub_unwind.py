"""#1297: leaving a SUB/FUNCTION must drop the loop frames it opened.

EXIT SUB from inside a DO/WHILE loop used to leave that loop frame on the
control stack, so the caller's next LOOP/EXIT DO picked the stale frame:
the program stopped silently or the SUB's next call failed with ?UNDECLARED.
"""

import pytest


def _run_program(console, program):
    assert console.send_line("NEW") == ""
    for line in program:
        assert console.send_line(line) == ""
    out = console.send_line("RUN")
    return [ln.strip() for ln in out.replace("\r", "\n").split("\n") if ln.strip()]


def _sub_in_do_loop_program(body):
    """A SUB whose body is ``body``, called four times from the caller's DO."""
    program = ["10 SUB S(N)", "20 LOCAL I = 0"]
    n = 30
    for stmt in body:
        program.append(f"{n} {stmt}")
        n += 10
    program.append(f"{n} END SUB")
    n += 10
    for stmt in (
        "G = 0",
        "DO WHILE G < 4",
        "G = G + 1",
        "S 3",
        'PRINT "call"; G',
        "LOOP",
        'PRINT "done"',
    ):
        program.append(f"{n} {stmt}")
        n += 10
    return program


VARIANTS = {
    "do": ["DO", "I = I + 1", "IF I >= N THEN EXIT SUB", "LOOP"],
    "do_while": ["DO WHILE I < N", "I = I + 1", "IF I >= N THEN EXIT SUB", "LOOP"],
    "do_loop_until": [
        "DO",
        "I = I + 1",
        "IF I >= N THEN EXIT SUB",
        "LOOP UNTIL I >= N",
    ],
    "while_wend": ["WHILE I < N", "I = I + 1", "IF I >= N THEN EXIT SUB", "WEND"],
    "nested_do": [
        "DO",
        "DO",
        "I = I + 1",
        "IF I >= N THEN EXIT SUB",
        "LOOP",
        "LOOP",
    ],
}


@pytest.mark.parametrize("body", VARIANTS.values(), ids=VARIANTS.keys())
def test_exit_sub_drops_its_loop_frame(console, body):
    lines = _run_program(console, _sub_in_do_loop_program(body))
    assert lines == ["call1", "call2", "call3", "call4", "done"], lines


def test_minimal_repro_exit_sub_inside_do(console):
    program = [
        "10 SUB T(N)",
        "20 LOCAL I = 0",
        "30 DO",
        "40 I = I + 1",
        "50 IF I >= N THEN EXIT SUB",
        "60 LOOP",
        "70 END SUB",
        "80 G = 0",
        "90 DO WHILE G < 3",
        "100 G = G + 1",
        "110 T 2",
        '120 PRINT "call"; G; " ok"',
        "130 LOOP",
        '140 PRINT "done"',
    ]
    lines = _run_program(console, program)
    assert lines == ["call1 ok", "call2 ok", "call3 ok", "done"], lines


def test_end_sub_reached_through_goto_drops_loop_frame(console):
    program = [
        "10 SUB S(N)",
        "20 LOCAL I = 0",
        "30 DO",
        "40 I = I + 1",
        "50 IF I >= N THEN GOTO 70",
        "60 LOOP",
        "70 END SUB",
        "80 G = 0",
        "90 DO WHILE G < 4",
        "100 G = G + 1",
        "110 S 3",
        '120 PRINT "call"; G',
        "130 LOOP",
        '140 PRINT "done"',
    ]
    lines = _run_program(console, program)
    assert lines == ["call1", "call2", "call3", "call4", "done"], lines


def test_end_in_function_stops_the_program(console):
    """#1343: END inside a FUNCTION must stop RUN, not just leave the call."""
    program = [
        "10 OPTION EXPLICIT",
        "20 OPTION DEFAULT INTEGER",
        "30 DIM X",
        "40 FUNCTION f()",
        '50 PRINT "in f"',
        "60 END",
        "70 f = 1",
        "80 END FUNCTION",
        "90 X = f()",
        '100 PRINT "AFTER X="; X',
    ]
    lines = _run_program(console, program)
    assert lines == ["in f"], lines


def test_end_in_sub_called_from_function_stops_the_program(console):
    """#1343: END in a SUB reached from a FUNCTION must unwind past both."""
    program = [
        "10 OPTION EXPLICIT",
        "20 OPTION DEFAULT INTEGER",
        "30 DIM X",
        "40 SUB g()",
        '50 PRINT "in g"',
        "60 END",
        "70 END SUB",
        "80 FUNCTION f()",
        "90 g",
        "100 f = 1",
        "110 END FUNCTION",
        "120 X = f()",
        '130 PRINT "AFTER X="; X',
    ]
    lines = _run_program(console, program)
    assert lines == ["in g"], lines


def test_end_in_statement_sub_still_stops_the_program(console):
    program = [
        "10 SUB s()",
        '20 PRINT "in s"',
        "30 END",
        "40 END SUB",
        "50 s",
        '60 PRINT "AFTER"',
    ]
    lines = _run_program(console, program)
    assert lines == ["in s"], lines


def test_end_in_main_program_still_stops(console):
    program = [
        '10 PRINT "before"',
        "20 END",
        '30 PRINT "AFTER"',
    ]
    lines = _run_program(console, program)
    assert lines == ["before"], lines


def test_nested_sub_calls_keep_the_outer_loop_frame(console):
    program = [
        "10 SUB INNER(N)",
        "20 LOCAL J = 0",
        "30 DO",
        "40 J = J + 1",
        "50 IF J >= N THEN EXIT SUB",
        "60 LOOP",
        "70 END SUB",
        "80 SUB OUTER(N)",
        "90 LOCAL I = 0",
        "100 DO",
        "110 I = I + 1",
        "120 INNER N",
        "130 IF I >= N THEN EXIT SUB",
        "140 LOOP",
        "150 END SUB",
        "160 G = 0",
        "170 DO WHILE G < 3",
        "180 G = G + 1",
        "190 OUTER 2",
        '200 PRINT "call"; G',
        "210 LOOP",
        '220 PRINT "done"',
    ]
    lines = _run_program(console, program)
    assert lines == ["call1", "call2", "call3", "done"], lines
