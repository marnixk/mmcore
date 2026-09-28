"""The macOS universal app bundle ships as a release asset.

`scripts/github-release.sh publish` builds it via `scripts/package-macos-app.sh`
and requires notarization by default (MMCORE_REQUIRE_NOTARY=1). An explicit
MMCORE_SKIP_NOTARY=1 escape hatch exists for local/emergency builds where no
notarytool credentials are available; it must build the bundle signed but
unnotarized instead of aborting the release.
"""

import os
import shutil
import subprocess

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCRIPTS = os.path.join(REPO, "scripts")
RELEASE = os.path.join(SCRIPTS, "github-release.sh")
PACKAGER = os.path.join(SCRIPTS, "package-macos-app.sh")
BASH = shutil.which("bash") or "/bin/bash"
ASSET = "mmcore-macos-universal.zip"


def _run(args):
    return subprocess.run(args, check=True, text=True, capture_output=True)


def test_release_scripts_parse_and_are_executable():
    for path in (RELEASE, PACKAGER):
        _run([BASH, "-n", path])
        assert os.access(path, os.X_OK), path


def test_release_builds_the_macos_asset():
    rel = open(RELEASE, encoding="utf-8").read()
    assert ASSET in rel
    assert "package-macos-app.sh" in rel


def test_notarization_is_required_by_default():
    rel = open(RELEASE, encoding="utf-8").read()
    # The normal path asks the packager to require notarization.
    assert "MMCORE_REQUIRE_NOTARY=1" in rel
    # And the packager refuses to ship an unnotarized bundle for a release.
    pkg = open(PACKAGER, encoding="utf-8").read()
    assert "MMCORE_REQUIRE_NOTARY" in pkg
    assert "want_notary=1" in pkg


def test_skip_notary_escape_hatch_builds_unnotarized():
    rel = open(RELEASE, encoding="utf-8").read()
    assert 'MMCORE_SKIP_NOTARY:-}" = "1"' in rel
    # The skip branch must pass REQUIRE_NOTARY=0 so the packager signs but does
    # not notarize, instead of dying on the missing notarytool credentials.
    assert 'MMCORE_REQUIRE_NOTARY=0 VERSION="${version}"' in rel
    # Documented in the usage text so it is discoverable.
    assert "MMCORE_SKIP_NOTARY=1" in rel
