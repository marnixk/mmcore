#!/usr/bin/env python3
"""Headless Jetpack inner-loop profile (investigation only).

Copies games/jetpack from a checkout of mmcore-apps, caps the copy at 48
logic frames, and runs native/mmcore at MODE 20 (683x384) with MMB_STMTPROF
and MMCORE_FRAME_PROFILE. Does not modify the published game or the
interpreter's default path.

Scenarios: level 0 and level 37, player idle and player holding Right.
Right is forced on the drive copy (key.right = TRUE at the end of key.logic)
because the SDL harness key command is a press-and-release, not a hold.
"""
from __future__ import annotations

import argparse
import csv
import os
import shutil
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
MMCORE = os.environ.get("MMCORE", os.path.join(ROOT, "native", "mmcore"))
DEFAULT_APPS = os.environ.get("MMCORE_APPS", "/tmp/mmcore-apps")


def patch(text: str, old: str, new: str, label: str) -> str:
    if old not in text:
        raise SystemExit("patch miss: %s" % label)
    return text.replace(old, new, 1)


def write_hook(path: str, level: int) -> None:
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(
            "const TH_START = %d\n"
            "const TH_KILL = 0\n"
            "const TH_GEMS = 0\n"
            "const TH_PHASE = 0\n"
            "const TH_LIVES = 5\n"
            "const TH_FAST = 0\n" % level
        )


def prepare_game(apps: str, dst: str, level: int, hold_right: bool) -> None:
    game = os.path.join(apps, "games", "jetpack")
    if os.path.isdir(dst):
        shutil.rmtree(dst)
    shutil.copytree(game, dst, ignore=shutil.ignore_patterns("art"))
    write_hook(os.path.join(dst, "includes", "testhook.inc"), level)
    play = os.path.join(dst, "includes", "play.inc")
    with open(play, encoding="utf-8") as fh:
        src = fh.read()
    src = patch(
        src,
        "    gm.draw()\n",
        "    if gm.frm >= 48 then gm.res = GE_TITLE\n    gm.draw()\n",
        "play cap",
    )
    with open(play, "w", encoding="utf-8") as fh:
        fh.write(src)
    if hold_right:
        inp = os.path.join(dst, "includes", "input.inc")
        with open(inp, encoding="utf-8") as fh:
            src = fh.read()
        src = patch(
            src,
            "  key.wasQ = key_down\n",
            "  key.wasQ = key_down\n  key.right = TRUE\n",
            "hold right",
        )
        with open(inp, "w", encoding="utf-8") as fh:
            fh.write(src)
    main_bas = os.path.join(dst, "main.bas")
    with open(main_bas, encoding="utf-8") as fh:
        src = fh.read()
    src = patch(
        src,
        "on key 0\nscr.init()\nscr.loadAssets()\n",
        'on key 0\nprint "JPBOOT"\nmode 20, 16\nscr.init()\nscr.loadAssets()\nprint "JPASSETS"\n',
        "mode and boot marks",
    )
    src = patch(
        src,
        "  gm.run()\nloop\n",
        "  gm.run()\n"
        "  if gm.frm >= 48 then\n"
        '    print "JPRESULT"; gm.frm; " "; gm.res; " "; gm.level; " "; pl.dead; " "; mm.hres; " "; mm.vres; " "; lv.name$\n'
        "    exit do\n"
        "  end if\n"
        "loop\n",
        "main cap",
    )
    with open(main_bas, "w", encoding="utf-8") as fh:
        fh.write(src)


def run_one(mmcore: str, work: str, level: int, hold_right: bool, timeout: int) -> dict:
    tag = "L%d-%s" % (level, "right" if hold_right else "idle")
    case = os.path.join(work, tag)
    drv = os.path.join(case, "drv")
    game = os.path.join(drv, "g")
    croot = os.path.join(case, "croot")
    os.makedirs(croot, exist_ok=True)
    apps = os.environ.get("MMCORE_APPS", DEFAULT_APPS)
    prepare_game(apps, game, level, hold_right)
    stmt = os.path.join(case, "stmt.csv")
    log_path = os.path.join(case, "serial.log")
    env = os.environ.copy()
    env.update(
        {
            "SDL_VIDEODRIVER": "dummy",
            "SDL_AUDIODRIVER": "dummy",
            "MMB_DRIVE_ROOT": croot,
            "MMB_SDL_SERIAL": "1",
            "MMB_STMTPROF": stmt,
            "MMCORE_FRAME_PROFILE": "1",
        }
    )
    cmd = [mmcore, "--drive", drv, 'RUN "D:/g/main.bas"']
    print("run", tag, flush=True)
    with open(log_path, "w", encoding="utf-8") as fh:
        proc = subprocess.run(
            ["timeout", "-s", "KILL", str(timeout)] + cmd,
            stdin=subprocess.DEVNULL,
            stdout=fh,
            stderr=subprocess.STDOUT,
            env=env,
        )
    frame = os.path.join(croot, "C", ".mmcore-frame-profile.csv")
    if not os.path.isfile(frame):
        alt = os.path.join(drv, ".mmcore-frame-profile.csv")
        if os.path.isfile(alt):
            frame = alt
    log = open(log_path, encoding="utf-8", errors="replace").read()
    print(" rc", proc.returncode, "stmt", os.path.isfile(stmt), "frame", os.path.isfile(frame))
    for line in log.splitlines():
        if line.startswith("JP") or line.startswith("?") or "Error" in line or "error" in line:
            print(" ", line[:200])
    return {
        "tag": tag,
        "rc": proc.returncode,
        "stmt": stmt if os.path.isfile(stmt) else "",
        "frame": frame if os.path.isfile(frame) else "",
        "log": log_path,
    }


