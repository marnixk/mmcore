"""OPTION PROFILING ON: counts/timings after RUN (#131).

Absolute milliseconds vary on QEMU. Tests assert that a [PERF] line is
present and parseable. Immediate-mode lines are not included in the RUN
report (counters reset at the start of RUN).
"""

import os
import re

import pytest

from harness import MMBasicConsole

# Timing guards are opt-in: QEMU timing under parallel load is noisy.
PERF_ENABLED = os.environ.get("MMCORE_PERF", "").strip().lower() in (
    "1",
    "true",
    "yes",
    "on",
)

PERF_RE = re.compile(
    r"\[PERF\] elapsed=(\d+) ms  statements=(\d+)  match=(\d+)  "
    r"expr=(\d+)  findvar=(\d+)  break=(\d+)  present=(\d+)"
    r"(?:  tcache=(\d+)/(\d+)/(\d+))?"
)


def _write_bas(console: MMBasicConsole, path: str, lines: list[str]) -> None:
    assert console.send_line(f'OPEN "{path}" FOR OUTPUT AS #1') == ""
    for line in lines:
        esc = line.replace('"', '""')
        assert console.send_line(f'PRINT #1, "{esc}"') == ""
    assert console.send_line("CLOSE #1") == ""


def parse_perf(out: str) -> dict[str, int]:
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


def _run_kernel(
    console: MMBasicConsole, name: str, lines: list[str], timeout: float = 8.0
) -> dict[str, int]:
    _write_bas(console, name, lines)
    assert console.send_line("OPTION PROFILING ON") == ""
    out = console.send_line(f'RUN "{name}"', timeout=timeout)
    return parse_perf(out)


def test_profiling_off_has_no_perf_line(console):
    assert console.send_line("OPTION PROFILING OFF") == ""
    assert console.send_line("NEW") == ""
    assert console.send_line("10 PRINT 1") == ""
    out = console.send_line("RUN")
    assert "[PERF]" not in out
    assert out == "1"


def test_profiling_integer_for(console):
    p = _run_kernel(
        console,
        "PFOR.BAS",
        ["A=0", "FOR I=1 TO 200", "A=A+1", "NEXT I", "PRINT A"],
    )
    assert p["statements"] >= 200
    assert p["match"] < 5000
    assert p["expr"] > 0
    assert p["findvar"] > 0
    assert p["findvar"] < 80
    assert p["break"] >= 200


def test_profiling_for_next_skips_findvar(console):
    p = _run_kernel(
        console,
        "PNXT.BAS",
        ["FOR I=1 TO 200", "NEXT I", "PRINT I"],
    )
    assert p["statements"] >= 200
    assert p["findvar"] < 20


def test_profiling_false_same_line_if_skip(console):
    p = _run_kernel(
        console,
        "PIF.BAS",
        [
            "A=0",
            "FOR I=1 TO 101",
            "IF I<0 THEN I=I+384",
            "IF I>999 THEN I=I-384",
            "A=A+1",
            "NEXT I",
            "PRINT A",
        ],
    )
    assert p["statements"] >= 400
    assert p["match"] < 4000
    assert p["findvar"] < 80


def test_profiling_cos_rnd_int_opcode(console):
    p = _run_kernel(
        console,
        "PCOS.BAS",
        [
            "LET X=0",
            "FOR I=1 TO 80",
            "LET X=COS(I/100)",
            "NEXT I",
            "PRINT INT(X*100)",
        ],
    )
    assert p["statements"] >= 80
    assert p["match"] < 2500


