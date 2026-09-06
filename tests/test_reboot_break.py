"""PrtScr/Ctrl-C can stop RUN; REBOOT is documented (not executed)."""

import time

from ihelp_util import dump_topic, open_ihelp, close_ihelp, scroll_all


def test_help_reboot(console):
    listing = scroll_all(console, open_ihelp(console))
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
