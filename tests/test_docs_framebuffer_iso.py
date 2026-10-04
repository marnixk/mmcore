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


def test_framebuffer_iso_doc_documents_installing_to_disk():
    text = _read(DOC)
    for needle in (
        "Install to hard disk",
        "mmcore-install",
        "mmcore-update",
        "MMCORE-SYS",
        "MMCORE-DATA",
        "mmcore-fb-linux-x86_64.tar.gz",
        "EFI system partition",
        "BOOTX64.EFI",
    ):
        assert needle in text, needle


def test_framebuffer_iso_doc_uses_ctrl_alt_for_vt_switching():
    """#909: Alt+F2 does not reach the Linux tty (SDL consumes it); the docs
    must say Ctrl+Alt+F2, with Ctrl+Alt+F1 to return to mmcore."""
    text = _read(DOC)
    assert "Ctrl+Alt+F2" in text
    assert "Ctrl+Alt+F1" in text
    assert "Alt+F2" not in text.replace("Ctrl+Alt+F2", "")


def test_framebuffer_iso_doc_documents_dual_boot():
    """#976/#979: the doc must explain that a live USB runs live even with a
    disk install present and that an installed boot mounts only its labelled
    device, how the installer's NVRAM default keeps USB preferred, plus the
    manual dual-disk QA checklist the CI smoke cannot cover."""
    text = _read(DOC)
    for needle in (
        "Live USB with a disk installed",
        "mmcore-live-media",
        "mmcore.live=1",
        "mmcore.sys=",
        "findfs",
        "installed boot is pinned",
        "--register-efi",
        "efibootmgr",
        "Manual dual-boot QA checklist",
    ):
        assert needle in text, needle


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
        "--read-write",
        "Persistent C: on the microSD",
    ):
        assert needle in text, needle


def test_framebuffer_iso_doc_documents_usb_update():
    """#1056: the doc must document install-usb --update, including that the
    persistent partition is kept."""
    text = _read(DOC)
    assert "--update" in text
    assert "mkfs.ext4" in text
    assert "survive" in text or "survives" in text


def test_framebuffer_iso_doc_documents_headphone_jack():
    """#1059: the doc must name the UCM profiles and the jack-follow helper."""
    text = _read(DOC)
    for needle in ("alsa-ucm-conf", "Headphone Jack", "Auto-Mute Mode",
                   "mmcore-audio.start"):
        assert needle in text, needle


def test_framebuffer_iso_doc_documents_chromebook_touchpad_drivers():
    """#1039: the doc must name the touchpad drivers baked into the ISO (so a
    built-in touchpad is claimed without a post-boot modprobe) and the models
    that need a kernel the image does not ship."""
    text = _read(DOC)
    for needle in (
        "intel_lpss",
        "i2c_designware_pci",
        "chromeos_laptop",
        "cyapatp",
        "elan_i2c",
        "synaptics_i2c",
        "psmouse",
        "evdev",
        "aarch64",
    ):
        assert needle in text, needle


def test_framebuffer_iso_doc_documents_the_chromebook_hang_and_watchdogs():
    """#1032: the doc must describe the bounded splash, the bounded video
    bring-up that logs to serial, and the hardware-QA steps QEMU cannot run."""
    text = _read(DOC)
    for needle in (
        "MMCORE_SPLASH_TIMEOUT",
        "MMCORE_VIDEO_TIMEOUT",
        "KMS/DRM modeset stuck",
        "If the boot hangs on the mmcore logo",
        "Chromebook hardware QA checklist",
        "dmesg | tail -80",
        "/dev/dri",
        "rc-status",
        "cros_ec",
        "Ctrl+Alt+F2",
    ):
        assert needle in text, needle


def test_framebuffer_iso_doc_documents_the_bounded_installed_probe():
    """#1034: the doc must note that the installed boot's label lookup/mounts
    are bounded so a slow block device cannot stall tty1 before the prompt."""
    text = _read(DOC)
    assert "MMCORE_PROBE_TIMEOUT" in text
    assert "blkid" in text
    assert "bounded" in text


def test_framebuffer_iso_doc_documents_the_firmware_keep_list():
    """#958/#964: the doc must say the ISO ships a desktop firmware keep-list,
    not the full linux-firmware meta, and name what is (and is not) covered."""
    text = _read(DOC)
    assert "Hardware firmware" in text
    assert "keep-list" in text
    assert "linux-firmware" in text
    for needle in (
        "amd-ucode",
        "ath9k_htc",
        "i915",
        "amdgpu",
        "iwlwifi",
        "brcm",
        "mediatek",
        "sof-firmware",
    ):
        assert needle in text, needle
    # The removed packages and the SEV omission are documented decisions.
    assert "libertas" in text
    assert "mrvl" in text
    assert "SEV" in text
    # #968: the builder fails the build when a kept package is missing.
    assert "fails the build" in text


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
