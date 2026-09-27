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
INSTALL = os.path.join(OVERLAY, "usr", "local", "bin", "mmcore-install")
UPDATE = os.path.join(OVERLAY, "usr", "local", "bin", "mmcore-update")
PROFILE = os.path.join(OVERLAY, "root", ".profile")
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


def test_rootfs_installs_mesa_runtime_libs_for_kmsdrm():
    """SDL dlopens libgbm/libEGL/libGLESv2 and the Gallium DRI backends, which
    on Alpine live in mesa subpackages, not the empty `mesa` metapackage."""
    text = open(BUILDER, encoding="utf-8").read()
    for pkg in ("mesa-gbm", "mesa-egl", "mesa-gles", "mesa-dri-gallium"):
        assert pkg in text, pkg


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


def test_installer_and_updater_scripts_are_present_and_executable():
    for path in (INSTALL, UPDATE):
        assert os.path.isfile(path), path
        assert os.access(path, os.X_OK), path
        _run(["bash", "-n", path])


def test_installer_creates_the_two_partition_boot_layout():
    text = open(INSTALL, encoding="utf-8").read()
    assert "grub-install" in text
    assert "MMCORE-SYS" in text
    assert "MMCORE-DATA" in text
    assert "mklabel" in text
    assert "mkpart" in text
    assert "255" in text  # the ~255 MiB system partition floor
    # The installed boot uses MMCORE-SYS as C: and MMCORE-DATA as D:.
    assert "mmcore.sys=LABEL=MMCORE-SYS" in text
    assert "mmcore.data=LABEL=MMCORE-DATA" in text
    profile = open(PROFILE, encoding="utf-8").read()
    assert "mmcore.sys=" in profile
    assert "/media/mmcore-sys" in profile
    assert "--drive /media/mmcore-data" in profile


def test_installer_help_documents_disk_selection():
    out = _run(["sh", INSTALL, "--help"]).stdout
    assert "--disk" in out
    assert "--boot-dir" in out
    assert "--yes" in out


def test_updater_targets_the_framebuffer_release_asset():
    text = open(UPDATE, encoding="utf-8").read()
    assert "mmcore-fb-linux-x86_64.tar.gz" in text
    assert "VERSION.txt" in text
    assert "--check" in text
    assert "--version" in text
    assert "--url" in text
    assert "MMCORE-SYS" in text
    assert "/media/mmcore-sys" in text


def test_updater_help_documents_the_flags():
    out = _run(["sh", UPDATE, "--help"]).stdout
    assert "--check" in out
    assert "--version" in out
    assert "--url" in out


def test_rootfs_ships_partition_and_bootloader_tools():
    text = open(BUILDER, encoding="utf-8").read()
    # The rootfs (not just the builder) must be able to partition a disk and
    # install GRUB, so these are on the `apk add --root` line.
    assert "parted gdisk util-linux-misc grub grub-efi grub-bios" in text


def test_update_version_compare_logic():
    """#892: the updater's version compare runs on the host, no network."""
    pairs = (
        ("0.215.0", "0.216.0", "-1"),
        ("0.216.0", "0.215.0", "1"),
        ("v0.216.0", "0.216.0", "0"),
        ("0.216", "0.216.0", "0"),
        ("0.10.0", "0.9.9", "1"),
        ("1.0.0", "0.999.999", "1"),
        ("0.216.0-rc1", "0.216.0", "0"),
        ("dev", "0.1.0", "-1"),
    )
    shell = (
        'MMCORE_UPDATE_SOURCED=1 . "$1"; '
        'mmcore_version_cmp "$2" "$3"'
    )
    for a, b, want in pairs:
        out = _run([BASH, "-c", shell, "bash", UPDATE, a, b]).stdout.strip()
        assert out == want, (a, b, out)



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


def test_install_usb_uses_a_free_partition_number():
    """The hybrid ISO's GPT already uses 1-4, so persistence is partition 5."""
    text = open(INSTALL_USB, encoding="utf-8").read()
    assert "--new=5:0:0" in text
    assert "--typecode=5:8300" in text
    assert 'part="${DEVICE}5"' in text
    assert "--move-second-header" in text
    assert "--new=2:" not in text


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


def test_boot_smoke_uses_a_kms_drm_gpu():
    """#861: -nographic has no DRM, so a crash-looping mmcore still passed
    `pgrep`. Boot a virtio-gpu instead and assert the framebuffer path."""
    wf = open(WORKFLOW, encoding="utf-8").read()
    assert "-nographic" not in wf
    assert "virtio-vga" in wf
    assert "-display none" in wf
    assert "-monitor none" in wf
    assert "-serial stdio" in wf
    # Positive checks: a DRM node, a stable (non-respawn) PID, and no SDL error.
    assert "MMCORE_DRM_OK" in wf
    assert "MMCORE_STABLE_OK" in wf
    assert "MMCORE_SDL_OK" in wf
    assert "/dev/dri/card0" in wf
    assert "could not open SDL window" in wf


def test_builder_ships_a_quiet_grub_with_a_splash():
    """#863: hidden GRUB for ~1 s, a centered logo, and a quiet kernel."""
    builder = open(BUILDER, encoding="utf-8").read()
    assert "set timeout=1" in builder
    assert "timeout_style=hidden" in builder
    assert "quiet" in builder
    assert "vt.global_cursor_default=0" in builder
    assert "splash.png" in builder
    assert "background_image" in builder
    assert os.path.isfile(os.path.join(ISO, "boot", "splash.png"))


def test_builder_installs_sof_firmware_for_intel_chromebooks():
    """#864: Sound Open Firmware is needed by many Intel Chromebooks."""
    assert "sof-firmware" in open(BUILDER, encoding="utf-8").read()


def test_overlay_loads_chromebook_modules():
    """#864: cros_ec + I2C-HID touchpad/touchscreen + SOF audio modules."""
    text = open(os.path.join(OVERLAY, "etc", "modules"), encoding="utf-8").read()
    for module in (
        "cros_ec",
        "cros_ec_i2c",
        "i2c_hid_acpi",
        "i2c_hid_of",
        "hid_multitouch",
        "snd_sof",
        "snd_sof_pci",
    ):
        assert module in text, module
    # The pre-existing KMS drivers must stay.
    for module in ("i915", "amdgpu", "virtio_gpu"):
        assert module in text, module


def test_rootfs_silences_the_display_banners():
    """#863: no getty/login banner on tty1."""
    issue = os.path.join(OVERLAY, "etc", "issue")
    motd = os.path.join(OVERLAY, "etc", "motd")
    assert os.path.isfile(issue)
    assert os.path.getsize(issue) == 0
    assert os.path.isfile(motd)
    assert os.path.getsize(motd) == 0


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
