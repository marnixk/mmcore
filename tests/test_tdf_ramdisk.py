"""TheDraw TDF font shortlist embedded in ramdisk/fonts/tdf (issue #554)."""

from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
TDF_DIR = REPO / "ramdisk" / "fonts" / "tdf"

EXPECTED = frozenset(
    """
    STANDARD.TDF BLOCK.TDF BLOCK3D.TDF SHADOW.TDF THIN2.TDF TINY.TDF SMALLR.TDF
    BIGOUT.TDF BIGICE_F.TDF ASCII.TDF ASCII_2.TDF 3D-ASCII.TDF 3D-FADE.TDF
    3DDIAG.TDF FUTURE.TDF TECH.TDF METAL.TDF METALIX2.TDF DIGITAL2.TDF ROMAN.TDF
    CLASSIC.TDF 4MAX.TDF AAA.TDF GRAFFITI.TDF DOSREBEL.TDF CYBERIA1.TDF
    CYBERLRG.TDF BOLDME.TDF SUPER.TDF AARDVARK.TDF
    """.split()
)

EXPECTED_TOTAL_BYTES = 156369


def test_tdf_shortlist_count_names_and_size():
    tdf_files = {p.name for p in TDF_DIR.glob("*.TDF")}
    assert tdf_files == EXPECTED
    assert len(list(TDF_DIR.iterdir())) == len(EXPECTED) + 1  # README.md only
    total = sum(p.stat().st_size for p in TDF_DIR.glob("*.TDF"))
    assert total == EXPECTED_TOTAL_BYTES
