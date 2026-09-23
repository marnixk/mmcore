"""LN-01 (#453): native host build boots and runs the interpreter.

Builds ``native/mmbasic`` (headless stdio platform) and checks the acceptance
behaviour: startup banner, immediate-mode PRINT, and RUN of a ramdisk .BAS.
"""
import os
import plistlib
import shutil
import socket
import struct
import subprocess
import sys
import threading
import zipfile

import pytest

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(REPO, "native", "mmbasic")

pytestmark = pytest.mark.skipif(
    shutil.which("cc") is None or shutil.which("make") is None,
    reason="needs a host C toolchain (cc/make)",
)


@pytest.fixture(scope="module")
def mmb_linux():
    subprocess.run(
        ["bash", os.path.join(REPO, "scripts", "build-native.sh")],
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


def test_quit_ends_headless_repl(mmb_linux):
    """QUIT at the prompt ends the headless app and drops later input."""
    proc = subprocess.run(
        [mmb_linux],
        input='PRINT "BEFORE"\nQUIT\nPRINT "AFTER"\n',
        text=True,
        capture_output=True,
        timeout=120,
    )
    out = proc.stdout + proc.stderr
    assert proc.returncode == 0
    assert "BEFORE" in out
    assert "AFTER" not in out, "QUIT must not run the next line"


def test_quit_stops_running_program(mmb_linux):
    """QUIT inside RUN stops the program and ends the app."""
    program = '10 PRINT "ONE"\n20 QUIT\n30 PRINT "TWO"\nRUN\n'
    proc = subprocess.run(
        [mmb_linux], input=program, text=True, capture_output=True, timeout=120
    )
    out = proc.stdout + proc.stderr
    assert proc.returncode == 0
    assert "ONE" in out
    assert "TWO" not in out



def test_run_ramdisk_program(mmb_linux):
    out = _run(mmb_linux, 'RUN "A:/apps/HELLO.BAS"\n')
    assert "Hello from A:/apps/HELLO.BAS" in out


def test_runtime_and_host_resolution(mmb_linux):
    """#498: MM.RUNTIME names the native host; MM.HOST.* are its drawable."""
    native = "mac" if sys.platform == "darwin" else "linux"
    out = _run(mmb_linux, "PRINT MM.RUNTIME$\nPRINT MM.RUNTIME\n")
    assert out.count(native) >= 2, out
    assert "?SYNTAX" not in out and "ERROR" not in out.upper()
    out = _run(mmb_linux, "PRINT MM.HOST.HRES, MM.HOST.VRES\n")
    assert "640" in out and "480" in out, out


def test_sdl_integer_scale_viewport(tmp_path):
    """#498: the SDL viewport integer-scales, centres and letterboxes."""
    src = os.path.join(REPO, "tests", "sdl_scale_host.c")
    impl = os.path.join(REPO, "native", "sdl_scale.c")
    exe = os.path.join(str(tmp_path), "sdl_scale_host")
    subprocess.run(
        [
            "cc",
            "-O0",
            "-Wall",
            "-Werror",
            "-I",
            os.path.join(REPO, "native"),
            "-o",
            exe,
            src,
            impl,
        ],
        check=True,
        cwd=REPO,
    )
    out = subprocess.run([exe], check=True, capture_output=True, text=True)
    assert "all checks passed" in out.stdout


def test_libc_sprintf_shim_formats_width_and_precision(tmp_path):
    """#549: the bare-metal sprintf must honour width, zero-pad and precision.

    Native builds use libc's sprintf, so compile libc_shims.c with
    MMB_PLATFORM_POSIX to exercise the shared formatter the Circle shim wraps.
    """
    exe = os.path.join(str(tmp_path), "libc_sprintf_host")
    subprocess.run(
        [
            "cc",
            "-O0",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-DMMB_PLATFORM_POSIX",
            "-o",
            exe,
            os.path.join(REPO, "tests", "libc_sprintf_host.c"),
            os.path.join(REPO, "mmbasic", "src", "libc_shims.c"),
        ],
        check=True,
        cwd=REPO,
    )
    out = subprocess.run([exe], check=True, capture_output=True, text=True)
    assert "all checks passed" in out.stdout, out.stdout + out.stderr


def test_sdl_input_line_capture(tmp_path):
    """A blocking INPUT prompt must keep routing window keys to the program.

    Regression: sdl_read_line blocked on fgetc(stdin) without pumping SDL, so
    typing at an INPUT prompt in the window froze the app.
    """
    sdl = subprocess.run(
        ["pkg-config", "--cflags", "--libs", "sdl2"],
        capture_output=True,
        text=True,
    )
    if sdl.returncode != 0:
        pytest.skip("SDL2 not found (pkg-config sdl2 missing)")
    exe = os.path.join(str(tmp_path), "sdl_input_host")
    subprocess.run(
        [
            "cc",
            "-O0",
            "-Wall",
            "-Werror",
            "-DMMB_PLATFORM_POSIX",
            "-I",
            os.path.join(REPO, "native"),
            "-I",
            os.path.join(REPO, "mmbasic", "include"),
            "-I",
            os.path.join(REPO, "mmbasic", "third_party"),
            "-I",
            os.path.join(REPO, "console"),
            *sdl.stdout.split(),
            "-o",
            exe,
            os.path.join(REPO, "tests", "sdl_input_host.c"),
            os.path.join(REPO, "native", "sdl_input.c"),
        ],
        check=True,
        cwd=REPO,
    )
    env = dict(os.environ, SDL_VIDEODRIVER="dummy")
    out = subprocess.run(
        [exe], check=True, capture_output=True, text=True, env=env
    )
    assert "all checks passed" in out.stdout


def test_sdl_clipboard_fallback(tmp_path):
    """#525: the SDL host clipboard falls back to an in-process buffer.

    The dummy video driver has no OS clipboard, so copy/paste must still
    round-trip within the session. Linux-native only: the bare-metal Pi
    leaves both platform clipboard hooks NULL.
    """
    sdl = subprocess.run(
        ["pkg-config", "--cflags", "--libs", "sdl2"],
        capture_output=True,
        text=True,
    )
    if sdl.returncode != 0:
        pytest.skip("SDL2 not found (pkg-config sdl2 missing)")
    exe = os.path.join(str(tmp_path), "sdl_clipboard_host")
    subprocess.run(
        [
            "cc",
            "-O0",
            "-Wall",
            "-Werror",
            "-DMMB_PLATFORM_POSIX",
            "-I",
            os.path.join(REPO, "native"),
            *sdl.stdout.split(),
            "-o",
            exe,
            os.path.join(REPO, "tests", "sdl_clipboard_host.c"),
            os.path.join(REPO, "native", "sdl_clipboard.c"),
        ],
        check=True,
        cwd=REPO,
    )
    out = subprocess.run(
        [exe],
        check=True,
        capture_output=True,
        text=True,
        env=dict(os.environ, SDL_VIDEODRIVER="dummy"),
    )
    assert "all checks passed" in out.stdout


def test_sdl_console_writes_do_not_present_per_char(tmp_path):
    """Console writes must only mark dirty, not present each character.

    Regression: sdl_console_write presented at the end of every call, so the
    line editor's one-character-per-write redraws ran one vsync frame apart and
    the prompt cursor slid across the line instead of jumping like the Pi.
    """
    exe = os.path.join(str(tmp_path), "sdl_console_present_host")
    subprocess.run(
        [
            "cc",
            "-O0",
            "-Wall",
            "-Werror",
            "-DMMB_PLATFORM_POSIX",
            "-I",
            os.path.join(REPO, "native"),
            "-I",
            os.path.join(REPO, "mmbasic", "include"),
            "-I",
            os.path.join(REPO, "mmbasic", "third_party"),
            "-I",
            os.path.join(REPO, "console"),
            "-o",
            exe,
            os.path.join(REPO, "tests", "sdl_console_present_host.c"),
            os.path.join(REPO, "native", "sdl_console.c"),
        ],
        check=True,
        cwd=REPO,
    )
    out = subprocess.run([exe], check=True, capture_output=True, text=True)
    assert "all checks passed" in out.stdout


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


def test_default_drive_root_creates_only_c(mmb_linux, tmp_path):
    """Default root is $HOME/.mmbasic; only the persistent C: drive is created."""
    home = tmp_path / "home"
    home.mkdir()
    env = dict(os.environ, HOME=str(home))
    env.pop("MMB_DRIVE_ROOT", None)
    _run_env(mmb_linux, "", env)
    assert (home / ".mmbasic" / "C").is_dir()
    for letter in "DEFGH":
        assert not (home / ".mmbasic" / letter).exists()
    for letter in "CDEFGH":
        assert not (home / letter).exists()


def test_unmounted_d_drive_is_absent(mmb_linux, tmp_path):
    """D: is not created unless --drive mounts it."""
    root = tmp_path / "root"
    env = dict(os.environ, MMB_DRIVE_ROOT=str(root))
    out = _run_env(mmb_linux, 'DIR "D:/"\n', env)
    assert (root / "C").is_dir()
    assert not (root / "D").exists()
    assert "?DRIVE" in out.upper()


def test_drive_cli_mount(mmb_linux, tmp_path):
    """--drive DIR binds that host directory to the D: drive."""
    mount = tmp_path / "usb"
    mount.mkdir()
    env = dict(os.environ, MMB_DRIVE_ROOT=str(tmp_path / "root"))
    program = (
        'OPEN "D:/F.TXT" FOR OUTPUT AS #1\n'
        'PRINT #1,"ON D"\n'
        'CLOSE #1\n'
        'DIR "D:/"\n'
    )
    proc = subprocess.run(
        [mmb_linux, "--drive", str(mount)],
        input=program,
        text=True,
        capture_output=True,
        timeout=120,
        env=env,
    )
    out = proc.stdout + proc.stderr
    assert "F.TXT" in out
    assert (mount / "F.TXT").read_text().strip() == "ON D"


def test_posix_drive_label_and_eject(mmb_linux, tmp_path):
    """#518/#523: a mounted drive shows a friendly name and can be ejected."""
    mount = tmp_path / "usb"
    mount.mkdir()
    (mount / "F.TXT").write_text("hi")
    env = dict(os.environ, MMB_DRIVE_ROOT=str(tmp_path / "root"))
    program = (
        'DRIVE "D:"\n'
        "DRIVE\n"
        'EJECT "D:"\n'
        'DIR "D:/"\n'
    )
    proc = subprocess.run(
        [mmb_linux, "--drive", str(mount)],
        input=program,
        text=True,
        capture_output=True,
        timeout=120,
        env=env,
    )
    out = proc.stdout + proc.stderr
    assert 'D: USB "usb"' in out, out
    assert "D: USB ejected" in out, out
    # Once ejected the volume is gone until it is re-inserted.
    assert "?DRIVE" in out.upper(), out


def test_posix_hotplug_notice(mmb_linux, tmp_path):
    """#518/#523: a volume appearing/disappearing raises a notice."""
    import time

    root = tmp_path / "root"
    env = dict(os.environ, MMB_DRIVE_ROOT=str(root))
    p = subprocess.Popen(
        [mmb_linux],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
        env=env,
    )

    def send(line):
        p.stdin.write(line + "\n")
        p.stdin.flush()
        time.sleep(0.4)

    try:
        send("PRINT 1")
        (root / "D").mkdir(parents=True, exist_ok=True)
        send("PRINT 2")
        (root / "D").rmdir()
        send("PRINT 3")
        send("QUIT")
        out = p.communicate(timeout=10)[0]
    finally:
        if p.poll() is None:
            p.kill()
    assert "D: USB mounted" in out, out
    assert "D: USB removed" in out, out


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


SDL_BIN = os.path.join(REPO, "native", "mmcore")


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


def _is_prompt_grey(rgb):
    r, g, b = rgb
    return (
        abs(r - g) <= 20
        and abs(g - b) <= 20
        and 120 <= r <= 200
        and r + g + b < 620
    )


def _solid_grey_cell(data, w, col, row):
    hits = 0
    for y in range(16):
        for x in range(8):
            i = ((row * 16 + y) * w + col * 8 + x) * 3
            if _is_prompt_grey((data[i], data[i + 1], data[i + 2])):
                hits += 1
    return hits >= 90


def test_prompt_block_cursor_visible(mmb_linux, tmp_path):
    """The SDL prompt must show a solid block cursor like the Pi (issue: no
    active cursor in the AppImage window)."""
    w, h, data = _run_sdl_dump(mmb_linux, "PRINT 1+1\n", tmp_path, "cursor")
    found = [
        (col, row)
        for row in range(20)
        for col in range(24)
        if _solid_grey_cell(data, w, col, row)
    ]
    assert found, "no solid prompt cursor in the SDL framebuffer"


def test_prompt_cursor_survives_mode(mmb_linux, tmp_path):
    """MODE repaints the screen; the prompt cursor must come back."""
    w, h, data = _run_sdl_dump(mmb_linux, "MODE 8\n", tmp_path, "cursor_mode")
    found = [
        (col, row)
        for row in range(20)
        for col in range(30)
        if _solid_grey_cell(data, w, col, row)
    ]
    assert found, "prompt cursor missing after MODE"


@pytest.mark.parametrize("command", ["HELP", "WORDPAD"])
def test_fullscreen_apps_hide_prompt_cursor(mmb_linux, tmp_path, command):
    """The REPL cursor/prompt must not survive into a full-screen TUI app."""
    w, h, data = _run_sdl_dump(mmb_linux, command + "\n", tmp_path, command.lower())
    found = [
        (col, row)
        for row in range(4)
        for col in range(12)
        if _solid_grey_cell(data, w, col, row)
    ]
    assert not found, f"{command} shows a stale prompt cursor at {found}"


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
        "BOX 200,200,50,50,1,RGB(0,255,0)\n"
        "PRINT PIXEL(10,10)\n"
    )
    w, h, data = _run_sdl_dump(mmb_linux, program, tmp_path, "shapes")
    assert _pixel_at(data, w, 10, 10) == (255, 0, 0)
    # Box sits below the echoed command rows (y > 80).
    assert _pixel_at(data, w, 200, 200) == (0, 255, 0)
    assert _pixel_at(data, w, 249, 249) == (0, 255, 0)
    assert _pixel_at(data, w, 225, 225) == (0, 0, 0)


