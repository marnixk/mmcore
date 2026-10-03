"""Host tests for scripts/gen_ramdisk.py."""

import importlib.util
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[1]
GEN = REPO / "scripts" / "gen_ramdisk.py"
VFS = REPO / "mmbasic" / "src" / "vfs.c"
MAKEFILE = REPO / "console" / "Makefile"
NATIVE_MAKEFILE = REPO / "native" / "Makefile"


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


def test_ramdisk_stamp_tracks_added_and_removed_files(tmp_path):
    """#874: the stamp records the embedded file list, not just the exclude
    string, so a removal still changes it."""
    root = tmp_path / "ramdisk"
    (root / "lib").mkdir(parents=True)
    (root / "lib" / "a.inc").write_text("A\n")
    out = tmp_path / "out.c"
    stamp = tmp_path / "stamp"

    res = _run(tmp_path, root, out, ("--stamp-out", str(stamp)))
    assert res.returncode == 0, res.stderr
    assert stamp.read_text() == "lib/a.inc\n"
    first = stamp.stat().st_mtime_ns
    first_out = out.read_text()

    # An unchanged tree must not rewrite the stamp (or make churn).
    res = _run(tmp_path, root, out, ("--stamp-out", str(stamp)))
    assert res.returncode == 0, res.stderr
    assert stamp.stat().st_mtime_ns == first
    assert out.read_text() == first_out

    # A rename changes the stamp and the generated table.
    (root / "lib" / "a.inc").rename(root / "lib" / "b.inc")
    res = _run(tmp_path, root, out, ("--stamp-out", str(stamp)))
    assert res.returncode == 0, res.stderr
    assert stamp.read_text() == "lib/b.inc\n"
    assert '"lib/a.inc"' not in out.read_text()
    assert '"lib/b.inc"' in out.read_text()


def test_makefile_regenerates_when_ramdisk_file_removed(tmp_path):
    """#874: `make` must rebuild ramdisk_data.c after a ramdisk/ file is
    removed, even though the removed path only drops out of the prerequisite
    list. Exercises console/Makefile's real rules in a miniature repo."""
    make = shutil.which("make")
    if make is None:
        pytest.skip("make is not available")
    if shutil.which("python3") is None:
        pytest.skip("python3 is not available")

    (tmp_path / "mmbasic" / "src").mkdir(parents=True)
    shutil.copy(VFS, tmp_path / "mmbasic" / "src" / "vfs.c")
    (tmp_path / "circle").mkdir()
    (tmp_path / "circle" / "Rules.mk").write_text("")
    (tmp_path / "scripts").mkdir()
    shutil.copy(GEN, tmp_path / "scripts" / "gen_ramdisk.py")
    (tmp_path / "console").mkdir()
    shutil.copy(MAKEFILE, tmp_path / "console" / "Makefile")
    lib = tmp_path / "ramdisk" / "lib"
    lib.mkdir(parents=True)
    (lib / "gone.inc").write_text("BYE\n")
    (lib / "keep.inc").write_text("HI\n")

    out = tmp_path / "mmbasic" / "src" / "ramdisk_data.c"

    def build():
        return subprocess.run(
            [make, "-C", str(tmp_path / "console"), "../mmbasic/src/ramdisk_data.c"],
            capture_output=True,
            text=True,
        )

    res = build()
    assert res.returncode == 0, res.stderr + res.stdout
    assert '"lib/gone.inc"' in out.read_text()

    # Make compares mtimes; keep the regenerated stamp a whole second newer
    # than the first output so this is not a same-timestamp race.
    time.sleep(1.1)
    (lib / "gone.inc").unlink()
    res = build()
    assert res.returncode == 0, res.stderr + res.stdout
    text = out.read_text()
    assert '"lib/gone.inc"' not in text
    assert '"lib/keep.inc"' in text


def test_native_makefile_regenerates_when_ramdisk_file_removed(tmp_path):
    """#879: native/Makefile has the same stale-ramdisk bug #874 fixed for the
    console, so a removed file must still force a regenerate. Exercises the
    real native rules in a miniature repo; native keeps its own stamp."""
    make = shutil.which("make")
    if make is None:
        pytest.skip("make is not available")
    if shutil.which("python3") is None:
        pytest.skip("python3 is not available")

    (tmp_path / "mmbasic" / "src").mkdir(parents=True)
    shutil.copy(VFS, tmp_path / "mmbasic" / "src" / "vfs.c")
    (tmp_path / "scripts").mkdir()
    shutil.copy(GEN, tmp_path / "scripts" / "gen_ramdisk.py")
    (tmp_path / "native").mkdir()
    shutil.copy(NATIVE_MAKEFILE, tmp_path / "native" / "Makefile")
    lib = tmp_path / "ramdisk" / "lib"
    lib.mkdir(parents=True)
    (lib / "gone.inc").write_text("BYE\n")
    (lib / "keep.inc").write_text("HI\n")

    # native/Makefile locates the repo with $(abspath $(CURDIR)/..), and
    # getcwd canonicalises symlinks (macOS /var -> /private/var), so drive it
    # through the same canonical path rather than the raw tmp_path.
    repo = Path(os.path.realpath(tmp_path))
    out = repo / "mmbasic" / "src" / "ramdisk_data.c"
    stamp = repo / "mmbasic" / "src" / "ramdisk_data.native.stamp"

    def build():
        return subprocess.run(
            [make, "-C", str(repo / "native"), str(out)],
            capture_output=True,
            text=True,
        )

    res = build()
    assert res.returncode == 0, res.stderr + res.stdout
    assert '"lib/gone.inc"' in out.read_text()

    # Make compares mtimes; keep the regenerated stamp a whole second newer
    # than the first output so this is not a same-timestamp race.
    time.sleep(1.1)
    (lib / "gone.inc").unlink()
    res = build()
    assert res.returncode == 0, res.stderr + res.stdout
    text = out.read_text()
    assert '"lib/gone.inc"' not in text
    assert '"lib/keep.inc"' in text
    assert stamp.read_text() == "lib/keep.inc\n"
    # The native build uses its own stamp, not the console one.
    assert not (repo / "mmbasic" / "src" / "ramdisk_data.stamp").exists()
