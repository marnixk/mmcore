"""EXECUTE runs dynamic code in its own execution frame (#1354).

Before the fix ``mmb_cmd_execute`` saved only ``G.p`` around
``exec_line_body``.  A block IF, SELECT or loop opened by the string stayed on
the caller's stacks, so the rest of the caller's arm/loop/SUB was skipped or
mispaired.  These cases pin the caller's structure and the branch semantics.

The invariant (the caller's ``if_depth``, ``if_shadow``, ``sel_sp``,
``ctrl_sp`` and ``for_sp`` are unchanged after EXECUTE) is enforced by the
exact frame pop and unit-tested in ``tests/exec_frame_host.c``; here it is
observed through the caller continuing exactly as it would without the call.
"""


def _run(console, lines, timeout=15.0):
    assert console.send_line("NEW") == ""
    for line in lines:
        assert console.send_line(line) == "", line
    out = console.send_line("RUN", timeout=timeout)
    return [ln.strip() for ln in out.replace("\r", "\n").split("\n") if ln.strip()]


def test_execute_block_if_does_not_escape_taken_arm(console):
    """A false block IF in the string must not swallow the caller's arm."""
    assert _run(
        console,
        [
            "10 IF 1 THEN",
            '20   PRINT "before"',
            '30   EXECUTE "IF 0 THEN"',
            '40   PRINT "after"',
            "50 ELSE",
            '60   PRINT "else"',
            "70 ENDIF",
            '80 PRINT "done"',
        ],
    ) == ["before", "after", "done"]


def test_execute_block_if_inside_for_loop(console):
    """Each iteration keeps its FOR frame after an unbalanced block IF."""
    assert _run(
        console,
        [
            "10 FOR I = 1 TO 2",
            '20   EXECUTE "IF 0 THEN"',
            "30   PRINT I",
            "40 NEXT I",
            '50 PRINT "done"',
        ],
    ) == ["1", "2", "done"]


def test_execute_do_inside_do_loop(console):
    """A DO left open by EXECUTE is discarded, not left on the caller."""
    assert _run(
        console,
        [
            "10 I = 0",
            "20 DO",
            "30   I = I + 1",
            '40   EXECUTE "DO"',
            "50   PRINT I",
            "60   EXIT DO",
            "70 LOOP",
            '80 PRINT "done"',
        ],
    ) == ["1", "done"]


def test_execute_select_inside_taken_case(console):
    """SELECT opened by EXECUTE does not hide or consume the caller's CASE."""
    assert _run(
        console,
        [
            "10 SELECT CASE 1",
            "20 CASE 1",
            '30   EXECUTE "SELECT CASE 2"',
            '40   PRINT "after"',
            "50 CASE 2",
            '60   PRINT "else"',
            "70 END SELECT",
            '80 PRINT "done"',
        ],
    ) == ["after", "done"]


def test_execute_inside_sub(console):
    """A structured string inside a SUB does not run past END SUB."""
    assert _run(
        console,
        [
            "10 SUB S",
            '20   PRINT "s-before"',
            '30   EXECUTE "IF 0 THEN"',
            '40   PRINT "s-after"',
            "50 END SUB",
            "60 S",
            '70 PRINT "done"',
        ],
    ) == ["s-before", "s-after", "done"]


def test_execute_unbalanced_if_is_discarded(console):
    """The string starts with clean IF state; its open frames are dropped."""
    assert _run(
        console,
        [
            "10 IF 1 THEN",
            '20   EXECUTE "IF 1 THEN"',
            '30   PRINT "after"',
            "40 ENDIF",
            '50 PRINT "done"',
        ],
    ) == ["after", "done"]

    # A stray ENDIF has no caller arm to close, so the rest of the string runs.
    assert _run(
        console,
        [
            "10 IF 1 THEN",
            '20   EXECUTE "ENDIF: PRINT 5"',
            '30   PRINT "after"',
            "40 ENDIF",
            '50 PRINT "done"',
        ],
    ) == ["5", "after", "done"]


def test_execute_unbalanced_loop_is_discarded(console):
    """Loop frames are shared, but the exact pop restores the caller's frame."""
    assert _run(
        console,
        [
            "10 I = 0",
            "20 DO",
            "30   I = I + 1",
            '40   EXECUTE "LOOP UNTIL 1"',
            "50   PRINT I",
            "60 LOOP UNTIL I > 2",
            '70 PRINT "done"',
        ],
    ) == ["1", "2", "3", "done"]


def test_execute_goto_branches_away(console):
    """A GOTO in the string is honoured after EXECUTE returns."""
    assert _run(
        console,
        [
            "10 IF 1 THEN",
            '20   EXECUTE "GOTO 100"',
            '30   PRINT "not"',
            "40 ENDIF",
            '50 PRINT "not2"',
            "60 END",
            '100 PRINT "target"',
        ],
    ) == ["target"]