SCENE = [
    "CLS",
    "BOX 60,120,200,150,GREEN",
    "LINE 60,120,260,270,RED",
    "CIRCLE 440,200,90,CYAN",
    "LINE 340,120,540,120,YELLOW",
    "PIXEL 500,300,MAGENTA",
]


def _is_red(p):
    r, g, b = p
    return r > 150 and g < 130 and b < 130


def _is_green(p):
    r, g, b = p
    return g > 150 and r < 130 and b < 130


def _is_blue(p):
    r, g, b = p
    return b > 150 and r < 130 and g < 130


def _is_yellow(p):
    r, g, b = p
    return r > 150 and g > 150 and b < 130


def _is_black(p):
    return all(c < 40 for c in p)


def _magick(*args):
    for tool in ("magick", "convert"):
        try:
            r = subprocess.run([tool, *args], capture_output=True)
            if r.returncode == 0 and r.stdout:
                return r.stdout
        except FileNotFoundError:
            pass
    return None


def _shape_pixels(mmb_linux, tmp_path, name, commands):
    w, h, data = _run_sdl_dump(mmb_linux, commands, tmp_path, name)
    return lambda x, y: _pixel_at(data, w, x, y)


def test_graphics_pixel_and_shapes(mmb_linux, tmp_path):
    """LN-21 (#473): exact per-pixel checks matching the Pi graphics tests."""
    p = _shape_pixels(
        mmb_linux, tmp_path, "pixel", "CLS\nPIXEL 300,300,RED\n"
    )
    assert _is_red(p(300, 300))
    assert _is_black(p(300, 340))

    p = _shape_pixels(
        mmb_linux, tmp_path, "line", "CLS\nLINE 50,200,300,200,GREEN\n"
    )
    assert _is_green(p(175, 200))
    assert _is_black(p(175, 260))

    p = _shape_pixels(
        mmb_linux, tmp_path, "box", "CLS\nBOX 100,120,200,150,BLUE\n"
    )
    assert _is_blue(p(200, 120))
    assert _is_blue(p(100, 195))
    assert _is_black(p(200, 195))

    p = _shape_pixels(
        mmb_linux, tmp_path, "circle", "CLS\nCIRCLE 320,240,100,YELLOW\n"
    )
    assert _is_yellow(p(420, 240))
    assert _is_black(p(320, 240))

    p = _shape_pixels(
        mmb_linux,
        tmp_path,
        "circle_fill",
        "CLS\nCIRCLE 200,180,60,1,RGB(255,0,0),RGB(0,255,0)\n",
    )
    assert _is_green(p(200, 180))
    assert _is_red(p(260, 180))
    assert _is_black(p(270, 180))


