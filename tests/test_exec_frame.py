"""Host test for the execution frame type (#1347).

Compiles ``mmbasic/src/exec_frame.c`` with a small driver, so the capture and
restore primitives are checked without booting the interpreter. The driver
sets every covered field, captures, clobbers, restores, and compares; it also
proves ``running`` and ``end_req`` are not part of the restored state.
"""
import os
import shutil
import subprocess

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
    exe = str(tmp_path_factory.mktemp("exec_frame") / "exec_frame_host")
    subprocess.run(
        [
            "cc",
            "-std=c11",
            "-O0",
            "-g",
            "-Wall",
            "-Wextra",
            "-Werror",
            *INCLUDES,
            "-o",
            exe,
            os.path.join(REPO, "tests", "exec_frame_host.c"),
            os.path.join(REPO, "mmbasic", "src", "exec_frame.c"),
        ],
        check=True,
        cwd=REPO,
    )
    return exe


def test_capture_restore_is_identity_and_keeps_lifecycle(host_bin):
    out = subprocess.run(
        [host_bin], check=True, capture_output=True, text=True
    )
    assert "all checks passed" in out.stdout
