"""Build-generator tests: generated ramdisk mode (#882) and the parallel-make
mmb_version.h race (#897).

These exercise the real Makefile rules and scripts in a miniature repo, so
they fail if a build links another build's ramdisk table or exposes a missing
version header under concurrent generation.
"""

import os
import shutil
import subprocess
import time
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[1]
GEN = REPO / "scripts" / "gen_ramdisk.py"
VERSION_SCRIPT = REPO / "scripts" / "gen-mmb-version.sh"
VFS = REPO / "mmbasic" / "src" / "vfs.c"
CONSOLE_MAKEFILE = REPO / "console" / "Makefile"
NATIVE_MAKEFILE = REPO / "native" / "Makefile"


def _have_tools():
    return (
        shutil.which("make") is not None
        and shutil.which("python3") is not None
        and shutil.which("bash") is not None
    )


def _mini_repo(root: Path) -> None:
    (root / "mmbasic" / "src").mkdir(parents=True, exist_ok=True)
    shutil.copy(VFS, root / "mmbasic" / "src" / "vfs.c")
    (root / "circle").mkdir(exist_ok=True)
    (root / "circle" / "Rules.mk").write_text("")
    (root / "scripts").mkdir(exist_ok=True)
    shutil.copy(GEN, root / "scripts" / "gen_ramdisk.py")
    (root / "console").mkdir(exist_ok=True)
    shutil.copy(CONSOLE_MAKEFILE, root / "console" / "Makefile")
    (root / "native").mkdir(exist_ok=True)
    shutil.copy(NATIVE_MAKEFILE, root / "native" / "Makefile")
    (root / "ramdisk" / "tests").mkdir(parents=True, exist_ok=True)
    (root / "ramdisk" / "tests" / "t.bas").write_text("PRINT 1\n")
    (root / "ramdisk" / "lib").mkdir(exist_ok=True)
    (root / "ramdisk" / "lib" / "a.inc").write_text("A\n")


def _console_build(repo: Path, exclude: str = ""):
    return subprocess.run(
        [
            "make",
            "-C",
            str(repo / "console"),
            "../mmbasic/src/ramdisk_data.c",
            "RAMDISK_EXCLUDE=" + exclude,
        ],
        capture_output=True,
        text=True,
    )


def _native_build(repo: Path):
    out = repo / "mmbasic" / "src" / "ramdisk_data.c"
    return subprocess.run(
        ["make", "-C", str(repo / "native"), str(out)],
        capture_output=True,
        text=True,
    )


def test_makefiles_force_ramdisk_regeneration():
    """#882: both builds must re-run the generator so a shared ramdisk_data.c
    cannot keep the other mode's table."""
    console = CONSOLE_MAKEFILE.read_text()
    native = NATIVE_MAKEFILE.read_text()
    assert "ramdisk_data.c: FORCE" in console
    assert "RAMDISK_C): FORCE" in native


@pytest.mark.skipif(not _have_tools(), reason="needs make/python3/bash")
def test_ramdisk_mode_not_shared_between_console_and_native(tmp_path):
    """#882: a native build must not link a console release's exclude=tests
    table (and vice versa) just because the shared generated file looks newer
    than this build's own stamp."""
    repo = Path(os.path.realpath(tmp_path))
    _mini_repo(repo)
    out = repo / "mmbasic" / "src" / "ramdisk_data.c"

    # Native first: it embeds the whole tree and writes its own stamp.
    res = _native_build(repo)
    assert res.returncode == 0, res.stderr + res.stdout
    text = out.read_text()
    assert "mode: exclude=-" in text
    assert '"tests/t.bas"' in text

    time.sleep(1.1)
    # A console release build rewrites the shared file with exclude=tests.
    res = _console_build(repo, exclude="tests")
    assert res.returncode == 0, res.stderr + res.stdout
    text = out.read_text()
    assert '"tests/t.bas"' not in text
    assert "mode: exclude=tests" in text

    time.sleep(1.1)
    # The next native build must regenerate: it embeds the whole tree again.
    res = _native_build(repo)
    assert res.returncode == 0, res.stderr + res.stdout
    text = out.read_text()
    assert "mode: exclude=-" in text
    assert '"tests/t.bas"' in text, "native linked the console exclude=tests table"

    time.sleep(1.1)
    # And the reverse: console after native must restore exclude=tests.
    res = _console_build(repo, exclude="tests")
    assert res.returncode == 0, res.stderr + res.stdout
    text = out.read_text()
    assert "mode: exclude=tests" in text
    assert '"tests/t.bas"' not in text, "console linked the native full table"


@pytest.mark.skipif(not _have_tools(), reason="needs make/python3")
def test_ramdisk_regen_does_not_rewrite_unchanged(tmp_path):
    """FORCE must not rewrite ramdisk_data.c when its content is unchanged, or
    every build would relink the kernel."""
    repo = Path(os.path.realpath(tmp_path))
    _mini_repo(repo)
    out = repo / "mmbasic" / "src" / "ramdisk_data.c"

    res = _native_build(repo)
    assert res.returncode == 0, res.stderr + res.stdout
    first = out.stat().st_mtime_ns

    res = _native_build(repo)
    assert res.returncode == 0, res.stderr + res.stdout
    assert out.stat().st_mtime_ns == first


def test_gen_mmb_version_concurrent_invocations(tmp_path):
    """#897: the header is FORCEd and shared by several objects, so parallel
    make can invoke the recipe concurrently. No invocation may delete the
    header out from under a compiler."""
    out = tmp_path / "mmb_version.h"
    env = dict(os.environ, MMB_VERSION="v1.2.3-race")
    procs = [
        subprocess.Popen(
            ["bash", str(VERSION_SCRIPT), str(out)],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            env=env,
        )
        for _ in range(16)
    ]
    failures = []
    for proc in procs:
        _stdout, stderr = proc.communicate(timeout=60)
        if proc.returncode != 0:
            failures.append((proc.returncode, stderr))
    assert not failures, failures

    text = out.read_text()
    assert '#define MMB_VERSION "v1.2.3-race"' in text
    assert text.count("#ifndef MMB_VERSION_H") == 1
    # Atomic generation leaves no temp files behind.
    assert not list(tmp_path.glob("mmb_version.h.*"))


def test_gen_mmb_version_rewrites_only_on_change(tmp_path):
    out = tmp_path / "mmb_version.h"
    script = ["bash", str(VERSION_SCRIPT), str(out)]

    subprocess.run(script, check=True, env=dict(os.environ, MMB_VERSION="v1"))
    first = out.stat().st_mtime_ns
    assert '#define MMB_VERSION "v1"' in out.read_text()

    subprocess.run(script, check=True, env=dict(os.environ, MMB_VERSION="v1"))
    assert out.stat().st_mtime_ns == first

    subprocess.run(script, check=True, env=dict(os.environ, MMB_VERSION="v2"))
    assert '#define MMB_VERSION "v2"' in out.read_text()
