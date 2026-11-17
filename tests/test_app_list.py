"""Host tests for the grouped APPS row model (#1524).

Compiles mmbasic/src/app_list.c + app_category.c against a tiny G/alloc shim
and checks grouping, fixed category order, sorting, filtering, flat picker
ordering, HUB pinning, navigation and category jumps. No QEMU involved.
"""

import os
import subprocess

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def test_app_list_rows(tmp_path):
    src = os.path.join(REPO, "tests", "app_list_host.c")
    impl = os.path.join(REPO, "mmbasic", "src", "app_list.c")
    cats = os.path.join(REPO, "mmbasic", "src", "app_category.c")
    shim = os.path.join(REPO, "tests", "app_list_shim")
    include = os.path.join(REPO, "mmbasic", "src")
    exe = os.path.join(str(tmp_path), "app_list_host")
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
