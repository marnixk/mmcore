"""OPTION ETHERNET ON/OFF: persist, exclusive with Wi-Fi, IPCONFIG."""

from ihelp_util import dump_topic, open_ihelp, close_ihelp, scroll_all


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


def _section(ini, name):
    key = f"[{name}]"
    lines = ini.splitlines()
    out = []
    grab = False
    for ln in lines:
        s = ln.strip()
        if s.startswith("[") and s.endswith("]"):
            grab = s.lower() == key
            continue
        if grab:
            out.append(s)
    return "\n".join(out)


def test_option_ethernet_on_off_qemu(console):
    assert console.send_line("FACTORY_RESET") == "Factory defaults restored"
    out = console.send_line("OPTION ETHERNET ON")
    assert "?SYNTAX ERROR" not in out.upper()
    assert "ethernet not available" in out.lower()
    listed = console.send_line("OPTION LIST")
    assert "OPTION ETHERNET ON" in listed
    off = console.send_line("OPTION ETHERNET OFF")
    assert "?SYNTAX ERROR" not in off.upper()
    assert "ethernet off" in off.lower()
    listed = console.send_line("OPTION LIST ALL")
    assert "OPTION ETHERNET OFF" in listed
    assert console.send_line("PRINT 2+2") == "4"


def test_option_ethernet_persists(console):
    assert console.send_line("FACTORY_RESET") == "Factory defaults restored"
    assert console.send_line("OPTION ETHERNET ON") != "?SYNTAX ERROR"
    ini = _read_ini(console)
    eth = _section(ini, "ethernet")
    assert "enabled=1" in eth
    assert console.send_line("OPTION ETHERNET OFF") != "?SYNTAX ERROR"
    ini = _read_ini(console)
    eth = _section(ini, "ethernet")
    assert "enabled=0" in eth


def test_option_ethernet_disables_wifi_join(console):
    assert console.send_line("FACTORY_RESET") == "Factory defaults restored"
    assert console.send_line('OPTION WIFI "TestSSID","secretpass"') != "?SYNTAX ERROR"
    ini = _read_ini(console)
    assert "enabled=1" in _section(ini, "wifi")
    assert console.send_line("OPTION ETHERNET ON") != "?SYNTAX ERROR"
    ini = _read_ini(console)
    assert "enabled=1" in _section(ini, "ethernet")
    assert "enabled=0" in _section(ini, "wifi")
    listed = console.send_line("OPTION LIST")
    assert "OPTION ETHERNET ON" in listed
    assert console.send_line("OPTION ETHERNET OFF") != "?SYNTAX ERROR"


def test_ipconfig_ethernet_when_enabled(console):
    assert console.send_line("FACTORY_RESET") == "Factory defaults restored"
    assert console.send_line("OPTION ETHERNET ON") != "?SYNTAX ERROR"
    out = console.send_line("IPCONFIG")
    assert "?SYNTAX ERROR" not in out.upper()
    assert "interface: ethernet" in out.lower()
    assert "not available" in out.lower() or "not connected" in out.lower()
    assert console.send_line("OPTION ETHERNET OFF") != "?SYNTAX ERROR"
    out = console.send_line("IPCONFIG")
    assert "wi-fi" in out.lower()
    assert console.send_line("PRINT 8") == "8"


def test_option_ntp_and_timezone_persist(console):
    assert console.send_line("FACTORY_RESET") == "Factory defaults restored"
    assert console.send_line("OPTION NTP ON") == ""
    out = console.send_line('OPTION TIMEZONE "Europe/Amsterdam"')
    assert "?SYNTAX ERROR" not in out.upper()
    out = console.send_line('OPTION NTP SERVER "time.example.org:1234"')
    assert "?SYNTAX ERROR" not in out.upper()
    ini = _read_ini(console)
    ntp = _section(ini, "ntp")
    assert "enabled=1" in ntp
    assert "server=time.example.org:1234" in ntp
    assert "timezone=Europe/Amsterdam" in ntp
    listed = console.send_line("OPTION LIST")
    assert "OPTION NTP ON" in listed
    assert "OPTION TIMEZONE \"Europe/Amsterdam\"" in listed
    assert "OPTION NTP SERVER \"time.example.org:1234\"" in listed
    assert console.send_line("OPTION NTP OFF") == ""
    ini = _read_ini(console)
    assert "enabled=0" in _section(ini, "ntp")


def test_option_timezone_accepts_utc_offsets(console):
    assert console.send_line("FACTORY_RESET") == "Factory defaults restored"
    assert console.send_line('OPTION TIMEZONE "UTC+2"') == ""
    listed = console.send_line("OPTION LIST")
    assert "OPTION TIMEZONE \"UTC+02\"" in listed
    assert console.send_line('OPTION TIMEZONE "UTC-5:30"') == ""
    listed = console.send_line("OPTION LIST")
    assert "OPTION TIMEZONE \"UTC-05:30\"" in listed


def test_option_timezone_rejects_unknown(console):
    assert console.send_line("FACTORY_RESET") == "Factory defaults restored"
    out = console.send_line('OPTION TIMEZONE "Mars/Olympus"')
    assert "?SYNTAX ERROR" in out.upper()
    assert "OPTION TIMEZONE" not in console.send_line("OPTION LIST")
    assert console.send_line("PRINT 5+5") == "10"


def test_timezone_shifts_clock_display_but_not_epoch(console):
    assert console.send_line("FACTORY_RESET") == "Factory defaults restored"
    assert console.send_line('DATE$="01-01-2026"') == ""
    assert console.send_line('TIME$="12:00:00"') == ""
    utc = int(console.send_line("PRINT EPOCH(NOW)"))
    assert console.send_line('OPTION TIMEZONE "UTC+2"') == ""
    assert console.send_line("PRINT TIME$").startswith("14:00:0")
    assert abs(int(console.send_line("PRINT EPOCH(NOW)")) - utc) < 120


def test_help_ntp(console):
    listing = scroll_all(console, open_ihelp(console, "INDEX"))
    assert "NTP" in listing
    close_ihelp(console)
    out = dump_topic(console, "NTP")
    assert out != "?SYNTAX ERROR"
    assert "pool.ntp.org" in out
    tz = dump_topic(console, "TIMEZONE")
    assert tz != "?SYNTAX ERROR"
    assert "Europe/Amsterdam" in tz
    opt = dump_topic(console, "OPTION")
    assert "NTP AUTO|ON|OFF" in opt
    assert "TIMEZONE" in opt


def test_help_ethernet(console):
    listing = scroll_all(console, open_ihelp(console, "INDEX"))
    assert "<ETHERNET>" in listing
    close_ihelp(console)
    out = dump_topic(console, "ETHERNET")
    assert out != "?SYNTAX ERROR"
    assert "ETHERNET" in out
    assert "DHCP" in out
    assert "IPCONFIG" in out
    assert "reboot" in out.lower()
    opt = dump_topic(console, "OPTION")
    assert "<ETHERNET>" in opt or "ETHERNET ON|OFF" in opt
