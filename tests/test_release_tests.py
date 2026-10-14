"""#1173: a release re-runs only the test suites related to the change since
the previous release tag, with a full-suite fallback for broad/unmappable
changes.

The selection logic lives in ``scripts/release_tests.py`` and is pure Python
(no QEMU), so these tests exercise it directly plus the wiring in the skills
and ``scripts/github-release.sh``.
"""

import importlib.util
import os
import stat
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCRIPTS = os.path.join(REPO, "scripts")
SELECTOR = os.path.join(SCRIPTS, "release_tests.py")
WRAPPER = os.path.join(SCRIPTS, "release-tests.sh")
GITHUB_RELEASE = os.path.join(SCRIPTS, "github-release.sh")
RELEASE_SKILL = os.path.join(REPO, ".cursor", "skills", "github-release", "SKILL.md")
PARALLEL_SKILL = os.path.join(
    REPO, ".cursor", "skills", "issue-loop-parallel", "SKILL.md"
)


def _load():
    spec = importlib.util.spec_from_file_location("release_tests", SELECTOR)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


rt = _load()


def _read(path):
    with open(path, encoding="utf-8") as fh:
        return fh.read()


# --------------------------------------------------------------------------
# Selection
# --------------------------------------------------------------------------


def test_wordpad_change_selects_only_wordpad_suites():
    sel = rt.select(["mmbasic/src/cmd_wordpad.c"])
    assert not sel.full
    assert sel.suites == ["tests/test_wordpad.py", "tests/test_wordpad_fonts.py"]
    assert any("wordpad" in reason and "cmd_wordpad.c" in reason for reason in sel.reasons)


def test_paint_change_selects_the_paint_suites():
    sel = rt.select(["mmbasic/src/paint_undo.c"])
    assert not sel.full
    assert sel.suites
    assert all(s.startswith("tests/test_paint") for s in sel.suites)


def test_core_interpreter_change_falls_back_to_full_suite():
    for path in (
        "mmbasic/src/core.c",
        "mmbasic/src/expr.c",
        "mmbasic/src/tok.c",
        "mmbasic/src/vars.c",
        "mmbasic/src/gfx.c",
    ):
        sel = rt.select([path])
        assert sel.full, path
        assert sel.suites == []
        assert path in sel.fallback


def test_header_change_falls_back_to_full_suite():
    sel = rt.select(["mmbasic/src/mmbasic.h"])
    assert sel.full
    assert "mmbasic/src/mmbasic.h" in sel.fallback


def test_build_change_falls_back_to_full_suite():
    for path in ("scripts/build.sh", "Makefile", "console/Makefile", "circle/lib/x.cpp"):
        sel = rt.select([path])
        assert sel.full, path


def test_patches_change_falls_back_to_full_suite():
    sel = rt.select(["patches/circle-wifi-149.patch"])
    assert sel.full


def test_unmapped_path_falls_back_to_full_suite():
    sel = rt.select(["mmbasic/src/brand_new_command.c"])
    assert sel.full
    assert "unmapped" in sel.fallback


def test_any_unmapped_path_forces_the_full_suite():
    sel = rt.select(["mmbasic/src/cmd_wordpad.c", "mmbasic/src/brand_new_command.c"])
    assert sel.full


def test_no_changed_paths_falls_back_to_full_suite():
    sel = rt.select([])
    assert sel.full
    assert "no changed paths" in sel.fallback


def test_docs_change_is_scoped():
    sel = rt.select(["docs/framebuffer-and-iso.md", "README.md"])
    assert not sel.full
    assert "tests/test_docs_framebuffer_iso.py" in sel.suites
    assert "tests/test_help_docs.py" in sel.suites


def test_skill_change_maps_to_the_doc_tests():
    sel = rt.select([".cursor/skills/github-release/SKILL.md"])
    assert not sel.full
    assert "tests/test_release_tests.py" in sel.suites
    assert "tests/test_loop_docs.py" in sel.suites


def test_release_tooling_change_maps_to_this_test():
    sel = rt.select(["scripts/release_tests.py"])
    assert not sel.full
    assert sel.suites == ["tests/test_release_tests.py"]


def test_changed_test_module_runs_itself():
    sel = rt.select(["tests/test_wordpad.py"])
    assert not sel.full
    assert sel.suites == ["tests/test_wordpad.py"]


def test_test_helper_change_falls_back_to_full_suite():
    for path in ("tests/conftest.py", "tests/ihelp_util.py", "tests/artifacts_util.py"):
        sel = rt.select([path])
        assert sel.full, path


