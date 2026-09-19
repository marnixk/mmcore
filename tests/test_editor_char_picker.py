"""EDIT Ctrl+Alt special-character picker (issue #402).

The picker opens after Ctrl+Alt are held for 1.5s, navigates with the arrow
keys, inserts the selected character with Enter, and closes when Ctrl+Alt are
released. Drive the real USB keyboard (``-device usb-kbd``) through QEMU's QMP
input layer so modifiers can be held while arrows and Enter are tapped.
"""

import re
import time

from harness import MMBasicConsole


def _plain(s: str) -> str:
    return re.sub(r"\x1b\[[0-9;?]*[A-Za-z]", "", s)


def _usb_console(kernel_image) -> MMBasicConsole:
    return MMBasicConsole(kernel_image, extra_qemu=["-device", "usb-kbd"])


def _edit(con, path: str) -> str:
    con.drain(quiet=0.2)
    con._ser.sendall(f'EDIT "{path}"\r'.encode())
    return _plain(con.drain(quiet=0.8).decode(errors="replace"))


def _hold_ctrl_alt(con, seconds: float = 1.8) -> None:
    con.key_down("ctrl")
    con.key_down("alt")
    time.sleep(seconds)


def _release_ctrl_alt(con) -> None:
    con.key_up("alt")
    con.key_up("ctrl")
    time.sleep(0.3)


def test_editor_hold_ctrl_alt_opens_and_release_closes_char_picker(kernel_image):
    con = _usb_console(kernel_image)
    con.start()
    try:
        _edit(con, "CP1.BAS")
        _hold_ctrl_alt(con)
        opened = _plain(con.drain(quiet=0.4).decode(errors="replace"))
        assert "Special characters" in opened
        assert "128" in opened
        _release_ctrl_alt(con)
        closed = _plain(con.drain(quiet=0.4).decode(errors="replace"))
        assert "Special characters" not in closed
    finally:
        con.stop()


def test_editor_char_picker_arrows_move_and_enter_inserts(kernel_image):
    con = _usb_console(kernel_image)
    con.start()
    try:
        _edit(con, "CP2.BAS")
        _hold_ctrl_alt(con)
        assert "Special characters" in _plain(
            con.drain(quiet=0.4).decode(errors="replace")
        )
        # Selection starts at 128; Down -> 144, Right -> 145 (ae).
        con.key_tap("down")
        time.sleep(0.2)
        con.key_tap("right")
        time.sleep(0.2)
        con.key_tap("ret")
        time.sleep(0.3)
        # The picker stays open: Right -> 146 (AE), Enter inserts again.
        con.key_tap("right")
        time.sleep(0.2)
        con.key_tap("ret")
        time.sleep(0.3)
        _release_ctrl_alt(con)
        con.drain(quiet=0.3)
        con._ser.sendall(bytes([19]))  # Ctrl+S
        time.sleep(0.4)
        con.drain(quiet=0.3)
        con._ser.sendall(bytes([1]) + b"x")  # Alt+X quit
        time.sleep(0.6)
        con.drain(quiet=0.4)
        assert con.send_line('OPEN "CP2.BAS" FOR INPUT AS #1') == ""
        assert con.send_line("LINE INPUT #1, A$") == ""
        assert con.send_line("PRINT LEN(A$)") == "2"
        assert con.send_line("PRINT ASC(A$)") == "145"
        assert con.send_line("PRINT ASC(MID$(A$,2,1))") == "146"
        assert con.send_line("CLOSE #1") == ""
    finally:
        con.stop()
