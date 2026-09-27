"""The Linux KMS/DRM framebuffer build ships as a release tarball."""

import os
import shutil
import subprocess

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCRIPTS = os.path.join(REPO, "scripts")
WORKFLOW = os.path.join(REPO, ".github", "workflows", "linux-framebuffer.yml")
BASH = shutil.which("bash") or "/bin/bash"
ASSET = "mmcore-fb-linux-x86_64.tar.gz"


def _run(args, **kwargs):
    return subprocess.run(args, check=True, text=True, capture_output=True, **kwargs)


def test_framebuffer_script_parses_and_is_executable():
    path = os.path.join(SCRIPTS, "package-framebuffer.sh")
    _run(["bash", "-n", path])
    assert os.access(path, os.X_OK), path


def test_package_framebuffer_names_the_release_asset():
    pkg = open(
        os.path.join(SCRIPTS, "package-framebuffer.sh"), encoding="utf-8"
    ).read()
    assert ASSET in pkg
    assert "mmcore-fb" in pkg
    assert "sdl-fb" in pkg
    assert "libsdl2" in pkg


def test_release_notes_list_the_framebuffer_asset():
    _run(["bash", "-n", os.path.join(SCRIPTS, "github-release.sh")])
    notes = _run(
        [os.path.join(SCRIPTS, "github-release.sh"), "release-notes", "9.9.9"]
    ).stdout
    assert ASSET in notes
    assert "framebuffer" in notes.lower()
    rel = open(os.path.join(SCRIPTS, "github-release.sh"), encoding="utf-8").read()
    assert ASSET in rel


def test_framebuffer_workflow_attaches_to_releases():
    wf = open(WORKFLOW, encoding="utf-8").read()
    assert "release:" in wf
    assert "types: [published]" in wf
    assert "ubuntu-22.04" in wf
    assert "SDL_VIDEODRIVER=dummy" in wf
    assert ASSET in wf
