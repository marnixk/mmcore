"""Console INPUT reads a typed line into variables."""


def test_input_console_string(fresh_console):
    c = fresh_console
    c.send_line("NEW")
    c.send_line("10 INPUT a$")
    c.send_line('20 PRINT "[" + a$ + "]"')
    out = c.send_keys(b"RUN\rhello\r", timeout=6.0)
    assert "[hello]" in out


def test_input_console_prompt_and_csv(fresh_console):
    c = fresh_console
    c.send_line("NEW")
    c.send_line('10 INPUT "Name"; n$')
    c.send_line("20 INPUT a, b$")
    c.send_line("30 PRINT n$")
    c.send_line('40 PRINT STR$(a) + "/" + b$')
    out = c.send_keys(b"RUN\rAda\r3,xyz\r", timeout=8.0)
    assert "Name?" in out or "Name? " in out
    assert "Ada" in out
    assert "3/xyz" in out


def test_line_input_console(fresh_console):
    c = fresh_console
    c.send_line("NEW")
    c.send_line('10 LINE INPUT "Go"; s$')
    c.send_line("20 PRINT s$")
    out = c.send_keys(b"RUN\rabc def\r", timeout=6.0)
    assert "abc def" in out


def test_help_input_describes_console(console):
    out = console.send_line("HELP INPUT")
    assert "not implemented" not in out.lower()
    assert "prompt" in out.lower()
    assert "INPUT" in out
