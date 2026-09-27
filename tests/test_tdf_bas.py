"""TheDraw .TDF fonts: native TDF commands (#866, #867, #868).

The old interpreted library (ramdisk/lib/TDF.BAS, #557) is gone; these checks
exercise the native commands against the same fonts and decoder.
"""

import re
import subprocess
import sys
from pathlib import Path

import pytest

# The QEMU tests here are timing-sensitive: under parallel load a slow boot
# makes the shared serial console sample garbage. Mark the module so it holds
# the QEMU lane exclusively and no other QEMU instance runs alongside it.
pytestmark = pytest.mark.qemu_exclusive

REPO = Path(__file__).resolve().parents[1]
DEMO = REPO / "ramdisk" / "apps" / "TDFDEMO.BAS"
TDF_DIR = REPO / "ramdisk" / "fonts" / "tdf"
MONO_DIR = TDF_DIR / "mono"
COLOR_DIR = TDF_DIR / "color"
DECO_DIR = TDF_DIR / "deco"

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


def tdf_records(path):
    """Walk every font record in a .TDF (mirrors mmb_tdf_count)."""
    b = Path(path).read_bytes()
    recs = []
    pos = 20
    i = 0
    while True:
        if pos + 45 > len(b) or b[pos : pos + 4] != b"\x55\xaa\x00\xff":
            break
        nl = b[pos + 4]
        if nl > 12:
            nl = 12
        name = b[pos + 5 : pos + 5 + nl].split(b"\x00")[0].decode("latin1")
        bs = b[pos + 23] | (b[pos + 24] << 8)
        ds = 233 if i == 0 else pos + 213
        if ds + bs > len(b):
            bs = len(b) - ds if ds < len(b) else 0
        recs.append({"pos": pos, "name": name, "type": b[pos + 21]})
        pos = (233 + bs) if i == 0 else (ds + bs)
        if pos + 4 <= len(b) and b[pos : pos + 4] != b"\x55\xaa\x00\xff":
            pos += 1
        i += 1
    return recs


# --- host checks -----------------------------------------------------------

def test_tdf_demo_uses_native_commands():
    src = DEMO.read_text(encoding="utf-8")
    assert '#INCLUDE' not in src.upper()
    assert "TDF LOAD" in src.upper()
    assert "TDF PRINT" in src.upper()
    assert "TDF.NAME$" in src.upper()
    assert "TDF CLOSE" in src.upper()


def test_ramdisk_generator_drops_tdf_lib(tmp_path):
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
    assert "lib/TDF.BAS" not in text
    assert "apps/TDFDEMO.BAS" in text


# --- QEMU checks -----------------------------------------------------------

def test_tdf_block_font_metadata_and_width(console):
    out = _run(
        console,
        "TDFBLK.BAS",
        [
            'TDF LOAD "A:/fonts/tdf/mono/STANDARD.TDF", 0, 0',
            "PRINT TDF.NAME$",
            "PRINT TDF.TYPE%",
            "PRINT TDF.SPACING%",
            "PRINT TDF.HEIGHT",
            'PRINT TDF.WIDTH("HELLO")',
            "TDF CLOSE 0",
        ],
    )
    info = parse_tdf(MONO_DIR / "STANDARD.TDF")
    assert info["type"] == TYPE_BLOCK
    assert _lines(out) == [
        info["name"],
        str(info["type"]),
        str(info["spacing"]),
        str(info["height"]),
        str(expected_width(MONO_DIR / "STANDARD.TDF", "HELLO")),
    ], out


def test_tdf_outline_font_metadata_and_width(console):
    out = _run(
        console,
        "TDFOUT.BAS",
        [
            'TDF LOAD "A:/fonts/tdf/deco/BIGOUT.TDF", 0, 0',
            "PRINT TDF.NAME$",
            "PRINT TDF.TYPE%",
            'PRINT TDF.WIDTH("HI")',
            "TDF CLOSE 0",
        ],
    )
    info = parse_tdf(DECO_DIR / "BIGOUT.TDF")
    assert info["type"] == TYPE_OUTLINE
    assert _lines(out) == [
        info["name"],
        str(info["type"]),
        str(expected_width(DECO_DIR / "BIGOUT.TDF", "HI")),
    ], out


