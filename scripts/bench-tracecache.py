#!/usr/bin/env python3
"""Benchmark the #1369 trace-cache capacity/growth fix on the native build.

Generates a game-shaped program: 280 distinct cacheable setup statements (more
than the old fixed table held) plus 100 distinct statements that cannot be
compiled, then a hot loop that is what a per-frame actor update looks like.

Runs it with ``OPTION TRACECACHE ON`` and ``OFF`` and prints the loop time,
speedup and cache counters. On the pre-#1369 256-slot table the ON run reports
0 hits and is no faster than OFF; with growth it replays the hot statements.

Usage:
    .venv/bin/python scripts/bench-tracecache.py [--iters N] [--reps N]
                                                [--no-build]

A timing assertion is deliberately not part of pytest: host timing is too
noisy for CI. This script is the reproducible check.
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

SETUP_COMPILED = 280
SETUP_FAILED = 100
HOT_PER_PASS = 3


def make_program(iters: int, tracecache: bool) -> str:
    lines = [
        f"OPTION TRACECACHE {'ON' if tracecache else 'OFF'}",
        "OPTION PROFILING ON",
        "DIM A(8)",
        "X=0",
        "T=0",
        "U=0",
        "Z=0",
    ]
    lines += [f"X={i}" for i in range(1, SETUP_COMPILED + 1)]
    lines += [f"X=ABS(-{i})" for i in range(1, SETUP_FAILED + 1)]
    lines += [
        "T0=TIMER",
        f"FOR J=1 TO {iters}",
        "FOR K=0 TO 4",
        "A(K)=A(K)+1",
        "T=T+A(K)",
        "IF T>0 THEN U=U+1",
        "IF T>0 THEN Z=ABS(-T)",
        "NEXT K",
        "NEXT J",
        'PRINT "LOOPMS=";TIMER-T0',
        'PRINT "RESULT=";A(0);A(3);T;U',
    ]
    return "\n".join(lines) + "\n"


def build_native() -> None:
    subprocess.run(
        [os.path.join(REPO, "scripts", "build-native.sh")],
        cwd=REPO,
        check=True,
        stdout=subprocess.DEVNULL,
    )


def run_once(iters: int, tracecache: bool) -> dict[str, float]:
    with tempfile.TemporaryDirectory(prefix="mmb-tcbench-") as drive:
        path = os.path.join(drive, "BENCH.BAS")
        with open(path, "w", encoding="ascii") as fh:
            fh.write(make_program(iters, tracecache))
        env = dict(os.environ)
        env.setdefault("SDL_VIDEODRIVER", "dummy")
        proc = subprocess.run(
            [NATIVE, "--drive", drive, 'RUN "D:/BENCH.BAS"'],
            cwd=REPO,
            env=env,
            capture_output=True,
            text=True,
            timeout=300,
        )
    out = proc.stdout
    if proc.returncode != 0:
        raise SystemExit(f"native run failed:\n{out}\n{proc.stderr}")

    loop_match = re.search(r"LOOPMS=([0-9.]+)", out)
    if not loop_match:
        raise SystemExit(f"benchmark output missing LOOPMS:\n{out}")
    result = {"loop": float(loop_match.group(1))}
    counts = re.search(r"tcache=(\d+)/(\d+)/(\d+)", out)
    if counts:
        result.update(
            hit=int(counts.group(1)),
            comp=int(counts.group(2)),
            bad=int(counts.group(3)),
        )
    else:
        result.update(hit=0, comp=0, bad=0)
    runs = 5 * iters
    if result["hit"] < HOT_PER_PASS * runs // 2:
        result["hits_expected"] = HOT_PER_PASS * runs
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--iters", type=int, default=20000, help="outer loop passes")
    parser.add_argument("--reps", type=int, default=3, help="runs to median (default 3)")
    parser.add_argument(
        "--no-build", action="store_true", help="use native/mmbasic as-is"
    )
    args = parser.parse_args()

    if not args.no_build:
        build_native()
    if not os.path.isfile(NATIVE):
        raise SystemExit(f"{NATIVE} is missing; run scripts/build-native.sh")

    on_runs = [run_once(args.iters, True) for _ in range(max(1, args.reps))]
    off_runs = [run_once(args.iters, False) for _ in range(max(1, args.reps))]
    on = {k: statistics.median(r[k] for r in on_runs) for k in ("loop", "hit", "comp", "bad")}
    off_loop = statistics.median(r["loop"] for r in off_runs)

    print(f"#1369 trace-cache capacity benchmark ({len(on_runs)} runs, medians)")
    print(
        f"  tracecache ON : {on['loop']:.1f} ms loop, "
        f"hits={on['hit']:.0f} comp={on['comp']:.0f} bad={on['bad']:.0f}"
    )
    print(f"  tracecache OFF: {off_loop:.1f} ms loop")
    if on["loop"] > 0:
        print(f"  speedup       : {off_loop / on['loop']:.2f}x")
    expected = HOT_PER_PASS * 5 * args.iters
    if on["hit"] < expected // 2:
        print(
            f"  WARNING: only {on['hit']:.0f} hits; expected about {expected} "
            "(hot statements are not being replayed)"
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
