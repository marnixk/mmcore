"""Trace-cache coverage for #1370 items 2 and 3.

The cache now compiles the hot expressions it used to reject: ``\\`` and MOD,
AND/OR/NOT, array subscripts built from expressions, and single-statement
``IF ... THEN GOTO/GOSUB`` (labels and line numbers).  Every test runs the same
program with the cache OFF and ON and requires byte-identical output, so the
compiled path cannot drift from expr.c/core.c semantics.  A profiling run then
checks the statements really were replayed instead of silently rejected.
"""

import re

PERF_RE = re.compile(
    r"\[PERF\] elapsed=(\d+) ms  statements=(\d+)  match=(\d+)  expr=(\d+)  "
    r"findvar=(\d+)  break=(\d+)  present=(\d+)"
    r"(?:  tcache=(\d+)/(\d+)/(\d+))?"
)


def _write_bas(console, path, lines):
    assert console.send_line(f'OPEN "{path}" FOR OUTPUT AS #1') == ""
    for line in lines:
        esc = line.replace('"', '""')
        assert console.send_line(f'PRINT #1, "{esc}"') == ""
    assert console.send_line("CLOSE #1") == ""


def _lines(out):
    return [ln.strip() for ln in out.replace("\r", "\n").split("\n") if ln.strip()]


def _compare(console, path, lines, timeout=30.0):
    _write_bas(console, path, lines)
    assert console.send_line("OPTION TRACECACHE OFF") == ""
    off = console.send_line(f'RUN "{path}"', timeout=timeout)
    assert console.send_line("OPTION TRACECACHE ON") == ""
    on = console.send_line(f'RUN "{path}"', timeout=timeout)
    off_lines = [ln for ln in _lines(off) if not ln.startswith("[PERF]")]
    on_lines = [ln for ln in _lines(on) if not ln.startswith("[PERF]")]
    assert off_lines == on_lines, (off, on)
    return on


def _perf(out):
    m = PERF_RE.search(out.replace("\r", "\n"))
    assert m, out
    keys = (
        "elapsed",
        "statements",
        "match",
        "expr",
        "findvar",
        "break",
        "present",
        "tcache_hit",
        "tcache_comp",
        "tcache_bad",
    )
    vals = tuple(int(v) if v is not None else 0 for v in m.groups())
    return dict(zip(keys, vals))


def test_tracecache_idiv_mod_and_or_not(console):
    """`\\`, MOD, AND/OR/NOT keep expr.c's int/float and negative semantics."""
    lines = [
        "DIM S1, S2, S3, S4, S5, I",
        "FOR I = 1 TO 50",
        "S1 = S1 + (I \\ 3) + (-7 \\ 3) + (7 \\ -3) + (7.9 \\ 2) + (7 \\ 0)",
        "S2 = S2 + (I MOD 3) + (-7 MOD 3) + (7 MOD -3) + (7.9 MOD 2) + (7 MOD 0)",
        "S3 = S3 + (1 / 0) + (I / 0)",
        "S4 = S4 + (3.5 AND 6.5) + (1.5 OR 4) + (NOT 0) + (NOT 2)",
        "S5 = S5 + ((I > 0) AND (I < 25)) + ((I = 50) OR (I > 25))",
        "NEXT I",
        'PRINT "R1=";S1;" R2=";S2;" R3=";S3;" R4=";S4;" R5=";S5',
    ]
    assert console.send_line("OPTION PROFILING ON") == ""
    out = _compare(console, "TCEXPR.BAS", lines)
    assert _lines(out)[0] == "R1=358 R2=101 R3=0 R4=550 R5=49", out
    perf = _perf(out)
    assert perf["tcache_hit"] > 100, perf
    assert perf["tcache_bad"] == 0, perf


def test_tracecache_compound_if_conditions(console):
    """AND/OR/NOT and implicit truthiness are cached IF conditions."""
    lines = [
        "DIM C1, C2, C3, C4, I",
        "FOR I = 1 TO 40",
        "IF I > 10 AND I < 20 THEN C1 = C1 + 1",
        "IF I = 5 OR I = 35 THEN C2 = C2 + 1",
        "IF NOT (I MOD 2) THEN C3 = C3 + 1",
        "IF I THEN C4 = C4 + 1",
        "NEXT I",
        'PRINT "C=";C1;",";C2;",";C3;",";C4',
    ]
    assert console.send_line("OPTION PROFILING ON") == ""
    out = _compare(console, "TCIF.BAS", lines)
    assert _lines(out)[0] == "C=9,2,20,40", out
    perf = _perf(out)
    assert perf["tcache_hit"] > 100, perf
    assert perf["tcache_bad"] == 0, perf


