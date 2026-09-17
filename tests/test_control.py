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
