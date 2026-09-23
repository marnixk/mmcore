"""TheDraw .TDF font library on the ramdisk (ramdisk/lib/TDF.BAS, issue #557)."""

import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
LIB = REPO / "ramdisk" / "lib" / "TDF.BAS"
DEMO = REPO / "ramdisk" / "apps" / "TDFDEMO.BAS"
TDF_DIR = REPO / "ramdisk" / "fonts" / "tdf"

TYPE_BLOCK = 1
TYPE_OUTLINE = 0
TYPE_COLOR = 2


def _read_bas(console, path, lines):
    assert console.send_line(f'OPEN "{path}" FOR OUTPUT AS #1') == ""
    for line in lines:
        esc = line.replace('"', '""')
        assert console.send_line(f'PRINT #1, "{esc}"') == ""
    assert console.send_line("CLOSE #1") == ""


def _run(console, name, lines):
    _read_bas(console, name, lines)
    return console.send_line(f'RUN "{name}"')


def _lines(out):
    return [ln.strip() for ln in out.replace("\r", "\n").split("\n") if ln.strip()]


# --- host-only TDF reference parser (mirrors the documented format) --------

def parse_tdf(path):
    b = Path(path).read_bytes()
    assert b[0] == 0x13 and b[1:19] == b"TheDraw FONTS file" and b[19] == 0x1A
    nl = b[24]
    name = b[25 : 25 + nl].split(b"\x00")[0].decode("latin1")
    ftype, spacing = b[41], b[42]
    data = 233
    offsets = [b[45 + 2 * i] | (b[45 + 2 * i + 1] << 8) for i in range(94)]
    widths = [0] * 94
    defined = [0] * 94
    height = 0
    for i, off in enumerate(offsets):
        if off == 0xFFFF:
            continue
        defined[i] = 1
        widths[i] = b[data + off]
        height = max(height, b[data + off + 1])
    return {
        "name": name,
        "type": ftype,
        "spacing": spacing,
        "height": height,
        "widths": widths,
        "defined": defined,
    }


def expected_width(path, s):
    info = parse_tdf(path)
    total = 0
    for ch in s:
        c = ord(ch)
        if 33 <= c <= 126 and info["defined"][c - 33]:
            total += info["widths"][c - 33] + info["spacing"]
        else:
            total += info["spacing"] if info["spacing"] > 1 else 1
    return total


# --- host checks -----------------------------------------------------------

def test_tdf_lib_declares_api():
    src = LIB.read_text(encoding="utf-8")
    for needle in (
        "SUB TDF.Load",
        "SUB TDF.LoadNamed",
        "SUB TDF.Print",
        "SUB TDF.Close",
        "FUNCTION TDF.Width",
        "FUNCTION TDF.OutlineCode",
        "TDF.Name$",
        "TDF.Type%",
        "TDF.Spacing%",
        "TDF.Height",
        "A:/fonts",
    ):
        assert needle in src, needle


def test_tdf_demo_includes_library():
    src = DEMO.read_text(encoding="utf-8")
    assert '#INCLUDE "A:/lib/TDF.BAS"' in src
    assert "TDF.Load" in src
    assert "TDF.Print" in src


def test_ramdisk_generator_embeds_tdf_lib(tmp_path):
    out = tmp_path / "ramdisk_data.c"
    gen = REPO / "scripts" / "gen_ramdisk.py"
    res = subprocess.run(
        [
            sys.executable,
            str(gen),
            "--root",
            str(REPO / "ramdisk"),
            "--out",
            str(out),
            "--vfs-src",
            str(REPO / "mmbasic" / "src" / "vfs.c"),
        ],
        capture_output=True,
        text=True,
    )
    assert res.returncode == 0, res.stderr
    text = out.read_text()
    assert "lib/TDF.BAS" in text
    assert "apps/TDFDEMO.BAS" in text


# --- QEMU checks -----------------------------------------------------------

def test_tdf_block_font_metadata_and_width(console):
    out = _run(
        console,
        "TDFBLK.BAS",
        [
            '#INCLUDE "A:/lib/TDF.BAS"',
            'TDF.Load "A:/fonts/tdf/STANDARD.TDF"',
            "PRINT TDF.Name$",
            "PRINT TDF.Type%",
            "PRINT TDF.Spacing%",
            "PRINT TDF.Height",
            'PRINT TDF.Width("HELLO")',
            "TDF.Close",
        ],
    )
    info = parse_tdf(TDF_DIR / "STANDARD.TDF")
    assert info["type"] == TYPE_BLOCK
    assert _lines(out) == [
        info["name"],
        str(info["type"]),
        str(info["spacing"]),
        str(info["height"]),
        str(expected_width(TDF_DIR / "STANDARD.TDF", "HELLO")),
    ], out


def test_tdf_outline_font_metadata_and_width(console):
    out = _run(
        console,
        "TDFOUT.BAS",
        [
            '#INCLUDE "A:/lib/TDF.BAS"',
            'TDF.Load "A:/fonts/tdf/BIGOUT.TDF"',
            "PRINT TDF.Name$",
            "PRINT TDF.Type%",
            'PRINT TDF.Width("HI")',
            "TDF.Close",
        ],
    )
    info = parse_tdf(TDF_DIR / "BIGOUT.TDF")
    assert info["type"] == TYPE_OUTLINE
    assert _lines(out) == [
        info["name"],
        str(info["type"]),
        str(expected_width(TDF_DIR / "BIGOUT.TDF", "HI")),
    ], out