def test_scene_matches_pi_golden(mmb_linux, tmp_path):
    """LN-21 (#473): the CMM2 scene matches the Pi golden within 2%."""
    golden = os.path.join(REPO, "tests", "golden", "scene.png")
    if not os.path.isfile(golden):
        pytest.skip("no golden scene")
    w, h, data = _run_sdl_dump(mmb_linux, "\n".join(SCENE) + "\n", tmp_path, "scene")
    assert (w, h) == (1280, 720)
    ref = _magick(golden, "-depth", "8", "rgb:-")
    if ref is None:
        pytest.skip("ImageMagick not available")
    n = min(len(ref), len(data))
    diff = 0
    for i in range(0, n, 3):
        d = max(
            abs(data[i] - ref[i]),
            abs(data[i + 1] - ref[i + 1]),
            abs(data[i + 2] - ref[i + 2]),
        )
        if d > 5:
            diff += 1
    ratio = diff / (n / 3)
    assert ratio < 0.02, "scene differs from Pi golden by %.4f%%" % (ratio * 100)


def test_audio_backend_runs(mmb_linux, tmp_path):
    """LN-10 (#462): PLAY TONE drives the SDL audio hooks without crashing."""
    if not os.path.isfile(SDL_BIN):
        pytest.skip("SDL2 backend not built (pkg-config sdl2 missing)")
    env = dict(
        os.environ,
        SDL_VIDEODRIVER="dummy",
        SDL_AUDIODRIVER="dummy",
        MMB_DRIVE_ROOT=str(tmp_path / "root"),
    )
    proc = subprocess.run(
        [SDL_BIN],
        input='PLAY TONE 440,50\nPAUSE 200\nPRINT "AUDIO_OK"\n',
        text=True,
        capture_output=True,
        timeout=60,
        env=env,
    )
    assert proc.returncode == 0
    assert "AUDIO_OK" in (proc.stdout + proc.stderr)


