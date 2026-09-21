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
    keys = ("elapsed", "statements", "match", "expr", "findvar", "break", "present")
    return {k: int(v) for k, v in zip(keys, m.groups())}


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
    assert p["present"] == 21  # 20 PAGE COPY presents plus the page-1 CLS present
    assert p["elapsed"] < 300  # generous: QEMU under parallel load is slow


@pytest.mark.skipif(
    not PERF_ENABLED, reason="set MMCORE_PERF=1 to run timing guards"
)
def test_profiling_page1_overlay_alpha_bulk(console):
    """#489: the page-1 alpha composite must not fall back to a per-pixel
    RGB round-trip for opaque/transparent pixels (the Xmas hot path)."""
    p = _run_kernel(
        console,
        "PALPHA.BAS",
        [
            "MODE 7,12",
            "PAGE WRITE 2",
            "CLS RGB(0,0,200)",
            "PAGE WRITE 1",
            "CLS",
            # A partial-alpha band forces the blend path (fades.inc does this).
            "FOR X=0 TO 100",
            "LINE X,0,X,50,RGB(255,0,0,1+(X MOD 14))",
            "NEXT X",
            "FOR I=1 TO 30",
            "PAGE COPY 2,1,B",
            "NEXT I",
        ],
        timeout=25.0,
    )
    assert p["present"] >= 30
    # Calibrated on QEMU: ~120 ms optimised vs ~340 ms with the old
    # per-pixel RGB round-trip. 250 catches a return to the slow path
    # while leaving headroom for parallel-load jitter.
    assert p["elapsed"] < 250