def test_profiling_tracecache_array_let(console):
    lines = [
        "DIM POS.X(100), SPEED(100)",
        "RATIO=1.5",
        "FOR I=0 TO 100",
        "POS.X(I)=I",
        "SPEED(I)=2",
        "NEXT I",
        "FOR I=0 TO 100",
        "POS.X(I)=POS.X(I)+(SPEED(I)*RATIO)",
        "IF POS.X(I)<0 THEN POS.X(I)=POS.X(I)+384",
        "NEXT I",
        "PRINT INT(POS.X(50))",
    ]
    assert console.send_line("OPTION TRACECACHE OFF") == ""
    off = _run_kernel(console, "TCOFF.BAS", lines)
    assert console.send_line("OPTION TRACECACHE ON") == ""
    on = _run_kernel(console, "TCON.BAS", lines)
    assert on["findvar"] < off["findvar"] // 2
    assert on["expr"] < off["expr"] // 2


def test_profiling_tracecache_2d_array_let(console):
    """#1002: the trace cache compiles 2-D array LET and element reads, so the
    2-D kernel stops taking the general path even with the cache ON."""
    lines = [
        "DIM A(20,20)",
        "FOR I=0 TO 19",
        "FOR J=0 TO 19",
        "A(I,J)=I+J",
        "NEXT J",
        "NEXT I",
        "PRINT A(3,4)",
    ]
    assert console.send_line("OPTION TRACECACHE OFF") == ""
    off = _run_kernel(console, "TC2OFF.BAS", lines)
    assert console.send_line("OPTION TRACECACHE ON") == ""
    on = _run_kernel(console, "TC2ON.BAS", lines)
    # The resolve cache already leaves findvar near its floor on both paths
    # (#1002); the trace cache additionally removes the per-iteration parse.
    assert off["findvar"] < 100
    assert on["findvar"] < 100
    assert on["expr"] < off["expr"] // 10


def _local_loop_kernel(local: bool) -> list[str]:
    lines = ["DIM TOTAL", "SUB HELPER(X)"]
    if local:
        lines += ["  LOCAL Z", "  Z = X * 2"]
    lines += [
        "END SUB",
        "FOR I = 1 TO 300",
        "  TOTAL = TOTAL + I",
        "  HELPER I",
        "NEXT I",
        "PRINT TOTAL",
    ]
    return lines


def test_local_does_not_flush_trace_cache_1368(console):
    """#1368: a SUB with LOCAL must not clear the trace cache, so the caller's
    loop statements keep replaying from cache and produce the same result."""
    assert console.send_line("OPTION TRACECACHE ON") == ""
    _write_bas(console, "TCLP.BAS", _local_loop_kernel(False))
    assert console.send_line("OPTION PROFILING ON") == ""
    out_plain = console.send_line('RUN "TCLP.BAS"', timeout=30)
    plain = parse_perf(out_plain)
    assert "45150" in out_plain, out_plain

    _write_bas(console, "TCLL.BAS", _local_loop_kernel(True))
    out_local = console.send_line('RUN "TCLL.BAS"', timeout=30)
    local = parse_perf(out_local)
    assert "45150" in out_local, out_local

    # Before #1368 the LOCAL call flushed every entry after each iteration:
    # the local variant recorded 0 hits and recompiled the whole loop.  The
    # caller's statements must still hit, i.e. only the LOCAL slot is
    # affected (its body naturally recompiles because the slot is freed).
    assert local["tcache_hit"] >= plain["tcache_hit"] // 2
    assert local["tcache_hit"] > 200
    assert local["tcache_comp"] < 400
    assert local["findvar"] <= plain["findvar"]


def _many_globals_lines(count: int) -> list[str]:
    names = [f"GV{i}" for i in range(1, count + 1)]
    return [
        "DIM INTEGER " + ", ".join(names[i : i + 8])
        for i in range(0, len(names), 8)
    ]


