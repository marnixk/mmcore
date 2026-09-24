"""PACKAGE / RUN name.app: zip archive mounted read-only as B:."""

import time

from ihelp_util import dump_topic


def _write_lines(con, path, lines):
    assert con.send_line(f'OPEN "{path}" FOR OUTPUT AS #1') == ""
    for line in lines:
        escaped = line.replace('"', '""')
        assert con.send_line(f'PRINT #1, "{escaped}"') == ""
    assert con.send_line("CLOSE #1") == ""


def _make_game(con, folder, main_lines=None):
    assert con.send_line('CHDIR "A:/"') == ""
    assert con.send_line(f'MKDIR "{folder}"') == ""
    if main_lines is None:
        main_lines = [
            "PRINT CWD$",
            'OPEN "GFX/DATA.TXT" FOR INPUT AS #1',
            "LINE INPUT #1, A$",
            "PRINT A$",
            "CLOSE #1",
            'CHDIR "GFX"',
            "PRINT CWD$",
        ]
    _write_lines(con, f"{folder}/MAIN.BAS", main_lines)
    assert con.send_line(f'MKDIR "{folder}/GFX"') == ""
    _write_lines(con, f"{folder}/GFX/DATA.TXT", ["HELLOPKG"])


def test_package_run_mounts_b(console):
    _make_game(console, "PKGAME")
    assert console.send_line('PACKAGE "GAME.APP", "PKGAME/"') == ""
    listing = console.send_line('DIR "A:/"')
    assert "GAME.APP" in listing.upper()
    out = console.send_line('RUN "GAME.APP"')
    lines = [ln.strip() for ln in out.splitlines() if ln.strip()]
    assert any(ln.upper().startswith("B:") for ln in lines), out
    assert "HELLOPKG" in out
    assert any("GFX" in ln.upper() for ln in lines), out
    cwd = console.send_line("PRINT CWD$")
    assert cwd.upper().startswith("A:")
    drv = console.send_line("DRIVE")
    assert "B:" not in drv
    assert console.send_line('CHDIR "B:"').startswith("?DRIVE")
    assert console.send_line("PRINT CWD$").upper().startswith("A:")


def test_package_readonly_and_bare_run(console):
    _make_game(
        console,
        "PKRO",
        [
            'OPEN "X.TXT" FOR OUTPUT AS #1',
            "PRINT 1",
        ],
    )
    assert console.send_line('PACKAGE "RO.APP", "PKRO/"') == ""
    out = console.send_line('RUN "RO.APP"')
    assert "?READ ONLY" in out.upper()
    cwd = console.send_line("PRINT CWD$")
    assert cwd.upper().startswith("A:")
    assert console.send_line('CHDIR "B:"').startswith("?DRIVE")
    listed = console.send_line("LIST")
    assert "OPEN" in listed.upper()


def test_package_file_exists_in_program(console):
    _make_game(console, "PKDUP")
    assert console.send_line('PACKAGE "DUP.APP", "PKDUP/"') == ""
    assert console.send_line("NEW") == ""
    assert console.send_line('10 PACKAGE "DUP.APP", "PKDUP/"') == ""
    out = console.send_line("RUN")
    assert "FILE EXISTS" in out.upper()


def test_package_overwrite_prompt(console):
    _make_game(console, "PKOW")
    assert console.send_line('PACKAGE "OW.APP", "PKOW/"') == ""
    con = console
    con.drain(quiet=0.1, timeout=0.4)
    con._ser.sendall(b'PACKAGE "OW.APP", "PKOW/"\r')
    deadline = time.time() + 4.0
    buf = b""
    while time.time() < deadline:
        chunk = con._recv(con._ser)
        if chunk:
            buf += chunk
            if b"overwrite" in buf.lower():
                break
    assert b"overwrite" in buf.lower(), buf
    con._ser.sendall(b"n\r")
    deadline = time.time() + 3.0
    while time.time() < deadline:
        chunk = con._recv(con._ser)
        if chunk:
            buf += chunk
            if buf.rstrip().endswith(b">"):
                break
    assert b"?FILE EXISTS" not in buf.upper()
    assert "HELLOPKG" in console.send_line('RUN "OW.APP"')
    con.drain(quiet=0.1, timeout=0.4)
    con._ser.sendall(b'PACKAGE "OW.APP", "PKOW/"\r')
    deadline = time.time() + 4.0
    buf = b""
    while time.time() < deadline:
        chunk = con._recv(con._ser)
        if chunk:
            buf += chunk
            if b"overwrite" in buf.lower():
                break
    con._ser.sendall(b"\r")
    deadline = time.time() + 3.0
    while time.time() < deadline:
        chunk = con._recv(con._ser)
        if chunk:
            buf += chunk
            if buf.rstrip().endswith(b">"):
                break
    assert "HELLOPKG" in console.send_line('RUN "OW.APP"')


