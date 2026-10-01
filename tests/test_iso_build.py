"""#833: the bootable Alpine ISO build script and its wiring.

The real build is Docker + QEMU gated (it needs an x86_64 Linux host); set
MMCORE_ISO_BUILD=1 to run it. The default checks are structural so they run
anywhere.
"""

import os
import re
import shutil
import signal
import subprocess
import sys
import time

import pytest

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCRIPTS = os.path.join(REPO, "scripts")
ISO = os.path.join(SCRIPTS, "iso")
ENTRY = os.path.join(SCRIPTS, "build-iso.sh")
BUILDER = os.path.join(ISO, "build-in-container.sh")
PACK = os.path.join(ISO, "pack-initramfs.sh")
LIVE_INIT = os.path.join(ISO, "live-init")
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
    for path in (ENTRY, BUILDER, PACK, LIVE_INIT):
        _run(["bash", "-n", path])
        assert os.access(path, os.X_OK), path


def test_entry_names_the_iso_asset():
    text = open(ENTRY, encoding="utf-8").read()
    assert ASSET in text
    assert "grub-mkrescue" not in text  # the container script owns the build
    assert "docker" in text
    assert "ISO_DIRECT" in text


def test_build_iso_mounts_custom_dist_into_the_container():
    """#912: a custom DIST must reach the Docker builder, not just the host
    existence check, or test_iso_builds can never pass."""
    text = open(ENTRY, encoding="utf-8").read()
    # The builder writes to a container path that is a mount of DIST...
    assert 'ISO_OUT=/iso-out/' in text
    assert '${DIST}:/iso-out' in text
    # ...and the host-side existence check still uses the host path.
    assert '[ -f "${OUT}" ]' in text


