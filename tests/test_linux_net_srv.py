"""LN-20 (#472): the POSIX TCP server contract (used by the FTP server).

Compiles native/net_posix.c with the host test and checks listen/accept/recv/
send with the same non-blocking semantics cmd_ftp.c expects.
"""
import os
import shutil
import subprocess

import pytest

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

pytestmark = pytest.mark.skipif(
    shutil.which("cc") is None, reason="needs a host C toolchain"
)


def test_posix_tcp_server_round_trip(tmp_path):
    exe = os.path.join(str(tmp_path), "net_srv_host")
    subprocess.run(
        [
            "cc",
            "-O0",
            "-DMMB_PLATFORM_POSIX",
            "-I",
            os.path.join(REPO, "mmbasic", "include"),
            "-I",
            os.path.join(REPO, "mmbasic", "third_party"),
            "-I",
            os.path.join(REPO, "console"),
            "-o",
            exe,
            os.path.join(REPO, "tests", "net_srv_host.c"),
            os.path.join(REPO, "native", "net_posix.c"),
        ],
        check=True,
        cwd=REPO,
    )
    out = subprocess.run([exe], check=True, capture_output=True, text=True)
    assert "all checks passed" in out.stdout
