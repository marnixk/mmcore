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
import tempfile

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


def test_notary_env_file_is_loaded_for_non_interactive_release():
    notary_helper = os.path.join(SCRIPTS, "mmcore-notary-env.sh")
    rel = open(RELEASE, encoding="utf-8").read()
    pkg = open(PACKAGER, encoding="utf-8").read()
    helper = open(notary_helper, encoding="utf-8").read()
    path = ".config/mmcore/notary.env"
    for text in (rel, pkg, helper):
        assert path in text
        assert "NOTARY_APPLE_ID" in text
        assert "NOTARY_TEAM_ID" in text
        assert "NOTARY_PASSWORD" in text
    assert "mmcore-notary-env.sh" in rel
    assert "mmcore-notary-env.sh" in pkg
    _run([BASH, "-n", notary_helper])


def test_notary_env_helper_sources_file_and_respects_existing_env():
    notary_helper = os.path.join(SCRIPTS, "mmcore-notary-env.sh")
    with tempfile.TemporaryDirectory() as tmp:
        cfg = os.path.join(tmp, ".config", "mmcore")
        os.makedirs(cfg)
        env_file = os.path.join(cfg, "notary.env")
        with open(env_file, "w", encoding="utf-8") as fh:
            fh.write(
                "export NOTARY_APPLE_ID=file@example.com\n"
                "export NOTARY_TEAM_ID=FILETEAM\n"
                "export NOTARY_PASSWORD=file-secret\n"
            )
        load_from_file = subprocess.run(
            [
                BASH,
                "-c",
                f'. "{notary_helper}"; printf "%s|%s|%s" '
                '"$NOTARY_APPLE_ID" "$NOTARY_TEAM_ID" "$NOTARY_PASSWORD"',
            ],
            check=True,
            text=True,
            capture_output=True,
            env={"HOME": tmp, "PATH": os.environ.get("PATH", "")},
        )
        assert load_from_file.stdout == "file@example.com|FILETEAM|file-secret"
        keep_env = subprocess.run(
            [
                BASH,
                "-c",
                f'. "{notary_helper}"; printf "%s" "$NOTARY_APPLE_ID"',
            ],
            check=True,
            text=True,
            capture_output=True,
            env={
                "HOME": tmp,
                "PATH": os.environ.get("PATH", ""),
                "NOTARY_APPLE_ID": "env@example.com",
                "NOTARY_TEAM_ID": "ENVTEAM",
                "NOTARY_PASSWORD": "env-secret",
            },
        )
        assert keep_env.stdout == "env@example.com"


def test_skip_notary_escape_hatch_builds_unnotarized():
    rel = open(RELEASE, encoding="utf-8").read()
    assert 'MMCORE_SKIP_NOTARY:-}" = "1"' in rel
    # The skip branch must pass REQUIRE_NOTARY=0 so the packager signs but does
    # not notarize, instead of dying on the missing notarytool credentials.
    assert 'MMCORE_REQUIRE_NOTARY=0 VERSION="${version}"' in rel
    # Documented in the usage text so it is discoverable.
    assert "MMCORE_SKIP_NOTARY=1" in rel
