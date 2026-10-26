"""Mid-line resume state across SETTICK event handlers (#1350).

A tick or ON KEY handler runs through ``mmb_run_nested``, which captures an
``mmb_exec_frame`` including ``branch_pos`` and restores it on the single pop
path.  These tests pin the visible behaviour: a program whose control flow
resumes mid-line (same-line FOR body, mid-line DO/LOOP, EXIT SUB after a
same-line SUB call) must print the same result whether or not a tick handler
interrupts it.

The pending-position case is deliberately deterministic: a same-line FOR body
runs to completion inside one ``exec_line_guarded`` call (the loop-back path in
``run_program`` does not poll events), so a 1 ms tick is still overdue when the
body finally executes ``EXIT DO`` on that same line.  ``EXIT DO`` leaves a
mid-line resume pending (``branch_pos`` points after the matching ``LOOP``), and
the overdue tick fires in ``run_program`` right there.  If the frame does not
save/restore ``branch_pos``, the line restarts at its first statement and the
printed counter changes.

Every RUN uses a short timeout: a regression must fail fast instead of stalling
the suite.
"""

RUN_TIMEOUT = 30.0


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


def _run_program(console, lines):
    _load(console, lines)
    return _lines(_run(console))


def test_same_line_for_body_polls_events_mid_line(console):
    """PAUSE inside a same-line FOR body sees ticks; output is unchanged.

    ``SETTICK 1`` fires while the same-line body is paused mid-line.  The loop
    must iterate exactly as it does with timers off.
    """
    def program(period):
        return [
            "10 DIM INTEGER TK",
            "20 SUB TICK()",
            "30 TK = TK + 1",
            "40 END SUB",
            "50 TK = 0",
            f"60 SETTICK {period}, TICK",
            "70 FOR I = 1 TO 6 : PAUSE 3 : PRINT I;: NEXT I",
            "80 SETTICK 0, TICK",
            "90 PRINT",
            '100 PRINT "tk"; TK > 0',
        ]

    timed = _run_program(console, program(1))
    untimed = _run_program(console, program(0))
    assert timed[0] == "123456", timed
    assert timed[1] == "tk1", timed
    assert untimed == ["123456", "tk0"], untimed


def test_pending_branch_pos_survives_tick(console):
    """A tick at the pending mid-line resume must not restart the line.

    ``G()`` spends more than one tick period inside a same-line FOR body that
    never polls events, then ``EXIT DO`` leaves ``branch_pos`` pointing after
    the matching ``LOOP`` on that same line.  The overdue tick therefore fires
    in ``run_program`` with a pending mid-line resume.  With the frame restore
    the line continues after ``LOOP`` and ``k`` stays 1; without it the line
    restarts, ``k`` increments again and the second pass is visible.
    """
    def program(period):
        return [
            "10 DIM INTEGER N, K, TK, L",
            "20 SUB TICK()",
            "30 TK = TK + 1",
            "40 END SUB",
            "50 SUB G()",
            '55 K = K + 1 : DO : FOR J = 1 TO L : N = N + 1 : NEXT J'
            ' : L = 0 : EXIT DO : LOOP : PRINT "after"',
            "60 END SUB",
            "70 N = 0 : K = 0 : L = 50000 : TK = 0",
            f"80 SETTICK {period}, TICK",
            '90 G() : PRINT "n="; N; " k="; K',
            "100 SETTICK 0, TICK",
            '110 PRINT "tk"; TK > 0',
        ]

    timed = _run_program(console, program(1))
    untimed = _run_program(console, program(0))
    assert timed == ["after", "n=50000 k=1", "tk1"], timed
    assert untimed == ["after", "n=50000 k=1", "tk0"], untimed


def _midline_do_program(period, body_lines, tail):
    lines = [
        "10 DIM INTEGER N, M, TK",
        "20 SUB TICK()",
        "30 TK = TK + 1",
        "40 END SUB",
        "50 N = 0 : M = 0",
        f"60 SETTICK {period}, TICK",
    ]
    lines.extend(body_lines)
    lines.extend(
        [
            "90 SETTICK 0, TICK",
            tail,
            '99 PRINT "tk"; TK > 0',
        ]
    )
    return lines


def test_midline_do_loop_with_settick(console):
    """The #1329 mid-line DO/LOOP resume survives ticks during PAUSE."""
    body = ["70 DO : N = N + 1 : PAUSE 1 : LOOP UNTIL N > 3"]
    tail = '80 PRINT "n="; N'
    timed = _run_program(console, _midline_do_program(1, body, tail))
    untimed = _run_program(console, _midline_do_program(0, body, tail))
    assert timed == ["n=4", "tk1"], timed
    assert untimed == ["n=4", "tk0"], untimed


def test_midline_do_multiline_loop_with_settick(console):
    """A mid-line DO whose LOOP is on a later line is unchanged by ticks."""
    body = [
        "70 M = 0 : DO : N = N + 1",
        "75 M = M + 1 : PAUSE 1",
        "80 LOOP UNTIL N > 3",
    ]
    tail = '85 PRINT "n="; N; " m="; M'
    timed = _run_program(console, _midline_do_program(1, body, tail))
    untimed = _run_program(console, _midline_do_program(0, body, tail))
    assert timed == ["n=4 m=4", "tk1"], timed
    assert untimed == ["n=4 m=4", "tk0"], untimed


def test_exit_do_from_midline_do_with_settick(console):
    """EXIT DO from a mid-line DO still lands after LOOP under ticks."""
    body = [
        "70 DO : N = N + 1 : PAUSE 1 : IF N = 2 THEN EXIT DO",
        "80 LOOP",
    ]
    tail = '85 PRINT "n="; N'
    timed = _run_program(console, _midline_do_program(1, body, tail))
    untimed = _run_program(console, _midline_do_program(0, body, tail))
    assert timed == ["n=2", "tk1"], timed
    assert untimed == ["n=2", "tk0"], untimed


def _exit_sub_program(period):
    return [
        "10 DIM INTEGER N, TK",
        "20 SUB TICK()",
        "30 TK = TK + 1",
        "40 END SUB",
        "50 SUB MK(M)",
        "55 PAUSE 1",
        "60 N = N + 1",
        "70 END SUB",
        "80 SUB G()",
        "90 MK(1) : EXIT SUB",
        '100 PRINT "never"',
        "110 END SUB",
        "120 N = 0 : TK = 0",
        f"130 SETTICK {period}, TICK",
        '140 G() : PRINT "back, n="; N',
        "150 SETTICK 0, TICK",
        '160 PRINT "tk"; TK > 0',
    ]


def test_exit_sub_after_same_line_sub_call_with_settick(console):
    """The #1332 same-line SUB call resume survives ticks during PAUSE."""
    timed = _run_program(console, _exit_sub_program(1))
    untimed = _run_program(console, _exit_sub_program(0))
    assert timed == ["back, n=1", "tk1"], timed
    assert untimed == ["back, n=1", "tk0"], untimed
