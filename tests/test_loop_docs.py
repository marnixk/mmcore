"""#885: issue-loop-parallel workers must be told to read AGENTS.md and the
skill files by literal path, and opencode must be able to discover the skills."""

import os

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SKILL = os.path.join(
    REPO, ".cursor", "skills", "issue-loop-parallel", "SKILL.md"
)
AGENTS = os.path.join(REPO, "AGENTS.md")
CURSOR_SKILLS = os.path.join(REPO, ".cursor", "skills")
OPENCODE_SKILLS = os.path.join(REPO, ".opencode", "skill")

TEST_SUITE_PROGRESS = ".cursor/skills/test-suite-progress/SKILL.md"
ISSUE_LOOP = ".cursor/skills/issue-loop/SKILL.md"


def _read(path):
    with open(path, encoding="utf-8") as fh:
        return fh.read()


def _worker_prompt_template(text):
    start = text.index("## Worker prompt template")
    fence = text.index("```text", start)
    body_start = fence + len("```text\n")
    body_end = text.index("```", body_start)
    return text[body_start:body_end]


def _spawning_section(text):
    start = text.index("## Spawning workers")
    end = text.index("## Worker prompt template", start)
    return text[start:end]


def test_worker_prompt_template_tells_workers_to_read_agents_md():
    tpl = _worker_prompt_template(_read(SKILL))
    assert "AGENTS.md" in tpl


def test_worker_prompt_template_names_literal_skill_paths():
    tpl = _worker_prompt_template(_read(SKILL))
    assert TEST_SUITE_PROGRESS in tpl
    assert ISSUE_LOOP in tpl


def test_spawning_recipe_cannot_omit_the_preamble():
    section = _spawning_section(_read(SKILL))
    assert "AGENTS.md" in section
    assert TEST_SUITE_PROGRESS in section
    assert "preamble" in section.lower()


def test_agents_md_documents_the_discovery_gap():
    text = _read(AGENTS)
    assert ".opencode/skill" in text
    assert ".cursor/skills/" in text
    flat = " ".join(text.replace("**", "").split())
    assert "does not scan `.cursor/skills/`" in flat


def test_opencode_skill_symlinks_resolve_to_cursor_originals():
    names = sorted(
        name
        for name in os.listdir(CURSOR_SKILLS)
        if os.path.isfile(os.path.join(CURSOR_SKILLS, name, "SKILL.md"))
    )
    assert names, "no .cursor/skills/*/SKILL.md found"
    for name in names:
        original = os.path.join(CURSOR_SKILLS, name, "SKILL.md")
        link = os.path.join(OPENCODE_SKILLS, name, "SKILL.md")
        assert os.path.islink(link), link
        assert os.path.realpath(link) == os.path.realpath(original), name
        assert _read(link) == _read(original), name