def test_tdf_color_font_metadata_and_width(console):
    out = _run(
        console,
        "TDFCOL.BAS",
        [
            'TDF LOAD "A:/fonts/tdf/color/BLOCK.TDF", 0, 0',
            "PRINT TDF.NAME$",
            "PRINT TDF.TYPE%",
            'PRINT TDF.WIDTH("HELLO")',
            "TDF CLOSE 0",
        ],
    )
    info = parse_tdf(COLOR_DIR / "BLOCK.TDF")
    assert info["type"] == TYPE_COLOR
    assert _lines(out) == [
        info["name"],
        str(info["type"]),
        str(expected_width(COLOR_DIR / "BLOCK.TDF", "HELLO")),
    ], out


def test_tdf_reload_after_close(console):
    """#627/#866: the whole file is buffered; a second load starts clean."""
    out = _run(
        console,
        "TDFRELOAD.BAS",
        [
            'TDF LOAD "A:/fonts/tdf/mono/STANDARD.TDF"',
            "PRINT TDF.NAME$",
            "TDF CLOSE",
            'TDF LOAD "A:/fonts/tdf/color/BLOCK.TDF"',
            "PRINT TDF.NAME$",
            "PRINT TDF.TYPE%",
            "TDF CLOSE",
        ],
    )
    assert _lines(out) == ["Standard", "Block", str(TYPE_COLOR)], out


def test_tdf_no_slot_loads_slot_zero(console):
    """#884: LOAD/NAMED without a slot default to 0 and replace it."""
    out = _run(
        console,
        "TDFSLOT0.BAS",
        [
            'TDF LOAD "A:/fonts/tdf/mono/STANDARD.TDF"',
            "PRINT TDF.NAME$",
            'TDF LOAD "A:/fonts/tdf/color/BLOCK.TDF"',
            "PRINT TDF.NAME$",
            'TDF LOAD "A:/fonts/tdf/deco/BIGOUT.TDF", 0, 1',
            'TDF LOAD "A:/fonts/tdf/mono/STANDARD.TDF", 0, 0',
            "PRINT TDF.NAME$",
            "PRINT TDF.NAME$(1)",
            'TDF NAMED "A:/fonts/tdf/color/BLOCK.TDF", "Block"',
            "PRINT TDF.NAME$",
            "TDF CLOSE 1",
            "TDF CLOSE",
        ],
    )
    assert "no free slot" not in out.lower(), out
    assert "?SYNTAX" not in out.upper(), out
    assert _lines(out) == [
        "Standard",
        "Block",
        "Standard",
        "BigOutline",
        "Block",
    ], out


def test_tdf_variants_exposed(console):
    """#629/#866: every record is addressable by index and by name."""
    path = COLOR_DIR / "ACIDSC2X.TDF"
    recs = tdf_records(path)
    assert len(recs) > 1
    names = [r["name"] for r in recs]
    out = _run(
        console,
        "TDFVAR.BAS",
        [
            'TDF LOAD "A:/fonts/tdf/color/ACIDSC2X.TDF"',
            "PRINT TDF.VARIANTS%",
            "PRINT TDF.VARIANT%",
            "PRINT TDF.NAME$",
            "PRINT TDF.VARIANTNAME$(0)",
            "PRINT TDF.VARIANTNAME$(1)",
            'TDF LOAD "A:/fonts/tdf/color/ACIDSC2X.TDF", 2',
            "PRINT TDF.VARIANT%",
            "PRINT TDF.NAME$",
            "TDF CLOSE 1",
            "TDF USE 0",
            "TDF CLOSE 0",
        ],
    )
    assert _lines(out) == [
        str(len(recs)),
        "0",
        names[0],
        names[0],
        names[1],
        "2",
        names[2],
    ], out


