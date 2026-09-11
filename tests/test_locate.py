"""LOCATE command and PRINT cursor position (GH-264)."""


def test_locate_sets_hpos_vpos(console):
    console.send_line("LOCATE 0, 0")
    assert console.send_line("LOCATE 16, 32") == ""
    assert console.send_line("PRINT MM.HPOS;") == "16"
    assert console.send_line("PRINT MM.VPOS;") == "32"


def test_print_at_advances_cursor(console):
    console.send_line("LOCATE 0, 0")
    assert console.send_line('PRINT @(0,0)"HELLO"') == "HELLO"
    assert console.send_line("PRINT MM.HPOS;") == "40"
    console.send_line("LOCATE 0, 0")
    assert console.send_line('PRINT @(0,0)"HI"') == "HI"
    assert console.send_line("PRINT POS(0);") == "3"


def test_locate_then_print(console):
    console.send_line("LOCATE 0, 0")
    console.send_line("LOCATE 8, 16")
    assert console.send_line('PRINT "X";') == "X"
    assert console.send_line("PRINT MM.HPOS;") == "16"
    assert console.send_line("PRINT MM.VPOS;") == "16"
