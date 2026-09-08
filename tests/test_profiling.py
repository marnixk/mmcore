"""OPTION PROFILING ON: counts/timings after RUN (#131).

Absolute milliseconds vary on QEMU. Tests assert that a [PERF] line is
present and parseable. Immediate-mode lines are not included in the RUN
report (counters reset at the start of RUN).
"""

import re

from harness import MMBasicConsole

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
    assert p["break"] >= 200


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
    assert p["present"] == 20
    assert p["elapsed"] < 120
