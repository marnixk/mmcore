"""#833: the bootable Alpine ISO build script and its wiring.

The real build is Docker + QEMU gated (it needs an x86_64 Linux host); set
MMCORE_ISO_BUILD=1 to run it. The default checks are structural so they run
anywhere.
"""

import os
import shutil
import subprocess
import sys

import pytest

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCRIPTS = os.path.join(REPO, "scripts")
ISO = os.path.join(SCRIPTS, "iso")
ENTRY = os.path.join(SCRIPTS, "build-iso.sh")
BUILDER = os.path.join(ISO, "build-in-container.sh")
MMCORE = os.path.join(ISO, "build-mmcore.sh")
OVERLAY = os.path.join(ISO, "rootfs-overlay")
INSTALL_USB = os.path.join(SCRIPTS, "install-usb.sh")
WORKFLOW = os.path.join(REPO, ".github", "workflows", "linux-iso.yml")
ASSET = "mmcore-fb-x86_64.iso"
COMPRESSED = "mmcore-fb-x86_64.iso.zst"
BASH = shutil.which("bash") or "/bin/bash"


def _run(args, **kwargs):
    return subprocess.run(args, check=True, text=True, capture_output=True, **kwargs)


def test_iso_scripts_parse_and_are_executable():
    for path in (ENTRY, BUILDER):
        _run(["bash", "-n", path])
        assert os.access(path, os.X_OK), path


def test_entry_names_the_iso_asset():
    text = open(ENTRY, encoding="utf-8").read()
    assert ASSET in text
    assert "grub-mkrescue" not in text  # the container script owns the build
    assert "docker" in text
    assert "ISO_DIRECT" in text


def test_builder_boots_alpine_with_network_packages():
    text = open(BUILDER, encoding="utf-8").read()
    for needle in (
        "alpine-base",
        "openrc",
        "linux-lts",
        "linux-firmware",
        "wpa_supplicant",
        "iw",
        "grub-mkrescue",
        "initramfs",
        "rootfs-overlay",
        "build-mmcore.sh",
    ):
        assert needle in text, needle
    assert "console=ttyS0" in text


def test_mmcore_builder_targets_kmsdrm_and_installs():
    assert os.access(MMCORE, os.X_OK), MMCORE
    _run(["bash", "-n", MMCORE])
    text = open(MMCORE, encoding="utf-8").read()
    assert "sdl-fb" in text
    assert "kmsdrm" in text
    assert "SDL_KMSDRM=ON" in text
    assert "/usr/local/bin/mmcore" in text


def test_overlay_autostarts_mmcore_on_the_framebuffer():
    profile = open(
        os.path.join(OVERLAY, "root", ".profile"), encoding="utf-8"
    ).read()
    assert "/usr/local/bin/mmcore" in profile
    assert "tty1" in profile
    assert "kmsdrm" in profile
    assert os.path.isfile(os.path.join(OVERLAY, "etc", "modules"))


def test_persistence_script_mounts_the_labeled_partition():
    path = os.path.join(OVERLAY, "etc", "local.d", "mmcore-persist.start")
    assert os.access(path, os.X_OK), path
    _run(["bash", "-n", path])
    text = open(path, encoding="utf-8").read()
    assert "blkid -L MMCORE" in text
    assert "/media/mmcore" in text
    assert "mkfs.ext4" in text


def test_install_usb_script_parses_and_documents_persistence():
    assert os.access(INSTALL_USB, os.X_OK), INSTALL_USB
    _run(["bash", "-n", INSTALL_USB])
    help_out = _run([INSTALL_USB, "--help"]).stdout
    assert "--no-persist" in help_out
    assert "--iso" in help_out
    text = open(INSTALL_USB, encoding="utf-8").read()
    assert "MMCORE" in text
    assert "dd of=" in text
    assert "mkfs.ext4" in text
    # compressed release artifact is decompressed on the fly
    assert "zstd -dc" in text
    assert "*.zst)" in text


def test_overlay_wires_ethernet_dhcp_and_wifi():
    net = os.path.join(OVERLAY, "etc", "local.d", "mmcore-net.start")
    assert os.access(net, os.X_OK), net
    _run(["bash", "-n", net])
    net_text = open(net, encoding="utf-8").read()
    assert "udhcpc" in net_text
    assert "wireless" in net_text

    wpa = os.path.join(OVERLAY, "etc", "wpa_supplicant", "wpa_supplicant.conf")
    assert os.path.isfile(wpa)
    assert "ctrl_interface" in open(wpa, encoding="utf-8").read()


def test_builder_ships_wifi_firmware_and_supplicant():
    text = open(BUILDER, encoding="utf-8").read()
    assert "linux-firmware-iwlwifi" in text
    assert "wpa_supplicant default" in text
    assert "rootfs-overlay" in text


def test_iso_workflow_builds_boot_smokes_and_attaches():
    wf = open(WORKFLOW, encoding="utf-8").read()
    assert "release:" in wf
    assert "types: [published]" in wf
    assert "ubuntu-22.04" in wf
    assert "qemu-system-x86_64" in wf
    assert "MMCORE_ISO_BOOT_OK" in wf
    assert ASSET in wf
    assert "install-usb.sh" in wf
    assert "linux-iso" in wf


def test_iso_workflow_publishes_only_the_compressed_iso():
    wf = open(WORKFLOW, encoding="utf-8").read()
    # CI compresses after boot-smoking the raw ISO, then uploads the .zst...
    assert "zstd" in wf
    assert COMPRESSED in wf
    assert f"dist/{COMPRESSED} scripts/install-usb.sh" in wf
    # ...and never uploads the raw .iso as a release asset.
    assert f"dist/{ASSET} scripts/install-usb.sh" not in wf


def test_release_notes_list_the_iso_assets():
    _run(["bash", "-n", os.path.join(SCRIPTS, "github-release.sh")])
    notes = _run(
        [os.path.join(SCRIPTS, "github-release.sh"), "release-notes", "9.9.9"]
    ).stdout
    assert COMPRESSED in notes
    assert "install-usb.sh" in notes
    assert "zstd -d" in notes
    assert "Bootable USB" in notes or "bootable" in notes.lower()


@pytest.mark.skipif(
    os.environ.get("MMCORE_ISO_BUILD") != "1",
    reason="set MMCORE_ISO_BUILD=1 to run the Docker ISO build",
)
def test_iso_builds(tmp_path):
    if shutil.which("docker") is None:
        pytest.skip("docker is not available")
    env = dict(os.environ, DIST=str(tmp_path))
    _run([BASH, ENTRY], env=env, timeout=3600)
    assert (tmp_path / ASSET).is_file()
