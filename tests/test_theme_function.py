"""THEME() identifiers must cover every mmb_ed_theme colour field.

Static check: parse the colour fields out of ``mmb_ed_theme`` (``mmb_priv.h``),
the name->field table from ``editor.c``, and the identifier table from
``docs/help/theme.txt``; assert all three agree. A new theme colour that is not
surfaced by THEME() (and documented) fails this test.
"""

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PRIV = ROOT / "mmbasic" / "include" / "mmb_priv.h"
EDITOR = ROOT / "mmbasic" / "src" / "editor.c"
THEME_DOC = ROOT / "docs" / "help" / "theme.txt"


def _struct_fields():
    src = PRIV.read_text()
    body = src[src.index("typedef struct mmb_ed_theme {"):]
    body = body[: body.index("} mmb_ed_theme;")]
    fields = []
    for m in re.finditer(r"unsigned char ([^;]+);", body):
        for name in m.group(1).split(","):
            fields.append(name.strip())
    return fields


def _role_table():
    src = EDITOR.read_text()
    body = src[src.index("static const ed_theme_role k_theme_roles[]"):]
    body = body[: body.index("};")]
    pairs = re.findall(
        r'\{\s*"([A-Z_]+)",\s*offsetof\(mmb_ed_theme,\s*(\w+)\)\s*\}', body
    )
    return pairs


def _doc_table():
    text = THEME_DOC.read_text()
    body = text[text.index("Identifiers"): text.index("Example:")]
    return re.findall(r"\b([A-Z][A-Z_]+)\s+([a-z_]+)\b", body)


def test_doc_and_role_table_cover_every_theme_field():
    fields = set(_struct_fields())
    roles = _role_table()
    role_fields = {f for _n, f in roles}
    role_names = [n for n, _f in roles]
    doc = _doc_table()
    doc_fields = {f for _n, f in doc}
    doc_names = [n for n, _f in doc]

    missing_roles = fields - role_fields
    missing_docs = fields - doc_fields
    assert not missing_roles, f"fields missing from THEME(): {sorted(missing_roles)}"
    assert not missing_docs, f"fields missing from theme.txt: {sorted(missing_docs)}"
    assert set(role_names) == set(doc_names), (
        sorted(set(role_names) ^ set(doc_names))
    )
    assert len(role_names) == len(set(role_names)), "duplicate THEME() names"


def test_theme_help_topic_exists_and_is_linked():
    text = THEME_DOC.read_text()
    assert "name: THEME" in text
    assert "OPTION EDIT THEME" in text
    assert "RGB()" in text
    contents = (ROOT / "docs" / "help" / "contents.txt").read_text()
    assert "~THEME~" in contents
    colour = (ROOT / "docs" / "help" / "colour.txt").read_text()
    assert "~THEME~" in colour
    functions = (ROOT / "docs" / "help" / "functions.txt").read_text()
    assert "THEME(name$)" in functions
    # THEME must keep its own page, not be folded into FUNCTIONS aliases.
    alias_line = [l for l in functions.splitlines() if l.startswith("alias:")][0]
    assert "THEME" not in alias_line
