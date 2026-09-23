"""PrtScr/Ctrl-C can stop RUN; REBOOT is documented (not executed)."""

import time

from harness import MMBasicConsole
from ihelp_util import dump_topic, open_ihelp, close_ihelp, scroll_all


def _usb_console(kernel_image) -> MMBasicConsole:
    return MMBasicConsole(kernel_image, extra_qemu=["-device", "usb-kbd"])


def test_help_reboot(console):
    listing = scroll_all(console, open_ihelp(console, "INDEX"))
    assert "REBOOT" in listing
    close_ihelp(console)
    out = dump_topic(console, "REBOOT")
    assert out != "?SYNTAX ERROR"
    assert "REBOOT" in out
    assert "reset" in out.lower() or "watchdog" in out.lower()
    assert "Ctrl+Alt+Del" in out or "ctrl+alt+del" in out.lower()
    alias = dump_topic(console, "RESTART")
    assert "REBOOT" in alias
    run = dump_topic(console, "RUN")
    assert "PrtScr" in run or "Print Screen" in run
    assert "BREAK" in run


def test_ctrl_c_breaks_running_program(fresh_console):
    c = fresh_console
    assert c.send_line("10 PAUSE 20") == ""
    assert c.send_line("20 GOTO 10") == ""
    c.drain(quiet=0.1)
    c._ser.sendall(b"RUN\r")
    time.sleep(0.4)
    out = c.send_keys(b"\x03", timeout=6.0)
    assert "BREAK" in out.upper()
    assert c.send_line("PRINT 1+1") == "2"


def test_ctrl_c_stops_play(fresh_console):
    c = fresh_console
    assert c.send_line("PLAY TONE 440, 440") == ""
    assert c.send_line("PRINT PLAYING()") == "1"
    assert c.send_line("10 PAUSE 20") == ""
    assert c.send_line("20 GOTO 10") == ""
    c.drain(quiet=0.1)
    c._ser.sendall(b"RUN\r")
    time.sleep(0.4)
    out = c.send_keys(b"\x03", timeout=6.0)
    assert "BREAK" in out.upper()
    assert c.send_line("PRINT PLAYING()") == "0"


def test_ctrl_c_page_write_loop_restores_console(fresh_console):
    c = fresh_console
    assert c.send_line("NEW") == ""
    assert c.send_line("10 PAGE WRITE 1") == ""
    assert c.send_line("20 PAGE DISPLAY 1") == ""
    assert c.send_line("30 CLS") == ""
    assert c.send_line("40 GOTO 30") == ""
    c.drain(quiet=0.1)
    c._ser.sendall(b"RUN\r")
    time.sleep(0.4)
    out = c.send_keys(b"\x03", timeout=6.0)
    assert "BREAK" in out.upper()
    assert c.send_line("PRINT 9") == "9"
    assert c.send_line("PAGE WRITE 0") == ""
    assert c.send_line("CLS RGB(255,0,0)") == ""
    pix = int(c.send_line("PRINT PIXEL(4,4)"))
    assert ((pix >> 16) & 255) > 150


def _ctrl_alt_del(con: MMBasicConsole) -> None:
    con.key_down("ctrl")
    con.key_down("alt")
    time.sleep(0.15)
    con.key_down("delete")
    time.sleep(0.3)
    con.key_up("delete")
    time.sleep(0.15)
    con.key_up("alt")
    con.key_up("ctrl")


def test_ctrl_alt_del_returns_to_prompt(kernel_image):
    """#577: Ctrl+Alt+Del is a warm reset. It must land at a ready prompt with
    the interpreter state cleared, not leave the session without a REPL."""
    con = _usb_console(kernel_image)
    con.start()
    try:
        assert con.send_line("A = 1234") == ""
        assert con.send_line("PRINT A") == "1234"

        con.drain(quiet=0.2)
        _ctrl_alt_del(con)
        seen = ""
        deadline = time.time() + 12.0
        while time.time() < deadline:
            seen += con.drain(quiet=0.3).decode(errors="ignore")
            if "MMBasic" in seen:
                break
        assert "MMBasic" in seen

        # Fresh session: variables and the program are gone.
        assert con.send_line("PRINT A") == "0"
        assert con.send_line("PRINT 6 * 7") == "42"
    finally:
        con.stop()
