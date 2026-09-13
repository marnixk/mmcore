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


def test_locate_omitted_args_keep_position(console):
    console.send_line("LOCATE 3, 5")
    assert console.send_line("LOCATE , 7") == ""
    assert console.send_line("PRINT MM.HPOS;") == "56"
    assert console.send_line("PRINT MM.VPOS;") == "48"
    assert console.send_line("LOCATE 4") == ""
    assert console.send_line("PRINT MM.HPOS;") == "56"
    assert console.send_line("PRINT MM.VPOS;") == "64"


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