def _run_sdl_timed(tmp_path, first_line, quit_line, wait, extra_env=None, args=None):
    if not os.path.isfile(SDL_BIN):
        pytest.skip("SDL2 backend not built (pkg-config sdl2 missing)")
    import time as _time

    env = dict(os.environ, SDL_VIDEODRIVER="dummy", MMB_DRIVE_ROOT=str(tmp_path / "root"))
    if extra_env:
        env.update(extra_env)
    p = subprocess.Popen(
        [SDL_BIN] + list(args or []),
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        env=env,
    )
    p.stdin.write(first_line)
    p.stdin.flush()
    _time.sleep(wait)
    p.stdin.write(quit_line)
    p.stdin.flush()
    _time.sleep(0.2)
    p.stdin.close()
    out = p.stdout.read()
    p.wait(timeout=20)
    return out, p.returncode


def test_term_demo_session(mmb_linux, tmp_path):
    """LN-15 (#467): TERM's local demo host renders and the menu appears."""
    out, rc = _run_sdl_timed(tmp_path, 'TERM "demo",23\n', "\x01x\n", 1.2)
    assert rc == 0
    assert "SYNTAX ERROR" not in out
    assert ("Echo ON" in out) or ("Bookmarks" in out) or ("Boxed" in out)


def test_term_native_presents_session(mmb_linux, tmp_path):
    """TERM must present its session page on the native framebuffer.

    Regression for the black window on `--term host:port`: with no async DMA
    hook the present fell back to the generic path, which sources
    ``display_page`` (the console page) while TERM paints PAGE 2, so the
    session was never shown.
    """
    if not os.path.isfile(SDL_BIN):
        pytest.skip("SDL2 backend not built (pkg-config sdl2 missing)")
    ppm = os.path.join(str(tmp_path), "term.ppm")
    env = dict(os.environ, SDL_VIDEODRIVER="dummy", MMB_SDL_DUMP=ppm)
    subprocess.run(
        [SDL_BIN, "--term", "demo"],
        input="",
        text=True,
        capture_output=True,
        timeout=60,
        env=env,
    )
    w, h, data = _ppm_pixels(ppm)
    nonblack = sum(
        1 for i in range(0, len(data), 3) if data[i] or data[i + 1] or data[i + 2]
    )
    assert nonblack > 200, "TERM painted nothing to the native framebuffer"


