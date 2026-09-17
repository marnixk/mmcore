"""PicoMite system/options gaps: BIT, BYTE, EPOCH, EXECUTE."""

from ihelp_util import dump_topic


def test_bit_read_write(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("DIM A AS INTEGER") == ""
    assert console.send_line("A = 0") == ""
    assert console.send_line("BIT(A, 3) = 1") == ""
    assert console.send_line("PRINT A") == "8"
    assert console.send_line("PRINT BIT(A, 3); BIT(A, 2)") == "10"
    assert console.send_line("BIT(A, 3) = 0") == ""
    assert console.send_line("PRINT A") == "0"
    assert console.send_line("PRINT BIT(5, 0); BIT(5, 1); BIT(5, 2)") == "101"
    assert console.send_line("BIT(A, 63) = 1") == ""
    assert console.send_line("PRINT BIT(A, 63)") == "1"


def test_bit_type_and_range(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("A = 1") == ""
    err = console.send_line("BIT(A, 2) = 1")
    assert "TYPE MISMATCH" in err.upper()
    assert console.send_line("DIM B AS INTEGER") == ""
    err = console.send_line("BIT(B, 64) = 1")
    assert "BIT" in err.upper()


def test_byte_read_write(console):
    assert console.send_line("NEW") == ""
    assert console.send_line('S$ = "ABC"') == ""
    assert console.send_line("PRINT BYTE(S$, 1); BYTE(S$, 3)") == "6567"
    assert console.send_line("BYTE(S$, 2) = 90") == ""
    assert console.send_line("PRINT S$") == "AZC"
    assert console.send_line("BYTE(S$, 5) = 33") == ""
    assert console.send_line('PRINT "["; S$; "]"') == "[AZC !]"
    assert console.send_line("PRINT BYTE(S$, 9)") == "0"


def test_epoch_function(console):
    assert console.send_line("NEW") == ""
    assert console.send_line('PRINT EPOCH("01-01-1970")') == "0"
    assert console.send_line('PRINT EPOCH("01-01-1970", "01:00:00")') == "3600"
    assert console.send_line('PRINT EPOCH("02-01-1970")') == "86400"
    assert console.send_line("PRINT EPOCH(12345)") == "12345"
    assert (
        console.send_line('PRINT DATETIME$(EPOCH("15-06-2024", "12:30:45"))')
        == "15-06-2024 12:30:45"
    )
    assert console.send_line("PRINT SGN(EPOCH(NOW) - 1600000000)") == "1"


def test_execute_string(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("X = 0") == ""
    assert console.send_line('EXECUTE "X = X + 5"') == ""
    assert console.send_line("PRINT X") == "5"
    assert console.send_line('EXECUTE "PRINT 42"') == "42"
    assert console.send_line('EXECUTE "A$ = CHR$(104) + CHR$(105)"') == ""
    assert console.send_line("PRINT A$") == "hi"


def test_execute_trailing_statement(console):
    assert console.send_line("NEW") == ""
    assert console.send_line('EXECUTE "X = 21" : PRINT X') == "21"


def test_execute_errors(console):
    assert console.send_line("NEW") == ""
    err = console.send_line("EXECUTE 5")
    assert "TYPE MISMATCH" in err.upper()
    err = console.send_line('EXECUTE "1 +"')
    assert "SYNTAX" in err.upper()


def test_help_system_gaps(console):
    for topic in ("BIT", "BYTE", "EPOCH", "EXECUTE"):
        out = dump_topic(console, topic)
        assert out != "?SYNTAX ERROR"
        assert topic in out