def test_builder_boots_alpine_with_network_packages():
    text = open(BUILDER, encoding="utf-8").read()
    for needle in (
        "alpine-base",
        "openrc",
        "acpid",
        "linux-lts",
        "linux-firmware-i915",
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


def test_acpid_power_and_lid_handlers_are_in_the_overlay():
    for rel in (
        "etc/acpi/events/powerbtn",
        "etc/acpi/events/lid",
        "etc/acpi/mmcore-power.sh",
        "etc/acpi/mmcore-lid.sh",
        "etc/acpi/mmcore-sync-storage.sh",
    ):
        path = os.path.join(OVERLAY, rel)
        assert os.path.isfile(path), path
    text = open(BUILDER, encoding="utf-8").read()
    assert "enable acpid default" in text
    power = open(os.path.join(OVERLAY, "etc/acpi/mmcore-power.sh"), encoding="utf-8").read()
    assert "/sbin/poweroff" in power
    lid = open(os.path.join(OVERLAY, "etc/acpi/mmcore-lid.sh"), encoding="utf-8").read()
    assert "/sys/power/state" in lid
    assert "mem" in lid
    sync = open(
        os.path.join(OVERLAY, "etc/acpi/mmcore-sync-storage.sh"), encoding="utf-8"
    ).read()
    assert "/media/mmcore" in sync
    for script in ("mmcore-power.sh", "mmcore-lid.sh", "mmcore-sync-storage.sh"):
        _run(["bash", "-n", os.path.join(OVERLAY, "etc/acpi", script)])


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
    assert "--recheck" not in text
    assert "(hd0)" in text
    assert "set 1 esp on" in text
    assert "set 1 boot on" in text
    assert "--removable" in text
    assert "--bootloader-id=mmcore" in text
    assert "--no-nvram" in text
    assert "/sys/firmware/efi" in text
    assert "search --no-floppy --set=root --label MMCORE-SYS" in text
    assert "MMCORE-SYS" in text
    assert "MMCORE-DATA" in text
    assert "mklabel" in text
    assert "mkpart" in text
    assert "255" in text  # the ~255 MiB system partition floor
    # The installed boot uses MMCORE-SYS as C: and MMCORE-DATA as D:.
    assert "mmcore.sys=LABEL=MMCORE-SYS" in text
    assert "mmcore.data=LABEL=MMCORE-DATA" in text
    assert 'cp "${PAYLOAD}/boot/rootfs.squashfs"' in text
    assert "/mnt/live/media" in text
    profile = open(PROFILE, encoding="utf-8").read()
    assert "mmcore.sys=" in profile
    assert "/media/mmcore-sys" in profile
    assert "--drive /media/mmcore-data" in profile


def test_installer_help_documents_disk_selection():
    out = _run(["sh", INSTALL, "--help"]).stdout
    assert "--disk" in out
    assert "--boot-dir" in out
    assert "--yes" in out
    assert "--register-efi" in out


def test_installer_defaults_to_no_sticky_uefi_entry():
    """#976: a named NVRAM entry outranks removable media, so with mmcore on an
    internal disk a live USB boots the installed build. The default writes only
    the removable fallback; --register-efi is the explicit opt-in."""
    text = open(INSTALL, encoding="utf-8").read()
    assert "REGISTER_EFI=0" in text
    assert "--register-efi" in text
    assert "--removable --no-nvram" in text
    # The named entry is behind the flag, never on the always-taken path.
    guard = text.index('if [ "${REGISTER_EFI}" = "1" ]; then')
    named = text.index("--bootloader-id=mmcore")
    assert guard < named, "the named NVRAM entry must be behind --register-efi"
    assert "--bootloader-id=mmcore" in text


def test_live_boot_pins_root_to_the_live_media():
    """#976: the live boot must use the stick's own squashfs, never an installed
    MMCORE-SYS /boot/rootfs.squashfs. GRUB pins $root by a marker file the
    installer never copies, and the live kernel line passes mmcore.live=1 --
    and never mmcore.sys=, the installed path."""
    builder = open(BUILDER, encoding="utf-8").read()
    assert '${ISOROOT}/mmcore-live-media' in builder, "the live marker is not built"
    assert "--file /mmcore-live-media" in builder, "GRUB does not pin root by marker"
    assert "mmcore.live=1" in builder, "the live kernel line does not pin live boot"
    live_cfg = builder.split('cat > "${ISOROOT}/boot/grub/grub.cfg"', 1)[1]
    live_cfg = live_cfg.split("\nEOF", 1)[0]
    assert "search --no-floppy --set=root --label MMCORE-SYS" not in live_cfg
    assert "mmcore.sys=" not in live_cfg

    live = open(LIVE_INIT, encoding="utf-8").read()
    assert "mmcore-live-media" in live, "live-init ignores the live marker"
    assert "mmcore.live=" in live, "live-init ignores mmcore.live="
    # On a live boot the marked media is probed before the generic squashfs.
    assert "scan_media try_live_media" in live
    assert live.index("scan_media try_live_media") < live.index("scan_media try_one")


def test_installed_boot_pins_root_to_the_system_partition():
    """#979: an installed boot (mmcore.sys=) must resolve its MMCORE-SYS device
    by label and mount the squashfs only from that device, so a live USB stick
    enumerated first can never supply its root."""
    live = open(LIVE_INIT, encoding="utf-8").read()
    assert "mmcore.sys=" in live, "live-init ignores the installed system partition"
    assert "findfs" in live, "live-init does not resolve the system partition label"
    assert "try_sys_media" in live
    assert "scan_sys_media" in live
    # The installed boot is resolved through findfs, not the generic
    # first-rootfs.squashfs scan.
    assert 'findfs "$spec"' in live
    # The generic scan is only reached when the boot named no system partition:
    # an installed boot (sys_spec non-empty) must never fall through to it.
    assert "scan_sys_media" in live
    assert live.index("scan_sys_media") < live.index("scan_media try_one")
    fallback = live.split('if [ "$found" != 1 ]', 1)[1]
    fallback = fallback.split("scan_media try_one", 1)[0]
    assert "${sys_spec}" in fallback, (
        "the generic scan is not excluded on an installed boot"
    )


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
    assert "parted gptfdisk util-linux-misc grub grub-efi grub-bios efibootmgr" in text


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
    for pkg in (
        "linux-firmware-brcm",
        "linux-firmware-mediatek",
        "linux-firmware-rtw88",
        "linux-firmware-ath10k",
    ):
        assert pkg in text, pkg
    assert "wpa_supplicant default" in text
    assert "rootfs-overlay" in text
    # The old names are not Alpine 3.20 packages and silently no-op'd (#958).
    assert "linux-firmware-iwlwifi" not in text
    assert "linux-firmware-realtek" not in text


def test_builder_ships_only_the_desktop_firmware_keep_list():
    """#958/#964: replace the whole `linux-firmware` meta with an explicit
    desktop keep-list (CPU microcode + GPU + WiFi/BT) and drop the
    ARM/SoC/server-NIC/embedded classes. Intel iwlwifi is uncategorized
    (`linux-firmware-other`), so that one package is pruned to the iwlwifi
    ucodes rather than dropped wholesale."""
    text = open(BUILDER, encoding="utf-8").read()
    # The kernel line no longer names the meta.
    assert "linux-lts sof-firmware" in text
    # Assert against the FW_KEEP list only, so prose in the surrounding
    # comments (which names the dropped packages) is ignored.
    keep = text.split('FW_KEEP="', 1)[1].split('"', 1)[0]
    for pkg in (
        # CPU (security-relevant late-loadable microcode)
        "linux-firmware-amd-ucode",
        # GPU
        "linux-firmware-i915",
        "linux-firmware-amdgpu",
        "linux-firmware-radeon",
        "linux-firmware-nvidia",
        "linux-firmware-intel",
        "linux-firmware-xe",
        # WiFi/BT
        "linux-firmware-brcm",
        "linux-firmware-mediatek",
        "linux-firmware-rtw88",
        "linux-firmware-rtw89",
        "linux-firmware-rtlwifi",
        "linux-firmware-rtl_bt",
        "linux-firmware-ath10k",
        "linux-firmware-ath11k",
        "linux-firmware-ath12k",
        "linux-firmware-ath6k",
        "linux-firmware-ath9k_htc",
        "linux-firmware-qca",
    ):
        assert pkg in keep, pkg
    # The dropped classes are never installed by name.
    for pkg in (
        "linux-firmware-qcom",
        "linux-firmware-netronome",
        "linux-firmware-mellanox",
        "linux-firmware-qed",
        "linux-firmware-dpaa2",
        "linux-firmware-liquidio",
        "linux-firmware-cxgb4",
        "linux-firmware-bnx2x",
        "linux-firmware-cnm",
        "linux-firmware-amlogic",
        "linux-firmware-s5p-mfc",
        "linux-firmware-cirrus",
        "linux-firmware-ueagle-atm",
        "linux-firmware-mwl8k",
        "linux-firmware-mwlwifi",
        "linux-firmware-ar3k",
        # #964: the legacy Marvell pair (hard circular dependency in Alpine
        # 3.20; mrvl also drags Prestera/Octeon server blobs) is dropped.
        "linux-firmware-libertas",
        "linux-firmware-mrvl",
    ):
        assert pkg not in keep, pkg
    # #964: AMD SEV firmware (`linux-firmware-amd`) is dropped, while the CPU
    # microcode package (`linux-firmware-amd-ucode`) is kept. The kept name is a
    # prefix of the dropped one, so match the bare package with a boundary.
    assert re.search(r"linux-firmware-amd(?![\w-])", keep) is None
    # iwlwifi is kept from the uncategorized package via an ownership prune:
    # install linux-firmware-other, then remove its non-iwlwifi /lib/firmware
    # entries using apk's own `info -L` list.
    assert "linux-firmware-other" in text
    assert "info -L linux-firmware-other" in text
    assert "*lib/firmware/iwlwifi-*" in text
    assert '*lib/firmware/*) rm -f' in text


def test_builder_verifies_every_firmware_package_landed():
    """#968: a typo or an Alpine package rename must fail the build, not ship
    an ISO silently missing that firmware (the #958 failure mode)."""
    text = open(BUILDER, encoding="utf-8").read()
    # The keep-list is part of the base apk transaction: that is what makes apk
    # satisfy linux-lts's `linux-firmware-any` from these packages instead of
    # the `linux-firmware` meta. Installing them in a best-effort loop
    # afterwards leaves the meta in the rootfs and apk exits non-zero while
    # purging it, so the old `|| true` hid a real no-op (#968).
    assert "efibootmgr \\\n\t${FW_KEEP}" in text, (
        "the firmware keep-list must be in the base apk transaction"
    )
    # Every requested package is then checked to be really present in the
    # rootfs using apk's own installed test...
    assert 'FW_KEEP=' in text
    assert 'apk --root "${ROOTFS}" info -e "$fw"' in text
    # ...and a missing package aborts the build with a clear message.
    assert '|| die "firmware package missing from the rootfs: $fw"' in text
    # The verification loop must never swallow a failure or install anything.
    verify = text.split("for fw in", 1)[1].split("done", 1)[0]
    assert "apk add" not in verify, verify
    assert "|| true" not in verify, verify
    assert "die" in verify, verify
    # `|| true` stays only on the genuinely optional builder tools.
    tools = text.split("for pkg in", 1)[1].split("done", 1)[0]
    assert "|| true" in tools, tools


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


def test_live_root_is_a_squashfs_the_kernel_reads():
    """Firmware USB reads on a ThinkPad T420 (and on flash drives tuned for
    large sequential transfers) are too slow to load a full rootfs initramfs.
    GRUB must load only a small initramfs; the rootfs is a 1 MiB-block squashfs."""
    builder = open(BUILDER, encoding="utf-8").read()
    assert "mksquashfs" in builder
    assert "rootfs.squashfs" in builder
    assert "pack-initramfs.sh" in builder
    assert "-b 1048576" in builder
    assert 'cd "${ROOTFS}" && find' not in builder
    live = open(LIVE_INIT, encoding="utf-8").read()
    assert "rootfs.squashfs" in live
    assert "switch_root" in live
    assert "overlay" in live
    assert "blacklist uas" in live
    assert "modprobe uas" not in live
    assert "modprobe.blacklist=uas" in builder
    assert "modprobe.blacklist=uas" in open(INSTALL, encoding="utf-8").read()
    assert "while" in live and "sleep 1" in live
    # /proc must exist before it is mounted, or PID 1 exits and the kernel panics.
    # mkdir is not on PATH until --install, so the first one goes through busybox.
    assert "/bin/busybox mkdir -p" in live
    assert live.index("/bin/busybox mkdir -p") < live.index("mount -t proc")
    assert "/proc" in live.split("mount -t proc", 1)[0]
    wf = open(WORKFLOW, encoding="utf-8").read()
    match = re.search(r"-m\s+(\d+)", wf)
    assert match, "the boot smoke has no -m RAM option"
    ram = int(match.group(1))
    assert 2048 <= ram <= 3072, ram
    doc = open(
        os.path.join(REPO, "docs", "framebuffer-and-iso.md"), encoding="utf-8"
    ).read()
    assert "at least 2 GiB" in doc
    assert "squashfs" in doc
    assert "T420" in doc
    assert "5 GiB" not in doc


def test_boot_smoke_retries_dhcp():
    """#901: DHCP can be slow, so the NET_OK marker is not a single one-shot
    grep right after the shell appears."""
    wf = open(WORKFLOW, encoding="utf-8").read()
    net_lines = [ln for ln in wf.splitlines() if "MMCORE_%s" in ln and "NET_OK" in ln]
    assert len(net_lines) == 1, net_lines
    assert "while" in net_lines[0], net_lines[0]


def test_boot_smoke_waits_for_a_stable_mmcore_pid():
    """#918/#941: mmcore starts on tty1 via agetty autologin, which lags the
    ttyS0 shell. The STABLE_OK sample must poll (bounded) until a PID is present
    and unchanged. BusyBox `pgrep -x` anchors the whole argv[0] path
    (`/usr/local/bin/mmcore`), so it never matched; match the kernel `comm`
    name instead."""
    wf = open(WORKFLOW, encoding="utf-8").read()
    stable_lines = [
        ln for ln in wf.splitlines() if "MMCORE_%s" in ln and "STABLE_OK" in ln
    ]
    assert len(stable_lines) == 1, stable_lines
    line = stable_lines[0]
    assert "while" in line, line
    assert "/proc/[0-9]*" in wf, wf
    assert "/comm" in wf, wf
    assert "pgrep -x mmcore" not in wf, "BusyBox -x anchors the full argv[0]"


def test_boot_smoke_verifies_the_firmware_keep_list_landed():
    """#958: the smoke must assert representative kept firmware is present, so
    a bad prune cannot ship silently without it."""
    wf = open(WORKFLOW, encoding="utf-8").read()
    fw_lines = [ln for ln in wf.splitlines() if "iwlwifi-*.ucode" in ln]
    assert len(fw_lines) == 1, fw_lines
    line = fw_lines[0]
    for path in ("/lib/firmware/i915", "/lib/firmware/amdgpu", "/lib/firmware/brcm"):
        assert path in line, path
    assert "FW_OK" in line, line
    assert 'grep -q "MMCORE_FW_OK" boot.log' in wf


def test_boot_smoke_is_enabled():
    """#941: the gate must run again now STABLE_OK matches mmcore's comm name."""
    wf = open(WORKFLOW, encoding="utf-8").read()
    assert "if: false" not in wf
    assert "if: ${{ !inputs.skip_smoke }}" in wf
    # The dispatch default must smoke, or a manual re-attach cannot exercise it.
    assert "default: false" in wf


def test_boot_smoke_dumps_diagnostics_on_failure():
    """#941: a STABLE miss must dump why (process table, mmcore stderr, tty1
    agetty/login/mmcore state) before poweroff, so boot.log alone is enough."""
    wf = open(WORKFLOW, encoding="utf-8").read()
    # Assembled in the guest with printf (like the other markers), so the
    # source spells out the printf, not the concatenated marker.
    assert 'printf "MMCORE_%s\\n" DIAG_BEGIN' in wf
    assert 'printf "MMCORE_%s\\n" DIAG_END' in wf
    diag = [ln for ln in wf.splitlines() if "DIAG_BEGIN" in ln]
    assert len(diag) == 1, diag
    line = diag[0]
    # Gated on a failed STABLE sample.
    assert '[ "$ok" = 1 ] ||' in line, line
    assert "ps w" in line, line
    assert "/tmp/mmcore.stderr" in line, line
    assert "/proc/[0-9]*" in line and "/comm" in line, line


def test_profile_documents_ctrl_alt_vt_switching():
    """#909: Alt+F2 never reaches the Linux tty; it must be Ctrl+Alt+F2."""
    profile = open(PROFILE, encoding="utf-8").read()
    assert "Ctrl+Alt+F2" in profile
    assert "Ctrl+Alt+F1" in profile
    assert "Alt+F2" not in profile.replace("Ctrl+Alt+F2", "")


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
    """#863: no getty/login banner on tty1. /etc/issue and /etc/motd are
    printed by agetty/login on *every* console (including tty1), so they stay
    empty; the #923 install/update hint is a tty-guarded shell message instead."""
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


PNG2PPM = os.path.join(ISO, "boot", "png_to_ppm.py")
SPLASH = os.path.join(ISO, "boot", "splash.png")
SPLASH_C = os.path.join(ISO, "boot", "mmcore-splash.c")
SPLASH_START = os.path.join(OVERLAY, "usr", "local", "bin", "mmcore-splash-start")


def test_builder_bakes_and_starts_the_boot_splash():
    """#902: repaint GRUB's logo from userspace until mmcore takes over."""
    builder = open(BUILDER, encoding="utf-8").read()
    assert "mmcore-splash.c" in builder
    assert "png_to_ppm.py" in builder
    assert "/usr/share/mmcore/splash.ppm" in builder
    assert "/usr/local/bin/mmcore-splash" in builder
    # The early inittab hook starts the repainter before OpenRC.
    inittab = builder.split("cat > \"${ROOTFS}/etc/inittab\"", 1)[1]
    assert "::sysinit:/usr/local/bin/mmcore-splash-start" in inittab
    assert inittab.index("mmcore-splash-start") < inittab.index("openrc sysinit")


def test_splash_sources_and_wrapper_are_present():
    assert os.path.isfile(SPLASH_C), SPLASH_C
    assert os.path.isfile(PNG2PPM), PNG2PPM
    assert os.access(PNG2PPM, os.X_OK), PNG2PPM
    assert os.path.isfile(SPLASH_START), SPLASH_START
    assert os.access(SPLASH_START, os.X_OK), SPLASH_START
    _run(["bash", "-n", SPLASH_START])
    wrapper = open(SPLASH_START, encoding="utf-8").read()
    assert "mmcore-splash -i" in wrapper
    helper = open(SPLASH_C, encoding="utf-8").read()
    assert "P6" in helper
    assert "/usr/share/mmcore/splash.ppm" in helper


def test_splash_launch_records_a_pidfile_and_profile_kills_it(tmp_path):
    """#921: the helper is launched by absolute path, its PID is recorded, and
    the profile kills that PID. BusyBox `pkill -x mmcore-splash` cannot match
    the absolute argv[0], so the old name-only stop silently left the logo
    repainting over tty2 forever."""
    start = open(SPLASH_START, encoding="utf-8").read()
    profile = open(PROFILE, encoding="utf-8").read()

    # Launch path: absolute, with the child PID recorded for the stop path.
    assert "/usr/local/bin/mmcore-splash -i 2" in start
    assert "echo $! >/run/mmcore-splash.pid" in start
    # Stop path: kill the recorded PID, not a BusyBox-quirk name match.
    assert 'kill "$(cat /run/mmcore-splash.pid)"' in profile
    assert "pkill -x mmcore-splash" not in profile
    assert profile.index("/run/mmcore-splash.pid") < profile.index(
        "exec /usr/local/bin/mmcore"
    )

    # Behaviour: run the real launcher against a stub helper and a temp
    # pidfile (only the absolute paths are rewritten), then kill the recorded
    # PID the way the profile does and confirm it is really gone.
    stub = tmp_path / "mmcore-splash"
    stub.write_text("#!/bin/sh\nexec sleep 60\n")
    stub.chmod(0o755)
    pidfile = tmp_path / "mmcore-splash.pid"
    launcher = tmp_path / "mmcore-splash-start"
    launcher.write_text(
        start.replace("/usr/local/bin/mmcore-splash", str(stub)).replace(
            "/run/mmcore-splash.pid", str(pidfile)
        )
    )
    launcher.chmod(0o755)
    _run([BASH, launcher])
    pid = int(pidfile.read_text().strip())
    try:
        assert os.getpgid(pid)  # the recorded PID names a live process
        os.kill(pid, signal.SIGTERM)
        for _ in range(50):
            try:
                os.kill(pid, 0)
            except ProcessLookupError:
                break
            time.sleep(0.1)
        with pytest.raises(ProcessLookupError):
            os.kill(pid, 0)
    finally:
        try:
            os.kill(pid, signal.SIGKILL)
        except ProcessLookupError:
            pass


def test_png_to_ppm_converts_the_splash(tmp_path):
    out = tmp_path / "splash.ppm"
    _run([sys.executable, PNG2PPM, SPLASH, str(out)])
    data = out.read_bytes()
    assert data[:2] == b"P6"
    header, _, payload = data.partition(b"255\n")
    width, height = (int(x) for x in header.split(b"\n")[1].split())
    assert (width, height) == (1024, 768)
    assert len(payload) == width * height * 3

    def pixel(x, y):
        o = (y * width + x) * 3
        return payload[o:o + 3]

    # Black border all round...
    for x, y in ((0, 0), (width - 1, 0), (0, height - 1), (width - 1, height - 1)):
        assert pixel(x, y) == b"\x00\x00\x00"
    # ...and the wordmark, not the old metallic "M": a solid band of visible
    # pixels in the centre rows, at ramdisk/mmcore.png's native 303x50 size
    # (its visible bbox is 296x47), centred on a 1024x768 canvas.
    visible = [
        (x, y)
        for y in range(height)
        for x in range(width)
        if pixel(x, y) != b"\x00\x00\x00"
    ]
    assert len(visible) > 1000, len(visible)
    xs = [x for x, _ in visible]
    ys = [y for _, y in visible]
    assert (max(xs) - min(xs) + 1, max(ys) - min(ys) + 1) == (296, 47)
    assert abs((min(xs) + max(xs)) / 2 - width / 2) <= 1
    assert abs((min(ys) + max(ys)) / 2 - height / 2) <= 1
    # NOTE: the exact centre pixel can fall in the gap between glyphs, so the
    # band/bbox checks above (not a single pixel) are what prove the wordmark.


def test_live_console_hints_install_and_update_on_tty2():
    """#923: mmcore only paints tty1, so tty2 (and serial) get one unobtrusive
    line naming the installer/updater. tty1 must stay banner-free (#863), so
    the hint is a tty-guarded shell message, not /etc/motd."""
    profile = open(PROFILE, encoding="utf-8").read()
    hint = re.search(
        r"mmcore live media\.[^\n]*mmcore-install[^\n]*mmcore-update", profile
    )
    assert hint, "the tty2 install/update hint is missing"
    # The hint is inside a non-tty1 guard, so it never prints on tty1.
    guard = profile.index('!= "/dev/tty1"')
    assert guard < profile.index("mmcore-install")
    # /etc/motd is printed by login on tty1 too and must stay empty.
    motd = os.path.join(OVERLAY, "etc", "motd")
    assert os.path.getsize(motd) == 0


def test_builder_guarantees_the_ca_bundle():
    """#896: mmcore-update's HTTPS download must verify the GitHub cert."""
    builder = open(BUILDER, encoding="utf-8").read()
    assert "/etc/ssl/certs/ca-certificates.crt" in builder
    # If apk left the bundle unbuilt, assemble it from the shipped certs...
    assert "usr/share/ca-certificates" in builder
    # ...and fail loudly rather than ship a TLS-less image.
    assert "[ -s \"${CA_BUNDLE}\" ] || die" in builder


def test_iso_workflow_verifies_guest_tls():
    """#896: the boot smoke fetches the GitHub API over HTTPS from the guest."""
    wf = open(WORKFLOW, encoding="utf-8").read()
    assert "api.github.com/repos/marnixk/mmcore/releases/latest" in wf
    assert "MMCORE_TLS_OK" in wf
    assert "grep -q \"MMCORE_TLS_OK\" boot.log" in wf


def test_pack_initramfs_keeps_the_storage_closure_and_drops_uas(tmp_path):
    """The initramfs GRUB reads must stay small: storage modules and their
    deps, not unrelated drivers, and never uas."""
    if shutil.which("cpio") is None or shutil.which("gzip") is None:
        pytest.skip("cpio and gzip are required")
    root = tmp_path / "rootfs"
    kver = root / "lib" / "modules" / "9.9.9"

    def put(rel):
        path = kver / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b"mod")

    put("kernel/fs/squashfs/squashfs.ko")
    put("kernel/lib/libcrc32c.ko")
    put("kernel/fs/overlayfs/overlay.ko")
    put("kernel/drivers/usb/storage/usb-storage.ko")
    put("kernel/drivers/usb/storage/uas.ko.xz")
    put("kernel/drivers/net/wireless/extra.ko")
    (kver / "modules.dep").write_text(
        "kernel/fs/squashfs/squashfs.ko: kernel/lib/libcrc32c.ko\n"
        "kernel/lib/libcrc32c.ko:\n"
        "kernel/fs/overlayfs/overlay.ko:\n"
        "kernel/drivers/usb/storage/usb-storage.ko: "
        "kernel/lib/libcrc32c.ko kernel/drivers/usb/storage/uas.ko.xz\n"
        "kernel/drivers/usb/storage/uas.ko.xz: kernel/lib/libcrc32c.ko\n"
        "kernel/drivers/net/wireless/extra.ko:\n",
        encoding="utf-8",
    )
    (root / "bin").mkdir(parents=True)
    (root / "bin" / "busybox").write_bytes(b"busybox")
    (root / "lib" / "ld-musl-x86_64.so.1").write_bytes(b"musl")
    out = tmp_path / "initramfs-lts"
    _run(["sh", PACK, str(root), str(out)])
    listing = _run(
        ["sh", "-c", 'gzip -dc "$1" | cpio -t', "sh", str(out)]
    ).stdout
    assert "squashfs.ko" in listing
    assert "libcrc32c.ko" in listing
    assert "overlay.ko" in listing
    assert "usb-storage.ko" in listing
    assert "uas.ko" not in listing
    assert "extra.ko" not in listing
    assert "bin/busybox" in listing
    assert "init" in listing
    selected = _run(
        ["sh", PACK, "--select-modules", str(kver / "modules.dep")]
    ).stdout
    assert "squashfs.ko" in selected
    assert "uas.ko" not in selected


