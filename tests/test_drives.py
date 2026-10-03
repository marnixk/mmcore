"""Drive-letter VFS: A: ramdisk, C: SD slot, D: USB volumes."""

import os
import shutil
import subprocess
import tempfile

import pytest

from harness import MMBasicConsole

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


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


def test_name_moves_file_between_folders(console):
    """#1028: NAME across folders reparents a file on the A: ramdisk, so the
    FILES cross-folder move no longer reports "Move failed"."""
    assert console.send_line('CHDIR "A:"') == ""
    assert console.send_line('MKDIR "RN1028A"') == ""
    assert console.send_line('MKDIR "RN1028B"') == ""
    _write_text(console, "A:/RN1028A/MV.TXT", "hi")
    assert console.send_line(
        'NAME "A:/RN1028A/MV.TXT" AS "A:/RN1028B/MV.TXT"'
    ) == ""
    assert "MV.TXT" not in console.send_line('DIR "A:/RN1028A"').upper()
    assert "MV.TXT" in console.send_line('DIR "A:/RN1028B"').upper()


def test_dir_sorts_folders_then_files(console):
    assert console.send_line('CHDIR "A:"') == ""
    assert console.send_line('MKDIR "SORT391"') == ""
    assert console.send_line('MKDIR "A:/SORT391/SUB"') == ""
    _write_text(console, "A:/SORT391/ZETA.TXT")
    _write_text(console, "A:/SORT391/alpha.txt")
    assert _lines(console.send_line('DIR "A:/SORT391"')) == [
        "SUB/           <DIR>",
        "alpha.txt          2",
        "ZETA.TXT           2",
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
    # #981: each directory's entries align to their own name column, so the
    # nested SUB/DEEP.BAS no longer widens the root listing's column.
    assert _lines(console.send_line('DIR /S "A:/SRCH391"')) == [
        "SUB/           <DIR>",
        "SUB/DEEP.BAS          2",
        "alpha.txt          2",
        "TOP.BAS            2",
    ]


def test_dir_search_glob_matches_full_path(console):
    assert console.send_line('MKDIR "GLOB391"') == ""
    assert console.send_line('MKDIR "A:/GLOB391/SUB"') == ""
    _write_text(console, "A:/GLOB391/TOP.BAS")
    _write_text(console, "A:/GLOB391/SUB/DEEP.BAS")
    _write_text(console, "A:/GLOB391/NOTE.TXT")
    assert _lines(console.send_line('DIR "A:/GLOB391/*.BAS" /S')) == [
        "SUB/DEEP.BAS          2",
        "TOP.BAS          2",
    ]


def test_storage_latches_mount_probe():
    """#982: an unmountable volume must not force a mount on every poll."""
    src = open(os.path.join(REPO, "console", "storage.cpp"), encoding="utf-8").read()
    assert "s_probed" in src
    assert "s_ready[idx] || s_ejected[idx] || s_probed[idx]" in src


def test_storage_chunks_usb_work_and_keeps_read_handle():
    """#983/#984: FS work yields between chunks and reuses an open read FIL."""
    src = open(os.path.join(REPO, "console", "storage.cpp"), encoding="utf-8").read()
    assert "storage_yield" in src
    assert "s_rd[idx]" in src


def test_storage_keeps_append_write_handle():
    """#1000: FAT append reuses one open write FIL instead of reopening and
    f_lseek(f_size()) - a FAT-chain walk from cluster 0 - for every chunk."""
    src = open(os.path.join(REPO, "console", "storage.cpp"), encoding="utf-8").read()
    assert "s_wr[idx]" in src
    assert "close_write_cache_path" in src


def test_chdir_c_without_media(console):
    out = console.send_line('CHDIR "C:"')
    assert out.startswith("?")


def test_eject_missing_drive_errors(console):
    """EJECT on a volume that is not present must fail, not crash."""
    assert console.send_line('EJECT "D:"').startswith("?")
    assert console.send_line('EJECT "G:"').startswith("?")


def test_eject_ramdisk_and_unknown_letter_error(console):
    assert console.send_line('EJECT "A:"').startswith("?")
    assert console.send_line('EJECT "Z:"').startswith("?")


def test_drive_never_labels_ramdisk(console):
    out = console.send_line("DRIVE")
    assert "A: RAM" in out
    assert 'A: RAM "' not in out


def test_drive_select_a(console):
    assert console.send_line('DRIVE "A:"') == ""
    cwd = console.send_line("PRINT CWD$")
    assert cwd.upper().startswith("A:")


def test_cat_prompt_prints_file(console):
    assert console.send_line('OPEN "A:/CAT391.TXT" FOR OUTPUT AS #1') == ""
    assert console.send_line('PRINT #1, "HELLO"') == ""
    assert console.send_line('PRINT #1, "WORLD"') == ""
    assert console.send_line("CLOSE #1") == ""
    assert console.send_line('cat "A:/CAT391.TXT"') == "HELLO\nWORLD"


def test_cat_prompt_accepts_bare_path(console):
    _write_text(console, "A:/CATBARE.TXT", "BARE")
    assert console.send_line("cat A:/CATBARE.TXT") == "BARE"


def test_cat_prompt_missing_file(console):
    assert console.send_line("cat A:/NOPE391.TXT").startswith("?")


def test_cat_prompt_keeps_string_concat(console):
    assert console.send_line('A$="MM"') == ""
    assert console.send_line('CAT A$,"BASIC"') == ""
    assert console.send_line("PRINT A$") == "MMBASIC"


@pytest.mark.skipif(shutil.which("mkfs.vfat") is None, reason="mkfs.vfat not installed")
def test_sd_volume_label_shown(kernel_image):
    """A mounted FAT volume reports its label in DRIVE (#518/#523)."""
    fd, img = tempfile.mkstemp(suffix=".img")
    os.close(fd)
    try:
        subprocess.run(
            ["dd", "if=/dev/zero", f"of={img}", "bs=1M", "count=64"],
            check=True, capture_output=True,
        )
        subprocess.run(
            ["mkfs.vfat", "-F", "32", "-n", "MMBTEST", img],
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
            assert 'C: SD "MMBTEST"' in drv.upper(), drv
            # The SD slot is the system drive and must never be ejectable.
            assert con.send_line('EJECT "C:"').startswith("?")
            assert con.send_line('CHDIR "C:"') == ""
        finally:
            con.stop()
    finally:
        os.unlink(img)


@pytest.mark.skipif(shutil.which("mkfs.vfat") is None, reason="mkfs.vfat not installed")
def test_fat_append_reuses_write_handle(kernel_image):
    """#1000: a FAT append session keeps prior bytes and order across a reused
    open write FIL (the cache must not truncate or misposition the file)."""
    fd, img = tempfile.mkstemp(suffix=".img")
    os.close(fd)
    try:
        subprocess.run(
            ["dd", "if=/dev/zero", f"of={img}", "bs=1M", "count=64"],
            check=True, capture_output=True,
        )
        subprocess.run(
            ["mkfs.vfat", "-F", "32", "-n", "MMBAPP", img],
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
            assert con.send_line('CHDIR "C:"') == ""
            assert con.send_line('OPEN "APP.TXT" FOR OUTPUT AS #1') == ""
            assert con.send_line('PRINT #1, "ONE"') == ""
            assert con.send_line("CLOSE #1") == ""
            assert con.send_line('OPEN "APP.TXT" FOR APPEND AS #1') == ""
            for i in range(20):
                assert con.send_line(f'PRINT #1, "APP{i}"') == ""
            assert con.send_line("CLOSE #1") == ""
            out = con.send_line('cat "C:/APP.TXT"')
            assert "ONE" in out
            assert "APP0" in out and "APP19" in out
            assert out.index("ONE") < out.index("APP0") < out.index("APP19")
            assert "APP.TXT" in con.send_line('DIR "C:/"').upper()
        finally:
            con.stop()
    finally:
        os.unlink(img)


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
            assert con.send_line('OPEN "ROOT.TXT" FOR OUTPUT AS #1') == ""
            assert con.send_line('PRINT #1, "root"') == ""
            assert con.send_line("CLOSE #1") == ""
            assert con.send_line('CHDIR "SUB"') == ""
            assert con.send_line('OPEN "N.TXT" FOR OUTPUT AS #1') == ""
            assert con.send_line('PRINT #1, "42"') == ""
            assert con.send_line("CLOSE #1") == ""
            listing = con.send_line("DIR")
            assert "N.TXT" in listing
            # A FAT volume root must count as a directory even though FatFs
            # f_stat() rejects the root path; recursive DIR from a subfolder
            # must search the root, not the current folder (#ftp-cwd-root).
            recursive = con.send_line('DIR /S "C:/"')
            assert "ROOT.TXT" in recursive.upper()
            assert "SUB/N.TXT" in recursive.upper()
            assert con.send_line('CHDIR "A:"') == ""
            assert con.send_line("PRINT CWD$").upper().startswith("A:")
        finally:
            con.stop()
    finally:
        os.unlink(img)
