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


def _run_env(binary, text, env):
    proc = subprocess.run(
        [binary], input=text, text=True, capture_output=True, timeout=120, env=env
    )
    return proc.stdout + proc.stderr


def test_posix_storage_roundtrip(mmb_linux, tmp_path):
    """LN-08 (#460): physical drive write/list/read over a host directory.

    The lowercase ``c:/a/b.txt`` read also exercises the case-insensitive
    component resolver (the host filesystem is case-sensitive).
    """
    root = tmp_path / "drives"
    env = dict(os.environ, MMB_DRIVE_ROOT=str(root))
    program = (
        'OPEN "C:/A/B.TXT" FOR OUTPUT AS #1\n'
        'PRINT #1,"HELLO STORAGE"\n'
        'CLOSE #1\n'
        'OPEN "c:/a/b.txt" FOR INPUT AS #1\n'
        'LINE INPUT #1, A$\n'
        'PRINT A$\n'
        'CLOSE #1\n'
        'DIR "C:/A"\n'
    )
    out = _run_env(mmb_linux, program, env)
    assert "HELLO STORAGE" in out
    assert "B.TXT" in out
    assert (root / "C" / "A" / "B.TXT").read_text().strip() == "HELLO STORAGE"


def test_posix_storage_case_insensitive_listing(mmb_linux, tmp_path):
    """LN-08 (#460): DIR glob matching is case-insensitive."""
    root = tmp_path / "drives"
    env = dict(os.environ, MMB_DRIVE_ROOT=str(root))
    subprocess.run([mmb_linux], input="", text=True, capture_output=True, env=env)
    target = root / "C" / "Mixed.TXT"
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text("x")
    out = _run_env(mmb_linux, 'DIR "C:/mixed.*"\n', env)
    assert "Mixed.TXT" in out


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


def _pixel_at(data, w, x, y):
    i = (y * w + x) * 3
    return data[i], data[i + 1], data[i + 2]


def test_console_lf_returns_carriage(mmb_linux, tmp_path):
    """LF must behave as CR+LF, else lines staircase to the right."""
    program = 'CLS\nPRINT "AA"\nPRINT "BB"\n'
    w, h, data = _run_sdl_dump(mmb_linux, program, tmp_path, "crlf")
    rows_left = 0
    for y in range(h):
        base = y * w * 3
        if any(data[base + x * 3 + c] for x in range(8) for c in range(3)):
            rows_left += 1
    assert rows_left >= 2, "lines did not return to column 0"


def test_graphics_shapes_and_readback(mmb_linux, tmp_path):
    """LN-07 (#459): MODE/PIXEL/BOX paint the framebuffer and PIXEL() reads."""
    program = (
        "MODE 8,16\n"
        "CLS RGB(0,0,0)\n"
        "PIXEL 10,10,RGB(255,0,0)\n"
        "BOX 20,20,50,50,1,RGB(0,255,0)\n"
        "PRINT PIXEL(10,10)\n"
    )
    w, h, data = _run_sdl_dump(mmb_linux, program, tmp_path, "shapes")
    assert _pixel_at(data, w, 10, 10) == (255, 0, 0)
    assert _pixel_at(data, w, 20, 20) == (0, 255, 0)
    assert _pixel_at(data, w, 69, 69) == (0, 255, 0)
    assert _pixel_at(data, w, 0, 0) == (0, 0, 0)


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
