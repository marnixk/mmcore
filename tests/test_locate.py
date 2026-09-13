"""LOCATE command: character cells, Y then X, optional cursor (GH-282)."""


def test_locate_sets_hpos_vpos(console):
    console.send_line("LOCATE 0, 0")
    assert console.send_line("LOCATE 2, 2") == ""
    assert console.send_line("PRINT MM.HPOS;") == "16"
    assert console.send_line("PRINT MM.VPOS;") == "32"


def test_locate_is_row_then_column(console):
    console.send_line("LOCATE 0, 0")
    assert console.send_line("LOCATE 1, 3") == ""
    assert console.send_line("PRINT MM.HPOS;") == "24"
    assert console.send_line("PRINT MM.VPOS;") == "16"


def test_locate_80x25_bottom_right(fresh_console):
    assert fresh_console.send_line("MODE 2, 16") == ""
    assert fresh_console.send_line("PRINT MM.HRES;") == "640"
    assert fresh_console.send_line("PRINT MM.VRES;") == "400"
    assert fresh_console.send_line("LOCATE 24, 79") == ""
    assert fresh_console.send_line("PRINT MM.HPOS;") == "632"
    assert fresh_console.send_line("PRINT MM.VPOS;") == "384"


def test_locate_omitted_args_keep_position(fresh_console):
    c = fresh_console
    assert c.send_line("NEW") == ""
    assert c.send_line("10 LOCATE 3, 5") == ""
    assert c.send_line("20 LOCATE , 7") == ""
    assert c.send_line("30 A=MM.HPOS:B=MM.VPOS") == ""
    assert c.send_line("40 LOCATE 4") == ""
    assert c.send_line("50 C=MM.HPOS:D=MM.VPOS") == ""
    assert c.send_line("60 PRINT A") == ""
    assert c.send_line("70 PRINT B") == ""
    assert c.send_line("80 PRINT C") == ""
    assert c.send_line("90 PRINT D") == ""
    out = c.send_line("RUN")
    lines = [ln.strip() for ln in out.splitlines() if ln.strip()]
    assert lines[:4] == ["56", "48", "56", "64"], out


def test_locate_cursor_arg_is_optional(console):
    assert console.send_line("LOCATE , , 0") == ""
    assert console.send_line("LOCATE , , 1") == ""
    assert console.send_line("LOCATE 0, 0, 0") == ""
    assert console.send_line("PRINT 1+1") == "2"


def test_print_at_advances_cursor(console):
    console.send_line("LOCATE 0, 0")
    assert console.send_line('PRINT @(0,0)"HELLO"') == "HELLO"
    assert console.send_line("PRINT MM.HPOS;") == "40"
    console.send_line("LOCATE 0, 0")
    assert console.send_line('PRINT @(0,0)"HI"') == "HI"
    assert console.send_line("PRINT POS(0);") == "3"


def test_locate_then_print(console):
    console.send_line("LOCATE 0, 0")
    console.send_line("LOCATE 1, 1")
    assert console.send_line('PRINT "X";') == "X"
    assert console.send_line("PRINT MM.HPOS;") == "16"
    assert console.send_line("PRINT MM.VPOS;") == "16"
