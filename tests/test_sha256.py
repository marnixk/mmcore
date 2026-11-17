"""SHA-256 standard vectors for the HUB checksum (#1525).

Compiles the self-contained mmbasic/src/sha256.c against a tiny host harness.
No QEMU or TLS involved.
"""

import os
import shutil
import subprocess

import pytest

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

pytestmark = pytest.mark.skipif(
    shutil.which("cc") is None,
    reason="needs a host C compiler",
)


def test_sha256_standard_vectors(tmp_path):
    src = os.path.join(REPO, "tests", "sha256_host.c")
    impl = os.path.join(REPO, "mmbasic", "src", "sha256.c")
    include = os.path.join(REPO, "mmbasic", "src")
    exe = os.path.join(str(tmp_path), "sha256_host")
    subprocess.run(
        [
            "cc",
            "-O0",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            include,
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
