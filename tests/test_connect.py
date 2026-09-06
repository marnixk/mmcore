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
    assert "network not available" in out.lower() or "connect failed" in out.lower()
    assert console.send_line("PRINT 6*7") == "42"


def test_connect_numeric_host_still_fails_cleanly(console):
    out = console.send_line('CONNECT "127.0.0.1", 1')
    assert "?SYNTAX ERROR" not in out.upper()
    assert "network not available" in out.lower() or "connect failed" in out.lower()
    assert console.send_line("PRINT 1+1") == "2"


def test_help_connect(console):
    listing = scroll_all(console, open_ihelp(console))
    assert "CONNECT" in listing
    close_ihelp(console)
    out = dump_topic(console, "CONNECT")
    assert out != "?SYNTAX ERROR"
    assert "host" in out.lower()
    assert "Ctrl+]" in out or "quit" in out.lower()
    assert "telnet" in out.lower() or "ANSI" in out or "ansi" in out.lower()
