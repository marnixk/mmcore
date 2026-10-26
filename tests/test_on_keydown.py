"""#1330: ON KEYDOWN runs on held-set changes, press or release.

ON KEY only reports key-down because it watches the INKEY$ queue. These tests
drive the real USB keyboard (``-device usb-kbd``) so the Circle build fills
KEYDOWN()'s held-key set, and assert that:

* the handler fires for a press and for the matching release;
* repeating the same key-down report (OS auto-repeat) does not fire again;
* the handler does not consume the INKEY$ copy of the typed key;
* bare ON KEYDOWN removes the handler.
"""

import time

import pytest

from harness import MMBasicConsole
from ihelp_util import dump_topic


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


def _load_program(con, lines) -> None:
    con.send_line("NEW")
    for line in lines:
        con.send_line(line)
    con.drain(quiet=0.1, timeout=2.0)


def _run(con, ready_timeout: float = 8.0) -> None:
    con._ser.sendall(b"RUN\r")
    assert b"READY" in _wait_contains(con, b"READY", timeout=ready_timeout)


_PRESS_RELEASE = [
    "10 ON KEYDOWN Held",
    "20 N=0",
    '30 PRINT "READY"',
    "40 DO",
    "50 PAUSE 20",
    "60 LOOP UNTIL N>=2",
    '70 PRINT "DONE"',
    "80 END",
    "100 SUB Held",
    "110 LOCAL I,S$",
    '120 S$=""',
    "130 FOR I=1 TO KEYDOWN(0)",
    '140 S$=S$+STR$(KEYDOWN(I))+","',
    "150 NEXT I",
    "160 N=N+1",
    '170 PRINT "EV";N;"[";S$;"]"',
    "180 END SUB",
]


def test_on_keydown_fires_for_press_and_release(usb_console):
    con = usb_console
    _load_program(con, _PRESS_RELEASE)
    _run(con)
    con.key_down("left")
    assert b"EV1[130,]" in _wait_contains(con, b"EV1[130,]", timeout=8.0)
    con.key_up("left")
    assert b"EV2[]" in _wait_contains(con, b"EV2[]", timeout=8.0)
    assert b"DONE" in _wait_contains(con, b"DONE", timeout=8.0)


_REPEAT = [
    "10 ON KEYDOWN Held",
    "20 N=0",
    '30 PRINT "READY"',
    "40 PAUSE 1200",
    '50 PRINT "COUNT";N',
    "60 END",
    "100 SUB Held",
    "110 N=N+1",
    "120 END SUB",
]


def test_on_keydown_ignores_repeated_keydown_reports(usb_console):
    con = usb_console
    _load_program(con, _REPEAT)
    _run(con)
    con.key_down("left")
    time.sleep(0.15)
    # The same held set must not look like a second press.
    con.key_down("left")
    time.sleep(0.15)
    con.key_up("left")
    out = _wait_contains(con, b"COUNT2", timeout=8.0)
    assert b"COUNT2" in out, out
    assert b"COUNT3" not in out, out


def test_on_keydown_does_not_consume_inkey(usb_console):
    con = usb_console
    _load_program(
        con,
        [
            "10 ON KEYDOWN Held",
            "20 N=0",
            '30 PRINT "READY"',
            "40 PAUSE 900",
            "50 K$=INKEY$",
            '60 IF K$="" THEN PRINT "COUNT";N;" K0"',
            '70 IF K$<>"" THEN PRINT "COUNT";N;" K";ASC(K$)',
            "80 END",
            "100 SUB Held",
            "110 N=N+1",
            "120 END SUB",
        ],
    )
    _run(con)
    con.key_down("q")
    time.sleep(0.2)
    con.key_up("q")
    out = _wait_contains(con, b"COUNT2 K113", timeout=8.0)
    assert b"COUNT2 K113" in out, out


def test_bare_on_keydown_clears_handler(usb_console):
    con = usb_console
    _load_program(
        con,
        [
            "10 ON KEYDOWN Held",
            "20 ON KEYDOWN",
            "30 N=0",
            '40 PRINT "READY"',
            "50 PAUSE 900",
            '60 PRINT "COUNT";N',
            "70 END",
            "100 SUB Held",
            "110 N=N+1",
            "120 END SUB",
        ],
    )
    _run(con)
    con.key_down("q")
    time.sleep(0.2)
    con.key_up("q")
    out = _wait_contains(con, b"COUNT0", timeout=8.0)
    assert b"COUNT0" in out, out


def test_help_documents_on_keydown(usb_console):
    out = dump_topic(usb_console, "ON")
    assert "ON KEYDOWN" in out
    assert "release" in out.lower()
    assert "repeat" in out.lower()

