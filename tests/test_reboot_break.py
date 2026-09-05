"""PrtScr/Ctrl-C can stop RUN; REBOOT is documented (not executed)."""

import time


def test_help_reboot(console):
    listing = console.send_line("HELP")
    assert "REBOOT" in listing
    out = console.send_line("HELP REBOOT")
    assert out != "?SYNTAX ERROR"
    assert "REBOOT" in out
    assert "reset" in out.lower() or "watchdog" in out.lower()
    alias = console.send_line("HELP RESTART")
    assert "REBOOT" in alias
    run = console.send_line("HELP RUN")
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