def test_package_missing_main(console):
    assert console.send_line('CHDIR "A:/"') == ""
    assert console.send_line('MKDIR "NOMAIN"') == ""
    _write_lines(console, "GAMELESS.TXT", ["x"])
    assert console.send_line('COPY "GAMELESS.TXT" TO "NOMAIN/X.TXT"') == ""
    out = console.send_line('PACKAGE "BAD.APP", "NOMAIN/"')
    assert "MAIN" in out.upper()


def test_package_chdir_dotdot_stays_on_b(console):
    _make_game(
        console,
        "PKDOT",
        [
            'CHDIR ".."',
            "PRINT CWD$",
        ],
    )
    assert console.send_line('PACKAGE "DOT.APP", "PKDOT/"') == ""
    out = console.send_line('RUN "DOT.APP"')
    assert "B:" in out.upper()
    assert console.send_line("PRINT CWD$").upper().startswith("A:")


def test_package_run_bas_unmounts(console):
    _write_lines(console, "OTHER.BAS", ["PRINT 99"])
    _make_game(console, "PKNEST", ['RUN "A:/OTHER.BAS"'])
    assert console.send_line('PACKAGE "NEST.APP", "PKNEST/"') == ""
    out = console.send_line('RUN "NEST.APP"')
    assert "99" in out
    assert console.send_line("PRINT CWD$").upper().startswith("A:")


def test_help_package(console):
    out = dump_topic(console, "PACKAGE")
    assert "PACKAGE" in out.upper()
    assert "MAIN.BAS" in out.upper()
    assert "RUN" in out.upper()
    assert ".APP" in out.upper()
    assert ".PKG" not in out.upper()
    assert "WIZARD" in out.upper()


def _wiz_open(con):
    assert con._ser is not None
    con.drain(quiet=0.2, timeout=2.0)
    con._ser.sendall(b"PACKAGE\r")
    return con.drain(quiet=0.9).decode(errors="replace")


def _wiz_keys(con, data, quiet=0.45):
    assert con._ser is not None
    con._ser.sendall(data)
    return con.drain(quiet=quiet).decode(errors="replace")


def _wiz_root(con, root):
    assert con.send_line('CHDIR "A:/"') == ""
    assert con.send_line(f'MKDIR "{root}"') == ""
    assert con.send_line(f'CHDIR "A:/{root}"') == ""


def test_package_wizard_creates_app(console):
    con = console
    _wiz_root(con, "WIZA")
    assert con.send_line('MKDIR "GAME"') == ""
    _write_lines(con, "GAME/MAIN.BAS", ['PRINT "WIZOK"'])
    assert con.send_line('MKDIR "GAME/GFX"') == ""
    _write_lines(con, "GAME/GFX/DATA.TXT", ["WIZDATA"])

    seen = _wiz_open(con)
    upper = seen.upper()
    assert "PACKAGE WIZARD" in upper
    assert "GAME" in upper
    assert "MAIN.BAS" in upper

    seen = _wiz_keys(con, b"\x1b[B")  # highlight GAME
    assert "GAME" in seen.upper()
    seen = _wiz_keys(con, b"\r")  # choose folder -> name form
    assert "GAME.APP" in seen.upper()
    _wiz_keys(con, b"\r")  # -> title
    _wiz_keys(con, b"\r")  # -> author
    seen = _wiz_keys(con, b"\r")  # create
    assert "WROTE" in seen.upper()
    _wiz_keys(con, b" ")  # any key closes

    listing = con.send_line('DIR "A:/WIZA"')
    assert "GAME.APP" in listing.upper()
    out = con.send_line('RUN "A:/WIZA/GAME.APP"')
    assert "WIZOK" in out
    cwd = con.send_line("PRINT CWD$")
    assert cwd.upper().startswith("A:")


