"""Host tests for the HUB screen layout maths (#1526).

Compiles mmbasic/src/hub_layout.c on its own and checks wide/narrow selection,
the integer pixel unit, region disjointness and that the list is maximised at
1920x1080, 1366x768, 1280x800, 1024x768 and 800x600.
"""

import os
import subprocess

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def test_hub_layout(tmp_path):
    src = os.path.join(REPO, "tests", "hub_layout_host.c")
    impl = os.path.join(REPO, "mmbasic", "src", "hub_layout.c")
    include = os.path.join(REPO, "mmbasic", "src")
    exe = os.path.join(str(tmp_path), "hub_layout_host")
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
