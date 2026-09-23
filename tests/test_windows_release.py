"""The Windows native build ships as a release asset."""

import importlib.util
import os
import struct
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCRIPTS = os.path.join(REPO, "scripts")
WORKFLOW = os.path.join(REPO, ".github", "workflows", "windows.yml")
ASSET = "mmcore-windows-x86_64.zip"


def _run(args, **kwargs):
    return subprocess.run(args, check=True, text=True, capture_output=True, **kwargs)


def _load_gen_appicon():
    path = os.path.join(SCRIPTS, "gen-appicon.py")
    spec = importlib.util.spec_from_file_location("gen_appicon", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def test_windows_scripts_parse_and_are_executable():
    for name in ("build-windows.sh", "package-windows.sh"):
        path = os.path.join(SCRIPTS, name)
        _run(["bash", "-n", path])
        assert os.access(path, os.X_OK), name


def test_package_windows_names_the_release_asset():
    pkg = open(os.path.join(SCRIPTS, "package-windows.sh"), encoding="utf-8").read()
    assert ASSET in pkg
    assert "SDL2.dll" in pkg
    assert "libwinpthread-1.dll" in pkg


def test_makefile_builds_net_win_and_separates_objects():
    mk = open(os.path.join(REPO, "native", "Makefile"), encoding="utf-8").read()
    assert "net_win.c" in mk
    assert "TARGET_WINDOWS" in mk
    assert "SDL_MAIN_HANDLED" in mk
    for name in ("net_win.c", "win_compat.h", "win/sys/select.h"):
        assert os.path.exists(os.path.join(REPO, "native", *name.split("/"))), name


def test_release_notes_list_the_windows_asset():
    notes = _run([os.path.join(SCRIPTS, "github-release.sh"), "release-notes", "9.9.9"]).stdout
    assert ASSET in notes
    assert "Windows native" in notes
    rel = open(os.path.join(SCRIPTS, "github-release.sh"), encoding="utf-8").read()
    assert ASSET in rel


def test_windows_workflow_attaches_to_releases():
    wf = open(WORKFLOW, encoding="utf-8").read()
    assert "release:" in wf
    assert "types: [published]" in wf
    assert "windows-latest" in wf
    assert "msys2/setup-msys2" in wf
    assert "scripts/package-windows.sh" in wf
    assert "gh release upload" in wf
    assert ASSET in wf


def test_windows_packaging_derives_icon_from_branding():
    """#537: the .exe icon comes from the shared branding art, not the old blue
    procedural tile."""
    gen = open(os.path.join(SCRIPTS, "gen-appicon.py"), encoding="utf-8").read()
    assert "assets" in gen and "mmcore-app-icon.png" in gen
    build = open(os.path.join(SCRIPTS, "build-windows.sh"), encoding="utf-8").read()
    pkg = open(os.path.join(SCRIPTS, "package-windows.sh"), encoding="utf-8").read()
    assert "--ico" in build and "gen-appicon.py" in build
    assert "--ico" in pkg and "gen-appicon.py" in pkg and "mmcore.ico" in pkg
    mk = open(os.path.join(REPO, "native", "Makefile"), encoding="utf-8").read()
    assert "windres" in mk and "mmcore.rc" in mk


def test_gen_appicon_emits_branding_png_and_ico(tmp_path):
    gen = os.path.join(SCRIPTS, "gen-appicon.py")
    mod = _load_gen_appicon()
    w, h, bpp, src = mod.load_branding()
    assert (w, h) == (1024, 1024), "branding art must stay 1024x1024"

    rgba = mod.resize(src, w, h, bpp, 64, 64)
    reds = sum(
        1
        for i in range(0, len(rgba), 4)
        if rgba[i] > 120 and rgba[i + 1] < 90 and rgba[i + 2] < 90
    )
    silvers = sum(
        1
        for i in range(0, len(rgba), 4)
        if min(rgba[i], rgba[i + 1], rgba[i + 2]) > 140
    )
    assert reds > 0, "branding art should carry red energy streaks"
    assert silvers > 0, "branding art should carry the chrome M"

    png = tmp_path / "icon256.png"
    _run([sys.executable, gen, str(png), "256"])
    data = png.read_bytes()
    assert data[:8] == b"\x89PNG\r\n\x1a\n"
    assert struct.unpack(">II", data[16:24]) == (256, 256)

    ico = tmp_path / "mmcore.ico"
    _run([sys.executable, gen, "--ico", str(ico)])
    idata = ico.read_bytes()
    assert idata[:4] == b"\x00\x00\x01\x00", "ICO header"
    count = struct.unpack("<H", idata[4:6])[0]
    assert count == len(mod.ICO_SIZES)
    for i in range(count):
        off = 6 + 16 * i
        blob_off = struct.unpack("<I", idata[off + 12 : off + 16])[0]
        assert idata[blob_off : blob_off + 8] == b"\x89PNG\r\n\x1a\n"
