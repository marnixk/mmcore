"""#1419: multi-line IF/ELSEIF conditions replay through the trace cache.

The cache used to refuse a THEN at end of line, so every block-IF condition
was re-parsed (and every name re-resolved) on each execution.  Each test runs
the same program with OPTION TRACECACHE OFF and ON and requires byte-identical
output, including error lines and skipped-code side effects.
"""

import re

PERF_RE = re.compile(r"\[PERF\].*")
PERF_TC_RE = re.compile(r"tcache=(\d+)/(\d+)/(\d+)")


def _write_bas(console, path, lines):
    assert console.send_line(f'OPEN "{path}" FOR OUTPUT AS #1') == ""
    for line in lines:
        esc = line.replace('"', '""')
        assert console.send_line(f'PRINT #1, "{esc}"') == ""
    assert console.send_line("CLOSE #1") == ""


def _lines(out):
    return [ln.strip() for ln in out.replace("\r", "\n").split("\n")
            if ln.strip() and not ln.startswith("[PERF]")]


def _run_off_on(console, path, lines, timeout=60.0):
    _write_bas(console, path, lines)
    assert console.send_line("OPTION TRACECACHE OFF") == ""
    off = console.send_line(f'RUN "{path}"', timeout=timeout)
    assert console.send_line("OPTION TRACECACHE ON") == ""
    on = console.send_line(f'RUN "{path}"', timeout=timeout)
    off_lines, on_lines = _lines(off), _lines(on)
    assert off_lines == on_lines, (off, on)
    return on_lines, on


def _tcache(on):
    m = PERF_TC_RE.search(on.replace("\r", "\n"))
    assert m, on
    hit, comp, bad = (int(g) for g in m.groups())
    return hit, comp, bad


def test_block_if_elseif_nested_and_cache_hits(console):
    lines = [
        "OPTION PROFILING ON",
        "DIM I, C1, C2, C3",
        "FOR I = 1 TO 40",
        "  IF I < 5 THEN",
        "    C1 = C1 + 1",
        "  ELSEIF I < 10 THEN",
        "    C2 = C2 + 1",
        "  ELSEIF I = 20 THEN",
        "    IF I MOD 2 = 0 THEN",
        "      C3 = C3 + 1",
        "    ELSE",
        "      C3 = C3 + 100",
        "    END IF",
        "  ELSE",
        "    C1 = C1 + 10",
        "  END IF",
        "NEXT I",
        'PRINT "R";C1;",";C2;",";C3',
    ]
    out, on = _run_off_on(console, "TCBIF.BAS", lines)
    assert out == ["R304,5,1"], out
    hit, comp, bad = _tcache(on)
    assert hit > 100, (hit, comp, bad)
    assert bad == 0, (hit, comp, bad)


def test_block_if_exit_function_and_exit_sub(console):
    lines = [
        "FUNCTION F(N)",
        "  IF N < 0 THEN",
        "    F = -1",
        "    EXIT FUNCTION",
        "  END IF",
        "  F = N * 2",
        "END FUNCTION",
        "SUB S(N)",
        "  IF N > 3 THEN",
        "    EXIT SUB",
        "  END IF",
        '  PRINT "S";N',
        "END SUB",
        "FOR I = -1 TO 4",
        '  PRINT "F";F(I)',
        "  S I",
        "NEXT I",
    ]
    out, _ = _run_off_on(console, "TCBEXIT.BAS", lines)
    assert out == ["F-1", "S-1", "F0", "S0", "F2", "S1", "F4", "S2",
                   "F6", "S3", "F8"], out


def test_block_if_and_or_evaluate_both_sides(console):
    """A failing right operand still raises identically on both paths."""
    lines = [
        "DIM A(3)",
        "IF 0 AND A(99) THEN",
        '  PRINT "not reached"',
        "END IF",
        'PRINT "after"',
    ]
    _write_bas(console, "TCBERR.BAS", lines)
    assert console.send_line("OPTION TRACECACHE OFF") == ""
    off = console.send_line('RUN "TCBERR.BAS"')
    assert console.send_line("OPTION TRACECACHE ON") == ""
    on = console.send_line('RUN "TCBERR.BAS"')
    assert "?INDEX OUT OF BOUNDS @20" in off, off
    assert "?INDEX OUT OF BOUNDS @20" in on, on
    assert "not reached" not in off and "not reached" not in on
    assert "after" not in off and "after" not in on


def test_block_if_string_compare_and_trailing_colon(console):
    lines = [
        'S$ = "x"',
        "IF S$ = \"x\" THEN",
        '  PRINT "STR"',
        "ELSE",
        '  PRINT "NO"',
        "END IF",
        'IF 1 THEN : PRINT "COLON1"',
        'IF 0 THEN : PRINT "COLON0" : PRINT "COLON0B"',
        "END IF",
        'PRINT "AFTER"',
    ]
    out, _ = _run_off_on(console, "TCBCOL.BAS", lines)
    assert out == ["STR", "COLON1", "COLON0", "COLON0B", "AFTER"], out


def test_block_if_elseif_condition_side_effect(console):
    """ELSEIF conditions are cached too, but calls stay on the interpreter."""
    lines = [
        "OPTION PROFILING ON",
        "DIM I, N, C, K",
        "FOR I = 1 TO 30",
        "  IF I = 5 THEN",
        "    C = C + 1",
        "  ELSEIF BUMP(N) = 7 THEN",
        "    C = C + 10",
        "  ELSE",
        "    C = C + 100",
        "  END IF",
        "NEXT I",
        'PRINT "R";C;",";N',
        "FUNCTION BUMP(X)",
        "  X = X + 1",
        "  BUMP = 7",
        "END FUNCTION",
    ]
    out, on = _run_off_on(console, "TCBSIDE.BAS", lines)
    # I=5 takes the first arm (BUMP not called); the other 29 iterations call
    # BUMP once each. Parameters are by value, so the caller's N stays 0.
    assert out == ["R291,0"], out
    hit, comp, bad = _tcache(on)
    assert hit > 30, (hit, comp, bad)
