"""THEME() language function over serial: forms, palette resolution, errors."""

from harness import MMBasicConsole


def test_theme_function_forms_and_palette(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        assert con.send_line('OPTION EDIT THEME "Slate"') == ""
        expected = con.send_line("PRINT RGB(42,44,50)")  # Slate TEXT_BG
        quoted = con.send_line('PRINT THEME("TEXT_BG")')
        bare = con.send_line("PRINT THEME(TEXT_BG)")
        assert con.send_line('C$ = "TEXT_BG"') == ""
        variable = con.send_line("PRINT THEME(C$)")
        assert quoted == bare == variable == expected, (quoted, bare, variable, expected)

        assert con.send_line('COLOUR THEME("MENU_FG"), THEME("MENU_BG")') == ""
        assert con.send_line("PRINT THEME(ERROR_FG)") == con.send_line("PRINT RGB(244,245,248)")

        assert con.send_line("OPTION EDIT THEME TURBO") == ""
        assert con.send_line('PRINT THEME("TEXT_BG")') == con.send_line("PRINT RGB(0,0,170)")
        assert con.send_line('PRINT THEME("NUMBER_FG")') == con.send_line("PRINT RGB(85,255,255)")
        assert con.send_line('PRINT THEME(FIELD_FG)') == con.send_line("PRINT RGB(0,0,0)")

        bad = con.send_line('PRINT THEME("NOPE")').upper()
        assert "SYNTAX ERROR" in bad, bad
        assert con.send_line("OPTION EDIT THEME SLATE") == ""
    finally:
        con.stop()
