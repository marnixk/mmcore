"""#581: NTP is on by default (AUTO) once the network is available.

The default QEMU console has no NIC, so these tests exercise the policy and
the quiet-offline path. The wire format itself is covered by the loopback test
in test_linux_native.py.
"""


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
    out = []
    grab = False
    for ln in ini.splitlines():
        s = ln.strip()
        if s.startswith("[") and s.endswith("]"):
            grab = s.lower() == key
            continue
        if grab:
            out.append(s)
    return "\n".join(out)


def test_ntp_defaults_to_auto(console):
    """Fresh defaults do not show an NTP line, but LIST ALL reveals AUTO."""
    assert console.send_line("FACTORY_RESET") == "Factory defaults restored"
    listed = console.send_line("OPTION LIST")
    assert "OPTION NTP" not in listed
    all_listed = console.send_line("OPTION LIST ALL")
    assert "OPTION NTP AUTO" in all_listed


def test_ntp_explicit_off_persists_and_lists(console):
    assert console.send_line("FACTORY_RESET") == "Factory defaults restored"
    assert console.send_line("OPTION NTP OFF") == ""
    listed = console.send_line("OPTION LIST")
    assert "OPTION NTP OFF" in listed
    ini = _read_ini(console)
    assert "enabled=0" in _section(ini, "ntp")


def test_ntp_on_persists_and_lists(console):
    assert console.send_line("FACTORY_RESET") == "Factory defaults restored"
    assert console.send_line("OPTION NTP ON") == ""
    listed = console.send_line("OPTION LIST")
    assert "OPTION NTP ON" in listed
    ini = _read_ini(console)
    assert "enabled=1" in _section(ini, "ntp")


def test_ntp_auto_restores_default(console):
    assert console.send_line("FACTORY_RESET") == "Factory defaults restored"
    assert console.send_line("OPTION NTP OFF") == ""
    assert console.send_line("OPTION NTP AUTO") == ""
    ini = _read_ini(console)
    assert "enabled=-1" in _section(ini, "ntp")
    listed = console.send_line("OPTION LIST")
    assert "OPTION NTP" not in listed
    all_listed = console.send_line("OPTION LIST ALL")
    assert "OPTION NTP AUTO" in all_listed


def test_ntp_stays_quiet_offline(console):
    """AUTO with no NIC must not print a failure or stall the prompt."""
    assert console.send_line("FACTORY_RESET") == "Factory defaults restored"
    late = console.drain(quiet=0.3, timeout=3.0).decode(errors="replace")
    assert "NTP" not in late.upper()
    assert console.send_line("PRINT 6*7") == "42"


def test_ntp_command_errors_offline(console):
    assert console.send_line("FACTORY_RESET") == "Factory defaults restored"
    out = console.send_line("NTP")
    assert "?SYNTAX ERROR" not in out.upper()
    low = out.lower()
    assert "ntp failed" in low
    assert "network not available" in low or "no reply" in low or "not reachable" in low
    assert console.send_line("PRINT 3+4") == "7"
