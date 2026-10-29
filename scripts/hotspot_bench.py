#!/usr/bin/env python3
"""Run the isolated hotspot microbenchmarks and print ns/exec.

Interpreter cases use native/mmbasic (no SDL harness). Graphics cases use
native/mmcore under SDL_VIDEODRIVER=dummy. MMB_STMTPROF is left unset: TIMER
deltas are the numbers in the write-up. The empty FOR/NEXT of the same trip
count is subtracted.
"""
from __future__ import annotations

import os
import shutil
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
BENCH = os.path.join(ROOT, "bench", "hotspots")
MMBASIC = os.path.join(ROOT, "native", "mmbasic")
MMCORE = os.path.join(ROOT, "native", "mmcore")


def ensure_benches() -> None:
    os.makedirs(BENCH, exist_ok=True)
    # The .bas files are the committed sources. This only refuses to run
    # without them.
    for name in ("interp.bas", "interp_few.bas", "gfx.bas"):
        path = os.path.join(BENCH, name)
        if not os.path.isfile(path):
            raise SystemExit("missing " + path)


def run_bas(binary: str, src_name: str, work: str, sdl: bool, timeout: int) -> str:
    case = os.path.join(work, src_name.replace(".bas", ""))
    if os.path.isdir(case):
        shutil.rmtree(case)
    drv = os.path.join(case, "drv")
    os.makedirs(drv)
    shutil.copy(os.path.join(BENCH, src_name), os.path.join(drv, src_name))
    croot = os.path.join(case, "croot")
    os.makedirs(croot)
    env = os.environ.copy()
    env["MMB_DRIVE_ROOT"] = croot
    env.pop("MMB_STMTPROF", None)
    cmd = [binary, "--drive", drv, 'RUN "D:/%s"' % src_name]
    if sdl:
        env["SDL_VIDEODRIVER"] = "dummy"
        env["SDL_AUDIODRIVER"] = "dummy"
        env["MMB_SDL_SERIAL"] = "1"
    print("bench", src_name, flush=True)
    proc = subprocess.run(
        cmd,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        env=env,
        timeout=timeout,
    )
    log = os.path.join(case, "out.txt")
    with open(log, "w", encoding="utf-8") as fh:
        fh.write(proc.stdout)
    print(proc.stdout)
    if proc.returncode != 0:
        print("rc", proc.returncode, file=sys.stderr)
    return proc.stdout


def parse(text: str):
    rows = []
    for line in text.splitlines():
        if not line.startswith("BENCH,"):
            continue
        parts = [p.strip() for p in line.split(",")]
        if len(parts) < 4:
            continue
        rows.append((parts[1], int(parts[2]), int(float(parts[3]))))
    return rows


def report(text: str) -> None:
    rows = parse(text)
    empty = {}
    for name, n, ms in rows:
        if name == "empty_for":
            empty[n] = ms
    print("--- ns/exec (empty FOR subtracted) ---")
    print("name,n,ms,empty_ms,ns")
    for name, n, ms in rows:
        base = empty.get(n, 0)
        delta = ms - base
        ns = (delta * 1_000_000) / n if n else 0
        print("%s,%d,%d,%d,%.1f" % (name, n, ms, base, ns))


def main() -> int:
    work = os.environ.get("HOTSPOT_WORK", "/tmp/jp-hotspot-bench")
    os.makedirs(work, exist_ok=True)
    ensure_benches()
    if not os.path.isfile(MMBASIC):
        print("missing", MMBASIC, file=sys.stderr)
        return 1
    out = run_bas(MMBASIC, "interp_few.bas", work, False, 120)
    report(out)
    out = run_bas(MMBASIC, "interp.bas", work, False, 180)
    report(out)
    if os.path.isfile(MMCORE):
        out = run_bas(MMCORE, "gfx.bas", work, True, 120)
        report(out)
    else:
        print("mmcore missing, skipped gfx")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