def test_tdf_load_by_name(console):
    out = _run(
        console,
        "TDFNAME.BAS",
        [
            'TDF NAMED "A:/fonts/tdf/mono/STANDARD.TDF", "Standard", 0',
            "PRINT TDF.NAME$",
            "TDF CLOSE 0",
        ],
    )
    assert _lines(out) == ["Standard"], out


def test_tdf_slots_are_independent(console):
    """#867: two fonts load and report their own metadata."""
    out = _run(
        console,
        "TDFSLOT.BAS",
        [
            'TDF LOAD "A:/fonts/tdf/mono/STANDARD.TDF", 0, 0',
            'TDF LOAD "A:/fonts/tdf/color/BLOCK.TDF", 0, 1',
            "PRINT TDF.NAME$(0)",
            "PRINT TDF.NAME$(1)",
            "PRINT TDF.TYPE%(0)",
            "PRINT TDF.TYPE%(1)",
            "TDF USE 1",
            "PRINT TDF.NAME$",
            "TDF CLOSE 0",
            "PRINT TDF.NAME$(1)",
            "TDF CLOSE 1",
        ],
    )
    assert _lines(out) == [
        "Standard",
        "Block",
        str(TYPE_BLOCK),
        str(TYPE_COLOR),
        "Block",
        "Block",
    ], out


def test_tdf_slots_draw_two_fonts(fresh_console):
    """#867: both slots draw in one program, selected by USE and by slot%."""
    c = fresh_console
    out = _run(
        c,
        "TDF2DRAW.BAS",
        [
            "CLS RGB(0,0,0)",
            "COLOUR RGB(255,255,255), RGB(0,0,0)",
            'TDF LOAD "A:/fonts/tdf/mono/STANDARD.TDF", 0, 0',
            'TDF LOAD "A:/fonts/tdf/color/BLOCK.TDF", 0, 1',
            'TDF USE 0 : TDF PRINT 0, 1, "A"',
            'TDF USE 1 : TDF PRINT 0, 20, "A"',
            'TDF PRINT 0, 40, "A", 0',
            'PRINT "DONE"',
            "TDF CLOSE 0",
            "TDF CLOSE 1",
        ],
    )
    assert "?SYNTAX" not in out.upper(), out
    assert "DONE" in out, out


def test_tdf_unknown_name_fails(console):
    out = _run(
        console,
        "TDFBAD.BAS",
        [
            'TDF NAMED "A:/fonts/tdf/mono/STANDARD.TDF", "NoSuchFont"',
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
            'TDF LOAD "A:/fonts/tdf/mono/NOPE.TDF"',
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
            'TDF LOAD "NOTFONT.TDF"',
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
            "CLS RGB(0,0,0)",
            "COLOUR RGB(255,255,255), RGB(0,0,0)",
            'TDF LOAD "A:/fonts/tdf/mono/STANDARD.TDF"',
            'TDF PRINT 2, 3, "I"',
            "H% = MM.HPOS : V% = MM.VPOS",
            "LOCATE 20, 0",
            'PRINT H%; ","; V%',
            "TDF CLOSE",
        ],
    )
    # Serial output concatenates the glyph cells, then the saved cursor.
    assert out.rstrip().endswith("64,48"), out

    # The 8x16 CP437 face should light exactly the glyph's ink pixels in cells
    # starting at column 2, row 3.
    font = load_cp437_font()
    _, rows = glyph_rows(MONO_DIR / "STANDARD.TDF", "I")
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


def _first_glyph_pixels(path, ch, base_x, base_y):
    """One ink and one paper pixel inside a TDF glyph's first drawn cells."""
    from test_term_cp437 import load_cp437_font

    font = load_cp437_font()
    _, rows = glyph_rows(path, ch)
    ink = paper = None
    for r, row in enumerate(rows):
        for col, code in enumerate(row):
            bitmap = font[code]
            for y in range(16):
                for x in range(8):
                    px = (base_x + col * 8 + x, base_y + r * 16 + y)
                    if bitmap[y] & (0x80 >> x):
                        if ink is None:
                            ink = px
                    elif paper is None:
                        paper = px
    return ink, paper


