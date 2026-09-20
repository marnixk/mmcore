"""UNPACK: extract .app and .zip archives relative to the current folder."""

from ihelp_util import dump_topic


def _write_lines(con, path, lines):
    assert con.send_line(f'OPEN "{path}" FOR OUTPUT AS #1') == ""
    for line in lines:
        escaped = line.replace('"', '""')
        assert con.send_line(f'PRINT #1, "{escaped}"') == ""
    assert con.send_line("CLOSE #1") == ""


def _read_line(con, path):
    assert con.send_line(f'OPEN "{path}" FOR INPUT AS #1') == ""
    assert con.send_line("LINE INPUT #1, A$") == ""
    out = con.send_line("PRINT A$")
    con.send_line("CLOSE #1")
    return out


def _make_app(con, folder, files):
    assert con.send_line('CHDIR "A:/"') == ""
    assert con.send_line(f'MKDIR "{folder}"') == ""
    for name, lines in files.items():
        if "/" in name:
            sub = name.rsplit("/", 1)[0]
            assert con.send_line(f'MKDIR "{folder}/{sub}"') == ""
        _write_lines(con, f"{folder}/{name}", lines)
    assert con.send_line(f'PACKAGE "{folder}.APP", "{folder}/"') == ""


def test_unpack_app_relative_to_cwd(console):
    _make_app(
        console,
        "UPKSRC",
        {
            "MAIN.BAS": ["PRINT 1"],
            "DATA.TXT": ["PACKED DATA"],
            "GFX/PIC.TXT": ["PACKED PIC"],
        },
    )
    assert console.send_line('CHDIR "A:/"') == ""
    assert console.send_line('MKDIR "UPDST"') == ""
    assert console.send_line('CHDIR "UPDST"') == ""
    out = console.send_line('UNPACK "A:/UPKSRC.APP"')
    assert "?" not in out, out
    listing = console.send_line('DIR "A:/UPDST"')
    assert "MAIN.BAS" in listing.upper(), listing
    assert "DATA.TXT" in listing.upper(), listing
    assert "GFX" in listing.upper(), listing
    assert _read_line(console, "A:/UPDST/GFX/PIC.TXT") == "PACKED PIC"
    assert console.send_line("PRINT CWD$").upper().startswith("A:/UPDST")


def test_unpack_deflate_zip(console):
    assert console.send_line('CHDIR "A:/"') == ""
    assert console.send_line('MKDIR "UPDEF"') == ""
    assert console.send_line('CHDIR "UPDEF"') == ""
    out = console.send_line('UNPACK "A:/tests/DEFLATE.ZIP"')
    assert "?" not in out, out
    assert _read_line(console, "A:/UPDEF/HELLO.TXT") == "UNPACK DEFLATE OK"
    assert _read_line(console, "A:/UPDEF/SUB/NESTED.TXT") == "NESTED DEFLATE OK"
    assert console.send_line("PRINT CWD$").upper().startswith("A:/UPDEF")


def test_unpack_flat_app_lands_in_cwd(console):
    _make_app(console, "UPFLAT", {"MAIN.BAS": ["PRINT 1"], "FLAT.TXT": ["FLAT FILE"]})
    assert console.send_line('CHDIR "A:/"') == ""
    assert console.send_line('MKDIR "UPHERE"') == ""
    assert console.send_line('CHDIR "UPHERE"') == ""
    assert console.send_line('UNPACK "A:/UPFLAT.APP"') == ""
    assert _read_line(console, "FLAT.TXT") == "FLAT FILE"
    assert _read_line(console, "MAIN.BAS") == "PRINT 1"


def test_unpack_missing_file(console):
    out = console.send_line('UNPACK "A:/NOPE.ZIP"')
    assert "FILE NOT FOUND" in out.upper(), out


def test_unpack_bad_archive(console):
    assert console.send_line('CHDIR "A:/"') == ""
    _write_lines(console, "NOTAZIP.TXT", ["this is not an archive"])
    out = console.send_line('UNPACK "A:/NOTAZIP.TXT"')
    assert "?UNPACK" in out.upper(), out


def test_unpack_file_exists_in_program(console):
    _make_app(console, "UPEX", {"MAIN.BAS": ["PRINT 1"]})
    assert console.send_line('CHDIR "A:/"') == ""
    assert console.send_line('MKDIR "UPEXDEST"') == ""
    _write_lines(console, "UPEXDEST/MAIN.BAS", ["PRINT 2"])
    assert console.send_line('CHDIR "UPEXDEST"') == ""
    assert console.send_line("NEW") == ""
    assert console.send_line('10 UNPACK "A:/UPEX.APP"') == ""
    out = console.send_line("RUN")
    assert "FILE EXISTS" in out.upper(), out


def test_help_unpack(console):
    out = dump_topic(console, "UNPACK")
    assert "UNPACK" in out.upper()
    assert ".ZIP" in out.upper()
    assert ".APP" in out.upper()
    assert "PACKAGE" in out.upper()
