"""THEME() language function over serial: forms, palette resolution, errors."""

import time

from harness import MMBasicConsole


def _near(rgb, want, tol=20):
    return all(abs(a - b) <= tol for a, b in zip(rgb, want))


def test_option_theme_sets_system_theme(kernel_image):
    """#509: OPTION THEME selects the system-wide theme."""
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        assert con.send_line("OPTION THEME NORD") == ""
        nord_bg = con.send_line("PRINT RGB(46,52,64)")  # Nord TEXT_BG
        assert con.send_line('PRINT THEME("TEXT_BG")') == nord_bg

        assert con.send_line("OPTION THEME 8") == ""  # Turbo
        assert con.send_line('PRINT THEME("TEXT_BG")') == con.send_line(
            "PRINT RGB(0,0,170)"
        )

        bad = con.send_line('OPTION THEME "NOPE"').upper()
        assert "THEME" in bad, bad
        assert con.send_line("OPTION THEME SLATE") == ""
    finally:
        con.stop()


def test_settings_picker_live_applies_and_persists(kernel_image):
    """#509: the SETTINGS picker previews live and saves on Enter."""
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        assert con.send_line("OPTION THEME SLATE") == ""

        # Open the hub on Appearance (index 5); Enter enters the theme
        # picker, whose title bar shows the active theme's menu_bg.
        con._ser.sendall(b"SETTINGS\r")
        time.sleep(0.8)
        con._ser.sendall(b"\r")
        time.sleep(0.4)
        slate_bar = con.screen_pixel(4, 4)
        assert _near(slate_bar, (42, 44, 50)), slate_bar

        # One Down lands on Forest (6); the title bar must recolour
        # immediately, before Enter is pressed (live preview).
        con._ser.sendall(b"\x1b[B")
        time.sleep(0.4)
        forest_bar = con.screen_pixel(4, 4)
        assert _near(forest_bar, (10, 20, 12)), forest_bar

        # A second Down lands on Violet (7).
        con._ser.sendall(b"\x1b[B")
        time.sleep(0.4)
        violet_bar = con.screen_pixel(4, 4)
        assert _near(violet_bar, (18, 10, 24)), violet_bar

        con._ser.sendall(b"\r")
        time.sleep(0.6)

        listed = con.send_line("OPTION LIST ALL").upper()
        assert "VIOLET" in listed, listed
        assert con.send_line('PRINT THEME("TEXT_BG")') == con.send_line(
            "PRINT RGB(18,10,24)"
        )

        # Esc cancels: the section backs to the hub (restoring the entry
        # theme) and a second Esc closes back to the prompt. Drain the live
        # preview redraw before each Esc so the UART is not mid-flush.
        assert con.send_line("OPTION THEME SLATE") == ""
        con._ser.sendall(b"SETTINGS\r")
        time.sleep(0.8)
        con._ser.sendall(b"\r")
        time.sleep(0.4)
        con._ser.sendall(b"\x1b[B")
        time.sleep(0.3)
        con.drain(quiet=0.3, timeout=1.5)
        con._ser.sendall(b"\x1b")
        time.sleep(0.4)
        con.drain(quiet=0.3, timeout=1.5)
        con._ser.sendall(b"\x1b")
        time.sleep(0.4)
        con.drain(quiet=0.3, timeout=1.5)
        assert con.send_line('PRINT THEME("TEXT_BG")') == con.send_line(
            "PRINT RGB(42,44,50)"
        )
        assert con.send_line("FACTORY_RESET") == "Factory defaults restored"
    finally:
        con.stop()

    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        assert con.send_line('OPTION THEME "Slate"') == ""
        expected = con.send_line("PRINT RGB(42,44,50)")  # Slate TEXT_BG
        quoted = con.send_line('PRINT THEME("TEXT_BG")')
        bare = con.send_line("PRINT THEME(TEXT_BG)")
        assert con.send_line('C$ = "TEXT_BG"') == ""
        variable = con.send_line("PRINT THEME(C$)")
        assert quoted == bare == variable == expected, (quoted, bare, variable, expected)

        assert con.send_line('COLOUR THEME("MENU_FG"), THEME("MENU_BG")') == ""
        assert con.send_line("PRINT THEME(ERROR_FG)") == con.send_line("PRINT RGB(244,245,248)")

        assert con.send_line("OPTION THEME TURBO") == ""
        assert con.send_line('PRINT THEME("TEXT_BG")') == con.send_line("PRINT RGB(0,0,170)")
        assert con.send_line('PRINT THEME("NUMBER_FG")') == con.send_line("PRINT RGB(85,255,255)")
        assert con.send_line('PRINT THEME(FIELD_FG)') == con.send_line("PRINT RGB(0,0,0)")

        bad = con.send_line('PRINT THEME("NOPE")').upper()
        assert "SYNTAX ERROR" in bad, bad
        assert con.send_line("OPTION THEME SLATE") == ""
    finally:
        con.stop()