def test_tdf_color_font_metadata_and_width(console):
    out = _run(
        console,
        "TDFCOL.BAS",
        [
            '#INCLUDE "A:/lib/TDF.BAS"',
            'TDF.Load "A:/fonts/tdf/BLOCK.TDF"',
            "PRINT TDF.Name$",
            "PRINT TDF.Type%",
            'PRINT TDF.Width("HELLO")',
            "TDF.Close",
        ],
    )
    info = parse_tdf(TDF_DIR / "BLOCK.TDF")
    assert info["type"] == TYPE_COLOR
    assert _lines(out) == [
        info["name"],
        str(info["type"]),
        str(expected_width(TDF_DIR / "BLOCK.TDF", "HELLO")),
    ], out


def test_tdf_load_by_name(console):
    out = _run(
        console,
        "TDFNAME.BAS",
        [
            '#INCLUDE "A:/lib/TDF.BAS"',
            'TDF.LoadNamed "A:/fonts/tdf/STANDARD.TDF", "Standard"',
            "PRINT TDF.Name$",
            "TDF.Close",
        ],
    )
    assert _lines(out) == ["Standard"], out


def test_tdf_unknown_name_fails(console):
    out = _run(
        console,
        "TDFBAD.BAS",
        [
            '#INCLUDE "A:/lib/TDF.BAS"',
            'TDF.LoadNamed "A:/fonts/tdf/STANDARD.TDF", "NoSuchFont"',
            "PRINT 1",
        ],
    )
    assert "not found" in out.lower(), out
    assert "1" not in _lines(out), out


def test_tdf_missing_file_fails(console):
    out = _run(
        console,
        "TDFMISS.BAS",
        [
            '#INCLUDE "A:/lib/TDF.BAS"',
            'TDF.Load "A:/fonts/tdf/NOPE.TDF"',
            "PRINT 1",
        ],
    )
    assert "tdf" in out.lower(), out
    assert "1" not in _lines(out), out


def test_tdf_bad_magic_fails(console):
    # A 220-byte file with the wrong header bytes (written by a guest program).
    _run(
        console,
        "MKNOT.BAS",
        [
            'OPEN "NOTFONT.TDF" FOR OUTPUT AS #1',
            'PRINT #1, STRING$(220, "X")',
            "CLOSE #1",
        ],
    )
    out = _run(
        console,
        "TDFMAG.BAS",
        [
            '#INCLUDE "A:/lib/TDF.BAS"',
            'TDF.Load "NOTFONT.TDF"',
            "PRINT 1",
        ],
    )
    assert "thedraw" in out.lower(), out
    assert "1" not in _lines(out), out


def glyph_rows(path, ch):
    """Decode one glyph's rows of CP437 char codes from a .TDF file."""
    b = Path(path).read_bytes()
    ftype = b[41]
    data = 233
    offs = [b[45 + 2 * i] | (b[45 + 2 * i + 1] << 8) for i in range(94)]
    off = offs[ord(ch) - 33]
    assert off != 0xFFFF, ch
    p = data + off + 2
    rows, cur = [], []
    while b[p] != 0:
        if b[p] == 13:
            rows.append(cur)
            cur = []
            p += 1
        else:
            cur.append(b[p])
            p += 2 if ftype == TYPE_COLOR else 1
    if cur:
        rows.append(cur)
    return ftype, rows


def test_tdf_print_advances_cursor_and_draws(fresh_console):
    from test_term_cp437 import load_cp437_font

    c = fresh_console
    # 'I' in STANDARD: width 5 + spacing 1 -> column advance 6 from x=2.
    out = _run(
        c,
        "TDFDRAW.BAS",
        [
            '#INCLUDE "A:/lib/TDF.BAS"',
            "CLS RGB(0,0,0)",
            "COLOUR RGB(255,255,255), RGB(0,0,0)",
            'TDF.Load "A:/fonts/tdf/STANDARD.TDF"',
            'TDF.Print 2, 3, "I"',
            "H% = MM.HPOS : V% = MM.VPOS",
            "LOCATE 20, 0",
            'PRINT H%; ","; V%',
            "TDF.Close",
        ],
    )
    # Serial output concatenates the glyph cells, then the saved cursor.
    assert out.rstrip().endswith("64,48"), out

    # The 8x16 CP437 face should light exactly the glyph's ink pixels in cells
    # starting at column 2, row 3.
    font = load_cp437_font()
    _, rows = glyph_rows(TDF_DIR / "STANDARD.TDF", "I")
    base_x, base_y = 2 * 8, 3 * 16
    lit, paper = [], []
    for r, row in enumerate(rows):
        for col, code in enumerate(row):
            bitmap = font[code]
            for y in range(16):
                for x in range(8):
                    px = (base_x + col * 8 + x, base_y + r * 16 + y)
                    if bitmap[y] & (0x80 >> x):
                        lit.append(px)
                    elif (x + y) % 8 == 0:
                        paper.append(px)
    assert lit, "glyph decoded to no ink"
    ink = c.screen_pixels(lit[:200])
    blank = c.screen_pixels(paper[:40])
    assert all(r > 150 and g > 150 and b > 150 for r, g, b in ink), ink
    assert all(r < 40 and g < 40 and b < 40 for r, g, b in blank), blank


def test_tdf_demo_runs(console):
    out = console.send_line('RUN "A:/apps/TDFDEMO.BAS"')
    assert "?SYNTAX" not in out.upper()
    assert "Standard" in out


def test_tdf_help_topic(console):
    from ihelp_util import dump_topic

    seen = dump_topic(console, "TDF")
    assert "TheDraw" in seen
    assert "Outline" in seen
