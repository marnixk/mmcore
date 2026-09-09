"""TCP as a file stream: INPUT$ order, QEMU OPEN stub, HELP."""

from ihelp_util import dump_topic


def test_input_dollar_count_then_handle(console):
    assert console.send_line("NEW") == ""
    assert console.send_line('OPEN "IN.TXT" FOR OUTPUT AS #1') == ""
    assert console.send_line('PRINT #1, "HELLO"') == ""
    assert console.send_line("CLOSE #1") == ""
    assert console.send_line('OPEN "IN.TXT" FOR INPUT AS #1') == ""
    assert console.send_line("PRINT INPUT$(2, #1)") == "HE"
    assert console.send_line("PRINT INPUT$(3, #1)") == "LLO"
    assert console.send_line("CLOSE #1") == ""
    assert console.send_line("PRINT 1") == "1"


def test_disk_loc_eof_unchanged(console):
    assert console.send_line('OPEN "EOF2.TXT" FOR OUTPUT AS #1') == ""
    assert console.send_line('PRINT #1, "hi";') == ""
    assert console.send_line("CLOSE #1") == ""
    assert console.send_line('OPEN "EOF2.TXT" FOR INPUT AS #1') == ""
    assert console.send_line("PRINT EOF(#1)") == "0"
    assert console.send_line("PRINT INPUT$(2, #1)") == "hi"
    assert console.send_line("PRINT EOF(#1)") == "1"
    assert console.send_line("CLOSE #1") == ""


def test_open_tcp_fails_without_network(console):
    out = console.send_line('OPEN "TCP:192.168.1.50:80" AS #1')
    assert "?SYNTAX ERROR" not in out.upper()
    low = out.lower()
    assert (
        "network not available" in low
        or "connect failed" in low
        or "dns failed" in low
        or "tcp timeout" in low
        or "tcp refused" in low
    )
    assert console.send_line("PRINT EOF(#1)") == "1"
    loc = console.send_line("PRINT LOC(#1)")
    assert "?FILE" in loc.upper()
    lof = console.send_line("PRINT LOF(#1)")
    assert "?FILE" in lof.upper()
    pr = console.send_line('PRINT #1, "GET /"')
    assert "?SYNTAX ERROR" not in pr.upper()
    assert console.send_line("CLOSE #1") == ""
    assert console.send_line("PRINT 6*7") == "42"


def test_open_tcp_hostname_fails_cleanly(console):
    out = console.send_line('OPEN "TCP:example.com:80" AS #1')
    low = out.lower()
    assert "network not available" in low or "connect failed" in low
    assert console.send_line("PRINT 1+1") == "2"


def test_open_tcp_requires_port(console):
    out = console.send_line('OPEN "TCP:example.com" AS #1')
    assert "?SYNTAX ERROR" in out.upper()


def test_open_tcp_rejects_append(console):
    out = console.send_line('OPEN "TCP:127.0.0.1:80" FOR APPEND AS #1')
    assert "?SYNTAX ERROR" in out.upper()


def test_help_open_tcp(console):
    out = dump_topic(console, "OPEN")
    assert out != "?SYNTAX ERROR"
    assert "TCP:" in out
    assert "WEB" not in out
    pr = dump_topic(console, "PRINT")
    assert "TCP" in pr
    cl = dump_topic(console, "CLOSE")
    assert "TCP" in cl
    inp = dump_topic(console, "INPUT")
    assert "TCP" in inp
    li = dump_topic(console, "LINE INPUT")
    assert "TCP" in li


def test_help_input_dollar_and_functions(console):
    dollar = dump_topic(console, "INPUT$")
    assert dollar != "?SYNTAX ERROR"
    assert "nbr" in dollar.lower() or "count" in dollar.lower()
    assert "INPUT$(nbr" in dollar or "INPUT$(nbr," in dollar.replace(" ", "")
    fns = dump_topic(console, "FUNCTIONS")
    assert "INPUT$" in fns
    assert "TCP" in fns
    assert "WEB" not in fns
    assert "count first" in fns.lower() or "RX" in fns or "non-blocking" in fns.lower()
    eof = dump_topic(console, "EOF")
    assert "TCP" in eof or "not connected" in eof.lower() or "RX" in eof
    loc = dump_topic(console, "LOC")
    assert "TCP" in loc or "RX" in loc or "waiting" in loc.lower()
