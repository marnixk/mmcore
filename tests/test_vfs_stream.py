"""Host test: A: ramdisk streaming writes (mmb_vfs_wopen/wwrite/wclose).

FTP STOR opens the target once and appends received chunks. On the ramdisk the
backing buffer grows geometrically. This compiles the real vfs.c against a
small shim and streams a multi-hundred-KB payload, then checks append and
truncate semantics.
"""

import os
import subprocess

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def test_vfs_ramdisk_streaming_write(tmp_path):
    src = os.path.join(REPO, "tests", "vfs_stream_host.c")
    impl = os.path.join(REPO, "mmbasic", "src", "vfs.c")
    shim = os.path.join(REPO, "tests", "vfs_shim")
    exe = os.path.join(tmp_path, "vfs_stream_host")
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
