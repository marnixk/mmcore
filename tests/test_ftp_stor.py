"""Host test: FTP STOR creates missing parent folders.

tnftp sends the full local path as the remote name when no remote name is
given ("put /home/me/pic.png"). The FTP server sandboxes that under its root,
where the parent folders do not exist, so it used to fail with a misleading
552. This compiles the real cmd_ftp.c against a small shim and checks the
server creates the parents and stores the exact binary payload.
"""

import os
import subprocess

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def test_ftp_stor_creates_missing_parents(tmp_path):
    src = os.path.join(REPO, "tests", "ftp_stor_host.c")
    impl = os.path.join(REPO, "mmbasic", "src", "cmd_ftp.c")
    shim = os.path.join(REPO, "tests", "ftp_shim")
    exe = os.path.join(tmp_path, "ftp_stor_host")
    subprocess.run(
        [
            "gcc",
            "-O0",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            shim,
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
