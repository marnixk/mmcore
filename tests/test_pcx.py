"""PCX codec host test (#631).

Compiles mmbasic/src/pcx.c against a tiny G/alloc shim and checks the encode /
decode round-trip, RLE edge cases, 24-bit and malformed-input handling.
"""

import os
import subprocess

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def test_pcx_codec(tmp_path):
    src = os.path.join(REPO, "tests", "pcx_host.c")
    impl = os.path.join(REPO, "mmbasic", "src", "pcx.c")
    shim = os.path.join(REPO, "tests", "pcx_shim")
    include = os.path.join(REPO, "mmbasic", "src")
    exe = os.path.join(str(tmp_path), "pcx_host")
    subprocess.run(
        [
            "cc",
            "-O0",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            shim,
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
