"""The Windows native build ships as a release asset."""

import base64
import importlib.util
import os
import shutil
import struct
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCRIPTS = os.path.join(REPO, "scripts")
WORKFLOW = os.path.join(REPO, ".github", "workflows", "windows.yml")
SIGN = os.path.join(SCRIPTS, "sign-windows-exe.sh")
BASH = shutil.which("bash") or "/bin/bash"
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
    for name in ("build-windows.sh", "package-windows.sh", "sign-windows-exe.sh"):
        path = os.path.join(SCRIPTS, name)
        _run(["bash", "-n", path])
        assert os.access(path, os.X_OK), name


def test_package_windows_names_the_release_asset():
    pkg = open(os.path.join(SCRIPTS, "package-windows.sh"), encoding="utf-8").read()
    assert ASSET in pkg
    assert "SDL2.dll" in pkg
    assert "libwinpthread-1.dll" in pkg


def test_windows_sdl_binary_is_renamed_to_mmcore():
    """#553: the shipped Windows/Linux/macOS SDL binary is mmcore(.exe)."""
    pkg = open(os.path.join(SCRIPTS, "package-windows.sh"), encoding="utf-8").read()
    build = open(os.path.join(SCRIPTS, "build-windows.sh"), encoding="utf-8").read()
    native = open(os.path.join(SCRIPTS, "build-native.sh"), encoding="utf-8").read()
    mk = open(os.path.join(REPO, "native", "Makefile"), encoding="utf-8").read()
    assert "native/mmcore.exe" in pkg
    assert "mmcore.exe" in pkg
    assert "mmcore" in build and "mmcore" in native
    assert "OUT_SDL   := $(CURDIR)/mmcore$(EXE)" in mk
    assert "mmbasic-sdl" not in mk
    assert "mmbasic-sdl" not in pkg


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
    assert "mmcore.exe" in wf
    assert "mmbasic-sdl" not in wf


def test_windows_workflow_gates_release_on_signing():
    """#552: a release build requires Authenticode signing; branch/rolling
    builds stay unsigned when no certificate is configured."""
    wf = open(WORKFLOW, encoding="utf-8").read()
    assert "MMCORE_REQUIRE_WIN_SIGN" in wf
    assert "github.event_name == 'release'" in wf
    assert "WIN_SIGN_PFX_BASE64" in wf
    assert "secrets.WIN_SIGN_PASSWORD" in wf


def test_windows_package_script_wires_authenticode_signing():
    """#552: the package script signs the staged exe and mirrors the macOS
    notary release gate."""
    pkg = open(os.path.join(SCRIPTS, "package-windows.sh"), encoding="utf-8").read()
    helper = open(SIGN, encoding="utf-8").read()
    assert "sign-windows-exe.sh" in pkg
    assert "MMCORE_REQUIRE_WIN_SIGN" in pkg and "MMCORE_SKIP_WIN_SIGN" in pkg
    assert "Authenticode" in pkg or "sign" in pkg.lower()
    for token in (
        "MMCORE_REQUIRE_WIN_SIGN",
        "MMCORE_SKIP_WIN_SIGN",
        "signtool",
        "osslsigncode",
        "WIN_SIGN_PFX",
        "WIN_SIGN_THUMBPRINT",
        "WIN_SIGN_SUBJECT",
        "WIN_SIGN_TIMESTAMP",
        "WIN_SIGN_COMMAND",
    ):
        assert token in helper, token


def test_release_notes_mention_windows_signing():
    notes = _run([os.path.join(SCRIPTS, "github-release.sh"), "release-notes", "9.9.9"]).stdout
    assert "mmcore.exe" in notes
    rel = open(os.path.join(SCRIPTS, "github-release.sh"), encoding="utf-8").read()
    assert "mmcore.exe" in rel
    assert "MMCORE_REQUIRE_WIN_SIGN" in rel


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


# --- Authenticode signing helper (#552) ------------------------------------


def _fake_tool(tmp_path, name):
    """Install a fake signing tool that logs its argv and exits 0.

    It also creates any path after ``-out`` so the osslsigncode path can move
    the result back over the input.
    """
    tool = tmp_path / name
    log = tmp_path / (name + ".log")
    tool.write_text(
        "#!/usr/bin/env bash\n"
        f'printf "%s\\n" "$*" >> "{log}"\n'
        'prev=""\n'
        'for a in "$@"; do\n'
        '  if [ "$prev" = "-out" ]; then : > "$a"; fi\n'
        '  prev="$a"\n'
        "done\n"
        "exit 0\n",
        encoding="utf-8",
    )
    tool.chmod(0o755)
    return tool, log


def _empty_path(tmp_path):
    empty = tmp_path / "emptybin"
    empty.mkdir()
    return str(empty)


