"""Host test: the native POSIX storage read cache (#1151).

Compiles the real ``native/storage_posix.c`` against a tiny shim and checks
``mmb_fat_read_at`` keeps one open handle per volume across forward reads,
invalidates it on write/rename/unlink, and still returns the right bytes.
"""

import os
import shutil
import subprocess

import pytest

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

pytestmark = pytest.mark.skipif(
    shutil.which("cc") is None,
    reason="needs a host C toolchain (cc)",
)


def test_storage_posix_read_cache(tmp_path):
    src = os.path.join(REPO, "tests", "storage_posix_host.c")
    impl = os.path.join(REPO, "native", "storage_posix.c")
    exe = os.path.join(str(tmp_path), "storage_posix_host")
    subprocess.run(
        [
            "cc",
            "-O0",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-DMMB_PLATFORM_POSIX",
            "-I",
            os.path.join(REPO, "native"),
            "-I",
            os.path.join(REPO, "mmbasic", "include"),
            "-I",
            os.path.join(REPO, "mmbasic", "src"),
            "-I",
            os.path.join(REPO, "mmbasic", "third_party"),
            "-o",
            exe,
            src,
            impl,
        ],
        check=True,
        cwd=REPO,
    )
    out = subprocess.run([exe], check=True, capture_output=True, text=True)
    assert "all checks passed" in out.stdout, out.stdout + out.stderr
