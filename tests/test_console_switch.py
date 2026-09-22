"""Virtual consoles (#510): Ctrl+Alt+F1..F4 switch between independent
interpreter sessions with their own screen and state.

The chord is driven through the real USB keyboard (``-device usb-kbd``) so
modifiers can be held, matching how it is pressed on hardware."""

import time

from harness import MMBasicConsole
from ihelp_util import dump_topic


def _usb_console(kernel_image) -> MMBasicConsole:
    return MMBasicConsole(kernel_image, extra_qemu=["-device", "usb-kbd"])


def test_help_documents_consoles(console):
    out = dump_topic(console, "CONSOLES")
    assert "Ctrl+Alt+F" in out
    assert "RUN" in out


def _switch(con, n: int) -> None:
    # Hold the F-key long enough for the guest's USB poll to see it; a fast
    # tap can fall between polls and be missed.
    con.key_down("ctrl")
    con.key_down("alt")
    time.sleep(0.15)
    con.key_down(f"f{n}")
    time.sleep(0.25)
    con.key_up(f"f{n}")
    time.sleep(0.15)
    con.key_up("alt")
    con.key_up("ctrl")
    time.sleep(0.5)


def test_switch_second_console_is_independent(kernel_image):
    con = _usb_console(kernel_image)
    con.start()
    try:
        assert con.send_line("PRINT 1+1") == "2"
        assert con.send_line("A = 111") == ""

        _switch(con, 2)
        banner = con.drain(quiet=0.3, timeout=3.0).decode(errors="ignore")
        assert "MMBasic" in banner

        # Console 2 is a fresh interpreter: it answers and has no leftovers.
        assert con.send_line("PRINT 10*10") == "100"
        assert con.send_line("PRINT A") == "0"
        assert con.send_line("A = 222") == ""

        # Switch back: console 1 keeps its own state and answers.
        _switch(con, 1)
        con.drain(quiet=0.3, timeout=2.0)
        assert con.send_line("PRINT A") == "111"

        # And console 2 still holds the variable set there.
        _switch(con, 2)
        con.drain(quiet=0.3, timeout=2.0)
        assert con.send_line("PRINT A") == "222"
        _switch(con, 1)
        con.drain(quiet=0.3, timeout=2.0)
        assert con.send_line("PRINT A") == "111"
    finally:
        con.stop()


def test_switch_restores_first_console_screen(kernel_image):
    con = _usb_console(kernel_image)
    con.start()
    try:
        assert con.send_line('PRINT "TOPSECRET"') == "TOPSECRET"
        _switch(con, 3)
        con.drain(quiet=0.3, timeout=2.0)
        assert con.send_line("PRINT 123") == "123"
        _switch(con, 1)
        con.drain(quiet=0.3, timeout=2.0)
        screen = con.wait_ocr("TOPSECRET", timeout=10.0, crop="1280x400+0+0")
        assert "TOPSECRET" in screen
    finally:
        con.stop()


def test_editor_survives_switch(kernel_image):
    """A full-screen TUI on one console keeps its buffer and screen while
    another console runs a REPL."""
    con = _usb_console(kernel_image)
    con.start()
    try:
        con.drain(quiet=0.3, timeout=2.0)
        con._ser.sendall(b'EDIT "VCSW.BAS"\r')
        time.sleep(1.2)
        con.drain(quiet=0.4)
        con._ser.sendall(b'PRINT 42')
        time.sleep(0.5)
        con.drain(quiet=0.3)

        _switch(con, 2)
        banner = con.drain(quiet=0.3, timeout=2.0).decode(errors="ignore")
        assert "MMBasic" in banner
        assert con.send_line("PRINT 7*6") == "42"

        _switch(con, 1)
        time.sleep(0.4)
        con.drain(quiet=0.3, timeout=2.0)
        screen = con.wait_ocr("Help", timeout=10.0, crop="1280x400+0+0")
        assert "Help" in screen
        assert "PRINT" in screen.upper()

        # The editor still owns console 1's keyboard: save and quit.
        con._ser.sendall(bytes([19]))  # Ctrl+S
        time.sleep(0.5)
        con.drain(quiet=0.3)
        con._ser.sendall(bytes([1]) + b"x")  # Alt+X
        time.sleep(0.6)
        con.drain(quiet=0.3)
        assert con.send_line("PRINT 100+1") == "101"
    finally:
        con.stop()


def test_running_program_suspends_and_resumes(kernel_image):
    """Switching away from RUN stops it at a line boundary; switching back
    continues where it left off (the INKEY$ loop only exits after input on
    the original console)."""
    con = _usb_console(kernel_image)
    con.start()
    try:
        con.drain(quiet=0.3, timeout=2.0)
        con._ser.sendall(
            b'10 A$=""\r'
            b"20 DO\r"
            b"30 A$=INKEY$\r"
            b"40 LOOP UNTIL A$<>\"\"\r"
            b'50 PRINT "GOT:"; A$\r'
            b"RUN\r"
        )
        time.sleep(0.8)
        con.drain(quiet=0.3)

        _switch(con, 2)
        away = con.drain(quiet=0.3, timeout=2.0).decode(errors="ignore")
        assert "MMBasic" in away
        assert con.send_line("PRINT 7*6") == "42"
        # Suspended: no result line while the program is parked.
        assert "GOT" not in con.drain(quiet=0.4).decode(errors="ignore")

        _switch(con, 1)
        time.sleep(0.5)
        con._ser.sendall(b"x")
        deadline = time.time() + 10.0
        seen = ""
        while time.time() < deadline:
            seen += con.drain(quiet=0.3).decode(errors="ignore")
            if "GOT" in seen:
                break
        assert "GOT" in seen
    finally:
        con.stop()



