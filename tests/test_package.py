"""PACKAGE / RUN name.pkg: zip archive mounted read-only as B:."""

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
    assert console.send_line('PACKAGE "GAME.PKG", "PKGAME/"') == ""
    listing = console.send_line('DIR "A:/"')
    assert "GAME.PKG" in listing.upper()
    out = console.send_line('RUN "GAME.PKG"')
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
    assert console.send_line('PACKAGE "RO.PKG", "PKRO/"') == ""
    out = console.send_line('RUN "RO.PKG"')
    assert "?READ ONLY" in out.upper()
    cwd = console.send_line("PRINT CWD$")
    assert cwd.upper().startswith("A:")
    assert console.send_line('CHDIR "B:"').startswith("?DRIVE")
    listed = console.send_line("LIST")
    assert "OPEN" in listed.upper()


def test_package_file_exists_in_program(console):
    _make_game(console, "PKDUP")
    assert console.send_line('PACKAGE "DUP.PKG", "PKDUP/"') == ""
    assert console.send_line("NEW") == ""
    assert console.send_line('10 PACKAGE "DUP.PKG", "PKDUP/"') == ""
    out = console.send_line("RUN")
    assert "FILE EXISTS" in out.upper()


def test_package_overwrite_prompt(console):
    _make_game(console, "PKOW")
    assert console.send_line('PACKAGE "OW.PKG", "PKOW/"') == ""
    con = console
    con.drain(quiet=0.1, timeout=0.4)
    con._ser.sendall(b'PACKAGE "OW.PKG", "PKOW/"\r')
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
    assert "HELLOPKG" in console.send_line('RUN "OW.PKG"')
    con.drain(quiet=0.1, timeout=0.4)
    con._ser.sendall(b'PACKAGE "OW.PKG", "PKOW/"\r')
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
    assert "HELLOPKG" in console.send_line('RUN "OW.PKG"')


def test_package_missing_main(console):
    assert console.send_line('CHDIR "A:/"') == ""
    assert console.send_line('MKDIR "NOMAIN"') == ""
    _write_lines(console, "GAMELESS.TXT", ["x"])
    assert console.send_line('COPY "GAMELESS.TXT" TO "NOMAIN/X.TXT"') == ""
    out = console.send_line('PACKAGE "BAD.PKG", "NOMAIN/"')
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
    assert console.send_line('PACKAGE "DOT.PKG", "PKDOT/"') == ""
    out = console.send_line('RUN "DOT.PKG"')
    assert "B:" in out.upper()
    assert console.send_line("PRINT CWD$").upper().startswith("A:")


def test_package_run_bas_unmounts(console):
    _write_lines(console, "OTHER.BAS", ["PRINT 99"])
    _make_game(console, "PKNEST", ['RUN "A:/OTHER.BAS"'])
    assert console.send_line('PACKAGE "NEST.PKG", "PKNEST/"') == ""
    out = console.send_line('RUN "NEST.PKG"')
    assert "99" in out
    assert console.send_line("PRINT CWD$").upper().startswith("A:")


def test_help_package(console):
    out = dump_topic(console, "PACKAGE")
    assert "PACKAGE" in out.upper()
    assert "MAIN.BAS" in out.upper()
    assert "RUN" in out.upper()
