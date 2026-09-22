"""The Windows native build ships as a release asset."""

import os
import subprocess

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCRIPTS = os.path.join(REPO, "scripts")
WORKFLOW = os.path.join(REPO, ".github", "workflows", "windows.yml")
ASSET = "mmcore-windows-x86_64.zip"


def _run(args, **kwargs):
    return subprocess.run(args, check=True, text=True, capture_output=True, **kwargs)


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
