"""#1421: compiled argument lists for BLIT / SPRITE statements.

A repeated graphics statement with names and arithmetic in its arguments
should replay the compiled expressions.  These tests draw the same scene with
the cache OFF and ON and require identical pixels/errors, and check the
profiling counters prove the lists were actually cached.
"""

import re

PERF_RE = re.compile(r"\[PERF\].*")
TC_RE = re.compile(r"tcache=(\d+)/(\d+)/(\d+)")


def _write_bas(console, path, lines):
    assert console.send_line(f'OPEN "{path}" FOR OUTPUT AS #1') == ""
    for line in lines:
        esc = line.replace('"', '""')
        assert console.send_line(f'PRINT #1, "{esc}"') == ""
    assert console.send_line("CLOSE #1") == ""


def _lines(out):
    return [ln.strip() for ln in out.replace("\r", "\n").split("\n")
            if ln.strip() and not ln.startswith("[PERF]")]


def _off_on(console, lines, path="TCGFX.BAS", timeout=60.0):
    _write_bas(console, path, lines)
    assert console.send_line("OPTION TRACECACHE OFF") == ""
    off = console.send_line(f'RUN "{path}"', timeout=timeout)
    assert console.send_line("OPTION TRACECACHE ON") == ""
    on = console.send_line(f'RUN "{path}"', timeout=timeout)
    assert _lines(off) == _lines(on), (off, on)
    return on


def _tcache(on):
    m = TC_RE.search(on.replace("\r", "\n"))
    assert m, on
    return tuple(int(g) for g in m.groups())


def test_repeated_blit_sprite_expression_args(console):
    lines = [
        "OPTION PROFILING ON",
        "MODE 8,16",
        "PAGE WRITE 1",
        "CLS RGB(200, 40, 40)",
        "BOX 0, 0, 120, 80, 1, RGB(0, 200, 0), RGB(0, 200, 0)",
        "PAGE WRITE 0",
        "CLS RGB(0, 0, 0)",
        "DIM I",
        "FOR I = 1 TO 12",
        "  BLIT (I MOD 5) * 8, ((I \\ 5) MOD 5) * 8, 24, 24, 24, 24, 1",
        "NEXT I",
        'PRINT "P";PIXEL(0,0);",";PIXEL(31,31);",";PIXEL(100,100)',
        "SPRITE READ 1, 0, 0, 8, 8, 1",
        "FOR I = 1 TO 6",
        "  SPRITE SHOW 1, 40 + (I MOD 5) * 8, 60 + ((I \\ 5) MOD 2) * 8, 1",
        "NEXT I",
        'PRINT "S";PIXEL(64,68)',
    ]
    on = _off_on(console, lines)
    hit, comp, bad = _tcache(on)
    assert hit > 12, (hit, comp, bad)
    assert bad == 0, (hit, comp, bad)


def test_blit_expression_errors_match_parser(console):
    for line, message in (
        ("BLIT 0,0,60,10,4", "?SYNTAX ERROR"),
        ('BLIT "x",0,60,10,4,4,1,0', "?TYPE MISMATCH"),
    ):
        assert console.send_line("NEW") == ""
        assert console.send_line(line) == message, line


def test_blit_array_expression_bounds_error_matches(console):
    lines = [
        "DIM A(3), I",
        "I = 99",
        "BLIT A(I),0,10,10,4,4,1,0",
        'PRINT "after"',
    ]
    _write_bas(console, "TCGFXERR.BAS", lines)
    assert console.send_line("OPTION TRACECACHE OFF") == ""
    off = console.send_line('RUN "TCGFXERR.BAS"')
    assert console.send_line("OPTION TRACECACHE ON") == ""
    on = console.send_line('RUN "TCGFXERR.BAS"')
    assert "?INDEX OUT OF BOUNDS" in off, off
    assert "?INDEX OUT OF BOUNDS" in on, on
    assert "after" not in off and "after" not in on
