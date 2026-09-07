"""Pi Zero 2 / Zero 2 W are first-class release and install targets."""

import os
import stat
import subprocess
import zipfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCRIPTS = os.path.join(REPO, "scripts")


def _run(args, **kwargs):
    return subprocess.run(args, check=True, text=True, capture_output=True, **kwargs)


def test_scripts_parse_and_help_lists_zero2_models():
    for name in ("package-release.sh", "install-sdcard.sh", "github-release.sh"):
        _run(["bash", "-n", os.path.join(SCRIPTS, name)])
    help_out = _run([os.path.join(SCRIPTS, "install-sdcard.sh"), "--help"]).stdout
    assert "pizero2" in help_out
    assert "pizero2w" in help_out
    assert "Zero 2 W" in help_out


def test_package_and_release_scripts_name_zero2_zips():
    pkg = open(os.path.join(SCRIPTS, "package-release.sh"), encoding="utf-8").read()
    rel = open(os.path.join(SCRIPTS, "github-release.sh"), encoding="utf-8").read()
    inst = open(os.path.join(SCRIPTS, "install-sdcard.sh"), encoding="utf-8").read()
    assert "package_pizero2" in pkg
    assert "package_pizero2w" in pkg
    assert "brcmfmac43436-sdio.bin" in pkg
    assert "bcm2710-rpi-zero-2-w.dtb" in pkg
    assert "mmbasic-console-pizero2-v" in rel
    assert "mmbasic-console-pizero2w-v" in rel
    assert "pizero2w|zero2w" in inst
    assert pkg.find("package_pizero2") < pkg.find("build_hardware 4")


def test_release_notes_list_zero2_artifacts():
    notes = _run([os.path.join(SCRIPTS, "github-release.sh"), "release-notes", "9.9.9"]).stdout
    assert "mmbasic-console-pizero2-v9.9.9.zip" in notes
    assert "mmbasic-console-pizero2w-v9.9.9.zip" in notes
    assert "--model pizero2" in notes
    assert "--model pizero2w" in notes


def test_pack_only_builds_four_zips(tmp_path):
    console = tmp_path / "console"
    boot = tmp_path / "boot"
    wlan = tmp_path / "wlan"
    dist = tmp_path / "dist"
    console.mkdir()
    boot.mkdir()
    wlan.mkdir()
    (console / "kernel8.img").write_bytes(b"k3")
    (console / "kernel8-rpi4.img").write_bytes(b"k4")
    for name in (
        "bootcode.bin",
        "start.elf",
        "start4.elf",
        "fixup.dat",
        "fixup4.dat",
        "LICENCE.broadcom",
        "COPYING.linux",
        "armstub8-rpi4.bin",
        "bcm2710-rpi-zero-2-w.dtb",
        "bcm2710-rpi-zero-2.dtb",
        "bcm2711-rpi-400.dtb",
        "bcm2711-rpi-4-b.dtb",
    ):
        (boot / name).write_bytes(name.encode())
    for name in (
        "brcmfmac43430-sdio.bin",
        "brcmfmac43436-sdio.bin",
        "brcmfmac43436s-sdio.bin",
        "brcmfmac43455-sdio.bin",
    ):
        (wlan / name).write_bytes(name.encode())

    env = os.environ.copy()
    env.update(
        {
            "PACK_ONLY": "1",
            "VERSION": "9.9.9",
            "CONSOLE_DIR": str(console),
            "BOOT_DIR": str(boot),
            "WLAN_FW_DIR": str(wlan),
            "DIST": str(dist),
        }
    )
    _run(["bash", os.path.join(SCRIPTS, "package-release.sh")], env=env)

    zips = {
        "rpi3": dist / "mmbasic-console-rpi3-v9.9.9.zip",
        "pizero2": dist / "mmbasic-console-pizero2-v9.9.9.zip",
        "pizero2w": dist / "mmbasic-console-pizero2w-v9.9.9.zip",
        "pi400": dist / "mmbasic-console-pi400-v9.9.9.zip",
    }
    for path in zips.values():
        assert path.is_file(), path
        with zipfile.ZipFile(path) as zf:
            names = set(zf.namelist())
            assert "install-sdcard.sh" in names
            assert "config.txt" in names
            mode = zf.getinfo("install-sdcard.sh").external_attr >> 16
            assert mode & stat.S_IXUSR

    with zipfile.ZipFile(zips["pizero2"]) as zf:
        names = set(zf.namelist())
        assert "kernel8.img" in names
        assert "bcm2710-rpi-zero-2-w.dtb" in names
        assert "bcm2710-rpi-zero-2.dtb" in names
        assert not any(n.startswith("firmware/") for n in names)
        assert b"[pi02]" in zf.read("config.txt")
        assert b"no onboard WLAN" in zf.read("VERSION.txt")

    with zipfile.ZipFile(zips["pizero2w"]) as zf:
        names = set(zf.namelist())
        assert "kernel8.img" in names
        assert "firmware/brcmfmac43436-sdio.bin" in names
        assert "firmware/brcmfmac43436s-sdio.bin" in names
        assert b"CYW43436" in zf.read("VERSION.txt")

    with zipfile.ZipFile(zips["rpi3"]) as zf:
        names = set(zf.namelist())
        assert "firmware/brcmfmac43430-sdio.bin" in names
        assert "bcm2710-rpi-zero-2-w.dtb" not in names

    with zipfile.ZipFile(zips["pi400"]) as zf:
        names = set(zf.namelist())
        assert "kernel8-rpi4.img" in names
        assert "firmware/brcmfmac43455-sdio.bin" in names
