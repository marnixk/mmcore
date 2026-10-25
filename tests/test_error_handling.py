"""ON ERROR SKIP/IGNORE/ABORT/CLEAR and MM.ERRNO / MM.ERRMSG$ (#1323)."""


def _lines(out):
    return [ln.strip() for ln in out.replace("\r", "\n").split("\n") if ln.strip()]


def _program(console, lines):
    assert console.send_line("NEW") == ""
    for line in lines:
        assert console.send_line(line) == ""


def test_errno_starts_clear(console):
    _program(console, ['10 PRINT "START="; MM.ERRNO; "["; MM.ERRMSG$; "]"'])
    assert _lines(console.send_line("RUN")) == ["START=0[]"]


def test_on_error_skip_default_one_statement(console):
    _program(
        console,
        [
            "10 ON ERROR SKIP",
            '20 ERROR "BOOM"',
            '30 PRINT "AFTER"',
            '40 PRINT "NO="; (MM.ERRNO <> 0)',
            '50 PRINT "MSG="; MM.ERRMSG$',
            "60 ON ERROR ABORT",
        ],
    )
    assert _lines(console.send_line("RUN")) == ["AFTER", "NO=1", "MSG=BOOM"]


def test_on_error_skip_n_then_aborts(console):
    _program(
        console,
        [
            "10 ON ERROR SKIP 2",
            '20 ERROR "E1"',
            '30 ERROR "E2"',
            '40 PRINT "AFTER="; MM.ERRMSG$',
            '50 ERROR "E3"',
            '60 PRINT "NOT REACHED"',
        ],
    )
    lines = _lines(console.send_line("RUN"))
    assert lines[0] == "AFTER=E2"
    assert "NOT REACHED" not in lines
    assert any("E3" in ln for ln in lines), lines


def test_on_error_skip_covers_next_two_statements(console):
    _program(
        console,
        [
            "10 ON ERROR SKIP 2",
            '20 PRINT "A"',
            '30 ERROR "E1"',
            '40 PRINT "B"',
            '50 ERROR "E2"',
            '60 PRINT "NOT REACHED"',
        ],
    )
    lines = _lines(console.send_line("RUN"))
    assert lines[:2] == ["A", "B"], lines
    assert "NOT REACHED" not in lines
    assert any("E2" in ln for ln in lines), lines


def test_on_error_ignore_keeps_going(console):
    _program(
        console,
        [
            "10 ON ERROR IGNORE",
            '20 ERROR "E1"',
            '30 ERROR "E2"',
            '40 PRINT "OK="; (MM.ERRNO <> 0); " "; MM.ERRMSG$',
            "50 ON ERROR CLEAR",
            '60 PRINT "CLEARED="; MM.ERRNO; "["; MM.ERRMSG$; "]"',
        ],
    )
    assert _lines(console.send_line("RUN")) == ["OK=1 E2", "CLEARED=0[]"]


def test_on_error_abort_stops_trapping_and_keeps_vars(console):
    _program(
        console,
        [
            "10 ON ERROR IGNORE",
            '20 ERROR "E1"',
            "30 ON ERROR ABORT",
            '40 PRINT "KEPT="; (MM.ERRNO <> 0); " "; MM.ERRMSG$',
            '50 ERROR "E2"',
            '60 PRINT "NOT REACHED"',
        ],
    )
    lines = _lines(console.send_line("RUN"))
    assert lines[0] == "KEPT=1 E1", lines
    assert "NOT REACHED" not in lines
    assert any("E2" in ln for ln in lines), lines


def test_on_error_clear_resets_vars_but_keeps_mode(console):
    _program(
        console,
        [
            "10 ON ERROR IGNORE",
            '20 ERROR "E1"',
            "30 ON ERROR CLEAR",
            '40 PRINT "CLEARED="; MM.ERRNO; "["; MM.ERRMSG$; "]"',
            '50 ERROR "E2"',
            '60 PRINT "AGAIN="; (MM.ERRNO <> 0); " "; MM.ERRMSG$',
        ],
    )
    assert _lines(console.send_line("RUN")) == ["CLEARED=0[]", "AGAIN=1 E2"]


def test_bare_on_error_ends_trapping(console):
    _program(
        console,
        [
            "10 ON ERROR IGNORE",
            '20 ERROR "E1"',
            "30 ON ERROR",
            '40 ERROR "E2"',
            '50 PRINT "NOT REACHED"',
        ],
    )
    lines = _lines(console.send_line("RUN"))
    assert "NOT REACHED" not in lines
    assert any("E2" in ln for ln in lines), lines


def test_mm_errmsg_for_on_error_goto(console):
    _program(
        console,
        [
            "10 ON ERROR GOTO 100",
            '20 ERROR "GOTO BOOM"',
            "30 END",
            "100 PRINT MM.ERRMSG$",
            "110 PRINT (MM.ERRNO <> 0)",
            "120 RESUME 30",
        ],
    )
    assert _lines(console.send_line("RUN")) == ["GOTO BOOM", "1"]


def test_skip_inside_sub_returns_to_caller(console):
    _program(
        console,
        [
            "10 SUB save_score()",
            "20 ON ERROR SKIP 1",
            '30 ERROR "SAVE FAILED"',
            '40 PRINT "ERRNO="; (MM.ERRNO <> 0)',
            '50 PRINT "MSG="; MM.ERRMSG$',
            "60 ON ERROR ABORT",
            "70 END SUB",
            "80 save_score()",
            '90 PRINT "after save"',
            "100 END",
        ],
    )
    assert _lines(console.send_line("RUN")) == [
        "ERRNO=1",
        "MSG=SAVE FAILED",
        "after save",
    ]


def test_skip_inside_function_returns_value(console):
    _program(
        console,
        [
            "10 FUNCTION safe()",
            "20 ON ERROR SKIP 1",
            '30 ERROR "FN FAILED"',
            "40 safe = 7",
            "50 ON ERROR ABORT",
            "60 END FUNCTION",
            "70 PRINT safe()",
            "80 END",
        ],
    )
    assert _lines(console.send_line("RUN")) == ["7"]


def test_in_sub_file_error_repro(console):
    """The issue's shape: a failing file operation inside a SUB is trapped in
    place, MM.ERRMSG$ says why, and the caller continues normally."""
    _program(
        console,
        [
            "10 SUB save_score()",
            "20 ON ERROR SKIP 1",
            '30 KILL "A:/NO_SUCH_FILE_1323.txt"',
            "40 IF MM.ERRNO <> 0 THEN PRINT \"SAVE FAILED: \"; MM.ERRMSG$",
            "50 ON ERROR ABORT",
            "60 END SUB",
            "70 save_score()",
            '80 PRINT "after save"',
            "90 END",
        ],
    )
    lines = _lines(console.send_line("RUN"))
    assert lines[-1] == "after save", lines
    assert lines[0].startswith("SAVE FAILED: "), lines
    assert "FILE" in lines[0].upper(), lines
