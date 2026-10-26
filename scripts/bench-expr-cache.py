#!/usr/bin/env python3
"""Benchmark the #1370 trace-cache expression/branch coverage.

Measures us per loop statement for the constructs the cache used to reject:

  idiv      x = i \\ 3
  mod       x = i MOD 7
  and/or    IF i > 0 AND i < 999999 THEN x = 1
  not       IF NOT (i = 0) THEN x = 1
  arr1      x = a(i mod 50)            (expression subscript, read)
  arr1w     a(i mod 50) = i            (expression subscript, write)
  arr2      x = b(i mod 8, (i+1) mod 8)
  arr2w     b(i mod 8, (i+1) mod 8) = i
  goto      IF i > 0 THEN GOTO L1      (label target)
  gosub     IF i > 0 THEN GOSUB S1     (label target)
  blockif   IF i > 0 THEN ... ENDIF    (already skipped by #1333)

Every case also runs with ``OPTION TRACECACHE OFF``.  Pass
``--baseline-binary`` with a pre-change ``native/mmbasic`` to print the
before/after comparison directly; without it, ``before`` is left blank and the
ON/OFF gain is still shown.  Host timing is too noisy for a pytest assertion,
so this script is the reproducible check.

Usage:
    .venv/bin/python scripts/bench-expr-cache.py [--iters N] [--reps N]
                                                [--baseline-binary PATH]
                                                [--no-build]
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

CASES = {
    "let": ["X = I + 1"],
    "idiv": ["X = I \\ 3"],
    "mod": ["X = I MOD 7"],
    "and": ["IF I > 0 AND I < 999999 THEN X = 1"],
    "or": ["IF I < 0 OR I > 0 THEN X = 1"],
    "not": ["IF NOT (I = 0) THEN X = 1"],
    "arr1": ["X = A(I MOD 50)"],
    "arr1w": ["A(I MOD 50) = I"],
    "arr2": ["X = B(I MOD 8, (I+1) MOD 8)"],
    "arr2w": ["B(I MOD 8, (I+1) MOD 8) = I"],
    "goto": ["IF I > 0 THEN GOTO L1", "L1: X = 1"],
    "gosub": ["IF I > 0 THEN GOSUB S1"],
    "blockif": ["IF I > 0 THEN", "X = 1", "ENDIF"],
}

PERF_RE = re.compile(r"tcache=(\d+)/(\d+)/(\d+)")
CASE_RE = re.compile(r"CASE=([0-9.]+)")
EMPTY_RE = re.compile(r"EMPTY=([0-9.]+)")


def make_program(case: str, stmts: list[str], iters: int, tracecache: bool) -> str:
    lines = [
        f"OPTION TRACECACHE {'ON' if tracecache else 'OFF'}",
        "OPTION PROFILING ON",
        "DIM A(60), B(10,10), X, I, J, Z",
        "Z = 0",
        "T0 = TIMER",
        f"FOR J = 1 TO {iters}",
        "NEXT J",
        'PRINT "EMPTY=";TIMER-T0',
        "T0 = TIMER",
        f"FOR J = 1 TO {iters}",
        *[f"  {s}" for s in stmts],
        "NEXT J",
        'PRINT "CASE=";TIMER-T0',
        'PRINT "RES=";X;Z',
        "END",
        "S1: Z = Z + 1",
        "RETURN",
    ]
    return "\n".join(lines) + "\n"


def build_native() -> None:
    subprocess.run(
        [os.path.join(REPO, "scripts", "build-native.sh")],
        cwd=REPO,
        check=True,
        stdout=subprocess.DEVNULL,
    )


def run_case(
    binary: str, case: str, stmts: list[str], iters: int, tracecache: bool
) -> dict[str, float]:
    with tempfile.TemporaryDirectory(prefix="mmb-exprbench-") as drive:
        path = os.path.join(drive, "BENCH.BAS")
        with open(path, "w", encoding="ascii") as fh:
            fh.write(make_program(case, stmts, iters, tracecache))
        env = dict(os.environ)
        env.setdefault("SDL_VIDEODRIVER", "dummy")
        proc = subprocess.run(
            [binary, "--drive", drive, 'RUN "D:/BENCH.BAS"'],
            cwd=REPO,
            env=env,
            capture_output=True,
            text=True,
            timeout=300,
        )
    out = proc.stdout
    if proc.returncode != 0:
        raise SystemExit(f"native run failed for {case}:\n{out}\n{proc.stderr}")
    empty = EMPTY_RE.search(out)
    case_ms = CASE_RE.search(out)
    if not empty or not case_ms:
        raise SystemExit(f"benchmark output missing timings for {case}:\n{out}")
    us = (float(case_ms.group(1)) - float(empty.group(1))) * 1000.0 / iters
    counts = PERF_RE.search(out)
    hit = int(counts.group(1)) if counts else 0
    bad = int(counts.group(3)) if counts else 0
    return {"us": us, "hit": float(hit), "bad": float(bad)}


def median_case(binary, case, stmts, iters, tracecache, reps):
    runs = [run_case(binary, case, stmts, iters, tracecache) for _ in range(reps)]
    return (
        statistics.median(r["us"] for r in runs),
        statistics.median(r["hit"] for r in runs),
        statistics.median(r["bad"] for r in runs),
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--iters", type=int, default=20000, help="loop passes")
    parser.add_argument("--reps", type=int, default=3, help="runs per median")
    parser.add_argument(
        "--baseline-binary",
        help="pre-change native/mmbasic to measure as the 'before' column",
    )
    parser.add_argument(
        "--no-build", action="store_true", help="use native/mmbasic as-is"
    )
    args = parser.parse_args()

    if not args.no_build:
        build_native()
    if not os.path.isfile(NATIVE):
        raise SystemExit(f"{NATIVE} is missing; run scripts/build-native.sh")
    baseline = args.baseline_binary
    if baseline and not os.path.isfile(baseline):
        raise SystemExit(f"baseline binary is missing: {baseline}")

    reps = max(1, args.reps)
    print(
        f"#1370 trace-cache expression/branch benchmark "
        f"({args.iters} iterations, {reps} runs, medians, us/statement)"
    )
    if baseline:
        print(f"  before = {baseline}")
    print(f"  {'construct':10s} {'before':>8s} {'after':>8s} {'OFF':>8s}  hits")
    for case, stmts in CASES.items():
        before = float("nan")
        if baseline:
            before, _, _ = median_case(
                baseline, case, stmts, args.iters, True, reps
            )
        after, hits, bad = median_case(
            NATIVE, case, stmts, args.iters, True, reps
        )
        off, _, _ = median_case(NATIVE, case, stmts, args.iters, False, reps)
        before_s = f"{before:8.3f}" if baseline else f"{'-':>8s}"
        print(
            f"  {case:10s} {before_s} {after:8.3f} {off:8.3f}  "
            f"{hits:.0f}/{bad:.0f}"
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
