"""Host test for the per-console string-temporary arena (#1351).

Compiles the real arena code from ``mmbasic/src/util.c`` with a small driver.
The driver creates two ``mmb`` contexts and proves each owns its own chunk
list: allocation, per-line reset, mark/release and free on one console never
disturb the other. It also proves a long per-line reset loop reuses one chunk.

The QEMU two-console test lives in ``tests/test_console_switch.py``
(``test_string_arena_is_per_console``); this host test checks the allocator
itself without booting the interpreter.
"""
import os
import shutil
import subprocess
import sys

import pytest

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

INCLUDES = [
    "-DMMB_PLATFORM_POSIX",
    "-I",
    os.path.join(REPO, "native"),
    "-I",
    os.path.join(REPO, "mmbasic", "include"),
    "-I",
    os.path.join(REPO, "mmbasic", "src"),
    "-I",
    os.path.join(REPO, "mmbasic", "third_party"),
    "-I",
    os.path.join(REPO, "console"),
]


@pytest.fixture(scope="module")
def host_bin(tmp_path_factory):
    if shutil.which("cc") is None:
        pytest.skip("needs a host C toolchain (cc)")
    out = tmp_path_factory.mktemp("arena")
    # util.c includes "mmb_version.h"; supply one so this test does not depend
    # on a generated console/ header.
    (out / "mmb_version.h").write_text(
        '#define MMB_VERSION "arena-host-test"\n', encoding="utf-8"
    )
    exe = str(out / "arena_host")
    gc = (
        ["-Wl,-dead_strip"]
        if sys.platform == "darwin"
        else ["-Wl,--gc-sections"]
    )
    subprocess.run(
        [
            "cc",
            "-std=c11",
            "-O0",
            "-g",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-ffunction-sections",
            "-fdata-sections",
            "-I",
            str(out),
            *INCLUDES,
            "-o",
            exe,
            os.path.join(REPO, "tests", "arena_host.c"),
            os.path.join(REPO, "mmbasic", "src", "util.c"),
            *gc,
        ],
        check=True,
        cwd=REPO,
    )
    return exe


def test_arena_is_per_console_and_reset_reuses_chunks(host_bin):
    out = subprocess.run(
        [host_bin], check=True, capture_output=True, text=True
    )
    assert "all checks passed" in out.stdout
