"""TYPE / STRUCT commands (PicoMite user-defined types)."""

from ihelp_util import dump_topic


def _prog(console, lines):
    assert console.send_line("NEW") == ""
    n = 10
    for line in lines:
        assert console.send_line(f"{n} {line}") == ""
        n += 10


def test_type_dim_dot_assign(console):
    _prog(
        console,
        [
            "TYPE Point",
            "x AS INTEGER",
            "y AS INTEGER",
            "END TYPE",
            "DIM p AS Point",
            "p.x = 10",
            "p.y = 20",
            "PRINT p.x + p.y",
        ],
    )
    assert console.send_line("RUN") == "30"


def test_type_initialiser_and_array(console):
    _prog(
        console,
        [
            "TYPE Point",
            "x AS INTEGER",
            "y AS INTEGER",
            "END TYPE",
            "DIM p AS Point = (3, 4)",
            "DIM pos(2) AS Point",
            "pos(0) = p",
            "pos(1).x = 5",
            "pos(1).y = 7",
            "PRINT p.x; pos(0).y; pos(1).x + pos(1).y",
        ],
    )
    out = console.send_line("RUN")
    assert "3" in out
    assert "4" in out
    assert "12" in out


def test_type_string_and_nested(console):
    _prog(
        console,
        [
            "TYPE Point",
            "x AS INTEGER",
            "y AS INTEGER",
            "END TYPE",
            "TYPE Rec",
            "n AS STRING LENGTH 8",
            "pt AS Point",
            "END TYPE",
            "DIM r AS Rec",
            'r.n = "hi"',
            "r.pt.x = 9",
            "PRINT r.n; r.pt.x",
        ],
    )
    out = console.send_line("RUN")
    assert "hi" in out
    assert "9" in out


def test_struct_copy_clear_swap_print(console):
    _prog(
        console,
        [
            "TYPE Point",
            "x AS INTEGER",
            "y AS INTEGER",
            "END TYPE",
            "DIM a AS Point",
            "DIM b AS Point",
            "a.x = 11",
            "a.y = 22",
            "STRUCT COPY a TO b",
            "PRINT b.x; b.y",
            "STRUCT SWAP a, b",
            "STRUCT CLEAR a",
            "PRINT a.x; b.x",
            "STRUCT PRINT b",
        ],
    )
    out = console.send_line("RUN")
    assert "11" in out
    assert "22" in out
    assert "0" in out
    assert "X=" in out.upper() or "x=" in out.lower()


def test_struct_sort_extract_sizeof(console):
    _prog(
        console,
        [
            "TYPE Point",
            "x AS INTEGER",
            "y AS INTEGER",
            "END TYPE",
            "DIM pos(2) AS Point",
            "DIM xs(2) AS INTEGER",
            "pos(0).x = 9",
            "pos(1).x = 1",
            "pos(2).x = 4",
            "STRUCT SORT pos().x",
            "STRUCT EXTRACT pos().x, xs()",
            "PRINT xs(0)+100; xs(1)+100; xs(2)+100; STRUCT(SIZEOF \"Point\")",
        ],
    )
    out = console.send_line("RUN")
    assert "1" in out
    assert "4" in out
    assert "9" in out
    assert "16" in out


def test_struct_save_load(console):
    _prog(
        console,
        [
            "TYPE Point",
            "x AS INTEGER",
            "y AS INTEGER",
            "END TYPE",
            "DIM a AS Point",
            "DIM b AS Point",
            "a.x = 6",
            "a.y = 7",
            'OPEN "P.DAT" FOR OUTPUT AS #1',
            "STRUCT SAVE #1, a",
            "CLOSE #1",
            'OPEN "P.DAT" FOR INPUT AS #1',
            "STRUCT LOAD #1, b",
            "CLOSE #1",
            "PRINT b.x; b.y",
        ],
    )
    out = console.send_line("RUN")
    assert "6" in out
    assert "7" in out


def test_function_as_type(console):
    _prog(
        console,
        [
            "TYPE Point",
            "x AS INTEGER",
            "y AS INTEGER",
            "END TYPE",
            "FUNCTION Origin() AS Point",
            "Origin.x = 3",
            "Origin.y = 4",
            "END FUNCTION",
            "DIM q AS Point",
            "q = Origin()",
            "PRINT q.x; q.y",
        ],
    )
    out = console.send_line("RUN")
    assert "3" in out
    assert "4" in out


def test_struct_copy_comma(console):
    _prog(
        console,
        [
            "TYPE Point",
            "x AS INTEGER",
            "y AS INTEGER",
            "END TYPE",
            "DIM a AS Point",
            "DIM b AS Point",
            "a.x = 8",
            "a.y = 9",
            "STRUCT COPY a, b",
            "PRINT b.x; b.y",
        ],
    )
    out = console.send_line("RUN")
    assert "8" in out
    assert "9" in out


