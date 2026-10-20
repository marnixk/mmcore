"""Static guard for the CMM2 compat games' KEYDOWN() list contract (#1250).

#1238 briefly changed KEYDOWN(n) for n > 0 from "return the n-th held key code"
to a boolean test for PC/AT set-1 scan code n, and #1241 rewrote the bundled
games to poll scan codes directly. #1250 restored the Colour Maximite 2
list/character-code contract. The games are only loaded and run by the compat
tests, so an old-contract poll does not fail CI by itself. Scan them here
instead: this test is cheap and never boots QEMU.
"""

from pathlib import Path
import re

REPO = Path(__file__).resolve().parent.parent
COMPAT = REPO / "tests" / "cmm2_compat"
SUFFIXES = {".bas", ".inc", ".bak"}

# The list contract: iterate the held-key list returned by KEYDOWN(0).
LIST_LOOP = re.compile(
    r"\bfor\s+\w+\s*=\s*\d+\s+to\s+keydown\s*\(\s*0\s*\)", re.I
)
# A positive numeric literal argument is the retired #1238 scan-code style.
SCAN_TEST = re.compile(r"\bkeydown\s*\(\s*[1-9]\d*\s*\)", re.I)


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


def test_compat_keydown_uses_the_list_contract():
    offenders = []
    keydown_users = []

    for path in _compat_sources():
        text = path.read_text(errors="replace")
        if re.search(r"\bkeydown\b", text, re.I):
            keydown_users.append(path)
        has_list_loop = False
        for number, code in _code_lines(text):
            if LIST_LOOP.search(code):
                has_list_loop = True
            for match in SCAN_TEST.finditer(code):
                offender = path.relative_to(REPO)
                offenders.append(
                    f"{offender}:{number}: tests KEYDOWN(n) as a scan code: "
                    f"{match.group(0)}"
                )
        if re.search(r"\bkeydown\b", text, re.I) and not has_list_loop:
            offenders.append(
                f"{path.relative_to(REPO)}: uses KEYDOWN without a "
                "FOR ... = 1 TO KEYDOWN(0) list loop"
            )

    assert keydown_users, "expected the bundled games to poll KEYDOWN"
    assert not offenders, "\n".join(offenders)
