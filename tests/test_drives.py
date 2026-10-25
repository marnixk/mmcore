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


def test_open_append_creates_file_at_open(console):
    """#1324: OPEN FOR APPEND creates the file immediately, before any write."""
    assert console.send_line('CHDIR "A:/"') == ""
    assert console.send_line('OPEN "A:/APP1324.TXT" FOR APPEND AS #1') == ""
    exists = console.send_line('PRINT "["; DIR$("A:/APP1324.TXT", FILE); "]"')
    assert exists == "[APP1324.TXT]", exists
    assert console.send_line("CLOSE #1") == ""


def test_open_creates_missing_parent_folders(console):
    """#1324: A: OPEN creates missing parents for OUTPUT and APPEND.

    This is the documented, intentional mkdir -p behaviour (unlike CMM2).
    """
    assert console.send_line('CHDIR "A:/"') == ""
    assert console.send_line('OPEN "A:/P1324/OUT/F.TXT" FOR OUTPUT AS #1') == ""
    assert console.send_line("CLOSE #1") == ""
    assert console.send_line('OPEN "A:/P1324/APP/G.TXT" FOR APPEND AS #1') == ""
    assert console.send_line("CLOSE #1") == ""
    assert "F.TXT" in console.send_line('DIR "A:/P1324/OUT"').upper()
    assert "G.TXT" in console.send_line('DIR "A:/P1324/APP"').upper()


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


def _dir_names(con, spec):
    """The bare entry names in a listing, upper-cased."""
    out = []
    for line in con.send_line(f'DIR "{spec}"').splitlines():
        line = line.strip()
        if not line or line == "(empty)":
            continue
        out.append(line.split()[0].upper())
    return out


def test_copy_directory_recursively(console):
    """#1112: COPY on a folder brings the whole tree, empty nests included."""
    assert console.send_line('CHDIR "A:/"') == ""
    assert console.send_line('MKDIR "C1112SRC"') == ""
    assert console.send_line('MKDIR "A:/C1112SRC/SUB"') == ""
    assert console.send_line('MKDIR "A:/C1112SRC/SUB/EMPTY"') == ""
    _write_text(console, "A:/C1112SRC/TOP.TXT", "top")
    _write_text(console, "A:/C1112SRC/SUB/DEEP.TXT", "deep")

    assert console.send_line('COPY "A:/C1112SRC" TO "A:/C1112DST"') == ""
    assert _dir_names(console, "A:/C1112DST") == ["SUB/", "TOP.TXT"]
    assert _dir_names(console, "A:/C1112DST/SUB") == ["EMPTY/", "DEEP.TXT"]
    # The nested empty folder came across as a folder.
    assert console.send_line('CHDIR "A:/C1112DST/SUB/EMPTY"') == ""
    assert console.send_line('CHDIR "A:/"') == ""


def test_copy_directory_into_existing_folder(console):
    """#1112: an existing destination folder receives the source under its own
    name; a same-named file inside it is overwritten."""
    assert console.send_line('CHDIR "A:/"') == ""
    assert console.send_line('MKDIR "C1112S2"') == ""
    _write_text(console, "A:/C1112S2/F.TXT", "new")
    assert console.send_line('MKDIR "C1112E2"') == ""
    _write_text(console, "A:/C1112E2/C1112S2/F.TXT", "old")
    _write_text(console, "A:/C1112E2/KEEP.TXT", "keep")

    assert console.send_line('COPY "A:/C1112S2" TO "A:/C1112E2"') == ""
    assert _dir_names(console, "A:/C1112E2/C1112S2") == ["F.TXT"]
    assert "new" in console.send_line('cat "A:/C1112E2/C1112S2/F.TXT"')
    assert _dir_names(console, "A:/C1112E2") == ["C1112S2/", "KEEP.TXT"]