def test_sub_as_type(console):
    _prog(
        console,
        [
            "TYPE Point",
            "x AS INTEGER",
            "y AS INTEGER",
            "END TYPE",
            "SUB PrintPoint(pt AS Point)",
            'PRINT "[" + STR$(pt.x) + "," + STR$(pt.y) + "]"',
            "END SUB",
            "DIM p AS Point = (3, 4)",
            "PrintPoint(p)",
        ],
    )
    out = console.send_line("RUN")
    assert "3" in out
    assert "4" in out


def test_function_as_type_copy_and_assign(console):
    _prog(
        console,
        [
            "TYPE Point",
            "x AS INTEGER",
            "y AS INTEGER",
            "END TYPE",
            "FUNCTION Make() AS Point",
            "LOCAL p AS Point",
            "p.x = 5",
            "p.y = 6",
            "Make = p",
            "END FUNCTION",
            "FUNCTION CopyOf() AS Point",
            "LOCAL p AS Point",
            "p.x = 7",
            "p.y = 8",
            "STRUCT COPY p, CopyOf",
            "END FUNCTION",
            "DIM a AS Point",
            "DIM b AS Point",
            "a = Make()",
            "b = CopyOf()",
            "PRINT a.x; a.y; b.x; b.y",
        ],
    )
    out = console.send_line("RUN")
    assert "5" in out
    assert "6" in out
    assert "7" in out
    assert "8" in out


def test_message_json_function_and_sub(console):
    _prog(
        console,
        [
            "TYPE Message",
            "nick AS STRING",
            "message AS STRING",
            "END TYPE",
            "FUNCTION waitForServerResponse() AS Message",
            "LOCAL response$",
            "LOCAL msg AS Message",
            "DO",
            "LINE INPUT #1, response$",
            "LOOP UNTIL LEN(response$)",
            "JSON_PARSE response$, msg",
            "STRUCT COPY msg, waitForServerResponse",
            "END FUNCTION",
            "SUB PrintMessage(msg AS Message)",
            "COLOR WHITE",
            'PRINT "[" + msg.nick + "] ";',
            "COLOR GREY",
            "PRINT msg.message",
            "END SUB",
            "DIM serverMessage AS Message",
            'OPEN "MSG.TXT" FOR OUTPUT AS #1',
            'PRINT #1, "{""nick"":""alice"",""message"":""hello""}"',
            "CLOSE #1",
            'OPEN "MSG.TXT" FOR INPUT AS #1',
            "serverMessage = waitForServerResponse()",
            "CLOSE #1",
            "PrintMessage(serverMessage)",
        ],
    )
    out = console.send_line("RUN")
    assert "[alice]" in out.replace(" ", "") or "[alice]" in out
    assert "hello" in out


