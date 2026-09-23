"""End-to-end tests driving the bare-metal console inside QEMU.

These prove the harness can inject keystrokes, read the console output back and
determine correctness. They target the placeholder PRINT interpreter today; the
same pattern will validate the real MMBasic implementation once it is ported
onto Circle from picomite-fork.
"""

import pytest

from harness import MMBasicConsole


ARITHMETIC = [
    ("PRINT 2+3", "5"),
    ("PRINT 7*6", "42"),
    ("PRINT 100-58", "42"),
    ("PRINT 84/2", "42"),
    ("PRINT 0+0", "0"),
    ("PRINT 12345+54321", "66666"),
    ("PRINT 9*9", "81"),
    ("PRINT 1000-1", "999"),
    ("PRINT 6*7", "42"),
    ("PRINT 144/12", "12"),
]

STRINGS = [
    ('PRINT "HELLO"', "HELLO"),
    ('PRINT "HELLO WORLD"', "HELLO WORLD"),
    ('PRINT "MMBASIC"', "MMBASIC"),
    ('PRINT "42 IS THE ANSWER"', "42 IS THE ANSWER"),
]

ERRORS = [
    "FOO",
    "PRINX 1+1",
    "HELLO",
    "1+1",
]


@pytest.mark.parametrize("cmd,expected", ARITHMETIC)
def test_arithmetic(console, cmd, expected):
    assert console.send_line(cmd) == expected


@pytest.mark.parametrize("cmd,expected", STRINGS)
def test_strings(console, cmd, expected):
    assert console.send_line(cmd) == expected


@pytest.mark.parametrize("cmd", ERRORS)
def test_syntax_errors(console, cmd):
    assert console.send_line(cmd) == "?SYNTAX ERROR"


def test_keystroke_echo(console):
    """Every injected keystroke is echoed straight back over serial."""
    console.drain(quiet=0.1)
    console._ser.sendall(b"ABC123")
    echoed = console.drain(quiet=0.4).decode(errors="replace")
    console.send_line("")  # flush the pending line back to a prompt
    assert "ABC123" in echoed


def test_screen_shows_typed_text(kernel_image):
    """Typed input and its result are visible on the emulated HDMI screen.

    Uses a fresh boot so the framebuffer holds only the banner and this
    command (the shared console's screen scrolls as other tests run).
    """
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        con.send_line('PRINT "SCREENCHECK"')
        text = con.ocr_screen()
        assert "SCREENCHECK" in text
        assert "HELP" in text or "introduction" in text.lower() or "get started" in text.lower()
    finally:
        con.stop()


def test_option_console_screen_hides_print_on_serial(fresh_console):
    import time

    c = fresh_console
    c.drain(quiet=0.1)
    c._ser.sendall(b"OPTION CONSOLE SCREEN\r")
    time.sleep(0.35)
    c._ser.sendall(b'PRINT "HIDDEN_XYZ"\r')
    time.sleep(0.35)
    c._ser.sendall(b"OPTION CONSOLE BOTH\r")
    time.sleep(0.5)
    seen = c.drain(quiet=0.4).decode(errors="replace")
    assert "HIDDEN_XYZ" not in seen
    assert c.send_line("PRINT 1+1") == "2"


def test_cls_homes_prompt_after_printed_lines(fresh_console):
    """CLS must wipe printed text and home the next prompt to the top.

    Graphics tests always CLS on a fresh boot then draw; they never type
    several PRINT lines first. Without homing Circle's text cursor, the
    framebuffer goes black but the next PRINT stays mid-screen.
    """
    fresh_console.send_line('PRINT "LINEONE"')
    fresh_console.send_line('PRINT "LINETWO"')
    fresh_console.send_line('PRINT "LINETHREE"')
    before = fresh_console.ocr_screen(crop=None)
    assert "LINEONE" in before
    assert "LINETWO" in before
    assert "LINETHREE" in before

    assert fresh_console.send_line("CLS") == ""

    after_cls = fresh_console.ocr_screen(crop=None)
    assert "LINEONE" not in after_cls
    assert "LINETWO" not in after_cls
    assert "LINETHREE" not in after_cls

    fresh_console.send_line('PRINT "TOPAFTERCLS"')
    # Font is 8x16. After three PRINTs the un-homed cursor is ~150px down,
    # so a 64px top crop misses TOPAFTERCLS unless CLS homes the prompt.
    top = fresh_console.ocr_screen(crop="640x64+0+0")
    assert "TOPAFTERCLS" in top
    assert "LINEONE" not in top
    assert "LINETWO" not in top
    assert "LINETHREE" not in top


def _ocr_compact(text: str) -> str:
    return "".join(text.split())


def test_backspace_del_corrects_line(fresh_console):
    """USB Backspace is 0x7f; serial often sends 0x08. Both must drop the last char."""
    out = fresh_console.send_keys(b"PRINT 123X\x7f\r")
    assert "?SYNTAX" not in out.upper()
    assert out.splitlines()[-1].strip() == "123"

    out = fresh_console.send_keys(b"PRINT 123X\x08\r")
    assert "?SYNTAX" not in out.upper()
    assert out.splitlines()[-1].strip() == "123"


def test_backspace_erases_glyph_on_hdmi(fresh_console):
    """Correcting a typo must remove the deleted glyph from HDMI, not only Line[]."""
    out = fresh_console.send_keys(b'PRINT "ABCX\x7f"\r')
    assert "ABC" in out
    assert "?SYNTAX" not in out.upper()
    text = _ocr_compact(fresh_console.ocr_screen(crop="640x300+0+0"))
    assert "ABC" in text
    assert "ABCX" not in text


def test_arrow_csi_does_not_corrupt_line(fresh_console):
    assert fresh_console.send_line("PRINT 9") == "9"
    out = fresh_console.send_keys(b"\x1b[A\r")
    assert "?SYNTAX" not in out.upper()
    assert "9" in out


def test_delete_key_csi_consumed(fresh_console):
    out = fresh_console.send_keys(b"PRINT 8\x1b[3~\r")
    assert "?SYNTAX" not in out.upper()
    assert out.splitlines()[-1].strip() == "8"


def test_ctrl_c_cancels_line(fresh_console):
    fresh_console.send_keys(b"PRINT 999\x03")
    assert fresh_console.send_line("PRINT 7") == "7"
