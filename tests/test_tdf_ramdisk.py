"""TheDraw TDF font collection embedded in ramdisk/fonts/tdf (issues #554, #626, #628)."""

import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
TDF_DIR = REPO / "ramdisk" / "fonts" / "tdf"

MONO = """
    ASCII ASCII_2 STANDARD ROMAN CLASSIC BOLDME SMALLR 4MAX
""".split()

# The colour group has the fonts whose glyphs carry >1 foreground attribute.
COLOR = """
    3D-ASCII AAA AARDVARK BIGICE_F BLOCK BLOCK3D CYBERIA1 DIGITAL2 METAL
    METALIX2 SUPER THIN2 TINY 1911X AAAX ABBADON ACHERONX ACID3DX ACIDNEWX
    ACIDSC2X ACRYLIC ADRENAX ADRKNESX ADVOCATE
""".split()

DECO = """
    3D-FADE 3DDIAG BIGOUT CYBERLRG DOSREBEL FUTURE GRAFFITI SHADOW TECH
""".split()

CATEGORIES = {"mono": MONO, "color": COLOR, "deco": DECO}
EXPECTED = {f"{n}.TDF" for names in CATEGORIES.values() for n in names}
EXPECTED_TOTAL_BYTES = 406184

# A known multi-record file from the colour group: record index -> font name.
MULTI_NAME = "ACIDSC2X.TDF"
MULTI_RECORDS = 6


def _names(cat):
    return {p.name for p in (TDF_DIR / cat).glob("*.TDF")}


def test_tdf_categories_names_and_size():
    assert set(TDF_DIR.iterdir()) == {
        TDF_DIR / "mono",
        TDF_DIR / "color",
        TDF_DIR / "deco",
        TDF_DIR / "README.md",
    }
    for cat, names in CATEGORIES.items():
        assert _names(cat) == {f"{n}.TDF" for n in names}, cat
    found = {p.name for p in TDF_DIR.glob("**/*.TDF")}
    assert found == EXPECTED
    total = sum(p.stat().st_size for p in TDF_DIR.glob("**/*.TDF"))
    assert total == EXPECTED_TOTAL_BYTES
    assert len(EXPECTED) == 41


def test_mmb_tdf_count_single_and_multi(tmp_path):
    """#629: the C record-count helper handles single- and multi-record files."""
    src = REPO / "tests" / "tdf_host.c"
    impl = REPO / "mmbasic" / "src" / "mmb_tdf.c"
    include = REPO / "mmbasic" / "include"
    exe = tmp_path / "tdf_host"
    subprocess.run(
        [
            "cc",
            "-O0",
            "-Wall",
            "-Werror",
            "-I",
            str(include),
            "-o",
            str(exe),
            str(src),
            str(impl),
        ],
        check=True,
        cwd=REPO,
    )
    single = TDF_DIR / "mono" / "STANDARD.TDF"
    multi = TDF_DIR / "color" / MULTI_NAME
    out = subprocess.run(
        [str(exe), str(single), "1", str(multi), str(MULTI_RECORDS)],
        check=True,
        capture_output=True,
        text=True,
    )
    assert "all checks passed" in out.stdout