def test_local_many_globals_does_not_recompile_1328(console):
    """#1328: with many globals, a SUB with LOCALs must not recompile its body
    on every call.  The old linear variable-table scans forced a trace-cache
    miss per call; the binding generations now survive a fresh LOCAL return."""
    lines = _many_globals_lines(400) + [
        "DIM INTEGER TOTAL",
        "SUB HELPER(X)",
        "  LOCAL Z, W",
        "  Z = X * 2",
        "  W = Z + 1",
        "  TOTAL = TOTAL + W",
        "END SUB",
        "FOR I = 1 TO 300",
        "  HELPER I",
        "NEXT I",
        "PRINT TOTAL",
    ]
    assert console.send_line("OPTION TRACECACHE ON") == ""
    _write_bas(console, "TCG.BAS", lines)
    assert console.send_line("OPTION PROFILING ON") == ""
    out = console.send_line('RUN "TCG.BAS"', timeout=30)
    # 2*sum(1..300) + 300 = 90600
    assert "90600" in out, out
    p = parse_perf(out)
    assert p["statements"] >= 600, p
    assert p["tcache_hit"] > 500, p
    assert p["tcache_comp"] < 60, p


def test_tracecache_chain_invalidates(console):
    """#1007: CHAIN re-tokenizes the next program, so the trace cache must be
    dropped too. Otherwise a compiled entry from the previous program is
    replayed against the new text at the same token address and raises a
    spurious syntax error."""
    assert console.send_line("OPTION TRACECACHE ON") == ""
    _write_bas(console, "TCCH1.BAS", ["X=5", 'CHAIN "TCCH2.BAS"'])
    _write_bas(console, "TCCH2.BAS", ["Y=X+1", "PRINT Y"])
    out = console.send_line('RUN "TCCH1.BAS"')
    lines = [ln.strip() for ln in out.replace("\r", "\n").split("\n") if ln.strip()]
    assert "?SYNTAX" not in out.upper(), out
    assert "6" in lines, out


def test_profiling_float_math(console):
    p = _run_kernel(
        console,
        "PFLT.BAS",
        ["X=0.5", "FOR I=1 TO 80", "X=X*1.01+0.5", "NEXT I", "PRINT INT(X)"],
    )
    assert p["expr"] > 0
    assert p["statements"] >= 80


def test_profiling_string_concat(console):
    p = _run_kernel(
        console,
        "PSTR.BAS",
        ['A$="X"', "FOR I=1 TO 20", 'A$=A$+"Y"', "NEXT I", "PRINT LEN(A$)"],
    )
    assert p["expr"] > 0
    assert p["findvar"] > 0


def test_profiling_pixel_line(console):
    p = _run_kernel(
        console,
        "PPIX.BAS",
        ["CLS", "FOR I=0 TO 20", "PIXEL I,I,RGB(255,0,0)", "LINE 0,I,20,I", "NEXT I"],
    )
    assert p["statements"] >= 20
    assert p["expr"] > 0


def test_profiling_page_copy(console):
    p = _run_kernel(
        console,
        "PPAGE.BAS",
        [
            "MODE 1",
            "PAGE WRITE 1",
            "CLS RGB(0,0,64)",
            "PAGE COPY 1 TO 0",
            "PAGE WRITE 0",
        ],
    )
    assert p["present"] >= 1
    assert p["statements"] >= 4


def test_profiling_page_copy_mode17_bulk(console):
    p = _run_kernel(
        console,
        "PPAGE17.BAS",
        [
            "MODE 17,16",
            "PAGE WRITE 1",
            "CLS RGB(0,32,0)",
            "FOR I=1 TO 20",
            "PAGE COPY 1 TO 0",
            "NEXT I",
        ],
        timeout=12.0,
    )
    assert p["present"] == 20  # 20 PAGE COPY presents; page-1 CLS no longer presents
    assert p["elapsed"] < 300  # generous: QEMU under parallel load is slow


@pytest.mark.skipif(
    not PERF_ENABLED, reason="set MMCORE_PERF=1 to run timing guards"
)
def test_profiling_page1_offscreen_copy_bulk(console):
    """#1051: page 1 is a plain page, so bulk PAGE COPY onto it is a native
    copy with no present and no per-pixel alpha composite."""
    p = _run_kernel(
        console,
        "PALPHA.BAS",
        [
            "MODE 7,12",
            "PAGE DISPLAY 0",
            "PAGE WRITE 2",
            "CLS RGB(0,0,200)",
            "PAGE WRITE 1",
            "CLS",
            "FOR I=1 TO 30",
            "PAGE COPY 2,1,B",
            "NEXT I",
        ],
        timeout=25.0,
    )
    # Page 1 is offscreen (display page 0), so none of the writes present.
    assert p["present"] == 0
    assert p["elapsed"] < 250