def test_cmm2_dotted_name_without_type(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("DIM INTEGER glove.pos.x(3) = (130, 210, 150, 170)") == ""
    assert console.send_line("PRINT glove.pos.x(0)") == "130"


def test_nested_type_array_member_subscript(console):
    _prog(
        console,
        [
            "TYPE Row",
            "vals(3) AS INTEGER",
            "END TYPE",
            "TYPE Game",
            "rows(2) AS Row",
            "END TYPE",
            "DIM state AS Game",
            "state.rows(1).vals(2) = 42",
            "PRINT state.rows(1).vals(2)",
        ],
    )
    assert console.send_line("RUN") == "42"


def test_nested_type_struct_array_member_field(console):
    _prog(
        console,
        [
            "TYPE Row",
            "n AS INTEGER",
            "END TYPE",
            "TYPE Board",
            "rows(2) AS Row",
            "grid(2,3) AS Row",
            "END TYPE",
            "DIM b AS Board",
            "b.rows(1).n = 66",
            "b.grid(2,3).n = 55",
            "PRINT b.rows(1).n; b.grid(2,3).n",
        ],
    )
    out = console.send_line("RUN")
    assert "66" in out
    assert "55" in out


def test_nested_type_two_level_array(console):
    _prog(
        console,
        [
            "TYPE Cell",
            "cols(3) AS INTEGER",
            "END TYPE",
            "TYPE Grid",
            "rows(2) AS Cell",
            "END TYPE",
            "DIM g(3) AS Grid",
            "g(2).rows(1).cols(3) = 7",
            "PRINT g(2).rows(1).cols(3)",
        ],
    )
    assert console.send_line("RUN") == "7"


def test_struct_member_array_loop_variable_indices(console):
    """Member arrays indexed by loop variables must resolve real offsets.

    Regression: the trace cache resolved a dotted LHS/RHS array with a NULL
    index vector, so member_index_off() read index 0/1 and either faulted
    (native Linux/macOS) or silently aliased every element to (0, 0) on bare
    metal. Writing then reading a 2D member array with loop variables must
    round-trip each element.
    """
    _prog(
        console,
        [
            "TYPE Board",
            "cells(2, 2) AS INTEGER",
            "END TYPE",
            "DIM b AS Board",
            "FOR i = 0 TO 2",
            "FOR j = 0 TO 2",
            "b.cells(i, j) = i * 10 + j",
            "NEXT j",
            "NEXT i",
            "total = 0",
            "FOR i = 0 TO 2",
            "FOR j = 0 TO 2",
            "total = total + b.cells(i, j)",
            "NEXT j",
            "NEXT i",
            "PRINT total",
        ],
    )
    assert console.send_line("RUN") == "99"



def test_list_type_after_run(console):
    _prog(
        console,
        [
            "TYPE Point",
            "x AS INTEGER",
            "y AS INTEGER",
            "END TYPE",
            "DIM p AS Point",
        ],
    )
    assert console.send_line("RUN") == ""
    out = console.send_line("LIST TYPE")
    assert "TYPE" in out.upper()
    assert "POINT" in out.upper()
    assert "INTEGER" in out.upper()


def test_pixel_and_math_member_view(fresh_console):
    c = fresh_console
    _prog(
        c,
        [
            "TYPE Point",
            "x AS INTEGER",
            "y AS INTEGER",
            "END TYPE",
            "DIM pos(2) AS Point",
            "pos(0).x = 80",
            "pos(0).y = 300",
            "pos(1).x = 90",
            "pos(1).y = 310",
            "pos(2).x = 100",
            "pos(2).y = 320",
            "CLS",
            "PIXEL pos().x, pos().y, RGB(255,0,0)",
            "PRINT PIXEL(80,300)",
            "MATH SCALE pos().x, 2, pos().x",
            "PRINT pos(0).x",
            "PRINT pos(1).x",
            "PRINT MATH(SUM pos().x)",
            "DIM INTEGER YY(2) = (40, 50, 60)",
            "PIXEL pos().x, YY(), RGB(0,255,0)",
            "PRINT PIXEL(160,40)",
        ],
    )
    lines = [ln.strip() for ln in c.send_line("RUN").split("\n") if ln.strip() != ""]
    red = int(lines[0])
    assert ((red >> 16) & 255) > 150 and ((red >> 8) & 255) < 80
    assert lines[1] == "160"
    assert lines[2] == "180"
    assert lines[3] == "540"
    green = int(lines[4])
    assert ((green >> 8) & 255) > 150 and ((green >> 16) & 255) < 80


def test_help_type_and_struct(console):
    t = dump_topic(console, "TYPE")
    assert t != "?SYNTAX ERROR"
    assert "HELP: TYPE" in t or "TYPE name" in t or "user type" in t.lower()
    assert "INTEGER" in t
    assert "DIM" in t
    assert "arr().member" in t or "pos().x" in t
    s = dump_topic(console, "STRUCT")
    assert "COPY" in s
    assert "SIZEOF" in s
    assert "PIXEL" in s and "MATH" in s
    subh = dump_topic(console, "SUB")
    assert "AS typename" in subh or "AS name" in subh.lower()
    assert "FUNCTION" in subh
    endt = dump_topic(console, "END TYPE")
    assert "INTEGER" in endt
    basic = dump_topic(console, "BASIC")
    assert "TYPE" in basic
    cmds = dump_topic(console, None)
    assert "STRUCT" in cmds
    fns = dump_topic(console, "FUNCTIONS")
    assert "STRUCT" in fns
    dim = dump_topic(console, "DIM")
    assert "typename" in dim.lower() or "TYPE" in dim
    lst = dump_topic(console, "LIST")
    assert "LIST TYPE" in lst or "TYPE" in lst


def test_struct_fields_named_after_colours(console):
    """#1307: colour-named struct members are paths, not colour constants."""
    _prog(
        console,
        [
            "TYPE Cells",
            "red AS INTEGER",
            "grey AS INTEGER",
            "END TYPE",
            "DIM c AS Cells",
            "c.red = 4",
            "c.grey = 8",
            "PRINT c.red + c.grey",
            "PRINT c.red",
        ],
    )
    out = console.send_line("RUN")
    assert "12" in out
    assert "4" in out


# #1496: a FUNCTION may return a TYPE larger than the old fixed 64 KB staging
# buffer. BigItem is 3 * (1024 + 4) = 3084 bytes, so BigBag (25 items) is
# ~77 KB.
_BIG_TYPE = [
    "TYPE BigItem",
    "a AS STRING LENGTH 1024",
    "b AS STRING LENGTH 1024",
    "c AS STRING LENGTH 1024",
    "END TYPE",
    "TYPE BigBag",
    "items(24) AS BigItem",
    "END TYPE",
]


def test_function_returns_type_over_64k(console):
    _prog(
        console,
        _BIG_TYPE
        + [
            "FUNCTION MakeBag() AS BigBag",
            "LOCAL b AS BigBag",
            'b.items(0).a = "first"',
            'b.items(12).b = "middle"',
            'b.items(24).c = "last"',
            "MakeBag = b",
            "END FUNCTION",
            "DIM got AS BigBag",
            "got = MakeBag()",
            "PRINT got.items(0).a",
            "PRINT got.items(12).b",
            "PRINT got.items(24).c",
            'PRINT STRUCT(SIZEOF "BigBag")',
        ],
    )
    out = console.send_line("RUN")
    lines = [ln.strip() for ln in out.splitlines() if ln.strip()]
    assert "first" in lines
    assert "middle" in lines
    assert "last" in lines
    assert any(ln.isdigit() and int(ln) > 65536 for ln in lines), out


def test_nested_and_recursive_large_type_return(console):
    _prog(
        console,
        _BIG_TYPE
        + [
            "FUNCTION MakeBag() AS BigBag",
            "LOCAL b AS BigBag",
            'b.items(0).a = "fromMake"',
            "MakeBag = b",
            "END FUNCTION",
            "FUNCTION WrapBag() AS BigBag",
            "LOCAL t AS BigBag",
            "t = MakeBag()",
            't.items(0).b = "wrapped"',
            "WrapBag = t",
            "END FUNCTION",
            "FUNCTION RecurBag(n AS INTEGER) AS BigBag",
            "LOCAL t AS BigBag",
            "IF n > 0 THEN",
            "t = RecurBag(n - 1)",
            "END IF",
            't.items(0).c = "level" + STR$(n)',
            "RecurBag = t",
            "END FUNCTION",
            "DIM w AS BigBag",
            "w = WrapBag()",
            "PRINT w.items(0).a",
            "PRINT w.items(0).b",
            "DIM r AS BigBag",
            "r = RecurBag(4)",
            "PRINT r.items(0).c",
        ],
    )
    out = console.send_line("RUN")
    lines = [ln.strip() for ln in out.splitlines() if ln.strip()]
    assert "fromMake" in lines
    assert "wrapped" in lines
    assert any("level4" == ln.replace(" ", "") for ln in lines), out


def test_struct_sort_and_swap_over_64k(console):
    _prog(
        console,
        [
            "TYPE BigItem",
            "a AS STRING LENGTH 1024",
            "b AS STRING LENGTH 1024",
            "c AS STRING LENGTH 1024",
            "END TYPE",
            "TYPE BigBag",
            "key AS INTEGER",
            "items(24) AS BigItem",
            "END TYPE",
            "DIM bags(2) AS BigBag",
            "bags(0).key = 3",
            'bags(0).items(0).a = "three"',
            "bags(1).key = 1",
            'bags(1).items(0).a = "one"',
            "bags(2).key = 2",
            'bags(2).items(0).a = "two"',
            "STRUCT SORT bags().key",
            "PRINT bags(0).items(0).a",
            "PRINT bags(1).items(0).a",
            "PRINT bags(2).items(0).a",
            "STRUCT SWAP bags(0), bags(2)",
            "PRINT bags(0).items(0).a",
            "PRINT bags(2).items(0).a",
        ],
    )
    out = console.send_line("RUN")
    lines = [ln.strip() for ln in out.splitlines() if ln.strip()]
    assert lines == ["one", "two", "three", "three", "one"], out


def test_type_layout_overflow_raises_error(console):
    _prog(
        console,
        [
            "TYPE Huge",
            "a(50000, 50000) AS INTEGER",
            "END TYPE",
        ],
    )
    out = console.send_line("RUN")
    assert "OVERFLOW" in out.upper(), out
    assert console.send_line("PRINT 1") == "1"


def test_type_size_overflow_raises_error(console):
    _prog(
        console,
        [
            "TYPE Huge",
            "a(2000000000) AS INTEGER",
            "END TYPE",
        ],
    )
    out = console.send_line("RUN")
    assert "OVERFLOW" in out.upper(), out
    assert console.send_line("PRINT 1") == "1"
