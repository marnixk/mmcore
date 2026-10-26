#!/usr/bin/env python3
"""Benchmark the LOCAL trace-cache fixes on the native interpreter.

Two modes:

  local-tcache  the #1368 repro: plain vs LOCAL vs TRACECACHE OFF
  globals       the #1328 repro at 0 / 400 / 1200 globals, two locals vs none

The #1368 fix is good when ``local`` is not slower than ``off``.  The #1328
fix is good when the two-LOCAL cost is flat as the global count grows.  A
timing assertion is deliberately not part of pytest: QEMU timing is too noisy
for CI, so this script is the reproducible check.

Usage:
    .venv/bin/python scripts/bench-local-tcache.py [--reps N] [--no-build]
    .venv/bin/python scripts/bench-local-tcache.py --mode globals

Exit status is nonzero when the local-tcache mode sees ``local`` slower than
``off``, or globals mode sees the two-LOCAL cost grow past --max-growth.
"""

from __future__ import annotations

import argparse
import os
import re
import statistics
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NATIVE = os.path.join(REPO, "native", "mmbasic")
LINE_RE = re.compile(r"(plain|local|off): us/iter\s+([0-9.]+)")
GLOBALS_RE = re.compile(r"(no locals|two locals): us/call\s+([0-9.]+)")

PROGRAM = """\
OPTION EXPLICIT
OPTION DEFAULT INTEGER
DIM i, a, b, c, d, e, f, g, h
DIM t0, base
CONST N = 20000
SUB plain()
END SUB
SUB withlocal()
  LOCAL z
END SUB
T0 = TIMER
FOR i = 1 TO N
  a = i + 1 : b = a * 2 : c = b - a : d = c + 3 : e = d * 2
  f = e - 1 : g = f + a : h = g - b
  plain
NEXT i
PRINT "plain: us/iter "; (TIMER - T0) / N * 1000
T0 = TIMER
FOR i = 1 TO N
  a = i + 1 : b = a * 2 : c = b - a : d = c + 3 : e = d * 2
  f = e - 1 : g = f + a : h = g - b
  withlocal
NEXT i
PRINT "local: us/iter "; (TIMER - T0) / N * 1000
OPTION TRACECACHE OFF
T0 = TIMER
FOR i = 1 TO N
  a = i + 1 : b = a * 2 : c = b - a : d = c + 3 : e = d * 2
  f = e - 1 : g = f + a : h = g - b
  plain
NEXT i
PRINT "off: us/iter "; (TIMER - T0) / N * 1000
"""


def build_native() -> None:
    subprocess.run(
        [os.path.join(REPO, "scripts", "build-native.sh")],
        cwd=REPO,
        check=True,
        stdout=subprocess.DEVNULL,
    )


def globals_program(n: int) -> str:
    """The #1328 repro: N globals, then calls to helpers with/without LOCALs."""
    lines = []
    names = [f"gv{i}" for i in range(1, n + 1)]
    for i in range(0, len(names), 8):
        lines.append("DIM INTEGER " + ", ".join(names[i : i + 8]))
    lines.append("DIM INTEGER i, k, t0")
    lines.append("t0 = TIMER")
    lines.append("FOR i = 1 TO 10000 : k = F1(3) : NEXT i")
    lines.append('PRINT "no locals: us/call "; (TIMER - t0) / 10000 * 1000')
    lines.append("t0 = TIMER")
    lines.append("FOR i = 1 TO 10000 : k = F3(3) : NEXT i")
    lines.append('PRINT "two locals: us/call "; (TIMER - t0) / 10000 * 1000')
    lines.append("END")
    lines.append("FUNCTION F1(a)")
    lines.append("  F1 = a")
    lines.append("END FUNCTION")
    lines.append("FUNCTION F3(a)")
    lines.append("  LOCAL x, y")
    lines.append("  x = a")
    lines.append("  F3 = x")
    lines.append("END FUNCTION")
    return "\n".join(lines) + "\n"


