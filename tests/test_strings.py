"""QuickBasic/PicoMite string gaps: LSET/RSET, LTRIM$/RTRIM$/TRIM$, BASE$,
FIELD$, DATETIME$, DAY$, DIR$."""

from ihelp_util import dump_topic


def test_lset_rset_fixed_length(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("DIM S AS STRING LENGTH 8") == ""
    assert console.send_line('LSET S = "AB"') == ""
    assert console.send_line('PRINT "["; S; "]"') == "[AB      ]"
    assert console.send_line('RSET S = "XY"') == ""
    assert console.send_line('PRINT "["; S; "]"') == "[      XY]"
    assert console.send_line('LSET S = "123456789"') == ""
    assert console.send_line('PRINT "["; S; "]"') == "[12345678]"


def test_lset_rset_dynamic_keeps_width(console):
    assert console.send_line("NEW") == ""
    assert console.send_line('A$ = "HELLO"') == ""
    assert console.send_line('LSET A$ = "AB"') == ""
    assert console.send_line('PRINT "["; A$; "]"') == "[AB   ]"
    assert console.send_line('RSET A$ = "XY"') == ""
    assert console.send_line('PRINT "["; A$; "]"') == "[   XY]"


def test_trim_functions(console):
    assert console.send_line("NEW") == ""
    assert console.send_line('PRINT "["; TRIM$("  hi  "); "]"') == "[hi]"
    assert console.send_line('PRINT "["; LTRIM$("  hi  "); "]"') == "[hi  ]"
    assert console.send_line('PRINT "["; RTRIM$("  hi  "); "]"') == "[  hi]"
    assert console.send_line('PRINT TRIM$("xxhixx", "x")') == "hi"
    assert console.send_line('PRINT TRIM$("xxhixx", "x", "L")') == "hixx"
    assert console.send_line('PRINT TRIM$("xxhixx", "x", "R")') == "xxhi"


def test_base_function(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("PRINT BASE$(16, 255)") == "FF"
    assert console.send_line("PRINT BASE$(2, 5)") == "101"
    assert console.send_line("PRINT BASE$(16, 255, 4)") == "00FF"
    assert console.send_line("PRINT BASE$(36, 35)") == "Z"
    assert console.send_line("PRINT BASE$(8, 0)") == "0"
    err = console.send_line("PRINT BASE$(1, 5)")
    assert "INVALID BASE" in err.upper()


def test_field_function(console):
    assert console.send_line("NEW") == ""
    assert console.send_line('PRINT FIELD$("aaa,bbb,ccc", 2)') == "bbb"
    assert console.send_line('PRINT FIELD$("a,b,c", 3, ",")') == "c"
    assert console.send_line('PRINT FIELD$("a,b", 3)') == ""
    assert (
        console.send_line('PRINT FIELD$("text1, \'quoted, text\', text3", 2, ",", "\'")')
        == "'quoted, text'"
    )


def test_datetime_function(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("PRINT DATETIME$(0)") == "01-01-1970 00:00:00"
    assert console.send_line("PRINT DATETIME$(90061)") == "02-01-1970 01:01:01"


def test_day_function(console):
    assert console.send_line("NEW") == ""
    assert console.send_line('PRINT DAY$("01-01-1970")') == "Thursday"
    assert console.send_line('PRINT DAY$("29-02-2024")') == "Thursday"
    assert console.send_line("PRINT DAY$(0)") == "Thursday"
    now = console.send_line("PRINT DAY$(NOW)")
    assert now in (
        "Monday",
        "Tuesday",
        "Wednesday",
        "Thursday",
        "Friday",
        "Saturday",
        "Sunday",
    )


def test_dir_function(console):
    assert console.send_line("NEW") == ""
    assert console.send_line('D$ = DIR$("tests/*.*")') == ""
    found = []
    for _ in range(64):
        name = console.send_line("PRINT D$")
        if name == "":
            break
        found.append(name)
        assert console.send_line('D$ = DIR$("")') == ""
    assert "TEST.WAV" in found
    assert console.send_line('D$ = DIR$("NOEXIST/*.*")') == ""
    assert console.send_line("PRINT D$") == ""


def test_help_string_gaps(console):
    out = dump_topic(console, "LSET")
    assert "RSET" in out
    assert "LSET" in out
    fn = dump_topic(console, "FUNCTIONS")
    for name in ("TRIM$", "BASE$", "FIELD$", "DATETIME$", "DAY$", "DIR$"):
        assert name in fn