def test_tracecache_survives_full_256_slots_1369(console):
    """#1369: the old fixed 256-slot table filled for good during setup, so
    hot loop statements never found a slot and were re-parsed on every pass.

    Generate a program with 280 distinct cacheable setup statements (more than
    the old table held) plus 100 distinct statements that cannot be compiled,
    then run a hot loop. The table must grow, failed compiles must sit in the
    compact negative set (one bad count, not one per execution), and the hot
    statements must replay with identical results.
    """
    generator = [
        'OPEN "TCCAP.BAS" FOR OUTPUT AS #1',
        'PRINT #1, "DIM A(8)"',
        'PRINT #1, "X=0"',
        'PRINT #1, "T=0"',
        'PRINT #1, "U=0"',
        'PRINT #1, "Z=0"',
        "FOR I = 1 TO 280",
        'PRINT #1, "X="+LTRIM$(STR$(I))',
        "NEXT I",
        "FOR I = 1 TO 100",
        'PRINT #1, "X=ABS(-"+LTRIM$(STR$(I))+")"',
        "NEXT I",
        'PRINT #1, "FOR J=1 TO 40"',
        'PRINT #1, "FOR K=0 TO 4"',
        'PRINT #1, "A(K)=A(K)+1"',
        'PRINT #1, "T=T+A(K)"',
        'PRINT #1, "IF T>0 THEN U=U+1"',
        'PRINT #1, "IF T>0 THEN Z=ABS(-T)"',
        'PRINT #1, "NEXT K"',
        'PRINT #1, "NEXT J"',
        'PRINT #1, "PRINT A(0);A(3);T;U"',
        "CLOSE #1",
    ]
    assert console.send_line("NEW") == ""
    _write_bas(console, "TCGEN.BAS", generator)
    gen = console.send_line('RUN "TCGEN.BAS"', timeout=30)
    assert "?SYNTAX" not in gen.upper(), gen

    def result_lines(text: str) -> list[str]:
        return [
            ln.strip()
            for ln in text.replace("\r", "\n").split("\n")
            if ln.strip() and "[PERF]" not in ln
        ]

    assert console.send_line("OPTION TRACECACHE OFF") == ""
    assert console.send_line("OPTION PROFILING ON") == ""
    off_out = console.send_line('RUN "TCCAP.BAS"', timeout=60)
    off = parse_perf(off_out)
    assert off["tcache_hit"] == 0
    assert off["tcache_comp"] == 0

    assert console.send_line("OPTION TRACECACHE ON") == ""
    on_out = console.send_line('RUN "TCCAP.BAS"', timeout=60)
    on = parse_perf(on_out)

    # A full table must never change the answer.
    assert result_lines(on_out) == result_lines(off_out)
    assert result_lines(on_out) == ["40404100200"], on_out

    # The 280 setup statements exceed the old capacity and are all compiled
    # once; the hot loop (3 cacheable statements x 200 passes) then replays.
    assert on["tcache_comp"] >= 280, on
    assert on["tcache_hit"] > 400, on
    # 100 setup failures + the one hot failure, not 100 + 200 retries.
    assert on["tcache_bad"] < 150, on

    # CLEAR must invalidate the grown table and the negative set, not leave
    # stale entries behind: the same run rebuilds both and still replays.
    assert console.send_line("CLEAR") == ""
    cleared_out = console.send_line('RUN "TCCAP.BAS"', timeout=60)
    cleared = parse_perf(cleared_out)
    assert result_lines(cleared_out) == ["40404100200"], cleared_out
    assert cleared["tcache_hit"] > 400, cleared
