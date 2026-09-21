"""Bitmap font descriptions under ramdisk/fonts (FontDescription JSON)."""

import json
import struct
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
FONTS = REPO / "ramdisk" / "fonts"

KEYS = {
    "source",
    "charset",
    "offsetX",
    "offsetY",
    "charWidth",
    "charHeight",
    "charsPerRow",
    "bgColour",
}


def _descriptions():
    return sorted(FONTS.glob("*.json"))


def _png_size(path):
    data = path.read_bytes()[:33]
    assert data[:8] == b"\x89PNG\r\n\x1a\n", path
    return struct.unpack(">II", data[16:24])


def test_every_font_json_parses_and_fits_its_sheet():
    descs = _descriptions()
    assert descs, "no font descriptions found"
    for path in descs:
        obj = json.loads(path.read_text(encoding="utf-8"))
        assert set(obj) == KEYS, path
        for key in ("offsetX", "offsetY", "charWidth", "charHeight", "charsPerRow", "bgColour"):
            assert isinstance(obj[key], int), (path, key)
        assert obj["charWidth"] > 0 and obj["charHeight"] > 0
        assert obj["charsPerRow"] > 0
        assert obj["charset"], path
        assert len(obj["charset"]) <= 1024
        assert "\n" not in obj["charset"]
        assert 0 <= obj["bgColour"] <= 0xFFFFFF
        sheet = FONTS / obj["source"]
        assert sheet.is_file(), sheet
        w, h = _png_size(sheet)
        rows = (len(obj["charset"]) + obj["charsPerRow"] - 1) // obj["charsPerRow"]
        assert obj["offsetX"] + obj["charsPerRow"] * obj["charWidth"] <= w, path
        assert obj["offsetY"] + rows * obj["charHeight"] <= h, path


def test_font_json_is_one_compact_line():
    for path in _descriptions():
        text = path.read_text(encoding="utf-8")
        assert text.count("\n") == 1 and text.endswith("\n"), path


def test_fonts_include_declares_type_and_loader():
    inc = (FONTS / "fonts.inc").read_text(encoding="utf-8")
    assert "TYPE FontDescription" in inc
    assert "source AS STRING" in inc
    assert "bgColour AS INTEGER" in inc
    assert "JSON_PARSE" in inc
    assert 'A:/fonts/' in inc


def _write_bas(console, path, lines):
    assert console.send_line(f'OPEN "{path}" FOR OUTPUT AS #1') == ""
    for line in lines:
        esc = line.replace('"', '""')
        assert console.send_line(f'PRINT #1, "{esc}"') == ""
    assert console.send_line("CLOSE #1") == ""


def test_font_load_reads_json_into_type(console):
    _write_bas(
        console,
        "FONT.BAS",
        [
            '#INCLUDE "A:/fonts/fonts.inc"',
            "DIM f AS FontDescription",
            'f = fontLoad("08X08-F1")',
            "PRINT f.source",
            "PRINT f.charWidth",
            "PRINT f.charHeight",
            "PRINT f.charsPerRow",
            "PRINT f.offsetX",
            "PRINT f.offsetY",
            "PRINT f.bgColour",
            "PRINT LEN(f.charset)",
        ],
    )
    out = console.send_line('RUN "FONT.BAS"')
    lines = [ln.strip() for ln in out.replace("\r", "\n").split("\n") if ln.strip()]
    assert lines == ["08X08-F1.png", "8", "8", "40", "0", "0", "0", "59"], out
