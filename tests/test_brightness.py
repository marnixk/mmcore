"""MM.BRIGHTNESS% reads/writes a sysfs backlight, defaulting to 100.

The device-directory helpers (``mmbasic/src/brightness.c``) are compiled and
driven on the host against a fake sysfs tree pytest writes, so the percentage
maths and clamping are checked without a real backlight. A native-binary test
then proves the variable is wired into the token/function tables and the
assignment path.
"""
import os
import re
import shutil
import subprocess

import pytest

from native_harness import build_native

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(REPO, "native", "mmbasic")
ANSI = re.compile(r"\x1b\[[0-9;]*m")

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
    exe = str(tmp_path_factory.mktemp("brightness") / "brightness_host")
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
            os.path.join(REPO, "tests", "brightness_host.c"),
            os.path.join(REPO, "mmbasic", "src", "brightness.c"),
        ],
        check=True,
        cwd=REPO,
    )
    return exe


def _dev(dirpath, max_brightness, brightness=None, actual_brightness=None):
    os.makedirs(dirpath, exist_ok=True)
    with open(os.path.join(dirpath, "max_brightness"), "w") as fh:
        fh.write(f"{max_brightness}\n")
    if brightness is not None:
        with open(os.path.join(dirpath, "brightness"), "w") as fh:
            fh.write(f"{brightness}\n")
    if actual_brightness is not None:
        with open(os.path.join(dirpath, "actual_brightness"), "w") as fh:
            fh.write(f"{actual_brightness}\n")
    return str(dirpath)


def _read(host_bin, devdir, fallback, expected):
    out = subprocess.run(
        [host_bin, "read", devdir, str(fallback), str(expected)],
        check=True,
        capture_output=True,
        text=True,
    )
    assert f"-> {expected}" in out.stdout


def _write(host_bin, devdir, pct, expected_raw):
    out = subprocess.run(
        [host_bin, "write", devdir, str(pct), str(expected_raw)],
        check=True,
        capture_output=True,
        text=True,
    )
    assert f"-> {expected_raw}" in out.stdout


@pytest.mark.parametrize(
    "max_b,cur,expected",
    [
        (100, 100, 100),
        (100, 0, 0),
        (100, 42, 42),
        (255, 128, 50),  # rounded to nearest percent
        (255, 255, 100),
        (937, 468, 50),
        (100, 150, 100),  # clamped down
        (100, -5, 0),  # clamped up
    ],
)
def test_read_percentage(host_bin, tmp_path, max_b, cur, expected):
    dev = _dev(tmp_path / "intel_backlight", max_b, brightness=cur)
    _read(host_bin, dev, 100, expected)


def test_read_falls_back_to_actual_brightness(host_bin, tmp_path):
    dev = _dev(
        tmp_path / "panel",
        max_brightness=200,
        actual_brightness=50,
    )
    _read(host_bin, dev, 100, 25)


@pytest.mark.parametrize(
    "max_b,brightness",
    [
        (None, 50),  # no max_brightness
        (0, 50),  # invalid max
        (100, None),  # no current value
        (100, "abc"),  # unparseable current
    ],
)
def test_read_falls_back(host_bin, tmp_path, max_b, brightness):
    devdir = tmp_path / "dev"
    os.makedirs(devdir, exist_ok=True)
    if max_b is not None:
        (devdir / "max_brightness").write_text(f"{max_b}\n")
    if brightness is not None:
        (devdir / "brightness").write_text(f"{brightness}\n")
    _read(host_bin, str(devdir), 100, 100)
    _read(host_bin, str(devdir), 7, 7)


def test_read_falls_back_when_missing(host_bin, tmp_path):
    _read(host_bin, str(tmp_path / "nope"), 100, 100)
    _read(host_bin, "", 100, 100)


@pytest.mark.parametrize(
    "max_b,pct,expected_raw",
    [
        (100, 50, 50),
        (100, 0, 0),
        (100, 100, 100),
        (100, 150, 100),  # clamped
        (100, -5, 0),  # clamped
        (255, 50, 128),  # rounded to nearest raw step
        (937, 25, 234),
    ],
)
def test_write_percentage(host_bin, tmp_path, max_b, pct, expected_raw):
    dev = _dev(tmp_path / "intel_backlight", max_b, brightness=max_b)
    _write(host_bin, dev, pct, expected_raw)


def test_write_no_max_fails(host_bin, tmp_path):
    dev = tmp_path / "dev"
    os.makedirs(dev, exist_ok=True)
    proc = subprocess.run(
        [host_bin, "write", str(dev), "50", "0"],
        capture_output=True,
        text=True,
    )
    assert proc.returncode != 0


@pytest.mark.skipif(
    shutil.which("cc") is None or shutil.which("make") is None,
    reason="needs a host C toolchain (cc/make)",
)
def test_brightness_wired_into_interpreter(tmp_path):
    """The native binary reads and assigns MM.BRIGHTNESS% against a fake
    backlight directory selected with MMB_BACKLIGHT."""
    build_native()
    assert os.path.isfile(BIN), "native build produced no binary"
    dev = _dev(tmp_path / "panel", max_brightness=100, brightness=37)
    env = dict(
        os.environ,
        MMB_DRIVE_ROOT=str(tmp_path / "root"),
        MMB_BACKLIGHT=dev,
    )
    proc = subprocess.run(
        [BIN],
        input=(
            "PRINT MM.BRIGHTNESS%\n"
            "MM.BRIGHTNESS% = 42\n"
            "PRINT MM.BRIGHTNESS%\n"
            "MM.BRIGHTNESS% = 150\n"
            "PRINT MM.BRIGHTNESS%\n"
            "PRINT MM.BRIGHTNESS%()\n"
        ),
        text=True,
        capture_output=True,
        timeout=120,
        env=env,
    )
    out = proc.stdout + proc.stderr
    values = []
    for line in ANSI.sub("", out).splitlines():
        if line.startswith("> "):
            try:
                values.append(int(line[2:].strip()))
            except ValueError:
                pass
    assert values[:4] == [37, 42, 100, 100], out
