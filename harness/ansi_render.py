"""Render an AnsiPane to PPM/PNG using the guest CP437 8x16 font."""

from __future__ import annotations

import re
import subprocess
from pathlib import Path

from .ansi_pane import PAL, AnsiPane

_FONT: bytes | None = None
_FONT_PATH = Path(__file__).resolve().parents[1] / "mmbasic" / "src" / "font_cp437_8x16.c"


def cp437_font() -> bytes:
    global _FONT
    if _FONT is not None:
        return _FONT
    text = _FONT_PATH.read_text()
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    nums = [int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", text)]
    if len(nums) < 256 * 16:
        raise RuntimeError(f"cp437 font parse failed: {len(nums)} bytes")
    _FONT = bytes(nums[: 256 * 16])
    return _FONT


def pane_to_ppm(pane: AnsiPane, path: str | Path) -> Path:
    font = cp437_font()
    cw, ch = 8, 16
    w, h = pane.cols * cw, pane.rows * ch
    path = Path(path)
    with path.open("wb") as f:
        f.write(f"P6\n{w} {h}\n255\n".encode("ascii"))
        for r in range(pane.rows):
            for py in range(ch):
                row = bytearray()
                for c in range(pane.cols):
                    chv = ord(pane.cell[r][c]) & 255
                    bits = font[chv * 16 + py]
                    fg = PAL[pane.fg[r][c] & 15]
                    bg = PAL[pane.bg[r][c] & 15]
                    for px in range(cw):
                        on = bits & (0x80 >> px)
                        col = fg if on else bg
                        row.extend(col)
                f.write(row)
    return path


def pane_to_png(pane: AnsiPane, path: str | Path) -> Path:
    path = Path(path)
    ppm = path.with_suffix(".ppm")
    pane_to_ppm(pane, ppm)
    subprocess.run(["convert", str(ppm), str(path)], check=True, capture_output=True)
    return path
