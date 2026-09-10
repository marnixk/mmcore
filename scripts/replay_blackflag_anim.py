"""Feed blackflag animation into QEMU TERM replay and screenshot HDMI as it runs."""

from __future__ import annotations

import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tests"))

from harness import MMBasicConsole, TermReplay, load_termlog, rx_records
from test_term import _quit
from test_term_replay import _open_replay

ART = Path("/opt/cursor/artifacts")
LOG = Path(__file__).resolve().parents[1] / "tests" / "term" / "blackflag-log-v2"
KERNEL = Path(__file__).resolve().parents[1] / "console" / "kernel8.img"


def main() -> int:
    ART.mkdir(parents=True, exist_ok=True)
    recs = rx_records(load_termlog(LOG.read_text()))
    recs = [r for r in recs if (r.in_n or 0) <= 15952]
    con = MMBasicConsole(str(KERNEL))
    con.start()
    replay = TermReplay(con, "127.0.0.1", 1)
    stop = threading.Event()
    shots: list[str] = []

    def snap_loop() -> None:
        n = 0
        while not stop.is_set():
            dest = ART / f"qemu_anim_live_{n:02d}.png"
            try:
                con.capture_png(str(dest))
                shots.append(str(dest))
                print(f"SHOT {dest.name}", flush=True)
            except Exception as exc:
                print(f"SHOT fail {exc}", flush=True)
            n += 1
            stop.wait(4.0)

    try:
        _open_replay(con, replay, connect=False)
        th = threading.Thread(target=snap_loop, daemon=True)
        th.start()
        t0 = time.time()
        for i, rec in enumerate(recs):
            t1 = time.time()
            replay._to_guest(rec.data)
            extra = b""
            for _ in range(6):
                extra += replay.pump_once(recv_tcp=False)
            dt = time.time() - t1
            print(
                f"R {i:02d} in_n={rec.in_n} len={len(rec.data)} {dt:.2f}s "
                f"extra={len(extra)} elapsed={time.time()-t0:.1f}s",
                flush=True,
            )
            if rec.in_n in (12751, 14197, 15952):
                name = {
                    12751: "qemu_black_b_animating.png",
                    14197: "qemu_flag_f_from_right.png",
                    15952: "qemu_black_flag_complete.png",
                }[rec.in_n]
                for _ in range(15):
                    extra += replay.pump_once(recv_tcp=False)
                    time.sleep(0.02)
                con.capture_png(str(ART / name))
                print(f"MILESTONE {name}", flush=True)
        stop.set()
        th.join(timeout=8)
        _quit(con)
    finally:
        stop.set()
        replay.stop()
        con.stop()
    print("shots", len(shots))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