def test_connect_loopback_session(mmb_linux, tmp_path):
    """LN-17 (#469) / LN-20 (#472): CONNECT displays data from a TCP server."""
    import socket
    import threading
    import time

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", 0))
    srv.listen(1)
    port = srv.getsockname()[1]

    def serve():
        conn, _ = srv.accept()
        conn.sendall(b"HELLO TELNET\r\n")
        time.sleep(1.5)
        conn.close()
        srv.close()

    threading.Thread(target=serve, daemon=True).start()
    out, rc = _run_sdl_timed(
        tmp_path, 'CONNECT "127.0.0.1",%d\n' % port, "\x01x\n", 1.0
    )
    assert rc == 0
    assert "HELLO TELNET" in out


def test_tcp_client_loopback(mmb_linux, tmp_path):
    """LN-18 (#470): TCP client connect/send/recv over POSIX sockets."""
    import socket
    import threading
    import time

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", 0))
    srv.listen(1)
    port = srv.getsockname()[1]
    got = {}

    def serve():
        conn, _ = srv.accept()
        got["req"] = conn.recv(64)
        conn.sendall(b"PONG\n")
        time.sleep(0.2)
        conn.close()
        srv.close()

    thread = threading.Thread(target=serve, daemon=True)
    thread.start()

    program = (
        'OPEN "TCP:127.0.0.1:%d" AS #1\n'
        "PAUSE 200\n"
        'PRINT #1,"PING"\n'
        "PAUSE 200\n"
        "LINE INPUT #1, A$\n"
        "PRINT A$\n"
        "CLOSE #1\n" % port
    )
    env = dict(os.environ, MMB_DRIVE_ROOT=str(tmp_path / "root"))
    out = _run_env(mmb_linux, program, env)
    thread.join(timeout=5)
    assert got.get("req") == b"PING\n"
    assert "PONG" in out


