"""Persistent INI settings, hidden dotfiles, FACTORY_RESET, and OPTION WIFI."""


def _ini_path(con):
    for path in ("A:/.mmbasic.ini", "C:/.mmbasic.ini"):
        out = con.send_line(f'OPEN "{path}" FOR INPUT AS #1')
        con.send_line("CLOSE #1")
        if out == "":
            return path
    raise AssertionError("settings INI not found on A: or C:")


def _read_ini(con, path=None):
    if not path:
        path = _ini_path(con)
    assert con.send_line("NEW") == ""
    assert con.send_line(f'10 OPEN "{path}" FOR INPUT AS #1') == ""
    assert con.send_line("20 IF EOF(#1) THEN GOTO 70") == ""
    assert con.send_line("30 LINE INPUT #1, A$") == ""
    assert con.send_line("40 PRINT A$") == ""
    assert con.send_line("50 GOTO 20") == ""
    assert con.send_line("70 CLOSE #1") == ""
    return con.send_line("RUN", timeout=8)


def test_eof_hash_file_number(console):
    assert console.send_line('OPEN "A:/EOFTEST.TXT" FOR OUTPUT AS #1') == ""
    assert console.send_line('PRINT #1, "hi"') == ""
    assert console.send_line("CLOSE #1") == ""
    assert console.send_line('OPEN "A:/EOFTEST.TXT" FOR INPUT AS #1') == ""
    assert console.send_line("PRINT EOF(#1)") == "0"
    assert console.send_line("LINE INPUT #1, A$") == ""
    assert console.send_line("PRINT A$") == "hi"
    assert console.send_line("PRINT EOF(#1)") == "1"
    assert console.send_line("CLOSE #1") == ""


def test_option_persists_to_hidden_ini(console):
    assert console.send_line("FACTORY_RESET") == "Factory defaults restored"
    assert console.send_line("OPTION TAB 4") == ""
    assert console.send_line("OPTION BREAK 4") == ""
    ini = _read_ini(console)
    assert "tab=4" in ini
    assert "break=4" in ini
    assert "[core]" in ini


def test_dir_hides_dotfiles(console):
    assert console.send_line('CHDIR "A:/"') == ""
    assert console.send_line('OPEN "A:/.hidden.txt" FOR OUTPUT AS #1') == ""
    assert console.send_line('PRINT #1, "secret"') == ""
    assert console.send_line("CLOSE #1") == ""
    assert console.send_line('OPEN "A:VISIBLE.TXT" FOR OUTPUT AS #1') == ""
    assert console.send_line('PRINT #1, "ok"') == ""
    assert console.send_line("CLOSE #1") == ""
    listing = console.send_line('DIR "A:/"')
    assert "VISIBLE.TXT" in listing.upper()
    assert ".HIDDEN" not in listing.upper()
    assert ".MMBASIC.INI" not in listing.upper()
    # OPEN by exact path still works
    assert console.send_line('OPEN "A:/.hidden.txt" FOR INPUT AS #1') == ""
    assert console.send_line("LINE INPUT #1, A$") == ""
    assert console.send_line("PRINT A$") == "secret"
    assert console.send_line("CLOSE #1") == ""


def test_factory_reset_restores_options_and_ini(console):
    assert console.send_line("OPTION TAB 4") == ""
    assert console.send_line('OPTION WIFI "KeepMe","oldpass"') != "?SYNTAX ERROR"
    listing = console.send_line('DIR "A:/"')
    assert ".MMBASIC.INI" not in listing.upper()
    assert console.send_line('OPEN "A:/KEEP.BAS" FOR OUTPUT AS #1') == ""
    assert console.send_line('PRINT #1, "10 PRINT 1"') == ""
    assert console.send_line("CLOSE #1") == ""
    out = console.send_line("FACTORY_RESET")
    assert "Factory defaults restored" in out
    listed = console.send_line("OPTION LIST")
    assert "TAB 4" not in listed
    ini = _read_ini(console)
    assert "tab=2" in ini
    assert "ssid=" in ini
    assert "KeepMe" not in ini
    assert "oldpass" not in ini
    assert "psk=" in ini
    listing = console.send_line('DIR "A:/"')
    assert "KEEP.BAS" in listing.upper()


def test_option_wifi_persists_credentials(console):
    out = console.send_line('OPTION WIFI "TestSSID","secretpass"')
    assert "?SYNTAX ERROR" not in out
    assert "secretpass" not in out
    ini = _read_ini(console)
    assert "[wifi]" in ini
    assert "ssid=TestSSID" in ini
    assert "psk=secretpass" in ini
    assert "enabled=1" in ini


def test_option_wifi_interactive_no_crash(console):
    out = console.send_line("OPTION WIFI")
    assert "?SYNTAX ERROR" not in out
    assert "Wi-Fi not available" in out or "not available" in out.lower()


def test_options_wifi_alias(console):
    out = console.send_line('OPTIONS WIFI "AliasNet","aliaspass"')
    assert "?SYNTAX ERROR" not in out
    ini = _read_ini(console)
    assert "ssid=AliasNet" in ini
    assert "psk=aliaspass" in ini


def test_option_wifi_debug_default_off(console):
    assert console.send_line("FACTORY_RESET") == "Factory defaults restored"
    listed = console.send_line("OPTION LIST")
    assert "WIFI DEBUG" not in listed
    all_listed = console.send_line("OPTION LIST ALL")
    assert "OPTION WIFI DEBUG OFF" in all_listed
    ini = _read_ini(console)
    assert "debug=0" in ini


def test_option_wifi_debug_on_persists(console):
    assert console.send_line("OPTION WIFI DEBUG ON") == ""
    listed = console.send_line("OPTION LIST")
    assert "OPTION WIFI DEBUG ON" in listed
    ini = _read_ini(console)
    assert "debug=1" in ini
    assert console.send_line("OPTION WIFI DEBUG OFF") == ""
    listed = console.send_line("OPTION LIST")
    assert "WIFI DEBUG" not in listed
    ini = _read_ini(console)
    assert "debug=0" in ini


def test_files_hides_dotfiles(fresh_console):
    con = fresh_console
    assert con.send_line('CHDIR "A:/"') == ""
    assert con.send_line('OPEN "A:/.secret.txt" FOR OUTPUT AS #1') == ""
    assert con.send_line('PRINT #1, "secret"') == ""
    assert con.send_line("CLOSE #1") == ""
    assert con.send_line('OPEN "A:/VISIBLE.TXT" FOR OUTPUT AS #1') == ""
    assert con.send_line('PRINT #1, "ok"') == ""
    assert con.send_line("CLOSE #1") == ""
    assert con.send_line("OPTION TAB 4") == ""
    listing = con.send_line('DIR "A:/"')
    assert "VISIBLE.TXT" in listing.upper()
    assert ".SECRET" not in listing.upper()
    assert con._ser is not None
    con.drain(quiet=0.15)
    con._ser.sendall(b"FILES\r")
    seen = con.drain(quiet=0.8).decode(errors="replace").upper()
    assert "VISIBLE.TXT" in seen
    assert ".SECRET" not in seen
    assert ".MMBASIC" not in seen
    con._ser.sendall(b"q")
    con.drain(quiet=0.3)
