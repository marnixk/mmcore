"""Drive-letter VFS: A: ramdisk, C: SD slot, D: USB volumes."""

import os
import shutil
import subprocess
import tempfile

import pytest

from harness import MMBasicConsole


def test_cwd_is_ramdisk(console):
    cwd = console.send_line("PRINT CWD$")
    assert cwd.upper().startswith("A:")


def test_drive_lists_a_and_c(console):
    out = console.send_line("DRIVE")
    assert "A: RAM" in out
    assert "C: SD" in out


def test_chdir_a_colon(console):
    assert console.send_line('CHDIR "A:"') == ""
    cwd = console.send_line("PRINT CWD$")
    assert cwd.upper().startswith("A:")


def test_open_on_a_drive(console):
    assert console.send_line('CHDIR "A:/"') == ""
    assert console.send_line('OPEN "A:DRV.TXT" FOR OUTPUT AS #1') == ""
    assert console.send_line('PRINT #1, "HI"') == ""
    assert console.send_line("CLOSE #1") == ""
    listing = console.send_line('DIR "A:/"')
    assert "DRV.TXT" in listing.upper()


def test_dir_a_glob(console):
    listing = console.send_line('DIR "A:/tests/*.PNG"')
    assert "TEST.PNG" in listing.upper()


def _write_text(con, path, text="X"):
    assert con.send_line(f'OPEN "{path}" FOR OUTPUT AS #1') == ""
    assert con.send_line(f'PRINT #1, "{text}"') == ""
    assert con.send_line("CLOSE #1") == ""


def _lines(text):
    return [line.strip() for line in text.splitlines() if line.strip()]


def test_dir_sorts_folders_then_files(console):
    assert console.send_line('CHDIR "A:"') == ""
    assert console.send_line('MKDIR "SORT391"') == ""
    assert console.send_line('MKDIR "A:/SORT391/SUB"') == ""
    _write_text(console, "A:/SORT391/ZETA.TXT")
    _write_text(console, "A:/SORT391/alpha.txt")
    assert _lines(console.send_line('DIR "A:/SORT391"')) == [
        "SUB/",
        "alpha.txt",
        "ZETA.TXT",
    ]


def test_dir_wide_packs_columns(console):
    assert console.send_line('MKDIR "WIDE391"') == ""
    _write_text(console, "A:/WIDE391/AAA.TXT")
    _write_text(console, "A:/WIDE391/BBB.TXT")
    _write_text(console, "A:/WIDE391/CCC.TXT")
    lines = _lines(console.send_line('DIR "A:/WIDE391" /W'))
    assert len(lines) == 1
    for name in ("AAA.TXT", "BBB.TXT", "CCC.TXT"):
        assert name in lines[0]


def test_dir_search_recurses_with_paths(console):
    assert console.send_line('MKDIR "SRCH391"') == ""
    assert console.send_line('MKDIR "A:/SRCH391/SUB"') == ""
    _write_text(console, "A:/SRCH391/TOP.BAS")
    _write_text(console, "A:/SRCH391/alpha.txt")
    _write_text(console, "A:/SRCH391/SUB/DEEP.BAS")
    assert _lines(console.send_line('DIR /S "A:/SRCH391"')) == [
        "SUB/",
        "SUB/DEEP.BAS",
        "alpha.txt",
        "TOP.BAS",
    ]


def test_dir_search_glob_matches_full_path(console):
    assert console.send_line('MKDIR "GLOB391"') == ""
    assert console.send_line('MKDIR "A:/GLOB391/SUB"') == ""
    _write_text(console, "A:/GLOB391/TOP.BAS")
    _write_text(console, "A:/GLOB391/SUB/DEEP.BAS")
    _write_text(console, "A:/GLOB391/NOTE.TXT")
    assert _lines(console.send_line('DIR "A:/GLOB391/*.BAS" /S')) == [
        "SUB/DEEP.BAS",
        "TOP.BAS",
    ]


def test_chdir_c_without_media(console):
    out = console.send_line('CHDIR "C:"')
    assert out.startswith("?")


def test_drive_select_a(console):
    assert console.send_line('DRIVE "A:"') == ""
    cwd = console.send_line("PRINT CWD$")
    assert cwd.upper().startswith("A:")


@pytest.mark.skipif(shutil.which("mkfs.vfat") is None, reason="mkfs.vfat not installed")
def test_sd_card_is_always_c(kernel_image):
    fd, img = tempfile.mkstemp(suffix=".img")
    os.close(fd)
    try:
        subprocess.run(
            ["dd", "if=/dev/zero", f"of={img}", "bs=1M", "count=64"],
            check=True, capture_output=True,
        )
        subprocess.run(
            ["mkfs.vfat", "-F", "32", "-n", "MMBASIC", img],
            check=True, capture_output=True,
        )
        os.sync()
        con = MMBasicConsole(
            kernel_image,
            extra_qemu=["-drive", f"file={img},if=sd,format=raw"],
            boot_timeout=30,
        )
        con.start()
        try:
            drv = con.send_line("DRIVE")
            assert "A: RAM" in drv
            assert "C: SD" in drv
            assert "no media" not in drv.lower()
            assert con.send_line('CHDIR "C:"') == ""
            assert con.send_line("PRINT CWD$").upper().startswith("C:")
            assert con.send_line('MKDIR "SUB"') == ""
            assert con.send_line('CHDIR "SUB"') == ""
            assert con.send_line('OPEN "N.TXT" FOR OUTPUT AS #1') == ""
            assert con.send_line('PRINT #1, "42"') == ""
            assert con.send_line("CLOSE #1") == ""
            listing = con.send_line("DIR")
            assert "N.TXT" in listing
            assert con.send_line('CHDIR "A:"') == ""
            assert con.send_line("PRINT CWD$").upper().startswith("A:")
        finally:
            con.stop()
    finally:
        os.unlink(img)