def _iso_build_enabled():
    return os.environ.get("MMCORE_ISO_BUILD") == "1"


@pytest.fixture(scope="module")
def built_iso(tmp_path_factory):
    """Build the ISO once for every MMCORE_ISO_BUILD=1 test in this module."""
    if not _iso_build_enabled():
        pytest.skip("set MMCORE_ISO_BUILD=1 to run the Docker ISO build")
    if shutil.which("docker") is None:
        pytest.skip("docker is not available")
    dist = tmp_path_factory.mktemp("iso-dist")
    _run([BASH, ENTRY], env=dict(os.environ, DIST=str(dist)), timeout=3600)
    iso = dist / ASSET
    assert iso.is_file(), "the ISO build produced no image"
    return iso


def test_iso_builds(built_iso):
    assert built_iso.is_file()


DUAL_BOOT_TIMEOUT = 900


def _make_installed_disk(path):
    """Build an installed-disk stand-in (#976): an ext4 MMCORE-SYS volume whose
    /boot/rootfs.squashfs is deliberately not a squashfs. A live-init that
    mounted the first rootfs.squashfs it found would pick this disk and fail;
    the fixed live-init must boot the live media instead. Built with the same
    alpine builder as the ISO so no host loop device is needed."""
    payload = path.parent / "installed-payload"
    (payload / "boot").mkdir(parents=True, exist_ok=True)
    (payload / "boot" / "rootfs.squashfs").write_bytes(
        b"not a squashfs\n" * 4096
    )
    (payload / "VERSION.txt").write_text("INSTALLED-SENTINEL\n")
    script = (
        "apk add --no-cache --quiet e2fsprogs >/dev/null && "
        f"dd if=/dev/zero of=/out/{path.name} bs=1M count=256 status=none && "
        f"mke2fs -F -q -t ext4 -L MMCORE-SYS -d /payload /out/{path.name}"
    )
    _run(
        [
            "docker", "run", "--rm", "--platform", "linux/amd64",
            "-v", f"{payload}:/payload:ro",
            "-v", f"{path.parent}:/out",
            "alpine:3.20", "sh", "-c", script,
        ],
        timeout=900,
    )