def test_package_wizard_status_hint_no_doubled_gt(console):
    """#569: a `<hotkey>` tag renders one closing '>', not two."""
    con = console
    _wiz_root(con, "WIZH")
    assert con.send_line('MKDIR "GAMEH"') == ""
    _write_lines(con, "GAMEH/MAIN.BAS", ['PRINT "OK"'])
    seen = _wiz_open(con)
    assert "Move" in seen
    assert "Cancel" in seen
    assert "<Up/Down>" in seen
    assert ">>" not in seen
    _wiz_keys(con, b"\x1b")  # Esc cancels
    assert con.send_line("PRINT 1+1") == "2"


def test_package_wizard_opens_as_inset_dialog(console):
    """#590: the wizard is a centred framed dialog, not a full-screen app."""
    con = console
    _wiz_root(con, "WIZD")
    assert con.send_line('MKDIR "GAME"') == ""
    _write_lines(con, "GAME/MAIN.BAS", ['PRINT "OK"'])
    seen = _wiz_open(con)
    assert "PACKAGE WIZARD" in seen.upper()
    rows = [ln.rstrip() for ln in seen.replace("\r", "\n").split("\n")]
    border = [ln for ln in rows if "+" in ln and "-" in ln]
    assert border, seen
    top = border[0]
    assert top.startswith(" ") and top.index("+") > 0, repr(top)
    _wiz_keys(con, b"\x1b")  # Esc cancels back to the prompt
    assert con.send_line("PRINT 1+1") == "2"


def test_package_wizard_requires_main(console):
    con = console
    _wiz_root(con, "WIZB")
    assert con.send_line('MKDIR "NOMAIN"') == ""
    _write_lines(con, "NOMAIN/X.TXT", ["x"])

    seen = _wiz_open(con)
    assert "NOMAIN" in seen.upper()
    _wiz_keys(con, b"\x1b[B")  # highlight NOMAIN
    seen = _wiz_keys(con, b"\r")  # refuse: no MAIN.BAS
    upper = seen.upper()
    assert "NO MAIN.BAS" in upper
    assert "PACKAGE :" not in upper  # never reached the name form

    _wiz_keys(con, b"\x1b")  # Esc cancels
    assert con.send_line("PRINT 1+1") == "2"


def test_package_wizard_metadata(console):
    con = console
    _wiz_root(con, "WIZC")
    assert con.send_line('MKDIR "META"') == ""
    _write_lines(con, "META/MAIN.BAS", ['PRINT "METAOK"'])

    _wiz_open(con)
    _wiz_keys(con, b"\x1b[B")  # highlight META
    seen = _wiz_keys(con, b"\r")  # choose -> package name defaults
    assert "META.APP" in seen.upper()
    _wiz_keys(con, b"\r")  # package -> title
    seen = _wiz_keys(con, b"My Game")  # title text
    assert "MY GAME" in seen.upper()
    _wiz_keys(con, b"\r")  # title -> author
    _wiz_keys(con, b"Me")  # author text
    seen = _wiz_keys(con, b"\r")  # create
    assert "WROTE" in seen.upper()
    _wiz_keys(con, b" ")  # close

    assert "META.APP" in con.send_line('DIR "A:/WIZC"').upper()
    assert "METAOK" in con.send_line('RUN "A:/WIZC/META.APP"')
    assert con.send_line('UNPACK "META.APP"') == ""
    meta = con.send_line('CAT "PACKAGE.INF"')
    assert "TITLE=MY GAME" in meta.upper()
    assert "AUTHOR=ME" in meta.upper()


def test_load_app_is_rejected(console):
    _make_game(console, "PKLD")
    assert console.send_line('PACKAGE "L.APP", "PKLD/"') == ""
    out = console.send_line('LOAD "L.APP"')
    assert "?FILE" in out.upper()


def test_package_default_extension_is_app(console):
    _make_game(console, "PKEXT")
    assert console.send_line('PACKAGE "EXTGAME", "PKEXT/"') == ""
    listing = console.send_line('DIR "A:/"')
    assert "EXTGAME.APP" in listing.upper()


