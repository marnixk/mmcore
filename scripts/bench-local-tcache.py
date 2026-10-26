#!/usr/bin/env python3
"""Benchmark the #1368 LOCAL trace-cache fix on the native interpreter.

Runs the issue's repro three ways and prints us/iteration for each:

  plain   8 cached LETs + a call to a SUB without LOCAL
  local   the same call, but the SUB declares LOCAL Z
  off     same as plain with OPTION TRACECACHE OFF

The fix is good when ``local`` is not slower than ``off`` (and close to
``plain``).  A timing assertion is deliberately not part of pytest: QEMU
timing is too noisy for CI, so this script is the reproducible check.

Usage:
    .venv/bin/python scripts/bench-local-tcache.py [--reps N] [--no-build]

Exit status is nonzero when the median ``local`` time is slower than the
median ``off`` time.
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


def run_once() -> dict[str, float]:
    with tempfile.TemporaryDirectory(prefix="mmb-bench-") as drive:
        with open(os.path.join(drive, "BENCH.BAS"), "w", encoding="ascii") as fh:
            fh.write(PROGRAM)
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
    found = {name: float(value) for name, value in LINE_RE.findall(out)}
    missing = {"plain", "local", "off"} - found.keys()
    if missing:
        raise SystemExit(f"benchmark output missing {sorted(missing)}:\n{out}")
    return found


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reps", type=int, default=5, help="runs to median (default 5)")
    parser.add_argument(
        "--no-build", action="store_true", help="use native/mmbasic as-is"
    )
    args = parser.parse_args()

    if not args.no_build:
        build_native()
    if not os.path.isfile(NATIVE):
        raise SystemExit(f"{NATIVE} is missing; run scripts/build-native.sh")

    runs = [run_once() for _ in range(max(1, args.reps))]
    medians = {k: statistics.median(r[k] for r in runs) for k in ("plain", "local", "off")}

    print(f"#1368 LOCAL trace-cache benchmark ({len(runs)} runs, medians)")
    print(f"  plain (SUB without LOCAL) : {medians['plain']:.2f} us/iter")
    print(f"  local (SUB with LOCAL Z)  : {medians['local']:.2f} us/iter")
    print(f"  off   (OPTION TRACECACHE OFF): {medians['off']:.2f} us/iter")
    if medians["local"] <= medians["off"]:
        print("  PASS: local is no slower than the TRACECACHE OFF path")
        return 0
    print("  FAIL: local is slower than the TRACECACHE OFF path")
    return 1


if __name__ == "__main__":
    sys.exit(main())