@pytest.mark.skipif(
    not _iso_build_enabled(),
    reason="set MMCORE_ISO_BUILD=1 to run the dual-disk boot smoke",
)
def test_dual_disk_boot_smoke_selects_the_live_media(built_iso, tmp_path):
    """#976: with an installed-like MMCORE-SYS disk enumerated first, booting the
    live media must still run the live image. The live ISO carries a marker the
    disk stand-in lacks, so live-init must pick the marked media. A regression
    (scanning for the first rootfs.squashfs) mounts the disk's bogus squashfs
    and never reaches the live session."""
    qemu = shutil.which("qemu-system-x86_64")
    if qemu is None:
        pytest.skip("qemu-system-x86_64 is not available")
    if shutil.which("docker") is None:
        pytest.skip("docker is not available")

    installed = tmp_path / "installed.img"
    _make_installed_disk(installed)
    assert installed.is_file()

    log = tmp_path / "dual-boot.log"
    cmd = [
        qemu, "-m", "2048",
        # The installed stand-in is /dev/sda (enumerated first); the live media
        # is /dev/sdb. bootindex starts the firmware on the live disk, which is
        # what picking the USB in the boot menu does.
        "-drive", f"file={installed},format=raw,if=none,id=inst0",
        "-device", "ide-hd,drive=inst0,bus=ide.0,bootindex=2",
        "-drive", f"file={built_iso},format=raw,if=none,id=live0",
        "-device", "ide-hd,drive=live0,bus=ide.1,bootindex=1",
        "-vga", "none", "-device", "virtio-vga",
        "-display", "none", "-monitor", "none",
        "-serial", f"file:{log}", "-no-reboot", "-accel", "tcg",
    ]
    errfile = tmp_path / "qemu.err"
    with open(errfile, "w", encoding="utf-8") as err:
        proc = subprocess.Popen(cmd, stdout=err, stderr=subprocess.STDOUT)
    try:
        deadline = time.time() + DUAL_BOOT_TIMEOUT
        while time.time() < deadline:
            if log.exists() and "mmcore live media" in log.read_text(
                encoding="utf-8", errors="replace"
            ):
                break
            if proc.poll() is not None:
                break
            time.sleep(5)
        text = (
            log.read_text(encoding="utf-8", errors="replace")
            if log.exists()
            else ""
        )
        assert "mmcore live media" in text, (
            "the live session did not start with an installed disk present\n"
            + text[-2000:]
            + "\n-- qemu stderr --\n"
            + errfile.read_text(encoding="utf-8", errors="replace")[-1000:]
        )
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=30)
        except subprocess.TimeoutExpired:
            proc.kill()