def test_package_archive_over_512k(console):
    """#405: bundles over the old 512 KiB cap package and mount again."""
    assert console.send_line('CHDIR "A:/"') == ""
    assert console.send_line('MKDIR "BIGPKG"') == ""
    _write_lines(console, "BIGPKG/MAIN.BAS", ['PRINT "OK"'])
    # 3000 * 200 bytes is ~600 KiB, above the old 512 KiB ceiling.
    chunk = "X" * 200
    _write_lines(
        console,
        "GEN.BAS",
        [
            'OPEN "A:/BIGPKG/BIG.DAT" FOR OUTPUT AS #1',
            "FOR I = 1 TO 3000",
            f'PRINT #1, "{chunk}"',
            "NEXT",
            "CLOSE #1",
        ],
    )
    console.send_line('RUN "GEN.BAS"')
    out = console.send_line('PACKAGE "BIG.APP", "BIGPKG/"')
    assert "?PACKAGE" not in out.upper()
    run = console.send_line('RUN "BIG.APP"')
    assert "?PACKAGE" not in run.upper()
    assert "OK" in run


def test_package_folder_listing_over_2k(fresh_console):
    """#706: a folder whose name listing exceeds the old 2 KB buffer packs.

    Before the fix pack_walk() read the folder into a fixed char[2048] and
    refused to pack once the newline listing overflowed it. The structured,
    heap-allocated walk keeps every entry, so a large folder now packages.

    60 names of ~41 chars is a >2.4 KB newline listing, and stays within the
    zip writer's 64-file archive cap. A fresh console avoids filling the
    RAM disk (256 nodes, 138 seeded files) with the module's earlier fixtures.
    """
    prefix = "PADDINGPADDINGPADDINGPADDINGPADDING"  # 35 chars
    console = fresh_console
    assert console.send_line('CHDIR "A:/"') == ""
    assert console.send_line('MKDIR "BIGLST"') == ""
    _write_lines(
        console,
        "BIGLST/MAIN.BAS",
        [
            f'OPEN "{prefix}60.TXT" FOR INPUT AS #1',
            "LINE INPUT #1, A$",
            "PRINT A$",
            "CLOSE #1",
        ],
    )
    _write_lines(
        console,
        "BIGGEN.BAS",
        [
            "FOR I = 1 TO 60",
            f'OPEN "A:/BIGLST/{prefix}"+LTRIM$(STR$(I))+".TXT" FOR OUTPUT AS #1',
            'PRINT #1, "M"+LTRIM$(STR$(I))',
            "CLOSE #1",
            "NEXT I",
            'PRINT "SEEDED"',
        ],
    )
    assert "SEEDED" in console.send_line('RUN "BIGGEN.BAS"')
    out = console.send_line('PACKAGE "BIGLST.APP", "BIGLST/"')
    assert "?PACKAGE" not in out.upper(), out
    run = console.send_line('RUN "BIGLST.APP"')
    assert "?PACKAGE" not in run.upper(), run
    assert "M60" in run, run


def test_package_edit_after_run_is_untitled(kernel_image):
    import re

    from harness import MMBasicConsole

    def _plain(s: str) -> str:
        return re.sub(r"\x1b\[[0-9;?]*[A-Za-z]", "", s)

    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        _make_game(con, "PKED")
        assert con.send_line('PACKAGE "GAME.APP", "PKED/"') == ""
        console_out = con.send_line('RUN "GAME.APP"')
        assert "HELLOPKG" in console_out
        assert con.send_line("PRINT MM.CMDLINE$") == ""
        con.drain(quiet=0.1)
        con._ser.sendall(b"EDIT\r")
        seen = _plain(con.drain(quiet=0.8, timeout=12).decode(errors="replace"))
        upper = seen.upper()
        assert "UNTITLED" in upper
        assert "GAME.APP" not in upper
        con._ser.sendall(bytes([1]) + b"x")
        _plain(con.drain(quiet=0.8, timeout=12).decode(errors="replace"))
        assert con.send_line("PRINT 1+1") == "2"
    finally:
        con.stop()