def test_copy_directory_into_itself_is_refused(console):
    """#1112: copying a folder into itself or its subtree must fail, not loop."""
    assert console.send_line('CHDIR "A:/"') == ""
    assert console.send_line('MKDIR "C1112SELF"') == ""
    assert console.send_line('MKDIR "A:/C1112SELF/SUB"') == ""
    _write_text(console, "A:/C1112SELF/SUB/X.TXT", "x")
    assert console.send_line(
        'COPY "A:/C1112SELF" TO "A:/C1112SELF/SUB"'
    ).startswith("?")
    assert console.send_line(
        'COPY "A:/C1112SELF" TO "A:/C1112SELF/SUB/NEW"'
    ).startswith("?")
    # Nothing was created and the source is intact.
    assert "X.TXT" in console.send_line('DIR "A:/C1112SELF/SUB"').upper()


def test_name_moves_directory_into_folder(console):
    """#1112: NAME on a folder moves the whole tree inside another folder."""
    assert console.send_line('CHDIR "A:/"') == ""
    assert console.send_line('MKDIR "M1112SRC"') == ""
    assert console.send_line('MKDIR "A:/M1112SRC/IN"') == ""
    _write_text(console, "A:/M1112SRC/TOP.TXT", "t")
    _write_text(console, "A:/M1112SRC/IN/DEEP.TXT", "d")
    assert console.send_line('MKDIR "M1112DST"') == ""

    assert console.send_line('NAME "A:/M1112SRC" AS "A:/M1112DST"') == ""
    assert "M1112SRC" not in console.send_line('DIR "A:/"').upper()
    assert _dir_names(console, "A:/M1112DST") == ["M1112SRC/"]
    assert _dir_names(console, "A:/M1112DST/M1112SRC") == ["IN/", "TOP.TXT"]
    assert _dir_names(console, "A:/M1112DST/M1112SRC/IN") == ["DEEP.TXT"]


def test_name_directory_into_itself_is_refused(console):
    """#1112: moving a folder into its own subtree is refused."""
    assert console.send_line('CHDIR "A:/"') == ""
    assert console.send_line('MKDIR "M1112SELF"') == ""
    assert console.send_line('MKDIR "A:/M1112SELF/SUB"') == ""
    assert console.send_line(
        'NAME "A:/M1112SELF" AS "A:/M1112SELF/SUB"'
    ).startswith("?")
    assert "SUB/" in console.send_line('DIR "A:/M1112SELF"').upper()


