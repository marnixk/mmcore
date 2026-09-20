"""Host test: FTP LIST/NLST line framing (bare-LF warning regression).

The FTP server builds directory listings as CRLF-terminated lines. A file
entry that is not NUL-terminated at its newline separator leaks the following
entries into the same line, so tnftp reports "bare linefeeds received in ASCII
mode" and duplicates the next name. This compiles the real cmd_ftp.c against a
small shim and checks the exact bytes written to the data connection.
"""

import os
import subprocess

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def test_ftp_list_frames_each_entry_with_crlf(tmp_path):
    src = os.path.join(REPO, "tests", "ftp_list_host.c")
    impl = os.path.join(REPO, "mmbasic", "src", "cmd_ftp.c")
    shim = os.path.join(REPO, "tests", "ftp_shim")
    exe = os.path.join(tmp_path, "ftp_list_host")
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
    assert "all checks passed" in out.stdout