@pytest.mark.parametrize("command", ["FILES", "WORDPAD", "HELP", "AFK"])
def test_tui_apps_render(mmb_linux, tmp_path, command):
    """The fullscreen apps take over the framebuffer through the TUI hooks."""
    w, h, data = _run_sdl_dump(mmb_linux, command + "\n", tmp_path, command)
    nonblack = sum(
        1 for i in range(0, len(data), 3) if data[i] or data[i + 1] or data[i + 2]
    )
    assert nonblack > 500, "app painted nothing"


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


def test_quit_ends_sdl_app(mmb_linux):
    """QUIT at the prompt closes the SDL app instead of returning a prompt."""
    if not os.path.isfile(SDL_BIN):
        pytest.skip("SDL2 backend not built (pkg-config sdl2 missing)")
    env = dict(os.environ, SDL_VIDEODRIVER="dummy")
    proc = subprocess.run(
        [SDL_BIN],
        input='PRINT "BEFORE"\nQUIT\nPRINT "AFTER"\n',
        text=True,
        capture_output=True,
        timeout=120,
        env=env,
    )
    out = proc.stdout + proc.stderr
    assert proc.returncode == 0
    assert "BEFORE" in out
    assert "AFTER" not in out, "QUIT must end the SDL app"


def _make_app(path):
    """A minimal packaged app: a zip with MAIN.BAS at the root."""
    with zipfile.ZipFile(path, "w") as z:
        z.writestr("MAIN.BAS", 'PRINT "APP MARKER"\n')
    return path


def _app_env(tmp_path):
    return dict(os.environ, MMB_DRIVE_ROOT=str(tmp_path / "root"))


def test_cli_app_runs_package_headless(mmb_linux, tmp_path):
    """#490: a positional .app runs as an app VM and the process exits."""
    app = _make_app(tmp_path / "HELLO.APP")
    proc = subprocess.run(
        [mmb_linux, str(app)],
        input="",
        text=True,
        capture_output=True,
        timeout=120,
        env=_app_env(tmp_path),
    )
    out = proc.stdout + proc.stderr
    assert proc.returncode == 0
    assert "APP MARKER" in out
    assert "> " not in out, "app VM must not drop to a REPL prompt"


