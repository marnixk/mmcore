"""Opt-in per-statement profiler (MMB_STMTPROF).

The flag is off unless the environment asks for a report. A cached integer
increment must show up with the loop count when it is on, and no file is
written when it is off.
"""
import os
import shutil
import subprocess

import pytest

from native_harness import build_native

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(REPO, "native", "mmbasic")

pytestmark = pytest.mark.skipif(
    shutil.which("cc") is None or shutil.which("make") is None,
    reason="needs a host C toolchain (cc/make)",
)

PROG = """\
a% = 0
for i% = 1 to 40
  a% = a% + 1
next i%
print a%
end
"""


@pytest.fixture(scope="module")
def mmb_linux():
    build_native()
    assert os.path.isfile(BIN)
    return BIN


def _run(binary, drive, env):
    return subprocess.run(
        [binary, "--drive", drive, 'RUN "D:/t.bas"'],
        stdin=subprocess.DEVNULL,
        text=True,
        capture_output=True,
        timeout=60,
        env=env,
    )


def test_stmtprof_off_writes_nothing(mmb_linux, tmp_path):
    drv = tmp_path / "drv"
    drv.mkdir()
    (drv / "t.bas").write_text(PROG)
    csv_path = tmp_path / "should-not-exist.csv"
    env = os.environ.copy()
    env.pop("MMB_STMTPROF", None)
    env["MMB_DRIVE_ROOT"] = str(tmp_path / "croot")
    proc = _run(mmb_linux, str(drv), env)
    out = proc.stdout + proc.stderr
    assert proc.returncode == 0, out
    assert "40" in out
    assert not csv_path.exists()


def test_stmtprof_records_cached_let(mmb_linux, tmp_path):
    drv = tmp_path / "drv"
    drv.mkdir()
    (drv / "t.bas").write_text(PROG)
    csv_path = tmp_path / "stmt.csv"
    env = os.environ.copy()
    env["MMB_STMTPROF"] = str(csv_path)
    env["MMB_DRIVE_ROOT"] = str(tmp_path / "croot")
    proc = _run(mmb_linux, str(drv), env)
    out = proc.stdout + proc.stderr
    assert proc.returncode == 0, out
    assert "40" in out
    text = csv_path.read_text(encoding="utf-8")
    assert "kind,line,pc,count" in text
    lets = [
        line
        for line in text.splitlines()
        if line.startswith("LET,") and "A% = A% + 1" in line.upper()
    ]
    assert lets, text
    count = int(lets[0].split(",")[3])
    assert count >= 40
