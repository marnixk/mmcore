"""#838: docs agree with the shipped framebuffer/ISO asset names."""

import os

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DOC = os.path.join(REPO, "docs", "framebuffer-and-iso.md")
ASSETS = (
    "mmcore-fb-linux-x86_64.tar.gz",
    "mmcore-fb-x86_64.iso.zst",
    "install-usb.sh",
)


def _read(path):
    with open(path, encoding="utf-8") as fh:
        return fh.read()


def test_framebuffer_iso_doc_exists_and_names_the_assets():
    text = _read(DOC)
    for asset in ASSETS:
        assert asset in text, asset
    assert "OPTION WIFI" in text
    assert "MMCORE" in text
    assert "kmsdrm" in text
    assert "zstd -d" in text


def test_readme_and_install_link_the_new_doc():
    for name in ("README.md", "INSTALL.md"):
        assert "docs/framebuffer-and-iso.md" in _read(os.path.join(REPO, name)), name


def test_framebuffer_iso_doc_documents_quiet_boot():
    text = _read(DOC)
    assert "Shift" in text
    assert "quiet" in text
    assert "hidden" in text


def test_framebuffer_iso_doc_documents_chromebooks():
    text = _read(DOC)
    for needle in (
        "Chromebook",
        "developer mode",
        "RW_LEGACY",
        "MrChromebox",
        "sof-firmware",
        "cros_ec",
        "i2c_hid_acpi",
        "IA32",
        "unsupported",
    ):
        assert needle in text, needle


def test_native_desktop_and_new_doc_cross_link():
    native = _read(os.path.join(REPO, "docs", "native-desktop.md"))
    iso = _read(DOC)
    assert "framebuffer-and-iso.md" in native
    assert "native-desktop.md" in iso


def test_release_skill_names_the_iso_and_framebuffer_assets():
    skill = _read(
        os.path.join(REPO, ".cursor", "skills", "github-release", "SKILL.md")
    )
    assert "mmcore-fb-linux-x86_64.tar.gz" in skill
    assert "mmcore-fb-x86_64.iso.zst" in skill
    assert "install-usb.sh" in skill
