"""CONNECT telnet-style client: syntax and a clean QEMU failure."""

from ihelp_util import dump_topic, open_ihelp, close_ihelp, scroll_all


def test_connect_requires_host_and_port(console):
    assert "?SYNTAX ERROR" in console.send_line("CONNECT").upper()
    assert "?SYNTAX ERROR" in console.send_line('CONNECT "example.com"').upper()
    assert "?SYNTAX ERROR" in console.send_line("CONNECT 23").upper()
    assert "?SYNTAX ERROR" in console.send_line('CONNECT "h", 0').upper()
    assert "?SYNTAX ERROR" in console.send_line('CONNECT "h", 70000').upper()


def test_connect_fails_cleanly_without_network(console):
    out = console.send_line('CONNECT "example.com", 23')
    assert "?SYNTAX ERROR" not in out.upper()
    low = out.lower()
    assert (
        "network not available" in low
        or "connect failed" in low
        or "dns failed" in low
        or "tcp timeout" in low
        or "tcp refused" in low
    )
    assert console.send_line("PRINT 6*7") == "42"


def test_connect_numeric_host_still_fails_cleanly(console):
    out = console.send_line('CONNECT "127.0.0.1", 1')
    assert "?SYNTAX ERROR" not in out.upper()
    low = out.lower()
    assert (
        "network not available" in low
        or "connect failed" in low
        or "dns failed" in low
        or "tcp timeout" in low
        or "tcp refused" in low
    )
    assert console.send_line("PRINT 1+1") == "2"


def test_help_connect(console):
    listing = scroll_all(console, open_ihelp(console))
    assert "CONNECT" in listing
    close_ihelp(console)
    out = dump_topic(console, "CONNECT")
    assert out != "?SYNTAX ERROR"
    assert "host" in out.lower()
    assert "Ctrl+]" in out or "quit" in out.lower()
    assert "f10" in out.lower()
    assert "alt" in out.lower()
    assert "esc" in out.lower()
    assert "backspace" in out.lower() or "DEL" in out or "del" in out.lower()
    assert "telnet" in out.lower() or "ANSI" in out or "ansi" in out.lower()
    assert "CR" in out
    assert "character mode" in out.lower() or "SGA" in out
    assert "drain" in out.lower() or "truncated" in out.lower()


def test_ipconfig_without_radio(console):
    out = console.send_line("IPCONFIG")
    assert "?SYNTAX ERROR" not in out.upper()
    assert "not available" in out.lower() or "not connected" in out.lower()
    assert console.send_line("PRINT 3+4") == "7"


def test_help_ipconfig(console):
    listing = scroll_all(console, open_ihelp(console))
    assert "IPCONFIG" in listing
    close_ihelp(console)
    out = dump_topic(console, "IPCONFIG")
    assert out != "?SYNTAX ERROR"
    assert "Connected as" in out
    assert "SSID" in out
    assert "gateway" in out.lower() or "DHCP" in out
    assert "probe" in out.lower() or "unreachable" in out.lower()