def _run_sign(args, env_extra):
    env = dict(os.environ)
    env.update(env_extra)
    return subprocess.run(
        [BASH, SIGN, *args], text=True, capture_output=True, env=env
    )


def test_sign_windows_skip_flag_is_green(tmp_path):
    exe = tmp_path / "mmcore.exe"
    exe.write_bytes(b"MZ")
    proc = _run_sign([str(exe)], {"MMCORE_SKIP_WIN_SIGN": "1"})
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert "SIGN" in (proc.stdout + proc.stderr).upper()


def test_sign_windows_release_gate_rejects_skip(tmp_path):
    exe = tmp_path / "mmcore.exe"
    exe.write_bytes(b"MZ")
    proc = _run_sign(
        [str(exe)],
        {"MMCORE_REQUIRE_WIN_SIGN": "1", "MMCORE_SKIP_WIN_SIGN": "1"},
    )
    assert proc.returncode != 0
    assert "SIGN" in (proc.stdout + proc.stderr).upper()


def test_sign_windows_release_gate_needs_a_tool(tmp_path):
    exe = tmp_path / "mmcore.exe"
    exe.write_bytes(b"MZ")
    proc = _run_sign(
        [str(exe)],
        {
            "MMCORE_REQUIRE_WIN_SIGN": "1",
            "PATH": _empty_path(tmp_path),
            "SIGNTOOL": "",
            "WIN_SIGN_TOOL": "",
        },
    )
    assert proc.returncode != 0
    assert "WIN_SIGN" in (proc.stdout + proc.stderr)


def test_sign_windows_release_gate_needs_a_certificate(tmp_path):
    exe = tmp_path / "mmcore.exe"
    exe.write_bytes(b"MZ")
    tool, _ = _fake_tool(tmp_path, "signtool")
    proc = _run_sign(
        [str(exe)],
        {"MMCORE_REQUIRE_WIN_SIGN": "1", "SIGNTOOL": str(tool)},
    )
    assert proc.returncode != 0
    assert "certificate" in (proc.stdout + proc.stderr).lower()


def test_sign_windows_default_is_green_without_tool(tmp_path):
    exe = tmp_path / "mmcore.exe"
    exe.write_bytes(b"MZ")
    proc = _run_sign(
        [str(exe)],
        {"PATH": _empty_path(tmp_path), "SIGNTOOL": "", "WIN_SIGN_TOOL": ""},
    )
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert "unsigned" in (proc.stdout + proc.stderr).lower()


def test_sign_windows_invokes_signtool_and_verifies(tmp_path):
    exe = tmp_path / "mmcore.exe"
    exe.write_bytes(b"MZ")
    tool, log = _fake_tool(tmp_path, "signtool")
    proc = _run_sign(
        [str(exe)],
        {
            "SIGNTOOL": str(tool),
            "WIN_SIGN_THUMBPRINT": "ABCDEF0123",
            "WIN_SIGN_TIMESTAMP": "http://tsa.example",
        },
    )
    assert proc.returncode == 0, proc.stdout + proc.stderr
    calls = log.read_text()
    assert "sign" in calls and "verify" in calls
    assert "ABCDEF0123" in calls
    assert "http://tsa.example" in calls


def test_sign_windows_uses_osslsigncode_for_cross_builds(tmp_path):
    exe = tmp_path / "mmcore.exe"
    exe.write_bytes(b"MZ")
    tool, log = _fake_tool(tmp_path, "osslsigncode")
    pfx = tmp_path / "codesign.pfx"
    pfx.write_bytes(b"pfx")
    proc = _run_sign(
        [str(exe)],
        {
            "SIGNTOOL": str(tool),
            "WIN_SIGN_PFX": str(pfx),
            "WIN_SIGN_PASSWORD": "s3cret",
        },
    )
    assert proc.returncode == 0, proc.stdout + proc.stderr
    calls = log.read_text()
    assert "-pkcs12" in calls
    assert "s3cret" in calls
    assert "verify" in calls


def test_sign_windows_decodes_base64_pfx_secret(tmp_path):
    exe = tmp_path / "mmcore.exe"
    exe.write_bytes(b"MZ")
    tool, log = _fake_tool(tmp_path, "signtool")
    blob = base64.b64encode(b"pfx-bytes").decode()
    proc = _run_sign(
        [str(exe)],
        {"SIGNTOOL": str(tool), "WIN_SIGN_PFX_BASE64": blob},
    )
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert "/f" in log.read_text()


def test_sign_windows_custom_command_receives_the_file(tmp_path):
    exe = tmp_path / "mmcore.exe"
    exe.write_bytes(b"MZ")
    marker = tmp_path / "signed_path"
    proc = _run_sign(
        [str(exe)],
        {"WIN_SIGN_COMMAND": f'printf "%s" "$1" > "{marker}"'},
    )
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert marker.read_text() == str(exe)