def test_selection_unions_and_dedupes():
    sel = rt.select(
        ["mmbasic/src/cmd_wordpad.c", "mmbasic/src/cmd_juke.c"]
    )
    assert not sel.full
    assert sorted(sel.suites) == sorted(
        [
            "tests/test_wordpad.py",
            "tests/test_wordpad_fonts.py",
            "tests/test_juke.py",
        ]
    )
    assert len(sel.suites) == len(set(sel.suites))


def test_selection_is_deterministic():
    changed = ["mmbasic/src/cmd_wordpad.c", "mmbasic/src/cmd_juke.c"]
    assert rt.select(changed).suites == rt.select(changed).suites


# --------------------------------------------------------------------------
# Mapping integrity
# --------------------------------------------------------------------------


def test_every_rule_names_are_unique():
    names = [rule.name for rule in rt.RULES]
    assert len(names) == len(set(names))


def test_every_rule_resolves_to_existing_test_files():
    stale = []
    for rule in rt.RULES:
        if not rt._expand_suites(rule.suites):
            stale.append(rule.name)
    assert not stale, f"rules with no existing suites: {stale}"


# --------------------------------------------------------------------------
# Output (plan / notes)
# --------------------------------------------------------------------------


def test_plan_names_suites_and_reasons():
    sel = rt.select(["mmbasic/src/cmd_wordpad.c"])
    text = rt.format_plan(sel, ["mmbasic/src/cmd_wordpad.c"], "v1.0.0")
    assert "tests/test_wordpad.py" in text
    assert "why" not in text  # no placeholder leaked
    assert "wordpad" in text
    assert "mmbasic/src/cmd_wordpad.c" in text


def test_plan_says_full_suite_with_the_fallback_reason():
    sel = rt.select(["mmbasic/src/core.c"])
    text = rt.format_plan(sel, ["mmbasic/src/core.c"], "v1.0.0")
    assert "FULL SUITE" in text
    assert "mmbasic/src/core.c" in text


def test_notes_section_names_suites_and_why():
    sel = rt.select(["mmbasic/src/cmd_wordpad.c"])
    notes = rt.format_notes(sel, "v1.0.0")
    assert notes.startswith("## Tests")
    assert "tests/test_wordpad.py" in notes
    assert "wordpad" in notes
    assert "v1.0.0" in notes


def test_notes_section_for_full_suite_explains_the_fallback():
    sel = rt.select(["mmbasic/src/core.c"])
    notes = rt.format_notes(sel, "v1.0.0")
    assert "Full test suite" in notes
    assert "mmbasic/src/core.c" in notes


# --------------------------------------------------------------------------
# CLI + wiring
# --------------------------------------------------------------------------


def test_since_ref_normalises_versions_and_refs():
    assert rt._since_ref("1.2.3") == "v1.2.3"
    assert rt._since_ref("v1.2.3") == "v1.2.3"
    assert rt._since_ref("0.0.0") is None
    assert rt._since_ref("") is None
    assert rt._since_ref("origin/master") == "origin/master"


def test_changed_paths_without_a_baseline_is_empty():
    assert rt.changed_paths("0.0.0") == []


def test_plan_cli_accepts_explicit_files():
    out = subprocess.run(
        ["python3", SELECTOR, "plan", "--files", "mmbasic/src/cmd_wordpad.c"],
        cwd=REPO,
        check=True,
        capture_output=True,
        text=True,
    ).stdout
    assert "tests/test_wordpad.py" in out


def test_wrapper_is_executable_and_delegates_to_the_selector():
    assert os.path.isfile(WRAPPER)
    mode = os.stat(WRAPPER).st_mode
    assert mode & stat.S_IXUSR
    assert "release_tests.py" in _read(WRAPPER)


def test_release_notes_include_the_tests_section():
    out = subprocess.run(
        ["bash", GITHUB_RELEASE, "release-notes", "9.9.9"],
        cwd=REPO,
        check=True,
        capture_output=True,
        text=True,
    ).stdout
    assert "## Tests" in out


def test_release_skill_documents_mapping_and_fallback():
    text = _read(RELEASE_SKILL)
    assert "release-tests.sh" in text
    assert "release_tests.py" in text
    assert "## Tests" in text
    flat = " ".join(text.replace("**", "").split()).lower()
    assert "full suite" in flat
    assert "back to the" in flat


def test_parallel_skill_uses_scoped_release_tests():
    text = _read(PARALLEL_SKILL)
    assert "release-tests.sh" in text
    assert "once, on `master`, before the release" in text
    assert "full suite once on `master`" not in text