def test_cli_app_repl_flag_stays(mmb_linux, tmp_path):
    """#490: --repl returns to the REPL after the package ends."""
    app = _make_app(tmp_path / "STAY.APP")
    proc = subprocess.run(
        [mmb_linux, "--repl", str(app)],
        input="PRINT 2+3\n",
        text=True,
        capture_output=True,
        timeout=120,
        env=_app_env(tmp_path),
    )
    out = proc.stdout + proc.stderr
    assert proc.returncode == 0
    assert "APP MARKER" in out
    assert "> 5" in out, "--repl should leave a working prompt"


def test_cli_app_sdl_sealed_no_repl(mmb_linux, tmp_path):
    """#490: the SDL build runs an .app and exits with no REPL."""
    if not os.path.isfile(SDL_BIN):
        pytest.skip("SDL2 backend not built (pkg-config sdl2 missing)")
    app = _make_app(tmp_path / "SDLAPP.APP")
    env = dict(os.environ, SDL_VIDEODRIVER="dummy", MMB_DRIVE_ROOT=str(tmp_path / "root"))
    proc = subprocess.run(
        [SDL_BIN, str(app)],
        input="",
        text=True,
        capture_output=True,
        timeout=120,
        env=env,
    )
    out = proc.stdout + proc.stderr
    assert proc.returncode == 0
    assert "APP MARKER" in out
    assert "> " not in out


def test_cli_term_sealed_exits(mmb_linux, tmp_path):
    """#491: --term runs a sealed TERM session; Ctrl+C cannot reach a prompt."""
    out, rc = _run_sdl_timed(
        tmp_path,
        "",
        "\x03\x01x\n",  # Ctrl+C then Alt+X (quit)
        1.2,
        args=["--term", "demo"],
    )
    assert rc == 0
    assert ("Echo ON" in out) or ("Bookmarks" in out) or ("Boxed" in out)
    assert "> " not in out, "sealed TERM must not drop to a REPL prompt"


def test_cli_term_host_parser(mmb_linux, tmp_path):
    """#491: --term accepts host:port; a failed connect exits, no REPL."""
    proc = subprocess.run(
        [mmb_linux, "--term", "127.0.0.1:1"],
        input="",
        text=True,
        capture_output=True,
        timeout=120,
        env=_app_env(tmp_path),
    )
    out = proc.stdout + proc.stderr
    assert proc.returncode == 0
    assert "> " not in out


@pytest.mark.skipif(
    sys.platform != "darwin" or shutil.which("iconutil") is None,
    reason="macOS app packaging needs darwin + iconutil",
)
def test_macos_app_bundle(mmb_linux, tmp_path):
    """Package the SDL build into a self-contained mmcore.app + zip."""
    if not os.path.isfile(SDL_BIN):
        pytest.skip("SDL2 backend not built (pkg-config sdl2 missing)")
    dist = tmp_path / "dist"
    env = dict(os.environ, DIST=str(dist), MMCORE_SKIP_SIGN="1", VERSION="9.9.9")
    subprocess.run(
        ["bash", os.path.join(REPO, "scripts", "package-macos-app.sh")],
        cwd=REPO,
        check=True,
        capture_output=True,
        text=True,
        env=env,
    )

    app = dist / "mmcore.app"
    contents = app / "Contents"
    exe = contents / "MacOS" / "mmcore"
    fw = contents / "Frameworks" / "libSDL2-2.0.0.dylib"
    assert exe.is_file()
    assert fw.is_file(), "SDL2 must be bundled into the app"
    assert (contents / "Resources" / "AppIcon.icns").is_file()

    with open(contents / "Info.plist", "rb") as f:
        plist = plistlib.load(f)
    assert plist["CFBundleIdentifier"] == "com.marnixk.mmcore"
    assert plist["CFBundleExecutable"] == "mmcore"
    assert plist["CFBundleShortVersionString"] == "9.9.9"

    deps = subprocess.run(
        ["otool", "-L", str(exe)], check=True, capture_output=True, text=True
    ).stdout
    assert "@rpath/libSDL2-2.0.0.dylib" in deps, "install name must be rewritten"

    with zipfile.ZipFile(dist / "mmcore-macos-arm64.zip") as z:
        names = z.namelist()
    assert any(n.startswith("mmcore.app/Contents/MacOS/mmcore") for n in names)

    # arm64 refuses to launch a modified, unsigned binary, so ad-hoc sign the
    # bundled dylib and app before the headless smoke test.
    for target in (fw, app):
        subprocess.run(
            ["codesign", "--force", "--sign", "-", str(target)],
            check=True,
            capture_output=True,
        )
    proc = subprocess.run(
        [str(exe)],
        input='PRINT "MACOS_BUNDLE_OK"\n',
        text=True,
        capture_output=True,
        timeout=120,
        env=dict(os.environ, SDL_VIDEODRIVER="dummy"),
    )
    assert "MACOS_BUNDLE_OK" in (proc.stdout + proc.stderr)


