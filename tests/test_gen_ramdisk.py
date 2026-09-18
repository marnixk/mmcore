"""Host tests for scripts/gen_ramdisk.py."""

import importlib.util
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
GEN = REPO / "scripts" / "gen_ramdisk.py"
VFS = REPO / "mmbasic" / "src" / "vfs.c"


def _run(tmp, root, out, extra=()):
    cmd = [
        sys.executable,
        str(GEN),
        "--root",
        str(root),
        "--out",
        str(out),
        "--vfs-src",
        str(VFS),
        *extra,
    ]
    return subprocess.run(cmd, capture_output=True, text=True)


def _entries(text):
    return re.findall(r'\{\s*"([^"]*)",\s*(\w+),\s*(\d+)u\s*\}', text)


def test_ramdisk_gen_maps_tree_and_skips_dotfiles(tmp_path):
    root = tmp_path / "ramdisk"
    (root / "lib").mkdir(parents=True)
    (root / "apps").mkdir(parents=True)
    (root / "deep" / "nested").mkdir(parents=True)
    (root / "lib" / "a.inc").write_text("CONST A=1\n")
    blob = bytes(range(256))
    (root / "apps" / "bin.dat").write_bytes(blob)
    (root / "deep" / "nested" / "x.txt").write_text("deep\n")
    (root / ".hidden").write_text("nope\n")
    (root / "lib" / ".secret").write_text("nope\n")
    (root / ".git").mkdir()
    (root / ".git" / "config").write_text("nope\n")

    out = tmp_path / "out.c"
    res = _run(tmp_path, root, out)
    assert res.returncode == 0, res.stderr
    text = out.read_text()
    entries = _entries(text)
    paths = [p for p, _n, _l in entries]
    assert paths == ["apps/bin.dat", "deep/nested/x.txt", "lib/a.inc"], paths
    assert ".hidden" not in text
    assert ".secret" not in text
    assert ".git" not in text
    # Byte-for-byte preservation of the binary file.
    m = re.search(r"static const unsigned char rd_0\[\] = \{(.*?)\};", text, re.S)
    assert m
    vals = re.findall(r"0x([0-9A-Fa-f]{2})", m.group(1))
    assert bytes(int(v, 16) for v in vals) == blob
    # Deterministic: a second run produces identical output.
    res2 = _run(tmp_path, root, tmp_path / "out2.c")
    assert res2.returncode == 0
    assert (tmp_path / "out2.c").read_text() == text


def test_ramdisk_gen_exclude_and_empty(tmp_path):
    root = tmp_path / "ramdisk"
    (root / "lib").mkdir(parents=True)
    (root / "tests").mkdir(parents=True)
    (root / "lib" / "a.inc").write_text("A\n")
    (root / "tests" / "t.bas").write_text("B\n")
    out = tmp_path / "out.c"
    res = _run(tmp_path, root, out, ("--exclude", "tests"))
    assert res.returncode == 0, res.stderr
    text = out.read_text()
    assert "lib/a.inc" in text
    assert "tests/t.bas" not in text

    # Missing ramdisk/ still produces a valid empty table.
    empty_out = tmp_path / "empty.c"
    res = _run(tmp_path, tmp_path / "nope", empty_out)
    assert res.returncode == 0, res.stderr
    empty = empty_out.read_text()
    assert "mmb_ramdisk_count = 0" in empty


def test_ramdisk_gen_rejects_long_component(tmp_path):
    root = tmp_path / "ramdisk"
    root.mkdir()
    longname = "a" * 80 + ".txt"
    (root / longname).write_text("x")
    res = _run(tmp_path, root, tmp_path / "out.c")
    assert res.returncode != 0
    assert "too long" in res.stderr