def load_stmt(path: str):
    header = []
    rows = []
    with open(path, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            if line.startswith("#") or line.startswith("kind,"):
                header.append(line.rstrip())
                continue
            if not line.strip():
                continue
            # stmt and source are quoted and may contain commas.
            row = next(csv.reader([line]))
            if len(row) < 11:
                continue
            rows.append(
                {
                    "kind": row[0],
                    "line": int(row[1]),
                    "pc": int(row[2]),
                    "count": int(row[3]),
                    "uncached": int(row[4]),
                    "raw_ns": int(row[5]),
                    "busy_ns": int(row[6]),
                    "raw_per": int(row[7]),
                    "busy_per": int(row[8]),
                    "stmt": row[9],
                    "source": row[10],
                }
            )
    return header, rows


def summarise(path: str, out_path: str, min_count: int = 24) -> None:
    header, rows = load_stmt(path)
    steady = [r for r in rows if r["count"] >= min_count and r["kind"] != "PAUSE"]
    steady.sort(key=lambda r: r["busy_ns"], reverse=True)
    pause = [r for r in rows if r["kind"] == "PAUSE"]
    oneshot = sorted(
        [r for r in rows if r["count"] <= 3 and r["kind"] != "BREAK"],
        key=lambda r: r["busy_ns"],
        reverse=True,
    )
    total_busy = sum(r["busy_ns"] for r in steady)
    lines = list(header)
    lines.append("# steady_busy_ns=%d min_count=%d" % (total_busy, min_count))
    lines.append(
        "rank,kind,count,uncached,busy_per_ns,busy_ns,pct,stmt"
    )
    for i, r in enumerate(steady[:40], 1):
        pct = (100.0 * r["busy_ns"] / total_busy) if total_busy else 0
        stmt = r["stmt"].replace("\n", " ")
        lines.append(
            "%d,%s,%d,%d,%d,%d,%.2f,%s"
            % (i, r["kind"], r["count"], r["uncached"], r["busy_per"], r["busy_ns"], pct, stmt)
        )
    lines.append("# oneshot")
    for r in oneshot[:12]:
        lines.append(
            "ONESHOT,%s,%d,%d,%s" % (r["kind"], r["count"], r["busy_ns"], r["stmt"][:120])
        )
    pbusy = sum(r["busy_ns"] for r in pause)
    praw = sum(r["raw_ns"] for r in pause)
    lines.append("# pause_rows=%d pause_raw_ns=%d pause_busy_ns=%d" % (len(pause), praw, pbusy))
    br = [r for r in rows if r["kind"] == "BREAK"]
    lines.append(
        "# break_rows=%d break_busy_ns=%d"
        % (len(br), sum(r["busy_ns"] for r in br))
    )
    text = "\n".join(lines) + "\n"
    with open(out_path, "w", encoding="utf-8") as fh:
        fh.write(text)
    print(text[:4000])


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--work", default="/tmp/jp-hotspot")
    ap.add_argument("--timeout", type=int, default=180)
    ap.add_argument("--only", default="")
    args = ap.parse_args()
    if not os.path.isfile(MMCORE):
        print("mmcore missing", MMCORE, file=sys.stderr)
        return 1
    os.makedirs(args.work, exist_ok=True)
    cases = [(0, False), (0, True), (37, False), (37, True)]
    if args.only:
        want = set(args.only.split(","))
        cases = [c for c in cases if ("L%d-%s" % (c[0], "right" if c[1] else "idle")) in want]
    for level, hold in cases:
        info = run_one(MMCORE, args.work, level, hold, args.timeout)
        if info["stmt"]:
            tag = info["tag"]
            summarise(info["stmt"], os.path.join(args.work, tag + ".summary.txt"))
            subs = info["stmt"][:-4] + ".subs.csv" if info["stmt"].endswith(".csv") else info["stmt"] + ".subs.csv"
            # sibling is stmt.csv -> stmt.subs.csv via sibling_path
            sib = info["stmt"].replace(".csv", ".subs.csv")
            if os.path.isfile(sib):
                shutil.copy(sib, os.path.join(args.work, tag + ".subs.csv"))
            bip = info["stmt"].replace(".csv", ".builtins.csv")
            if os.path.isfile(bip):
                shutil.copy(bip, os.path.join(args.work, tag + ".builtins.csv"))
            if info["frame"]:
                shutil.copy(info["frame"], os.path.join(args.work, tag + ".frame.csv"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
