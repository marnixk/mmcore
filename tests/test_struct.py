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


def test_cmm2_dotted_name_without_type(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("DIM INTEGER glove.pos.x(3) = (130, 210, 150, 170)") == ""
    assert console.send_line("PRINT glove.pos.x(0)") == "130"


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
