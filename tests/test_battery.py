"""#857: BATTERY%() reads a sysfs capacity on Linux and falls back to 100.

The path-taking helper (``mmbasic/src/battery.c``) is compiled and driven on
the host against files pytest writes, so the parsing and clamping are checked
without a real battery. A native-binary test then proves the function is wired
into the token/function tables, not just the helper.
"""
import os
import shutil
import subprocess
import sys

import pytest

from native_harness import build_native

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(REPO, "native", "mmbasic")
CAPACITY = "/sys/class/power_supply/BAT0/capacity"

INCLUDES = [
    "-DMMB_PLATFORM_POSIX",
    "-I",
    os.path.join(REPO, "native"),
    "-I",
    os.path.join(REPO, "mmbasic", "include"),
    "-I",
    os.path.join(REPO, "mmbasic", "src"),
    "-I",
    os.path.join(REPO, "mmbasic", "third_party"),
    "-I",
    os.path.join(REPO, "console"),
]


@pytest.fixture(scope="module")
def host_bin(tmp_path_factory):
    if shutil.which("cc") is None:
        pytest.skip("needs a host C toolchain (cc)")
    exe = str(tmp_path_factory.mktemp("battery") / "battery_host")
    subprocess.run(
        [
            "cc",
            "-std=c11",
            "-O0",
            "-g",
            "-Wall",
            "-Wextra",
            "-Werror",
            *INCLUDES,
            "-o",
            exe,
            os.path.join(REPO, "tests", "battery_host.c"),
            os.path.join(REPO, "mmbasic", "src", "battery.c"),
        ],
        check=True,
        cwd=REPO,
    )
    return exe


def _check(host_bin, path, fallback, expected):
    out = subprocess.run(
        [host_bin, path, str(fallback), str(expected)],
        check=True,
        capture_output=True,
        text=True,
    )
    assert f"-> {expected}" in out.stdout


@pytest.mark.parametrize(
    "content,expected",
    [
        ("77\n", 77),
        ("100\n", 100),
        ("0\n", 0),
        ("150\n", 100),  # clamped down
        ("-5\n", 0),  # clamped up
        ("42", 42),  # no trailing newline
        ("99 \n", 99),  # trailing spaces
    ],
)
def test_capacity_reads_and_clamps(host_bin, tmp_path, content, expected):
    f = tmp_path / "capacity"
    f.write_text(content)
    _check(host_bin, str(f), 100, expected)


@pytest.mark.parametrize("content", ["", "\n", "abc\n", "not a number\n"])
def test_capacity_falls_back_when_unparseable(host_bin, tmp_path, content):
    f = tmp_path / "capacity"
    f.write_text(content)
    _check(host_bin, str(f), 100, 100)
    _check(host_bin, str(f), 7, 7)


def test_capacity_falls_back_when_missing(host_bin, tmp_path):
    _check(host_bin, str(tmp_path / "nope"), 100, 100)
    _check(host_bin, "", 100, 100)


def _expected_capacity():
    if sys.platform.startswith("linux") and os.path.exists(CAPACITY):
        try:
            with open(CAPACITY) as fh:
                return max(0, min(100, int(fh.read().strip())))
        except (OSError, ValueError):
            pass
    return 100


@pytest.mark.skipif(
    shutil.which("cc") is None or shutil.which("make") is None,
    reason="needs a host C toolchain (cc/make)",
)
def test_battery_function_wired_into_interpreter(tmp_path):
    """The native binary dispatches BATTERY% (bare and parenthesised)."""
    build_native()
    assert os.path.isfile(BIN), "native build produced no binary"
    env = dict(os.environ, MMB_DRIVE_ROOT=str(tmp_path / "root"))
    proc = subprocess.run(
        [BIN],
        input="PRINT BATTERY%\nPRINT BATTERY%()\n",
        text=True,
        capture_output=True,
        timeout=120,
        env=env,
    )
    out = proc.stdout + proc.stderr
    expected = _expected_capacity()
    assert f"> {expected}" in out, out
    assert out.count(f"> {expected}") == 2, out
