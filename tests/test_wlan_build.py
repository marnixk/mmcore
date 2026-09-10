"""WLAN is compiled into hardware images only; QEMU keeps the radio stub."""

import os
import subprocess

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def test_makefile_gates_wlan_on_qemu_no_sdhost():
    text = open(os.path.join(REPO, "console", "Makefile"), encoding="utf-8").read()
    assert "NO_SDHOST" in text
    assert "MMB_CIRCLE_WLAN" in text
    assert "libwlan.a" in text
    assert "libwpa_supplicant.a" in text
    assert "libsched.a" in text
    assert "libnet.a" in text


def test_build_script_builds_hostap_only_for_hardware():
    text = open(os.path.join(REPO, "scripts", "build.sh"), encoding="utf-8").read()
    assert "addon/wlan/hostap" in text
    assert "${QEMU:-1}" in text
    assert "wpa_supplicant" in text
    assert 'QEMU:-1}" = "0"' in text or '[ "${QEMU:-1}" = "0" ]' in text
    assert "--kernel-max-size 8" in text


def test_package_and_install_ship_brcmfmac_firmware():
    pkg = open(os.path.join(REPO, "scripts", "package-release.sh"), encoding="utf-8").read()
    inst = open(os.path.join(REPO, "scripts", "install-sdcard.sh"), encoding="utf-8").read()
    assert "brcmfmac43430-sdio.bin" in pkg
    assert "brcmfmac43436-sdio.bin" in pkg
    assert "brcmfmac43455-sdio.bin" in pkg
    assert "copy_wlan_firmware" in pkg
    assert "copy_wlan_firmware_dir" in inst
    assert "copy_wlan_firmware_mtools" in inst
    assert "--kernel-max-size 8" in pkg
    assert "check_kernel_end" in pkg
    subprocess.run(["bash", "-n", os.path.join(REPO, "scripts", "package-release.sh")], check=True)
    subprocess.run(["bash", "-n", os.path.join(REPO, "scripts", "install-sdcard.sh")], check=True)
    subprocess.run(["bash", "-n", os.path.join(REPO, "scripts", "build.sh")], check=True)


def test_net_cpp_uses_hostname_connect_overload():
    """CSocket::Connect(CIPAddress, u16) hides the DNS overload; call it via the base."""
    text = open(os.path.join(REPO, "console", "net.cpp"), encoding="utf-8").read()
    assert "static_cast<CNetSocket *>" in text
    assert "Connect(host" in text


def test_net_cpp_drains_circle_into_512kb_ring():
    """Circle leftover is one frame; TERM owns the 512KB interpret ring."""
    hdr = open(os.path.join(REPO, "console", "net_rxbuf.h"), encoding="utf-8").read()
    net = open(os.path.join(REPO, "console", "net.cpp"), encoding="utf-8").read()
    term = open(os.path.join(REPO, "mmbasic", "src", "cmd_term.c"), encoding="utf-8").read()
    mk = open(os.path.join(REPO, "console", "Makefile"), encoding="utf-8").read()
    assert "512u * 1024u" in hdr
    assert "MMB_NET_RX_CAP" in hdr
    assert "static u8 s_rx[FRAME_BUFFER_SIZE]" in net
    assert "Receive(s_rx, FRAME_BUFFER_SIZE" in net
    assert "s_rx_store[MMB_NET_RX_CAP]" not in net
    assert "term_rx_store[MMB_NET_RX_CAP]" in term
    assert "term_rx_interpret" in term
    assert "net_rxbuf.o" in mk


def test_qemu_kernel_end_fits_configured_max(kernel_image):
    """Circle halt()s if _end >= MEM_KERNEL_START + KERNEL_MAX_SIZE (8MB)."""
    map_path = os.path.join(os.path.dirname(kernel_image), "kernel8.map")
    assert os.path.isfile(map_path), map_path
    end = None
    for line in open(map_path, encoding="utf-8", errors="replace"):
        if "_end =" in line:
            parts = line.split()
            for p in parts:
                if p.startswith("0x"):
                    end = int(p, 16)
                    break
    assert end is not None, "_end not found in kernel8.map"
    start = 0x80000
    limit = start + 8 * 0x100000
    assert end < limit, f"_end 0x{end:x} exceeds 8MB kernel window (0x{limit:x})"


def test_qemu_kernel_does_not_link_wlan_driver(kernel_image):
    """raspi3b has no CYW4343x; the QEMU image must keep the stub path."""
    map_path = os.path.join(os.path.dirname(kernel_image), "kernel8.map")
    assert os.path.isfile(map_path), map_path
    text = open(map_path, encoding="utf-8", errors="replace").read()
    assert "CBcm4343Device" not in text
    assert "CWPASupplicant" not in text
    assert "mmb_wlan_available" in text