@pytest.mark.skipif(
    sys.platform != "darwin",
    reason="macOS app packaging needs darwin",
)
def test_macos_release_requires_notarization(tmp_path):
    """A release build must refuse to produce an unnotarized bundle."""
    env = dict(
        os.environ,
        DIST=str(tmp_path / "dist"),
        MMCORE_REQUIRE_NOTARY="1",
        MMCORE_SKIP_SIGN="1",
        VERSION="9.9.9",
    )
    proc = subprocess.run(
        ["bash", os.path.join(REPO, "scripts", "package-macos-app.sh")],
        cwd=REPO,
        text=True,
        capture_output=True,
        env=env,
    )
    assert proc.returncode != 0
    assert "NOTARY" in (proc.stdout + proc.stderr).upper()


def test_native_packaging_derives_icons_from_branding():
    """#537: AppImage and .app icons derive from the shared branding art."""
    gen = open(os.path.join(REPO, "scripts", "gen-appicon.py"), encoding="utf-8").read()
    mac = open(
        os.path.join(REPO, "scripts", "package-macos-app.sh"), encoding="utf-8"
    ).read()
    lin = open(
        os.path.join(REPO, "scripts", "package-linux-appimage.sh"), encoding="utf-8"
    ).read()
    assert "assets" in gen and "mmcore-app-icon.png" in gen
    assert "gen-appicon.py" in mac and "--iconset" in mac
    assert "gen-appicon.py" in lin and "mmcore.png" in lin


NTP_UNIX_DELTA = 2208988800
NTP_TARGET = 1790157296  # 2026-09-23 09:54:56 UTC


def _ntp_server(srv, target, got):
    srv.settimeout(20)
    try:
        data, addr = srv.recvfrom(512)
    except OSError:
        return
    got.append(data)
    resp = bytearray(48)
    resp[0] = (4 << 3) | 4  # LI 0, VN 4, mode 4 (server)
    resp[24:32] = data[40:48]  # originate echoes our transmit nonce
    resp[40:44] = struct.pack("!I", target + NTP_UNIX_DELTA)
    srv.sendto(bytes(resp), addr)


def test_ntp_sync_from_loopback_server(mmb_linux):
    """The native UDP transport really speaks NTP and applies the timezone."""
    srv = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    srv.bind(("127.0.0.1", 0))
    port = srv.getsockname()[1]
    got = []
    th = threading.Thread(
        target=_ntp_server, args=(srv, NTP_TARGET, got), daemon=True
    )
    th.start()
    cmds = [
        f'OPTION NTP SERVER "127.0.0.1:{port}"',
        'OPTION TIMEZONE "Europe/Amsterdam"',
        "NTP",
        "PRINT DATETIME$(EPOCH(NOW))",
        "PRINT TIME$",
        "QUIT",
    ]
    try:
        proc = subprocess.Popen(
            [mmb_linux],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        out, _ = proc.communicate("\n".join(cmds) + "\n", timeout=90)
    finally:
        srv.close()
    th.join(timeout=5)
    assert got, "NTP server saw no request"
    assert "Time synced:" in out, out
    # Amsterdam standard time is UTC+1: 09:54:56 UTC -> 10:54:56 local.
    assert "23-09-2026 10:54:5" in out, out
    assert "10:54:5" in out, out
    assert "?SYNTAX ERROR" not in out.upper(), out
