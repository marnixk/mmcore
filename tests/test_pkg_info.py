"""Host tests for the PACKAGE.INF metadata parser (#1523).

Compiles mmbasic/src/pkg_info.c + app_category.c against a tiny G/alloc shim
and checks the tolerant parser, build text and dotted-version compare. No
QEMU involved.
"""

import os
import subprocess

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def test_pkg_info_parser(tmp_path):
    src = os.path.join(REPO, "tests", "pkg_info_host.c")
    impl = os.path.join(REPO, "mmbasic", "src", "pkg_info.c")
    cats = os.path.join(REPO, "mmbasic", "src", "app_category.c")
    shim = os.path.join(REPO, "tests", "pkg_info_shim")
    include = os.path.join(REPO, "mmbasic", "src")
    exe = os.path.join(str(tmp_path), "pkg_info_host")
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
            cats,
        ],
        check=True,
        cwd=REPO,
    )
    out = subprocess.run([exe], check=True, capture_output=True, text=True)
    assert "all checks passed" in out.stdout, out.stdout + out.stderr