def test_execute_gosub_returns_to_callers_arm(console):
    """GOSUB runs after EXECUTE and RETURN restores the caller's IF arm."""
    assert _run(
        console,
        [
            "10 IF 1 THEN",
            '20   EXECUTE "GOSUB 100"',
            '30   PRINT "after"',
            "40 ELSE",
            '50   PRINT "else"',
            "60 ENDIF",
            '70 PRINT "done"',
            "80 END",
            '100 PRINT "target"',
            "110 RETURN",
        ],
    ) == ["target", "after", "done"]


def test_execute_return_leaves_enclosing_gosub(console):
    """RETURN in the string returns from the enclosing GOSUB in caller state."""
    assert _run(
        console,
        [
            "10 IF 1 THEN",
            "20   GOSUB 100",
            '30   PRINT "after"',
            "40 ELSE",
            '50   PRINT "else"',
            "60 ENDIF",
            '70 PRINT "done"',
            "80 END",
            '100 EXECUTE "RETURN"',
            '110 PRINT "not"',
        ],
    ) == ["after", "done"]


def test_execute_end_stops_whole_program(console):
    """END is a program stop: the frame pop must not resurrect the caller."""
    assert _run(
        console,
        [
            '10 PRINT "before"',
            '20 EXECUTE "END"',
            '30 PRINT "not"',
        ],
    ) == ["before"]

    assert _run(
        console,
        [
            "10 SUB S",
            '20   PRINT "in"',
            '30   EXECUTE "END"',
            "40 END SUB",
            "50 S",
            '60 PRINT "not"',
        ],
    ) == ["in"]


def test_execute_same_line_for_runs_every_iteration(console):
    """#1364: the dynamic body stays alive for the loop's back-edge."""
    assert _run(
        console,
        [
            '10 PRINT "before"',
            '20 EXECUTE "FOR I=1 TO 2: PRINT I: NEXT"',
            '30 PRINT "after"',
        ],
    ) == ["before", "1", "2", "after"]


def test_execute_same_line_for_inside_caller_if(console):
    """The loop resumes in caller structure state, so ENDIF still pairs."""
    assert _run(
        console,
        [
            "10 IF 1 THEN",
            '20   EXECUTE "FOR I=1 TO 2: PRINT I: NEXT"',
            '30   PRINT "after"',
            "40 ENDIF",
            '50 PRINT "done"',
        ],
    ) == ["1", "2", "after", "done"]


def test_execute_same_line_for_calls_function_each_pass(console):
    """The heap body also supports nested FUNCTION calls inside the loop."""
    assert _run(
        console,
        [
            "10 FUNCTION DBL(N)",
            "20 DBL = N * 2",
            "30 END FUNCTION",
            '40 EXECUTE "FOR I=1 TO 2: PRINT DBL(I): NEXT"',
        ],
    ) == ["2", "4"]


def test_execute_nested_same_line_for(console):
    """Two same-line loops in one body resume from the same live buffer."""
    assert _run(
        console,
        [
            '10 EXECUTE "FOR I=1 TO 2: FOR J=1 TO 2: PRINT I;J: NEXT J: NEXT I"',
            '20 PRINT "done"',
        ],
    ) == ["11", "12", "21", "22", "done"]


def test_execute_same_line_exit_for(console):
    """EXIT FOR leaves the dynamic loop without dangling into the buffer."""
    assert _run(
        console,
        [
            '10 EXECUTE "FOR I=1 TO 5: IF I=2 THEN EXIT FOR: PRINT I: NEXT"',
            '20 PRINT "after"',
        ],
    ) == ["1", "after"]


def test_execute_same_line_for_calls_statement_sub(console):
    """#1391: a statement SUB in the live body returns into the loop."""
    assert _run(
        console,
        [
            "10 SUB SHOW(N)",
            "20 PRINT N",
            "30 END SUB",
            '40 EXECUTE "FOR I=1 TO 2: SHOW(I): NEXT"',
            '50 PRINT "done"',
        ],
    ) == ["1", "2", "done"]


def test_execute_same_line_for_calls_nested_statement_sub(console):
    """A SUB called from the deferred body may call another statement SUB."""
    assert _run(
        console,
        [
            "10 SUB OUTER(N)",
            "20   INNER(N)",
            "30 END SUB",
            "40 SUB INNER(N)",
            "50   PRINT N",
            "60 END SUB",
            '70 EXECUTE "FOR I=1 TO 2: OUTER(I): NEXT"',
        ],
    ) == ["1", "2"]


def test_execute_same_line_for_statement_sub_calls_function(console):
    """The SUB that runs inside the island can still call a FUNCTION."""
    assert _run(
        console,
        [
            "10 FUNCTION DBL(N)",
            "20   DBL = N * 2",
            "30 END FUNCTION",
            "40 SUB SHOW(N)",
            "50   PRINT DBL(N)",
            "60 END SUB",
            '70 EXECUTE "FOR I=1 TO 2: SHOW(I): NEXT"',
        ],
    ) == ["2", "4"]
