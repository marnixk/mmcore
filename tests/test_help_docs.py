"""Host tests for docs/help/ and scripts/gen_help.py (no QEMU)."""

import os
import sys

import pytest

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "scripts"))
import gen_help  # noqa: E402


def test_help_dir_has_overview_and_contents():
    topics = gen_help.load_topics()
    names = {t["name"].upper() for t in topics}
    assert "OVERVIEW" in names
    assert "CONTENTS" in names
    kinds = {t["name"].upper(): t["kind"] for t in topics}
    assert kinds["OVERVIEW"] == "page"
    assert kinds["CONTENTS"] == "page"
    assert "CLS" in names
    assert "FOR" in names


def test_overview_marks_for_explicitly():
    topics = gen_help.load_topics()
    ov = next(t for t in topics if t["name"].upper() == "OVERVIEW")
    assert "~FOR~" in ov["text"]
    assert "for every" in ov["text"]
    assert "for every loop we\ncan use ~FOR~ loops" in ov["text"]
    links = gen_help.extract_links(ov["text"])
    assert "FOR" in links
    assert "for" not in links


def test_contents_does_not_auto_link_prose():
    topics = gen_help.load_topics()
    body = next(t for t in topics if t["name"].upper() == "CONTENTS")["text"]
    assert "~for~" not in body
    assert "~if~" not in body
    assert "HELP topic for details" in body
    assert "~CLS~" in body
    assert "~FOR~" in body


def test_wrap_names_longest_match():
    names = ["LINE INPUT", "LINE", "INPUT", "FOR"]
    text = "LINE INPUT then LINE and for every FOR"
    out = gen_help.wrap_names(text, names)
    assert out == "~LINE INPUT~ then ~LINE~ and for every ~FOR~"


def test_unknown_link_fails_generate(tmp_path):
    bad = tmp_path / "oops.txt"
    bad.write_text("name: OOPS\nkind: command\n\nSee ~NOSUCH~\n", encoding="utf-8")
    topic = gen_help.parse_file(str(bad))
    with pytest.raises(SystemExit):
        gen_help.validate_links([topic], [])


def test_new_txt_files_are_topics(tmp_path, monkeypatch):
    src = os.path.join(REPO, "docs", "help")
    sample = open(os.path.join(src, "cls.txt"), encoding="utf-8").read()
    dest = tmp_path / "docs" / "help"
    dest.mkdir(parents=True)
    (dest / "overview.txt").write_text(
        "name: OVERVIEW\nkind: page\n\nHi ~CLS~\n", encoding="utf-8"
    )
    (dest / "contents.txt").write_text(
        "name: CONTENTS\nkind: page\n\nMap\n", encoding="utf-8"
    )
    (dest / "cls.txt").write_text(sample, encoding="utf-8")
    (dest / "brand-new.txt").write_text(
        "name: BRANDNEW\nkind: command\n\nA new topic.\n", encoding="utf-8"
    )
    monkeypatch.setattr(gen_help, "HELP_DIR", str(dest))
    topics = gen_help.load_topics(str(dest))
    names = {t["name"].upper() for t in topics}
    assert "BRANDNEW" in names
    assert "CLS" in names
