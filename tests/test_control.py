"""QuickBasic control-flow gaps: CHAIN, RESUME (with ON ERROR GOTO), STOP."""

from ihelp_util import dump_topic


def _write_bas(console, path, lines):
    assert console.send_line(f'OPEN "{path}" FOR OUTPUT AS #1') == ""
    for line in lines:
        esc = line.replace('"', '""')
        assert console.send_line(f'PRINT #1, "{esc}"') == ""
    assert console.send_line("CLOSE #1") == ""


def test_stop_halts_program(console):
    assert console.send_line("NEW") == ""
    assert console.send_line('10 PRINT "A"') == ""
    assert console.send_line("20 STOP") == ""
    assert console.send_line('30 PRINT "B"') == ""
    out = console.send_line("RUN")
    lines = [ln.strip() for ln in out.replace("\r", "\n").split("\n") if ln.strip()]
    assert lines == ["A"], out


def test_on_error_and_resume_next(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("10 DIM A(2)") == ""
    assert console.send_line("20 ON ERROR GOTO 100") == ""
    assert console.send_line("30 A(5) = 1") == ""
    assert console.send_line('40 PRINT "after"') == ""
    assert console.send_line("50 END") == ""
    assert console.send_line('100 PRINT "handled"') == ""
    assert console.send_line("110 RESUME NEXT") == ""
    out = console.send_line("RUN")
    lines = [ln.strip() for ln in out.replace("\r", "\n").split("\n") if ln.strip()]
    assert lines == ["handled", "after"], out


def test_on_error_resume_line(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("10 DIM A(2)") == ""
    assert console.send_line("20 ON ERROR GOTO 100") == ""
    assert console.send_line("30 A(5) = 1") == ""
    assert console.send_line('40 PRINT "done"') == ""
    assert console.send_line("50 END") == ""
    assert console.send_line('100 PRINT "trap"') == ""
    assert console.send_line("110 RESUME 40") == ""
    out = console.send_line("RUN")
    lines = [ln.strip() for ln in out.replace("\r", "\n").split("\n") if ln.strip()]
    assert lines == ["trap", "done"], out


def test_error_in_sub_resume_next_unwinds_frame(console):
    """#699: a trapped error unwinds the SUB frame before the handler runs, so
    the handler sees the caller's binding. RESUME NEXT then continues at the
    outer level: the rest of the SUB body runs without its frame and the
    enclosing END SUB ends the program instead of returning to the caller.
    """
    assert console.send_line("NEW") == ""
    assert console.send_line("10 ON ERROR GOTO 500") == ""
    assert console.send_line("20 SUB S(X)") == ""
    assert console.send_line("30 LOCAL X") == ""
    assert console.send_line("40 X = 99") == ""
    assert console.send_line("50 DIM A(2)") == ""
    assert console.send_line("60 A(5) = 1") == ""
    assert console.send_line('70 PRINT "in sub X="; X') == ""
    assert console.send_line("80 END SUB") == ""
    assert console.send_line("100 X = 5") == ""
    assert console.send_line("110 S(7)") == ""
    assert console.send_line('120 PRINT "back X="; X') == ""
    assert console.send_line("130 END") == ""
    assert console.send_line('500 PRINT "handler X="; X') == ""
    assert console.send_line("510 RESUME NEXT") == ""
    out = console.send_line("RUN")
    lines = [ln.strip() for ln in out.replace("\r", "\n").split("\n") if ln.strip()]
    assert lines == ["handler X=5", "in sub X=5"], out


def test_error_in_sub_bare_resume_continues_at_outer_level(console):
    """#699: bare RESUME re-runs the failing line at the outer level; the
    handler can re-establish the array it needs but the SUB frame is gone."""
    assert console.send_line("NEW") == ""
    assert console.send_line("10 ON ERROR GOTO 500") == ""
    assert console.send_line("20 SUB S") == ""
    assert console.send_line("30 DIM A(2)") == ""
    assert console.send_line("40 A(5) = 1") == ""
    assert console.send_line('50 PRINT "in sub"') == ""
    assert console.send_line("60 END SUB") == ""
    assert console.send_line("100 S") == ""
    assert console.send_line('110 PRINT "back"') == ""
    assert console.send_line("120 END") == ""
    assert console.send_line('500 PRINT "handler"') == ""
    assert console.send_line("510 REDIM A(5)") == ""
    assert console.send_line("520 RESUME") == ""
    out = console.send_line("RUN")
    lines = [ln.strip() for ln in out.replace("\r", "\n").split("\n") if ln.strip()]
    assert lines == ["handler", "in sub"], out


def test_resume_line_from_sub_error_returns_to_caller(console):
    """#699: RESUME line is the supported form for errors raised in a SUB; it
    continues at the named outer line and the caller resumes normally."""
    assert console.send_line("NEW") == ""
    assert console.send_line("10 ON ERROR GOTO 500") == ""
    assert console.send_line("20 SUB S") == ""
    assert console.send_line("30 DIM A(2)") == ""
    assert console.send_line("40 A(5) = 1") == ""
    assert console.send_line("50 END SUB") == ""
    assert console.send_line("100 S") == ""
    assert console.send_line('110 PRINT "back"') == ""
    assert console.send_line("120 END") == ""
    assert console.send_line('500 PRINT "handler"') == ""
    assert console.send_line("510 RESUME 110") == ""
    out = console.send_line("RUN")
    lines = [ln.strip() for ln in out.replace("\r", "\n").split("\n") if ln.strip()]
    assert lines == ["handler", "back"], out


def test_error_without_handler_aborts(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("10 DIM A(2)") == ""
    assert console.send_line("20 A(5) = 1") == ""
    out = console.send_line("RUN")
    assert "OUT OF BOUNDS" in out.upper() or "ERROR" in out.upper()


def test_resume_without_error_errors(console):
    assert console.send_line("NEW") == ""
    err = console.send_line("RESUME")
    assert "RESUME" in err.upper()


def test_chain_preserves_variables(console):
    assert console.send_line("NEW") == ""
    _write_bas(console, "CHAIN1.BAS", ["PRINT A + 1", "END"])
    assert console.send_line("A = 41") == ""
    assert console.send_line('CHAIN "CHAIN1.BAS"') == "42"


def test_chain_from_running_program(console):
    assert console.send_line("NEW") == ""
    _write_bas(console, "CHAIN2.BAS", ["PRINT A$", "END"])
    assert console.send_line('10 A$ = "from main"') == ""
    assert console.send_line("20 CHAIN \"CHAIN2.BAS\"") == ""
    assert console.send_line("RUN") == "from main"
    assert console.send_line("PRINT A$") == "from main"


def test_help_control_gaps(console):
    for topic in ("CHAIN", "RESUME", "STOP"):
        out = dump_topic(console, topic)
        assert out != "?SYNTAX ERROR"
        assert topic in out
