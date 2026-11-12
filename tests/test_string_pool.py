"""String-pool block boundaries and large allocations (#1501).

The pool rounds every block up to a power-of-two size class in a 64 KiB slab
(from 16 bytes to 64 KiB) and allocates anything larger directly. These tests
walk the boundaries through the interpreter and also grow a string by repeated
doubling, so a slab that cannot hold its largest class (or a direct block put
on the class-16 free list) crashes or corrupts the heap here.

They use the native stdio build, which is also the target used for the ASan
run of this module.
"""
import os
import shutil
import subprocess

import pytest

from native_harness import build_native

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.environ.get("MMB_NATIVE_BIN") or os.path.join(REPO, "native", "mmbasic")

pytestmark = pytest.mark.skipif(
    shutil.which("cc") is None or shutil.which("make") is None,
    reason="needs a host C toolchain (cc/make)",
)


@pytest.fixture(scope="module")
def mmb():
    # MMB_NATIVE_BIN lets a sanitizer run point these tests at a separate
    # ASan build without triggering the normal Makefile rebuild.
    if not os.environ.get("MMB_NATIVE_BIN"):
        build_native()
    assert os.path.isfile(BIN), "native build produced no binary"
    return BIN


def _run(mmb_bin, lines, root, timeout=120):
    program = "\n".join("%d %s" % (10 * (i + 1), line)
                        for i, line in enumerate(lines))
    program += "\nRUN\n"
    env = dict(os.environ, MMB_DRIVE_ROOT=str(root))
    proc = subprocess.run(
        [mmb_bin],
        input=program,
        text=True,
        capture_output=True,
        timeout=timeout,
        env=env,
    )
    out = proc.stdout + proc.stderr
    assert proc.returncode == 0, out[-1200:]
    return out


def test_block_sizes_across_every_class_boundary(mmb, tmp_path):
    # 16 KiB (class 15), 32 KiB and 64 KiB (class 16) live in a slab; 200 KiB
    # and 4 MiB are direct allocations. 65527 is the largest payload whose
    # block still fits a slab.
    sizes = [16000, 32000, 64000, 65527, 65528, 200000, 4 * 1024 * 1024]
    lines = []
    for size in sizes:
        lines += [
            'S$ = STRING$(%d, "x")' % size,
            'PRINT LEN(S$)',
            'S$ = ""',
        ]
    out = _run(mmb, lines, tmp_path)
    for size in sizes:
        assert str(size) in out, (size, out[-800:])


def test_string_doubling_growth(mmb, tmp_path):
    out = _run(
        mmb,
        [
            'S$ = "x"',
            "FOR I = 1 TO 21",
            "  S$ = S$ + S$",
            "NEXT I",
            "PRINT LEN(S$)",
        ],
        tmp_path,
    )
    assert str(2 ** 21) in out, out[-800:]


def test_repeated_alloc_and_free_cycles(mmb, tmp_path):
    out = _run(
        mmb,
        [
            "FOR I = 1 TO 400",
            '  A$ = STRING$(I * 97, "a")',
            "  B$ = A$ + A$",
            '  A$ = ""',
            '  B$ = ""',
            "NEXT I",
            'PRINT "CYCLES-OK"',
        ],
        tmp_path,
        timeout=180,
    )
    assert "CYCLES-OK" in out, out[-800:]
