"""Host test: packed escan SSIDs, not printable junk in the scan blob."""

import os
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def test_escan_reads_beacon_ssid_not_ie_junk(tmp_path):
    src = os.path.join(REPO, "tests", "wlan_escan_host.c")
    escan = os.path.join(REPO, "console", "wlan_escan.c")
    exe = os.path.join(tmp_path, "wlan_escan_host")
    subprocess.run(
        [
            "gcc",
            "-O0",
            "-Wall",
            "-Werror",
            "-I",
            os.path.join(REPO, "console"),
            "-o",
            exe,
            src,
            escan,
        ],
        check=True,
        cwd=REPO,
    )
    out = subprocess.run([exe], check=True, capture_output=True, text=True)
    assert "all checks passed" in out.stdout
