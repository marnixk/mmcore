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
    fns = dump_topic(console, "FUNCTIONS")
    assert "JSON$" in fns
    alias = dump_topic(console, "JSON")
    assert "path$" in alias or "JSON$" in alias