def test_tracecache_and_or_evaluate_both_sides(console):
    """No short-circuit: a failing right operand still raises on both paths.

    `0 AND A(99)` would succeed if the compiled AND skipped its right side.
    """
    for op in ("AND", "OR"):
        lines = [
            "DIM A(3)",
            f"X = 0 {op} A(99)",
            'PRINT "not reached"',
        ]
        _write_bas(console, "TCERR.BAS", lines)
        assert console.send_line("OPTION TRACECACHE OFF") == ""
        off = console.send_line('RUN "TCERR.BAS"')
        assert console.send_line("OPTION TRACECACHE ON") == ""
        on = console.send_line('RUN "TCERR.BAS"')
        assert "?INDEX OUT OF BOUNDS @20" in off, off
        assert "?INDEX OUT OF BOUNDS @20" in on, on
        assert "not reached" not in off and "not reached" not in on


def test_tracecache_expression_subscripts_base0(console):
    lines = [
        "DIM A(14), B(4,5), I, J, X",
        "FOR I = 1 TO 10",
        "  A(I+1) = I * 10",
        "NEXT I",
        "FOR I = 1 TO 10",
        "  X = X + A(I+1)",
        "NEXT I",
        "FOR I = 0 TO 3",
        "  FOR J = 0 TO 4",
        "    B(I,J) = I * 10 + J",
        "  NEXT J",
        "NEXT I",
        "FOR I = 0 TO 3",
        "  X = X + B(I, (J+1) MOD 5)",
        "  B(I+1, (J+2) MOD 5) = B(I, (J+1) MOD 5) + 1",
        "NEXT I",
        'PRINT "X=";X;" B=";B(0,1);B(1,2);B(3,4);B(3,0)',
    ]
    assert console.send_line("OPTION PROFILING ON") == ""
    out = _compare(console, "TCIDX0.BAS", lines)
    assert _lines(out)[0] == "X=614 B=123430", out
    perf = _perf(out)
    assert perf["tcache_hit"] > 20, perf
    assert perf["tcache_bad"] == 0, perf


def test_tracecache_expression_subscripts_base1(console):
    lines = [
        "OPTION BASE 1",
        "DIM A(12), B(4,5), I, J, X",
        "FOR I = 1 TO 10",
        "  A(I+1) = I * 10",
        "NEXT I",
        "FOR I = 1 TO 10",
        "  X = X + A(I+1)",
        "NEXT I",
        "FOR I = 1 TO 4",
        "  FOR J = 1 TO 5",
        "    B(I,J) = I * 10 + J",
        "  NEXT J",
        "NEXT I",
        "FOR I = 1 TO 4",
        "  X = X + B(I, (J MOD 5) + 1)",
        "  B(I MOD 4 + 1, (J MOD 5) + 1) = B(I, (J MOD 5) + 1) + 1",
        "NEXT I",
        'PRINT "X=";X;" B=";B(1,2);B(2,3);B(4,5);B(4,1)',
    ]
    assert console.send_line("OPTION PROFILING ON") == ""
    out = _compare(console, "TCIDX1.BAS", lines)
    assert _lines(out)[0] == "X=604 B=16234541", out
    perf = _perf(out)
    assert perf["tcache_hit"] > 20, perf
    assert perf["tcache_bad"] == 0, perf


def test_tracecache_expression_subscript_bounds_error(console):
    lines = [
        "DIM A(3,3), I",
        "FOR I = 0 TO 2",
        "  A(I, I+1) = 1",
        "NEXT I",
        "A(1, 2+2) = 5",
    ]
    _write_bas(console, "TCIDXE.BAS", lines)
    assert console.send_line("OPTION TRACECACHE OFF") == ""
    off = console.send_line('RUN "TCIDXE.BAS"')
    assert console.send_line("OPTION TRACECACHE ON") == ""
    on = console.send_line('RUN "TCIDXE.BAS"')
    assert "?INDEX OUT OF BOUNDS @50" in off, off
    assert "?INDEX OUT OF BOUNDS @50" in on, on


