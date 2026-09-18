#!/usr/bin/env python3
"""Run the pytest suite with visible, minute-by-minute progress.

Subcommands
-----------
  start [-- pytest args...]   launch the run detached, write log + pid
  status [--sleep N]          print one progress snapshot (after N seconds)
  wait [--interval N]         heartbeat until the run finishes, then summary
  stop                        terminate a detached run

State files (repo root): .pytest-watch.log, .pytest-watch.pid, .pytest-watch.started

``status`` is cheap and safe to call repeatedly, so a caller can report every
minute without the run blocking on a single long-lived process.
"""
from __future__ import annotations

import argparse
import os
import re
import signal
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOG = os.path.join(ROOT, ".pytest-watch.log")
PID = os.path.join(ROOT, ".pytest-watch.pid")
STAMP = os.path.join(ROOT, ".pytest-watch.started")

DEFAULT_ARGS = [
    "-v",
    "--tb=line",
    "-p",
    "no:cacheprovider",
    "-n",
    "6",
    "--dist",
    "loadscope",
]


def _alive(pid: int) -> bool:
    if pid <= 0:
        return False
    try:
        os.kill(pid, 0)
    except OSError:
        return False
    return True


def _pid() -> int:
    try:
        with open(PID) as fh:
            return int(fh.read().strip())
    except (OSError, ValueError):
        return 0


def _elapsed() -> float:
    try:
        with open(STAMP) as fh:
            return time.time() - float(fh.read().strip())
    except (OSError, ValueError):
        return 0.0


def _fmt(sec: float) -> str:
    sec = int(sec)
    return f"{sec // 60:02d}:{sec % 60:02d}"


def snapshot():
    """Parse the log into (total, passed, failed, skipped, last, summary)."""
    total = None
    passed = failed = skipped = 0
    last = ""
    summary: list[str] = []
    try:
        with open(LOG, "r", errors="replace") as fh:
            for raw in fh:
                line = raw.rstrip("\n")
                if total is None:
                    m = re.search(r"collected (\d+) items", line)
                    if not m:
                        m = re.search(r"\[(\d+) items\]", line)
                    if m:
                        total = int(m.group(1))
                tid = re.search(r"(tests/\S+?)(?:::\S+)?(?:\s|$)", line)
                if " PASSED" in line:
                    passed += 1
                    if tid:
                        last = tid.group(1)
                elif " FAILED" in line:
                    failed += 1
                    if tid:
                        last = tid.group(1)
                    if line.startswith(("FAILED", "ERROR")):
                        summary.append(line)
                elif " ERROR" in line:
                    failed += 1
                    if tid:
                        last = tid.group(1)
                    if line.startswith(("FAILED", "ERROR")):
                        summary.append(line)
                elif " SKIPPED" in line:
                    skipped += 1
                if re.match(r"^=+ .*(passed|failed|error).* =+$", line):
                    summary.append(line.strip("= "))
    except FileNotFoundError:
        pass
    return total, passed, failed, skipped, last, summary


def line(prefix: str = "") -> str:
    total, passed, failed, skipped, last, _ = snapshot()
    done = passed + failed + skipped
    tot = f"/{total}" if total else ""
    pct = f"{100 * done // total}%" if total else "?"
    return (
        f"{prefix}[{_fmt(_elapsed())}] {done}{tot} ({pct}) "
        f"pass={passed} fail={failed} skip={skipped} last={last or '-'}"
    )


def cmd_start(args: argparse.Namespace) -> int:
    if _alive(_pid()):
        print(f"already running pid={_pid()}", flush=True)
        return 0
    pytest_args = args.pytest_args
    if pytest_args and pytest_args[0] == "--":
        pytest_args = pytest_args[1:]
    if not pytest_args:
        pytest_args = DEFAULT_ARGS
    py = os.environ.get("PY", os.path.join(ROOT, ".venv", "bin", "python"))
    cmd = [py, "-m", "pytest", *pytest_args]
    with open(LOG, "w") as fh:
        fh.write(f"# {' '.join(cmd)}\n")
    log = open(LOG, "a")
    proc = subprocess.Popen(
        cmd,
        stdout=log,
        stderr=subprocess.STDOUT,
        cwd=ROOT,
        start_new_session=True,
    )
    with open(PID, "w") as fh:
        fh.write(str(proc.pid))
    with open(STAMP, "w") as fh:
        fh.write(str(time.time()))
    print(f"started pid={proc.pid} log={LOG}", flush=True)
    return 0


def cmd_status(args: argparse.Namespace) -> int:
    if args.sleep:
        time.sleep(args.sleep)
    running = _alive(_pid())
    print(line(prefix="RUNNING " if running else "DONE "), flush=True)
    if not running:
        _, _, _, _, _, summary = snapshot()
        for s in summary[-40:]:
            print(s, flush=True)
    return 0


def cmd_wait(args: argparse.Namespace) -> int:
    first = True
    while _alive(_pid()):
        time.sleep(2 if first else args.interval)
        first = False
        if _alive(_pid()):
            print(line(), flush=True)
    print(line(), flush=True)
    _, _, _, _, _, summary = snapshot()
    print("---- failures ----", flush=True)
    for s in summary[-40:]:
        print(s, flush=True)
    return 0


def cmd_stop(_args: argparse.Namespace) -> int:
    pid = _pid()
    if _alive(pid):
        try:
            os.killpg(os.getpgid(pid), signal.SIGTERM)
        except OSError:
            pass
        time.sleep(1)
    print(f"stopped pid={pid}", flush=True)
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("start")
    p.add_argument("pytest_args", nargs=argparse.REMAINDER)
    p.set_defaults(func=cmd_start)
    p = sub.add_parser("status")
    p.add_argument("--sleep", type=float, default=0.0)
    p.set_defaults(func=cmd_status)
    p = sub.add_parser("wait")
    p.add_argument("--interval", type=float, default=60.0)
    p.set_defaults(func=cmd_wait)
    p = sub.add_parser("stop")
    p.set_defaults(func=cmd_stop)
    ns = ap.parse_args()
    return ns.func(ns)


if __name__ == "__main__":
    sys.exit(main())
