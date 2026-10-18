"""Static guard for the CMM2 compat games' KEYDOWN() scan codes (#1241).

#1238 changed KEYDOWN(n) for n > 0 from "return the n-th held key code" to a
boolean test for PC/AT set-1 scan code n. The bundled games are only loaded
and run by the compat tests, so an old-contract poll does not fail CI. Scan
them here instead: this test is cheap and never boots QEMU.
"""

from pathlib import Path
import re

REPO = Path(__file__).resolve().parent.parent
COMPAT = REPO / "tests" / "cmm2_compat"
SUFFIXES = {".bas", ".inc", ".bak"}

# Old contract: iterate the held-key list returned by KEYDOWN(0).
LIST_LOOP = re.compile(
    r"\bfor\s+\w+\s*=\s*\d+\s+to\s+keydown\s*\(\s*0\s*\)", re.I
)
INDEXED = re.compile(
    r"\bkeydown\s*\(\s*(?:idx|keyidx|index|i|n)\s*\)", re.I
)
# Pre-set-1 ASCII/arrow codes that are not valid set-1 scan codes used by the
# games: 118 (v), 122 (z), 128..131 (arrows). Esc/Enter were also remapped,
# but their old values (27/10) collide with valid set-1 keys, so the loop and
# index checks above cover those.
RETIRED = re.compile(
    r"\bkeydown\s*\(\s*(?:118|122|128|129|130|131)\s*\)", re.I
)


def _compat_sources():
    return sorted(
        path
        for path in COMPAT.rglob("*")
        if path.is_file() and path.suffix.lower() in SUFFIXES
    )


def test_compat_sources_exist():
    assert _compat_sources(), "tests/cmm2_compat should contain bundled games"


def _code_lines(text: str):
    """Yield (line number, line) with BASIC comments removed."""
    for number, line in enumerate(text.splitlines(), 1):
        code = line.split("'", 1)[0]
        if re.match(r"\s*rem\b", code, re.I):
            code = ""
        yield number, code


def test_compat_keydown_uses_set1_scan_codes():
    offenders = []
    checks = (
        (LIST_LOOP, "loops over KEYDOWN(0)"),
        (INDEXED, "indexes KEYDOWN(...)"),
        (RETIRED, "uses a retired pre-set-1 code"),
    )

    for path in _compat_sources():
        text = path.read_text(errors="replace")
        for number, code in _code_lines(text):
            for pattern, why in checks:
                for match in pattern.finditer(code):
                    offender = path.relative_to(REPO)
                    offenders.append(
                        f"{offender}:{number}: {why}: {match.group(0)}"
                    )

    assert not offenders, "\n".join(offenders)
