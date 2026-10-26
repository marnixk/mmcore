#!/usr/bin/env python3
"""Benchmark the #1385 trace-cache variable lookup fix on the native build.

``tc_find_var()`` used to resolve a name with two linear scans over the whole
variable table on every (re)compile, so compiling one statement cost
O(number of globals).  It now goes through the #1303 name hash, so the cost is
flat as the table grows.

The program below forces a genuine recompile of one statement per iteration:
a SUB declares ``LOCAL G1``, shadowing a global of the same name.  Entering and
leaving the LOCAL rebinds that slot, so the ``G1 = G1 + 1`` body's cached entry
is stale on every call and has to be compiled again.  The LOCAL save/restore
itself is O(1) after #1328, so the measurement isolates the compile cost.

Usage:
    .venv/bin/python scripts/bench-tc-findvar.py [--iters N] [--reps N]
                                                 [--globals 0,400,1200]
                                                 [--no-build]

A timing assertion is deliberately not part of pytest: host timing is too
noisy for CI.  This script is the reproducible before/after check.
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
COMPILE_RE = re.compile(r"us/compile\s+([0-9.]+)")
COUNT_RE = re.compile(r"tcache=\d+/(\d+)/\d+")


def make_program(iters: int, nglobals: int) -> str:
    lines = [
        "OPTION TRACECACHE ON",
        "OPTION DEFAULT INTEGER",
        "OPTION PROFILING ON",
        "DIM I, T0",
    ]
    names = [f"G{j}" for j in range(1, nglobals + 1)] + ["ZZ"]
    for i in range(0, len(names), 8):
        lines.append("DIM " + ", ".join(names[i : i + 8]))
    lines += [
        "SUB F",
        "  LOCAL ZZ",
        "  ZZ = ZZ + 1",
        "END SUB",
        "T0 = TIMER",
        f"FOR I = 1 TO {iters}",
        "  F",
        "NEXT I",
        'PRINT "us/compile "; (TIMER - T0) / ' + str(iters) + " * 1000",
        'PRINT "check=";ZZ',
    ]
    return "\n".join(lines) + "\n"


def build_native() -> None:
    subprocess.run(
        [os.path.join(REPO, "scripts", "build-native.sh")],
        cwd=REPO,
        check=True,
        stdout=subprocess.DEVNULL,
    )


def run_once(iters: int, nglobals: int) -> tuple[float, int, str]:
    with tempfile.TemporaryDirectory(prefix="mmb-tcfv-") as drive:
        with open(os.path.join(drive, "BENCH.BAS"), "w", encoding="ascii") as fh:
            fh.write(make_program(iters, nglobals))
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
    m = COMPILE_RE.search(out)
    if not m:
        raise SystemExit(f"benchmark output missing us/compile:\n{out}")
    count = COUNT_RE.search(out)
    check = next(
        (ln.strip() for ln in out.splitlines() if ln.strip().startswith("check=")),
        "",
    )
    return float(m.group(1)), int(count.group(1)) if count else 0, check


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--iters", type=int, default=20000, help="recompiles per run")
    parser.add_argument("--reps", type=int, default=5, help="runs to median (default 5)")
    parser.add_argument(
        "--globals",
        default="0,400,1200",
        help="comma-separated global counts (default 0,400,1200)",
    )
    parser.add_argument(
        "--no-build", action="store_true", help="use native/mmbasic as-is"
    )
    args = parser.parse_args()

    if not args.no_build:
        build_native()
    if not os.path.isfile(NATIVE):
        raise SystemExit(f"{NATIVE} is missing; run scripts/build-native.sh")

    counts = [int(n) for n in args.globals.split(",") if n.strip()]
    if not counts:
        raise SystemExit("--globals needs at least one count")

    print(f"#1385 tc_find_var compile benchmark ({max(1, args.reps)} runs, medians)")
    print(f"  {'globals':>7}  {'us/compile':>12}  {'compiles':>9}  check")
    medians: dict[int, float] = {}
    for n in counts:
        runs = [run_once(args.iters, n) for _ in range(max(1, args.reps))]
        medians[n] = statistics.median(r[0] for r in runs)
        count = statistics.median(r[1] for r in runs)
        check = runs[0][2]
        print(f"  {n:>7}  {medians[n]:>10.3f}  {count:>9.0f}  {check}")
    first = medians[counts[0]]
    last = medians[counts[-1]]
    growth = last / first if first else float("inf")
    print(f"  compile growth {counts[0]} -> {counts[-1]}: x{growth:.2f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