def test_tdf_print_honours_page_write(fresh_console):
    """#883: PAGE WRITE 1 + TDF PRINT paints the selected page, not the console."""
    c = fresh_console
    ink, paper = _first_glyph_pixels(MONO_DIR / "STANDARD.TDF", "I", 2 * 8, 3 * 16)
    assert ink and paper

    # Default/visible: no explicit page keeps the console ANSI stamp path.
    _run(
        c,
        "TDFNOPAGE.BAS",
        [
            "CLS RGB(0,0,0)",
            "COLOUR RGB(255,255,255), RGB(0,0,0)",
            'TDF LOAD "A:/fonts/tdf/mono/STANDARD.TDF"',
            'TDF PRINT 2, 3, "I"',
            "TDF CLOSE",
        ],
    )
    console_ink = c.screen_pixels([ink])[0]
    console_paper = c.screen_pixels([paper])[0]
    assert all(v > 150 for v in console_ink), console_ink
    assert all(v < 40 for v in console_paper), console_paper

    # An offscreen (non-overlay) page write must leave the visible frame alone.
    _run(
        c,
        "TDFOFFSCR.BAS",
        [
            "MODE 8,16",
            "PAGE WRITE 0",
            "PAGE DISPLAY 0",
            "CLS RGB(0,0,0)",
            "COLOUR RGB(255,255,255), RGB(0,0,0)",
            'TDF LOAD "A:/fonts/tdf/mono/STANDARD.TDF"',
            "PAGE WRITE 2",
            'TDF PRINT 2, 3, "I"',
            "PRINT PIXEL({},{},2)".format(ink[0], ink[1]),
            "TDF CLOSE",
        ],
    )
    assert c.screen_pixel(ink[0], ink[1]) == (0, 0, 0), "offscreen write leaked"

    # Page 1: PAGE DISPLAY 1 reveals the stamp.
    out = _run(
        c,
        "TDFPAGE.BAS",
        [
            "MODE 8,16",
            "PAGE WRITE 1",
            "CLS RGB(0,0,0)",
            "COLOUR RGB(255,255,255), RGB(0,0,0)",
            'TDF LOAD "A:/fonts/tdf/mono/STANDARD.TDF"',
            'TDF PRINT 2, 3, "I"',
            "PAGE DISPLAY 1",
            "PAGE WRITE 1",
            "PRINT PIXEL({},{}); \",\"; PIXEL({},{})".format(
                ink[0], ink[1], paper[0], paper[1]
            ),
            "TDF CLOSE",
        ],
    )
    values = [int(v) for v in _lines(out)[-1].split(",")]
    assert (values[0] >> 16) & 255 > 150, values
    assert (values[0] >> 8) & 255 > 150, values
    assert (values[0] & 255) > 150, values
    assert (values[1] & 0xFFFFFF) == 0, values


def test_tdf_print_spaces_advance_safely(fresh_console):
    """#588: leading, trailing and repeated spaces must advance, not fault."""
    c = fresh_console
    text = " A  B "
    out = _run(
        c,
        "TDFSP.BAS",
        [
            "CLS",
            "COLOUR RGB(255,255,255), RGB(0,0,0)",
            'TDF LOAD "A:/fonts/tdf/mono/STANDARD.TDF"',
            'TDF PRINT 2, 3, "' + text + '"',
            "H% = MM.HPOS : V% = MM.VPOS",
            'W% = TDF.WIDTH("' + text + '")',
            "LOCATE 20, 0",
            'PRINT H%; ","; V%; ","; W%',
            "TDF CLOSE",
        ],
    )
    exp = expected_width(MONO_DIR / "STANDARD.TDF", text)
    assert _lines(out)[-1].endswith(f"{(2 + exp) * 8},48,{exp}"), out


def test_tdf_demo_runs(console):
    out = console.send_line('RUN "A:/apps/TDFDEMO.BAS"')
    assert "?SYNTAX" not in out.upper()
    assert "Standard" in out


def test_tdf_help_topic(console):
    from ihelp_util import dump_topic

    seen = dump_topic(console, "TDF")
    assert "TheDraw" in seen
    assert "Outline" in seen