def run_once(
    program: str = PROGRAM,
    pattern: re.Pattern = LINE_RE,
    expected: tuple[str, ...] = ("plain", "local", "off"),
) -> dict[str, float]:
    with tempfile.TemporaryDirectory(prefix="mmb-bench-") as drive:
        with open(os.path.join(drive, "BENCH.BAS"), "w", encoding="ascii") as fh:
            fh.write(program)
        env = dict(os.environ)
        env.setdefault("SDL_VIDEODRIVER", "dummy")
        out = subprocess.run(
            [NATIVE, "--drive", drive, 'RUN "D:/BENCH.BAS"'],
            cwd=REPO,
            env=env,
            check=True,
            capture_output=True,
            text=True,
            timeout=120,
        ).stdout
    found = {name: float(value) for name, value in pattern.findall(out)}
    missing = set(expected) - found.keys()
    if missing:
        raise SystemExit(f"benchmark output missing {sorted(missing)}:\n{out}")
    return found


def bench_local_tcache(reps: int) -> int:
    runs = [run_once() for _ in range(max(1, reps))]
    medians = {
        k: statistics.median(r[k] for r in runs) for k in ("plain", "local", "off")
    }

    print(f"#1368 LOCAL trace-cache benchmark ({len(runs)} runs, medians)")
    print(f"  plain (SUB without LOCAL) : {medians['plain']:.2f} us/iter")
    print(f"  local (SUB with LOCAL Z)  : {medians['local']:.2f} us/iter")
    print(f"  off   (OPTION TRACECACHE OFF): {medians['off']:.2f} us/iter")
    if medians["local"] <= medians["off"]:
        print("  PASS: local is no slower than the TRACECACHE OFF path")
        return 0
    print("  FAIL: local is slower than the TRACECACHE OFF path")
    return 1


def bench_globals(reps: int, counts: list[int], max_growth: float) -> int:
    medians: dict[int, dict[str, float]] = {}
    for n in counts:
        runs = [
            run_once(
                globals_program(n),
                GLOBALS_RE,
                ("no locals", "two locals"),
            )
            for _ in range(max(1, reps))
        ]
        medians[n] = {
            k: statistics.median(r[k] for r in runs)
            for k in ("no locals", "two locals")
        }

    print(f"#1328 LOCAL/global-count benchmark ({max(1, reps)} runs, medians)")
    print(f"  {'globals':>7}  {'no locals':>12}  {'two locals':>12}")
    for n in counts:
        m = medians[n]
        print(f"  {n:>7}  {m['no locals']:>10.2f} us  {m['two locals']:>10.2f} us")
    first = medians[counts[0]]["two locals"]
    last = medians[counts[-1]]["two locals"]
    growth = last / first if first else float("inf")
    print(f"  two-LOCAL growth {counts[0]} -> {counts[-1]}: x{growth:.2f}")
    if growth <= max_growth:
        print(f"  PASS: LOCAL cost is flat (within x{max_growth:.1f}) in globals")
        return 0
    print(f"  FAIL: LOCAL cost grows with the global count (limit x{max_growth:.1f})")
    return 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reps", type=int, default=5, help="runs to median (default 5)")
    parser.add_argument(
        "--no-build", action="store_true", help="use native/mmbasic as-is"
    )
    parser.add_argument(
        "--mode",
        choices=("local-tcache", "globals"),
        default="local-tcache",
        help="benchmark to run (default local-tcache)",
    )
    parser.add_argument(
        "--globals",
        default="0,400,1200",
        help="comma-separated global counts for --mode globals",
    )
    parser.add_argument(
        "--max-growth",
        type=float,
        default=3.0,
        help="largest allowed two-LOCAL growth ratio in --mode globals",
    )
    args = parser.parse_args()

    if not args.no_build:
        build_native()
    if not os.path.isfile(NATIVE):
        raise SystemExit(f"{NATIVE} is missing; run scripts/build-native.sh")

    if args.mode == "globals":
        counts = [int(n) for n in args.globals.split(",") if n.strip()]
        if not counts:
            raise SystemExit("--globals needs at least one count")
        return bench_globals(args.reps, counts, args.max_growth)
    return bench_local_tcache(args.reps)


if __name__ == "__main__":
    sys.exit(main())
