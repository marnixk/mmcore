"""Virtual-console framebuffer snapshot bookkeeping (#786).

The bare-metal restore path keeps a per-console snapshot buffer whose
allocated capacity grows to the largest mode it has ever captured. When a
console later shrinks (e.g. 640x480 boot -> PAINT's MODE 18 640x360), the
captured length is smaller than that capacity. Restore must copy only the
captured bytes for the live geometry; copying the stale capacity overran the
smaller framebuffer and corrupted the display path.

``console/console_state.h`` holds the pure length decision so it can be
exercised on the host.
"""

import os
import shutil
import subprocess

import pytest

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

PROG = r"""
#include "console_state.h"
#include <stdio.h>

int main(void)
{
	int fails = 0;

	/* #786: a stale 1280x720 capacity (1843200 bytes) carried into a
	 * 640x360 restore (460800 bytes) must never be copied. */
	if (mmb_console_fb_restore_len(1280, 360, 1843200, 1280, 360) != 0)
	{
		printf("FAIL: stale capacity accepted\n");
		fails++;
	}

	/* A snapshot captured for exactly the live geometry is copied whole. */
	if (mmb_console_fb_restore_len(1280, 360, 460800, 1280, 360) != 460800)
	{
		printf("FAIL: exact snapshot rejected\n");
		fails++;
	}

	/* Geometry mismatches are never copied. */
	if (mmb_console_fb_restore_len(1280, 480, 614400, 1280, 360) != 0)
	{
		printf("FAIL: mismatched rows accepted\n");
		fails++;
	}
	if (mmb_console_fb_restore_len(2560, 720, 1843200, 1280, 360) != 0)
	{
		printf("FAIL: mismatched pitch accepted\n");
		fails++;
	}
	if (mmb_console_fb_restore_len(1280, 360, 0, 1280, 360) != 0)
	{
		printf("FAIL: empty snapshot accepted\n");
		fails++;
	}

	if (fails)
		return 1;
	printf("all checks passed\n");
	return 0;
}
"""


def test_restore_len_rejects_stale_capacity(tmp_path):
    if shutil.which("cc") is None:
        pytest.skip("needs a host C toolchain (cc)")
    src = os.path.join(tmp_path, "console_state_host.c")
    exe = os.path.join(tmp_path, "console_state_host")
    with open(src, "w", encoding="utf-8") as fh:
        fh.write(PROG)
    subprocess.run(
        [
            "cc", "-std=c11", "-O0", "-g", "-Wall", "-Wextra", "-Werror",
            "-I", os.path.join(REPO, "console"),
            "-o", exe, src,
        ],
        check=True,
        cwd=REPO,
    )
    out = subprocess.run([exe], check=True, capture_output=True, text=True)
    assert "all checks passed" in out.stdout