REVERSE_BOOT_TIMEOUT = 600


def _extract_iso_boot(iso, dest):
    """Extract the live kernel and small initramfs from the built ISO, so the
    reverse smoke can boot them with QEMU's -kernel/-initrd. That bypasses
    GRUB (which the installed stand-in does not have) while still exercising
    the same initramfs live-init the disk install uses."""
    dest.mkdir(parents=True, exist_ok=True)
    script = (
        "apk add --no-cache --quiet xorriso >/dev/null && "
        f"xorriso -osirrox on -indev /iso/{iso.name} "
        "-extract /boot/vmlinuz-lts /out/vmlinuz-lts "
        "-extract /boot/initramfs-lts /out/initramfs-lts"
    )
    _run(
        [
            "docker", "run", "--rm", "--platform", "linux/amd64",
            "-v", f"{iso.parent}:/iso:ro",
            "-v", f"{dest}:/out",
            "alpine:3.20", "sh", "-c", script,
        ],
        timeout=900,
    )


def _make_reverse_boot_images(installed, live_stick):
    """Build the two stand-ins for the #979 smoke.

    `installed` is a whole-disk FAT volume labelled MMCORE-SYS whose
    /boot/rootfs.squashfs is a real (tiny) squashfs; its /sbin/init prints a
    marker over the serial console. `live_stick` is the stick stand-in: a FAT
    volume with the live marker and a /boot/rootfs.squashfs that is
    deliberately not a squashfs. The live-init generic scan would mount the
    stick first and then fail to mount its fake squashfs, so only an
    installed boot that resolves the labelled device reaches the marker. Built
    in an alpine container with mtools/squashfs-tools, so no host tools and no
    partition table are needed."""
    work = installed.parent / "reverse-work"
    work.mkdir(parents=True, exist_ok=True)
    script = (
        "set -e\n"
        "apk add --no-cache --quiet squashfs-tools mtools >/dev/null\n"
        "mkdir -p /work/rootfs/bin /work/rootfs/lib /work/rootfs/sbin\n"
        "cp /bin/busybox /work/rootfs/bin/busybox\n"
        "cp /lib/ld-musl-x86_64.so.1 /work/rootfs/lib/\n"
        "cat > /work/rootfs/sbin/init <<'INIT'\n"
        "#!/bin/busybox sh\n"
        'echo "MMCORE_INSTALLED_ROOTFS_OK" > /dev/console\n'
        "exec /bin/busybox sleep 86400\n"
        "INIT\n"
        "chmod 0755 /work/rootfs/sbin/init\n"
        "mksquashfs /work/rootfs /work/rootfs.squashfs "
        "-comp gzip -b 1M -noappend -no-progress >/dev/null\n"
        f"dd if=/dev/zero of=/out/{installed.name} bs=1M count=64 status=none\n"
        f"mformat -i /out/{installed.name} -F -v MMCORE-SYS ::\n"
        f"mmd -i /out/{installed.name} ::/boot\n"
        f"mcopy -i /out/{installed.name} /work/rootfs.squashfs "
        "::/boot/rootfs.squashfs\n"
        f"dd if=/dev/zero of=/out/{live_stick.name} bs=1M count=48 status=none\n"
        f"mformat -i /out/{live_stick.name} -F -v LIVESTICK ::\n"
        f"mmd -i /out/{live_stick.name} ::/boot\n"
        "printf 'not a squashfs\\n' > /work/bogus\n"
        f"mcopy -i /out/{live_stick.name} /work/bogus "
        "::/boot/rootfs.squashfs\n"
        "printf 'mmcore live media test\\n' > /work/marker\n"
        f"mcopy -i /out/{live_stick.name} /work/marker ::/mmcore-live-media"
    )
    _run(
        [
            "docker", "run", "--rm", "--platform", "linux/amd64",
            "-v", f"{work}:/work",
            "-v", f"{installed.parent}:/out",
            "alpine:3.20", "sh", "-c", script,
        ],
        timeout=900,
    )
    assert installed.is_file() and live_stick.is_file()


