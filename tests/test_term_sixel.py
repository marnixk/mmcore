"""TERM sixel support (docs/analysis/term-sixel.md).

Phase 1 locks the DCS framing: ``ESC P`` ... ``ST`` is swallowed, whichever
command the final byte selects, so a sixel payload never lands in the cell
grid as text.  Later phases add the decoder and the image plane.
"""

from harness import MMBasicConsole, TermReplay
from test_term import _plain, _quit
from test_term_replay import _open_replay


def _session(con, replay):
    seen = _open_replay(con, replay, connect=False)
    return _plain(seen)


def test_dcs_sixel_payload_is_not_printed(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    replay = TermReplay(con, "127.0.0.1", 1)
    try:
        seen = _session(con, replay)
        replay._to_guest(
            b"\x1bP0;0;0q#0;2;100;0;0~PAYLOADMARK\x1b\\AFTERST"
        )
        more = replay.wait_serial(lambda s: "AFTERST" in _plain(s), timeout=8.0)
        text = _plain(seen + more)
        assert "AFTERST" in text
        assert "PAYLOADMARK" not in text
        assert "0;0;0" not in text
        assert "100;0;0" not in text
        _quit(con)
        assert con.send_line("PRINT 1+1") == "2"
    finally:
        replay.stop()
        con.stop()


def test_dcs_split_across_reads(kernel_image):
    """ESC alone, then the DCS introducer and body in later frames."""
    con = MMBasicConsole(kernel_image)
    con.start()
    replay = TermReplay(con, "127.0.0.1", 1)
    try:
        seen = _session(con, replay)
        replay._to_guest(b"\x1b")
        replay._to_guest(b"P0;0;0q#0;2;100;0;0~SPLITSIX\x1b")
        replay._to_guest(b"\\AFTERSPLIT")
        more = replay.wait_serial(
            lambda s: "AFTERSPLIT" in _plain(s), timeout=8.0
        )
        text = _plain(seen + more)
        assert "AFTERSPLIT" in text
        assert "SPLITSIX" not in text
        assert "0;0;0" not in text
        _quit(con)
        assert con.send_line("PRINT 2+2") == "4"
    finally:
        replay.stop()
        con.stop()


def test_dcs_8bit_c1_framing(kernel_image):
    """8-bit DCS (0x90) and ST (0x9C) are consumed like the 7-bit forms."""
    con = MMBasicConsole(kernel_image)
    con.start()
    replay = TermReplay(con, "127.0.0.1", 1)
    try:
        seen = _session(con, replay)
        replay._to_guest(
            b"\x900;0;0q#0;2;100;0;0~C1PAYLOAD\x9cC1OK"
        )
        more = replay.wait_serial(lambda s: "C1OK" in _plain(s), timeout=8.0)
        text = _plain(seen + more)
        assert "C1OK" in text
        assert "C1PAYLOAD" not in text
        assert "0;0;0" not in text
        _quit(con)
        assert con.send_line("PRINT 3+3") == "6"
    finally:
        replay.stop()
        con.stop()


def test_dcs_non_sixel_command_is_swallowed(kernel_image):
    """A DCS whose final byte is not 'q' is discarded until ST."""
    con = MMBasicConsole(kernel_image)
    con.start()
    replay = TermReplay(con, "127.0.0.1", 1)
    try:
        seen = _session(con, replay)
        replay._to_guest(b"\x1bP1$rDECRQSSBODY\x1b\\DCSDONE")
        more = replay.wait_serial(lambda s: "DCSDONE" in _plain(s), timeout=8.0)
        text = _plain(seen + more)
        assert "DCSDONE" in text
        assert "DECRQSSBODY" not in text
        _quit(con)
        assert con.send_line("PRINT 4+4") == "8"
    finally:
        replay.stop()
        con.stop()


def test_dcs_bare_esc_aborts_and_is_refed(kernel_image):
    """A lone ESC in the body aborts, and that ESC starts a fresh sequence."""
    con = MMBasicConsole(kernel_image)
    con.start()
    replay = TermReplay(con, "127.0.0.1", 1)
    try:
        seen = _session(con, replay)
        replay._to_guest(b"\x1bPq~~~~\x1b[2JREFED")
        more = replay.wait_serial(lambda s: "REFED" in _plain(s), timeout=8.0)
        text = _plain(seen + more)
        assert "REFED" in text
        assert "~" not in text
        _quit(con)
        assert con.send_line("PRINT 5+5") == "10"
    finally:
        replay.stop()
        con.stop()


def test_dcs_can_aborts(kernel_image):
    """CAN (0x18) ends the DCS and normal text resumes."""
    con = MMBasicConsole(kernel_image)
    con.start()
    replay = TermReplay(con, "127.0.0.1", 1)
    try:
        seen = _session(con, replay)
        replay._to_guest(b"\x1bPq~CANPAY\x18AFTERCAN")
        more = replay.wait_serial(lambda s: "AFTERCAN" in _plain(s), timeout=8.0)
        text = _plain(seen + more)
        assert "AFTERCAN" in text
        assert "CANPAY" not in text
        _quit(con)
        assert con.send_line("PRINT 6+6") == "12"
    finally:
        replay.stop()
        con.stop()
