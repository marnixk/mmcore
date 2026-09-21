"""LN-01 (#453): native host build boots and runs the interpreter.

Builds ``linux/mmbasic`` (headless stdio platform) and checks the acceptance
behaviour: startup banner, immediate-mode PRINT, and RUN of a ramdisk .BAS.
"""
import os
import shutil
import subprocess

import pytest

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(REPO, "linux", "mmbasic")

pytestmark = pytest.mark.skipif(
    shutil.which("cc") is None or shutil.which("make") is None,
    reason="needs a host C toolchain (cc/make)",
)


@pytest.fixture(scope="module")
def mmb_linux():
    subprocess.run(
        ["bash", os.path.join(REPO, "scripts", "build-linux.sh")],
        cwd=REPO,
        check=True,
        capture_output=True,
    )
    assert os.path.isfile(BIN), "native build produced no binary"
    return BIN


def _run(binary, text):
    proc = subprocess.run(
        [binary], input=text, text=True, capture_output=True, timeout=120
    )
    return proc.stdout + proc.stderr


def test_banner_and_immediate_print(mmb_linux):
    out = _run(mmb_linux, 'PRINT 2+3\nPRINT "HELLO"\n')
    assert "MMBasic" in out
    assert "> 5" in out
    assert "> HELLO" in out


def test_run_ramdisk_program(mmb_linux):
    out = _run(mmb_linux, 'RUN "A:/apps/HELLO.BAS"\n')
    assert "Hello from A:/apps/HELLO.BAS" in out
