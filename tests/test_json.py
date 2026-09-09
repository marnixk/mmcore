"""JSON$ path query on a STRING fixture."""

from ihelp_util import dump_topic


def _js():
    return (
        '{"main":{"temp":12.5,"pressure":1012},'
        '"weather":[{"id":800,"description":"clear"}],'
        '"name":"Paris","ok":true}'
    )


def test_json_path_query(console):
    js = _js().replace('"', '""')
    assert console.send_line("NEW") == ""
    assert console.send_line(f'js$ = "{js}"') == ""
    assert console.send_line('PRINT JSON$(js$, "name")') == "Paris"
    assert console.send_line('PRINT JSON$(js$, "main.temp")') == "12.5"
    assert console.send_line('PRINT VAL(JSON$(js$, "main.pressure"))') == "1012"
    assert console.send_line('PRINT JSON$(js$, "weather[0].id")') == "800"
    assert console.send_line('PRINT JSON$(js$, "weather[0].description")') == "clear"
    assert console.send_line('PRINT JSON$(js$, "missing")') == ""
    assert console.send_line('PRINT JSON$(js$, "weather[9].id")') == ""
    assert console.send_line('PRINT JSON$(js$, "ok")') == "true"


def test_json_invalid_errors(console):
    out = console.send_line('PRINT JSON$("{", "a")')
    assert "?" in out
    assert console.send_line("PRINT 1") == "1"


def test_help_json(console):
    out = dump_topic(console, "JSON$")
    assert out != "?SYNTAX ERROR"
    assert "JSON$(json$" in out.replace(" ", "") or "path$" in out
    assert "main.temp" in out
    assert "WEB TCP" not in out
    assert "JSON_PARSE" in out
    fns = dump_topic(console, "FUNCTIONS")
    assert "JSON$" in fns
    assert "JSON_STRINGIFY$" in fns
    alias = dump_topic(console, "JSON")
    assert "path$" in alias or "JSON$" in alias


def _prog(console, lines):
    assert console.send_line("NEW") == ""
    n = 10
    for line in lines:
        assert console.send_line(f"{n} {line}") == ""
        n += 10


def test_json_parse_stringify_type(console):
    _prog(
        console,
        [
            "TYPE Weather",
            "temp AS FLOAT",
            "name AS STRING",
            "ok AS INTEGER",
            "hum AS INTEGER",
            "END TYPE",
            "DIM w AS Weather",
            "w.temp = 1",
            'w.name = "x"',
            "w.ok = 9",
            "w.hum = 7",
            'js$ = "{""temp"":12.5,""name"":""Paris"",""ok"":true,""extra"":1,""hum"":null}"',
            "JSON_PARSE js$, w",
            "PRINT w.temp",
            "PRINT w.name",
            "PRINT w.ok",
            "PRINT w.hum",
            "s$ = JSON_STRINGIFY$(w)",
            'PRINT JSON$(s$, "temp")',
            'PRINT JSON$(s$, "name")',
            'PRINT JSON$(s$, "ok")',
        ],
    )
    lines = [ln.strip() for ln in console.send_line("RUN").split("\n") if ln.strip() != ""]
    assert lines[0] == "12.5"
    assert lines[1] == "Paris"
    assert lines[2] == "1"
    assert lines[3] == "7"
    assert lines[4] == "12.5"
    assert lines[5] == "Paris"
    assert lines[6] == "1"


def test_json_parse_partial_case_nested(console):
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
            'r.n = "old"',
            "r.pt.x = 1",
            "r.pt.y = 2",
            'js$ = "{""N"":""hi"",""pt"":{""X"":9}}"',
            "JSON_PARSE js$, r",
            "PRINT r.n",
            "PRINT r.pt.x",
            "PRINT r.pt.y",
        ],
    )
    lines = [ln.strip() for ln in console.send_line("RUN").split("\n") if ln.strip() != ""]
    assert lines[0] == "hi"
    assert lines[1] == "9"
    assert lines[2] == "2"


def test_json_parse_bool_false_and_overflow(console):
    _prog(
        console,
        [
            "TYPE T",
            "ok AS INTEGER",
            "END TYPE",
            "DIM t AS T",
            "t.ok = 5",
            'js$ = "{""ok"":false}"',
            "JSON_PARSE js$, t",
            "PRINT t.ok",
        ],
    )
    assert console.send_line("RUN") == "0"
    _prog(
        console,
        [
            "TYPE T",
            "ok AS INTEGER",
            "END TYPE",
            "DIM t AS T",
            'JSON_PARSE "{""ok"":1e20}", t',
            "PRINT 1",
        ],
    )
    err = console.send_line("RUN")
    assert "?" in err


def test_help_json_parse_stringify(console):
    p = dump_topic(console, "JSON_PARSE")
    assert p != "?SYNTAX ERROR"
    assert "JSON_PARSE" in p
    assert "unchanged" in p.lower() or "null" in p.lower()
    assert "WEB TCP" not in p
    s = dump_topic(console, "JSON_STRINGIFY$")
    assert s != "?SYNTAX ERROR"
    assert "JSON_STRINGIFY$" in s
    assert "255" in s
    cmds = dump_topic(console, None)
    assert "JSON_PARSE" in cmds
    fns = dump_topic(console, "FUNCTIONS")
    assert "JSON_STRINGIFY$" in fns
    out = dump_topic(console, "JSON$")
    assert out != "?SYNTAX ERROR"
    assert "JSON$(json$" in out.replace(" ", "") or "path$" in out
    assert "main.temp" in out
    assert "WEB TCP" not in out
    fns = dump_topic(console, "FUNCTIONS")
    assert "JSON$" in fns
    alias = dump_topic(console, "JSON")
    assert "path$" in alias or "JSON$" in alias
