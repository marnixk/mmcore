"""Host-only regressions for ``scripts/gen_golden.py`` and strict capture.

``gen_golden.py`` regenerates ``tests/golden/scene.png``.  ``capture_png`` may
substitute a writable temp path when the requested destination cannot be
written (#894), so a regeneration that silently fell back must not report
success: the real golden would stay stale while the tool printed
``wrote <golden>`` (#906).  These tests need no QEMU; they stub the console or
the QEMU monitor and exercise only the file-writing contract.
"""

import importlib.util
import os
import subprocess
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[1]
GEN_GOLDEN = REPO / "scripts" / "gen_golden.py"


def _load_gen_golden():
    spec = importlib.util.spec_from_file_location("gen_golden", GEN_GOLDEN)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def test_gen_golden_fails_loudly_when_capture_falls_back(
    monkeypatch, capsys, tmp_path
):
    """#906: a fallback capture must not print ``wrote <golden>``."""
    gen = _load_gen_golden()
    fallback = tmp_path / "fallback.png"

    class FakeConsole:
        def __init__(self, kernel):
            self.kernel = kernel

        def start(self):
            return self

        def stop(self):
            pass

        def send_line(self, cmd):
            return ""

        def capture_png(self, dest_png=None, strict=False):
            fallback.write_bytes(b"png")
            return str(fallback)

    monkeypatch.setattr(gen, "MMBasicConsole", FakeConsole)

    with pytest.raises(SystemExit) as excinfo:
        gen.main()

    message = str(excinfo.value)
    assert str(fallback) in message
    assert "refusing to report success" in message
    assert "wrote" not in capsys.readouterr().out


def test_gen_golden_uses_strict_capture_and_the_exact_path(monkeypatch, capsys):
    """The happy path requests strict mode and prints the requested path."""
    gen = _load_gen_golden()
    seen = {}

    class FakeConsole:
        def __init__(self, kernel):
            self.kernel = kernel

        def start(self):
            return self

        def stop(self):
            pass

        def send_line(self, cmd):
            return ""

        def capture_png(self, dest_png=None, strict=False):
            seen["dest"] = dest_png
            seen["strict"] = strict
            return dest_png

    monkeypatch.setattr(gen, "MMBasicConsole", FakeConsole)

    gen.main()

    expected = os.path.join(gen.GOLDEN_DIR, "scene.png")
    assert seen == {"dest": expected, "strict": True}
    assert capsys.readouterr().out.strip() == "wrote " + expected


def test_strict_capture_raises_instead_of_falling_back(tmp_path, monkeypatch):
    """#906: ``capture_png(strict=True)`` rejects an unwritable destination
    rather than quietly writing somewhere else."""
    from harness import HarnessError, MMBasicConsole

    kernel = tmp_path / "kernel8.img"
    kernel.write_bytes(b"x")
    con = MMBasicConsole(str(kernel))
    try:
        counter = {"n": 0}

        def fake_monitor_cmd(cmd, *args, **kwargs):
            path = cmd.split(" ", 1)[1]
            counter["n"] += 1
            shade = (counter["n"] * 97) % 256
            subprocess.run(
                ["convert", "-size", "1x1", f"xc:rgb({shade},0,0)", path],
                check=True,
                capture_output=True,
            )

        monkeypatch.setattr(con, "drain", lambda *a, **k: b"")
        monkeypatch.setattr(con, "_monitor_drain", lambda *a, **k: b"")
        monkeypatch.setattr(con, "_monitor_cmd", fake_monitor_cmd)

        missing = str(tmp_path / "missing" / "scene.png")
        with pytest.raises(HarnessError) as excinfo:
            con.capture_png(missing, strict=True)
        assert missing in str(excinfo.value)

        # Non-strict callers keep the #894 fallback behaviour.
        fallback = con.capture_png(missing)
        assert fallback != missing
        assert os.path.isfile(fallback)
    finally:
        con.stop()