def test_tracecache_if_goto_label_and_line(console):
    lines = [
        "10 Z = 0",
        "20 FOR I = 1 TO 40",
        "30 IF I MOD 2 = 0 THEN GOTO EVEN",
        "40 Z = Z + 1",
        "50 GOTO NEXTI",
        "60 EVEN: Z = Z + 10",
        "70 NEXTI: NEXT I",
        '80 PRINT "LZ=";Z',
        "90 W = 0",
        "100 FOR I = 1 TO 40",
        "110 IF I = 3 THEN GOTO 150",
        "120 W = W + 1",
        "130 GOTO 160",
        "150 W = W + 100",
        "160 NEXT I",
        '170 PRINT "LW=";W',
    ]
    assert console.send_line("OPTION PROFILING ON") == ""
    out = _compare(console, "TCGOTO.BAS", lines)
    assert _lines(out)[0] == "LZ=220", out
    assert _lines(out)[1] == "LW=139", out
    perf = _perf(out)
    assert perf["tcache_hit"] > 60, perf
    assert perf["tcache_bad"] < 10, perf


def test_tracecache_if_gosub_label_and_line(console):
    lines = [
        "10 V = 0",
        "20 FOR I = 1 TO 40",
        "30 IF I MOD 10 = 0 THEN GOSUB ADD10",
        "40 V = V + 1",
        "50 NEXT I",
        '60 PRINT "V=";V',
        "70 V = 0",
        "80 FOR I = 1 TO 40",
        "90 IF I = 20 THEN GOSUB 300",
        "100 V = V + 1",
        "110 NEXT I",
        '120 PRINT "W=";V',
        "130 END",
        "140 ADD10: V = V + 10",
        "150 RETURN",
        "300 V = V + 100",
        "310 RETURN",
    ]
    assert console.send_line("OPTION PROFILING ON") == ""
    out = _compare(console, "TCGOSUB.BAS", lines)
    assert _lines(out)[0] == "V=80", out
    assert _lines(out)[1] == "W=140", out
    perf = _perf(out)
    assert perf["tcache_hit"] > 60, perf
    assert perf["tcache_bad"] < 10, perf


def test_tracecache_expression_subscripts_in_local_sub(console):
    """A LOCAL array re-binds on every call; stale entries must not replay."""
    lines = [
        "DIM TOTAL",
        "SUB HELPER(N)",
        "  LOCAL A(8)",
        "  LOCAL I",
        "  FOR I = 1 TO N",
        "    A(I+1) = I * 2",
        "  NEXT I",
        "  TOTAL = TOTAL + A(N+1)",
        "END SUB",
        "FOR J = 1 TO 30",
        "  HELPER 6",
        "NEXT J",
        'PRINT "T=";TOTAL',
    ]
    assert console.send_line("OPTION PROFILING ON") == ""
    out = _compare(console, "TCLOCAL.BAS", lines)
    assert _lines(out)[0] == "T=360", out
    perf = _perf(out)
    assert perf["tcache_hit"] > 100, perf
    assert perf["tcache_bad"] < 10, perf


def test_tracecache_expression_subscripts_after_redim(console):
    """REDIM bumps the array generation on every pass; recompiles stay correct."""
    lines = [
        "DIM A(5), X, I",
        "FOR I = 1 TO 30",
        "  REDIM A(5)",
        "  A((I MOD 4) + 1) = I",
        "  X = X + A((I MOD 4) + 1)",
        "NEXT I",
        'PRINT "X=";X',
    ]
    out = _compare(console, "TCREDIM.BAS", lines)
    assert _lines(out)[0] == "X=465", out


def test_tracecache_if_goto_in_sub(console):
    """Cached branch targets replay correctly inside a SUB frame."""
    lines = [
        "DIM TOTAL",
        "SUB HELPER(N)",
        "  LOCAL I",
        "  FOR I = 1 TO N",
        "    IF I MOD 2 = 0 THEN GOTO EVEN",
        "    TOTAL = TOTAL + 1",
        "    GOTO NEXTI",
        "EVEN:",
        "    TOTAL = TOTAL + 10",
        "NEXTI:",
        "  NEXT I",
        "END SUB",
        "FOR J = 1 TO 10",
        "  HELPER 10",
        "NEXT J",
        'PRINT "H=";TOTAL',
    ]
    assert console.send_line("OPTION PROFILING ON") == ""
    out = _compare(console, "TCSUB.BAS", lines)
    assert _lines(out)[0] == "H=550", out
    perf = _perf(out)
    assert perf["tcache_hit"] > 100, perf
    assert perf["tcache_bad"] < 10, perf
