"""#1250: KEYDOWN() keeps the CMM2 list/character-code contract.

The sprites.bas program on the user share polls held keys with the classic
CMM2 loop:

    for i = 1 to keydown(0)
        if keydown(i) = key.left then paddleX = paddleX - 3
        ...

#1238 briefly made KEYDOWN(n) a boolean test for a PC/AT scan code, so that
loop never matched the arrow/Escape codes and the paddle stopped moving on the
Linux ISO. These tests drive real arrow/Escape key presses through the guest's
USB keyboard and assert that the loop moves a paddle and quits.
"""

import time

import pytest

from harness import MMBasicConsole


def _usb_console(kernel_image) -> MMBasicConsole:
    # QEMU does not attach a USB keyboard to raspi3b unless asked; Circle's
    # KEYDOWN() table is fed by the raw USB HID report.
    return MMBasicConsole(kernel_image, extra_qemu=["-device", "usb-kbd"])


@pytest.fixture(scope="module")
def usb_console(kernel_image):
    con = _usb_console(kernel_image)
    con.start()
    yield con
    con.stop()


def _wait_contains(con, token: bytes, timeout: float = 8.0) -> bytes:
    deadline = time.time() + timeout
    seen = b""
    while time.time() < deadline:
        chunk = con._recv(con._ser)
        if chunk:
            seen += chunk
            if token in seen:
                return seen
        else:
            time.sleep(0.05)
    return seen


_MOVE_PROGRAM = (
    "10 CONST KEY.LEFT = 130, KEY.RIGHT = 131, KEY.ESCAPE = 27",
    "20 PADDLE = 100",
    '30 PRINT "GO"',
    "40 DO",
    "50 FOR I = 1 TO KEYDOWN(0)",
    "60 IF KEYDOWN(I) = KEY.LEFT THEN PADDLE = PADDLE - 3",
    "70 IF KEYDOWN(I) = KEY.RIGHT THEN PADDLE = PADDLE + 3",
    "80 IF KEYDOWN(I) = KEY.ESCAPE THEN DONE = 1",
    "90 NEXT I",
    '100 IF DONE = 1 THEN PRINT "QUIT"',
    "105 IF DONE = 1 THEN END",
    "110 LOOP UNTIL PADDLE <> 100",
    '120 PRINT "MOVED "; PADDLE',
)


def _run_with_key(con, key: str, token: bytes) -> str:
    con.send_line("NEW")
    for line in _MOVE_PROGRAM:
        con.send_line(line)
    con.drain(quiet=0.1, timeout=2.0)
    con._ser.sendall(b"RUN\r")
    assert b"GO" in _wait_contains(con, b"GO", timeout=10.0), "program did not start"
    con.key_down(key)
    try:
        seen = _wait_contains(con, token, timeout=10.0)
    finally:
        con.key_up(key)
    con.drain(quiet=0.2, timeout=2.0)
    return seen.decode(errors="replace")


def test_keydown_left_moves_paddle(usb_console):
    out = _run_with_key(usb_console, "left", b"MOVED 97")
    assert "MOVED 97" in out, out


def test_keydown_right_moves_paddle(usb_console):
    out = _run_with_key(usb_console, "right", b"MOVED 103")
    assert "MOVED 103" in out, out


def test_keydown_escape_quits(usb_console):
    out = _run_with_key(usb_console, "esc", b"QUIT")
    assert "QUIT" in out, out
