"""Host-side tests for the ``scripts/test-watch.py`` progress parser (#1244).

A scoped run logs ``collected 32 items / 28 deselected / 4 selected``; progress
must be measured against the selected count (4), not the pre-deselection total
(32). These tests feed representative pytest log lines through the parser and
need no QEMU.
"""

import importlib.util
import os
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WATCH = os.path.join(REPO, "scripts", "test-watch.py")


def _load():
    spec = importlib.util.spec_from_file_location("test_watch", WATCH)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


tw = _load()


def _snapshot(monkeypatch, tmp_path, lines):
    log = tmp_path / ".pytest-watch.log"
    log.write_text("\n".join(lines) + "\n", encoding="utf-8")
    monkeypatch.setattr(tw, "LOG", str(log))
    return tw.snapshot()


def test_collected_with_deselection_uses_selected_count():
    assert (
        tw._collected_total(
            "collecting ... collected 32 items / 28 deselected / 4 selected"
        )
        == 4
    )


def test_plain_collected_uses_collected_count():
    assert tw._collected_total("collecting ... collected 32 items") == 32


def test_deselection_without_explicit_selected_subtracts():
    assert tw._collected_total("collected 32 items / 28 deselected") == 4


def test_bracket_items_line_still_parses():
    assert tw._collected_total("[32 items]") == 32


def test_running_selected_set_reports_100_percent(monkeypatch, tmp_path):
    lines = [
        "============================= test session starts =============================",
        "collecting ... collected 32 items / 28 deselected / 4 selected",
        "tests/test_tui_prompt_cmm2.py::test_a PASSED [ 25%]",
        "tests/test_tui_prompt_cmm2.py::test_b PASSED [ 50%]",
        "tests/test_tui_prompt_cmm2.py::test_c SKIPPED [ 75%]",
        "tests/test_tui_prompt_cmm2.py::test_d PASSED [100%]",
        "===================== 3 passed, 1 skipped, 28 deselected in 1.2s =====================",
    ]
    total, passed, failed, skipped, last, _ = _snapshot(monkeypatch, tmp_path, lines)
    assert total == 4
    assert (passed, failed, skipped) == (3, 0, 1)
    assert last == "tests/test_tui_prompt_cmm2.py"
    assert "4/4 (100%)" in tw.line()
    assert "pass=3 fail=0 skip=1" in tw.line()


def test_terminal_summary_after_deselection_is_100_percent(monkeypatch, tmp_path):
    lines = [
        "collected 32 items / 28 deselected / 4 selected",
        "tests/test_tui_prompt_cmm2.py::test_a PASSED [ 25%]",
        "tests/test_tui_prompt_cmm2.py::test_b PASSED [ 50%]",
        "tests/test_tui_prompt_cmm2.py::test_c PASSED [ 75%]",
        "tests/test_tui_prompt_cmm2.py::test_d PASSED [100%]",
        "========================= 4 passed, 28 deselected in 1.2s =========================",
    ]
    _snapshot(monkeypatch, tmp_path, lines)
    assert "4/4 (100%)" in tw.line()


def test_progress_never_exceeds_100_percent(monkeypatch, tmp_path):
    lines = [
        "collected 10 items / 6 deselected / 4 selected",
        "tests/test_x.py::test_a PASSED [ 25%]",
        "tests/test_x.py::test_b PASSED [ 50%]",
        "tests/test_x.py::test_c PASSED [ 75%]",
        "tests/test_x.py::test_d PASSED [100%]",
    ]
    total, passed, _, _, _, _ = _snapshot(monkeypatch, tmp_path, lines)
    assert total == 4
    assert passed == 4
    assert "4/4 (100%)" in tw.line()