@pytest.mark.skipif(
    not _iso_build_enabled(),
    reason="set MMCORE_ISO_BUILD=1 to run the dual-disk boot smoke",
)
def test_dual_disk_boot_smoke_installed_boot_ignores_the_live_stick(
    built_iso, tmp_path
):
    """#979: the reverse of the #976 smoke. An installed boot (the kernel line
    carries mmcore.sys=LABEL=MMCORE-SYS) with a live stick enumerated first
    must mount the MMCORE-SYS device's squashfs, never the stick's. The stick's
    squashfs is deliberately invalid: the old generic scan selected it and the
    boot could not proceed, while the fixed live-init resolves the label and
    reaches the installed marker."""
    qemu = shutil.which("qemu-system-x86_64")
    if qemu is None:
        pytest.skip("qemu-system-x86_64 is not available")
    if shutil.which("docker") is None:
        pytest.skip("docker is not available")

    bootdir = tmp_path / "iso-boot"
    _extract_iso_boot(built_iso, bootdir)
    installed = tmp_path / "installed.img"
    live_stick = tmp_path / "live-stick.img"
    _make_reverse_boot_images(installed, live_stick)

    log = tmp_path / "reverse-boot.log"
    cmd = [
        qemu, "-m", "2048",
        "-kernel", str(bootdir / "vmlinuz-lts"),
        "-initrd", str(bootdir / "initramfs-lts"),
        "-append",
        "console=tty0 console=ttyS0,115200 quiet loglevel=3 "
        "vt.global_cursor_default=0 modprobe.blacklist=uas "
        "mmcore.sys=LABEL=MMCORE-SYS",
        # The stick stand-in is /dev/sda (enumerated first); the installed disk
        # is /dev/sdb. The generic scan would pick /dev/sda's fake squashfs.
        "-drive", f"file={live_stick},format=raw,if=none,id=stick0",
        "-device", "ide-hd,drive=stick0,bus=ide.0",
        "-drive", f"file={installed},format=raw,if=none,id=inst0",
        "-device", "ide-hd,drive=inst0,bus=ide.1",
        "-vga", "none", "-device", "virtio-vga",
        "-display", "none", "-monitor", "none",
        "-serial", f"file:{log}", "-no-reboot", "-accel", "tcg",
    ]
    errfile = tmp_path / "qemu-reverse.err"
    with open(errfile, "w", encoding="utf-8") as err:
        proc = subprocess.Popen(cmd, stdout=err, stderr=subprocess.STDOUT)
    try:
        deadline = time.time() + REVERSE_BOOT_TIMEOUT
        while time.time() < deadline:
            if log.exists() and "MMCORE_INSTALLED_ROOTFS_OK" in log.read_text(
                encoding="utf-8", errors="replace"
            ):
                break
            if proc.poll() is not None:
                break
            time.sleep(5)
        text = (
            log.read_text(encoding="utf-8", errors="replace")
            if log.exists()
            else ""
        )
        assert "MMCORE_INSTALLED_ROOTFS_OK" in text, (
            "an installed boot did not use the MMCORE-SYS squashfs with a "
            "live stick attached\n"
            + text[-2000:]
            + "\n-- qemu stderr --\n"
            + errfile.read_text(encoding="utf-8", errors="replace")[-1000:]
        )
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=30)
        except subprocess.TimeoutExpired:
            proc.kill()
