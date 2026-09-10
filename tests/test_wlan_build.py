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
    assert "--kernel-max-size 4" in text


def test_package_and_install_ship_brcmfmac_firmware():
    pkg = open(os.path.join(REPO, "scripts", "package-release.sh"), encoding="utf-8").read()
    inst = open(os.path.join(REPO, "scripts", "install-sdcard.sh"), encoding="utf-8").read()
    assert "brcmfmac43430-sdio.bin" in pkg
    assert "brcmfmac43436-sdio.bin" in pkg
    assert "brcmfmac43455-sdio.bin" in pkg
    assert "copy_wlan_firmware" in pkg
    assert "copy_wlan_firmware_dir" in inst
    assert "copy_wlan_firmware_mtools" in inst
    subprocess.run(["bash", "-n", os.path.join(REPO, "scripts", "package-release.sh")], check=True)
    subprocess.run(["bash", "-n", os.path.join(REPO, "scripts", "install-sdcard.sh")], check=True)
    subprocess.run(["bash", "-n", os.path.join(REPO, "scripts", "build.sh")], check=True)


def test_net_cpp_uses_hostname_connect_overload():
    """CSocket::Connect(CIPAddress, u16) hides the DNS overload; call it via the base."""
    text = open(os.path.join(REPO, "console", "net.cpp"), encoding="utf-8").read()
    assert "static_cast<CNetSocket *>" in text
    assert "Connect(host" in text


def test_net_cpp_drains_circle_into_512kb_ring():
    """Circle Receive drops unread remainder; hold a 512KB ring, never a 1600-byte leftover."""
    hdr = open(os.path.join(REPO, "console", "net_rxbuf.h"), encoding="utf-8").read()
    text = open(os.path.join(REPO, "console", "net.cpp"), encoding="utf-8").read()
    mk = open(os.path.join(REPO, "console", "Makefile"), encoding="utf-8").read()
    assert "512u * 1024u" in hdr
    assert "MMB_NET_RX_CAP" in hdr
    assert "s_rx_store[MMB_NET_RX_CAP]" in text
    assert "Receive(s_frame, FRAME_BUFFER_SIZE" in text
    assert "mmb_net_rxbuf_push" in text
    assert "mmb_net_rxbuf_pop" in text
    assert "net_rxbuf.o" in mk
    assert "static u8 s_rx[FRAME_BUFFER_SIZE]" not in text


def test_qemu_kernel_does_not_link_wlan_driver(kernel_image):
    """raspi3b has no CYW4343x; the QEMU image must keep the stub path."""
    map_path = os.path.join(os.path.dirname(kernel_image), "kernel8.map")
    assert os.path.isfile(map_path), map_path
    text = open(map_path, encoding="utf-8", errors="replace").read()
    assert "CBcm4343Device" not in text
    assert "CWPASupplicant" not in text
    assert "mmb_wlan_available" in text
