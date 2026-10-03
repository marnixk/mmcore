#!/usr/bin/env python3
"""Regenerate the golden framebuffer image for the graphics scene test.

Usage: .venv/bin/python scripts/gen_golden.py
"""
import os
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, REPO_ROOT)

from harness import MMBasicConsole
from tests.test_graphics import SCENE, GOLDEN_DIR

KERNEL = os.path.join(REPO_ROOT, "console", "kernel8.img")


def main() -> None:
    os.makedirs(GOLDEN_DIR, exist_ok=True)
    con = MMBasicConsole(KERNEL)
    con.start()
    try:
        for cmd in SCENE:
            con.send_line(cmd)
        golden = os.path.join(GOLDEN_DIR, "scene.png")
        written = con.capture_png(golden, strict=True)
        if os.path.realpath(written) != os.path.realpath(golden):
            raise SystemExit(
                f"capture_png wrote {written!r}, not the requested {golden!r}; "
                "refusing to report success (#906)"
            )
        print("wrote", golden)
    finally:
        con.stop()


if __name__ == "__main__":
    main()
