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


SDL_BIN = os.path.join(REPO, "linux", "mmbasic-sdl")


def _ppm_pixels(path):
    with open(path, "rb") as f:
        assert f.readline().strip() == b"P6"
        w, h = map(int, f.readline().split())
        f.readline()
        data = f.read()
    return w, h, data


def _run_sdl_dump(mmb_linux, program, tmp_path, name):
    if not os.path.isfile(SDL_BIN):
        pytest.skip("SDL2 backend not built (pkg-config sdl2 missing)")
    ppm = os.path.join(str(tmp_path), name + ".ppm")
    env = dict(os.environ, SDL_VIDEODRIVER="dummy", MMB_SDL_DUMP=ppm)
    subprocess.run(
        [SDL_BIN],
        input=program,
        text=True,
        capture_output=True,
        timeout=120,
        env=env,
    )
    assert os.path.isfile(ppm), "expected a framebuffer dump"
    return _ppm_pixels(ppm)


def test_ansi_console_renders_text(mmb_linux, tmp_path):
    """LN-05 (#457): the ANSI console paints text into the framebuffer."""
    w, h, data = _run_sdl_dump(mmb_linux, 'PRINT "RENDER ME"\n', tmp_path, "text")
    nonblack = sum(
        1 for i in range(0, len(data), 3) if data[i] or data[i + 1] or data[i + 2]
    )
    assert nonblack > 100, "no text pixels rendered"


def test_ansi_console_colour(mmb_linux, tmp_path):
    """LN-05 (#457): SGR colour from COLOUR reaches the framebuffer."""
    w, h, data = _run_sdl_dump(
        mmb_linux, 'COLOUR 10\nPRINT "GREENPIX"\n', tmp_path, "green"
    )
    green = 0
    for i in range(0, len(data), 3):
        r, g, b = data[i], data[i + 1], data[i + 2]
        if g > r + 40 and g > b + 40:
            green += 1
    assert green > 50, "expected green glyph pixels for COLOUR 10"


def test_sdl_backend_headless(mmb_linux):
    """LN-03 (#455): SDL2 core opens a window and runs the interpreter.

    Uses the dummy video driver so the check is headless; the process exits on
    stdin EOF.
    """
    if not os.path.isfile(SDL_BIN):
        pytest.skip("SDL2 backend not built (pkg-config sdl2 missing)")

    env = dict(os.environ, SDL_VIDEODRIVER="dummy")
    proc = subprocess.run(
        [SDL_BIN],
        input='PRINT 2+3\nPRINT "HELLO"\n',
        text=True,
        capture_output=True,
        timeout=120,
        env=env,
    )
    out = proc.stdout + proc.stderr
    assert proc.returncode == 0
    assert "MMBasic" in out
    assert "5" in out
    assert "HELLO" in out