@pytest.mark.skipif(shutil.which("mkfs.vfat") is None, reason="mkfs.vfat not installed")
def test_move_directory_across_drives_copies_and_deletes(kernel_image):
    """#1112: moving a folder A: -> C: has no rename primitive, so it copies
    the tree and then deletes the source, empty nests included."""
    fd, img = tempfile.mkstemp(suffix=".img")
    os.close(fd)
    try:
        subprocess.run(
            ["dd", "if=/dev/zero", f"of={img}", "bs=1M", "count=64"],
            check=True, capture_output=True,
        )
        subprocess.run(
            ["mkfs.vfat", "-F", "32", "-n", "MMBXDRV", img],
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
            assert con.send_line('CHDIR "A:/"') == ""
            assert con.send_line('MKDIR "X1112SRC"') == ""
            assert con.send_line('MKDIR "A:/X1112SRC/SUB"') == ""
            assert con.send_line('MKDIR "A:/X1112SRC/SUB/EMPTY"') == ""
            _write_text(con, "A:/X1112SRC/TOP.TXT", "top")
            _write_text(con, "A:/X1112SRC/SUB/DEEP.TXT", "deep")

            assert con.send_line('NAME "A:/X1112SRC" AS "C:/X1112SRC"') == ""
            assert "X1112SRC" not in con.send_line('DIR "A:/"').upper()
            assert _dir_names(con, "C:/X1112SRC") == ["SUB/", "TOP.TXT"]
            assert _dir_names(con, "C:/X1112SRC/SUB") == ["EMPTY/", "DEEP.TXT"]
            assert con.send_line('CHDIR "C:/X1112SRC/SUB/EMPTY"') == ""
            assert con.send_line('CHDIR "A:/"') == ""
        finally:
            con.stop()
    finally:
        os.unlink(img)


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


def test_storage_read_at_slices_and_yields():
    """#1149: a large physical read is served in bounded slices that call
    storage_yield() between f_read() calls, so the scheduler keeps running
    during the read instead of only after it returns."""
    src = open(os.path.join(REPO, "console", "storage.cpp"), encoding="utf-8").read()
    assert "#define MMB_FAT_READ_SLICE" in src
    # The f_read + storage_yield pair must sit inside the slice loop.
    body = src[src.index("while (total < n)"):]
    body = body[: body.index("s_rd_pos[idx] = pos + total;")]
    assert "f_read(fp, dst + total, want, &r)" in body
    assert "storage_yield();" in body


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
    assert console.send_line('EJECT "Y:"').startswith("?")


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
def test_fat_open_append_creates_file_at_open(kernel_image):
    """#1324: the FAT backend also creates an APPEND target at OPEN."""
    fd, img = tempfile.mkstemp(suffix=".img")
    os.close(fd)
    try:
        subprocess.run(
            ["dd", "if=/dev/zero", f"of={img}", "bs=1M", "count=64"],
            check=True, capture_output=True,
        )
        subprocess.run(
            ["mkfs.vfat", "-F", "32", "-n", "MMBAP1324", img],
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
            assert con.send_line('OPEN "APP1324.TXT" FOR APPEND AS #1') == ""
            exists = con.send_line(
                'PRINT "["; DIR$("C:/APP1324.TXT", FILE); "]"'
            )
            assert exists == "[APP1324.TXT]", exists
            assert con.send_line("CLOSE #1") == ""
            # OUTPUT likewise creates the target at OPEN, even with no write.
            assert con.send_line('OPEN "OUT1324.TXT" FOR OUTPUT AS #1') == ""
            exists = con.send_line(
                'PRINT "["; DIR$("C:/OUT1324.TXT", FILE); "]"'
            )
            assert exists == "[OUT1324.TXT]", exists
            assert con.send_line("CLOSE #1") == ""
        finally:
            con.stop()
    finally:
        os.unlink(img)


@pytest.mark.skipif(shutil.which("mkfs.vfat") is None, reason="mkfs.vfat not installed")
def test_large_physical_read_spans_slices(kernel_image):
    """#1149: a whole-file read larger than one slice (>256 KB here) is served
    from the SD volume in one VFS read and leaves the console responsive.

    The SD backend is a host file, so QEMU cannot reproduce the multi-second
    read whose blocking froze the interpreter; this exercises the slice loop
    end to end, while test_storage_read_at_slices_and_yields() pins the
    per-slice storage_yield() that keeps the scheduler running.
    """
    fd, img = tempfile.mkstemp(suffix=".img")
    os.close(fd)
    try:
        subprocess.run(
            ["dd", "if=/dev/zero", f"of={img}", "bs=1M", "count=64"],
            check=True, capture_output=True,
        )
        subprocess.run(
            ["mkfs.vfat", "-F", "32", "-n", "MMBSLICE", img],
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
            # Build a >256 KB file on C:. The generator program stays tiny so
            # it fits program memory; the payload is only ever read back.
            assert con.send_line('10 OPEN "C:/BIG.BIN" FOR OUTPUT AS #1') == ""
            assert con.send_line("20 FOR I=1 TO 320") == ""
            assert con.send_line(
                '30 PRINT #1, STRING$(1000, ".")+CHR$(13)+CHR$(10);'
            ) == ""
            assert con.send_line("40 NEXT I") == ""
            assert con.send_line("50 CLOSE #1") == ""
            assert con.send_line("RUN", timeout=120) == ""
            listing = con.send_line('DIR "C:/"').upper()
            assert "BIG.BIN" in listing
            size = int(listing.split("BIG.BIN")[1].split()[0])
            assert size > 256 * 1024
            # PLAY MP3 loads the whole file with one VFS read (load_bytes), so
            # it spans two slices. It is not valid audio, so a clean ?MP3
            # proves the read returned and the console kept running.
            assert con.send_line('PLAY MP3 "C:/BIG.BIN"').startswith("?")
            assert con.send_line("PRINT 6*7") == "42"
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
