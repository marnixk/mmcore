"""TERM sixel support (docs/analysis/term-sixel.md).

Phase 1 locks the DCS framing: ``ESC P`` ... ``ST`` is swallowed, whichever
command the final byte selects, so a sixel payload never lands in the cell
grid as text.  Later phases add the decoder and the image plane.
"""

import os
import shutil
import subprocess
import time

import pytest

from harness import MMBasicConsole, TermReplay
from test_term import _pane_grey_hits, _plain, _quit
from test_term_replay import _open_replay

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FIXTURES = os.path.join(REPO, "tests", "term", "sixel")


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


def _is_red(rgb) -> bool:
    r, g, b = rgb
    return r > 170 and g < 90 and b < 90


RED_SIXEL = b"\x1bPq#0;2;100;0;0~\x1b\\"
RED_COLUMN = b"\x1b[1;1H" + RED_SIXEL


def test_sixel_draws_red_at_cursor_cell(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    replay = TermReplay(con, "127.0.0.1", 1)
    try:
        seen = _session(con, replay)
        replay._to_guest(RED_COLUMN)
        time.sleep(0.4)
        replay.pump_once(recv_tcp=False)
        hits = con.screen_pixels([(160, 0), (161, 2), (163, 4), (167, 1)])
        assert any(_is_red(p) for p in hits), hits
        text = _plain(seen)
        assert "0;2;100" not in text
        _quit(con)
        assert con.send_line("PRINT 1+1") == "2"
    finally:
        replay.stop()
        con.stop()


def test_sixel_text_after_image_is_on_next_row(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    replay = TermReplay(con, "127.0.0.1", 1)
    try:
        seen = _session(con, replay)
        replay._to_guest(RED_COLUMN + b"OK")
        more = replay.wait_serial(lambda s: "OK" in _plain(s), timeout=8.0)
        time.sleep(0.3)
        replay.pump_once(recv_tcp=False)
        # The image occupies row 0; 'O' is drawn on row 1, column 0.
        assert _pane_grey_hits(con, 0, 1) > 0
        hits = con.screen_pixels([(160, 0), (161, 2)])
        assert any(_is_red(p) for p in hits), hits
        assert "OK" in _plain(seen + more)
        _quit(con)
        assert con.send_line("PRINT 2+2") == "4"
    finally:
        replay.stop()
        con.stop()


def test_sixel_moves_up_with_text_scroll(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    replay = TermReplay(con, "127.0.0.1", 1)
    try:
        _session(con, replay)
        # Place the image on pane row 5, then scroll the pane three rows.
        replay._to_guest(RED_COLUMN.replace(b"\x1b[1;1H", b"\x1b[6;1H"))
        time.sleep(0.3)
        replay._to_guest(b"\x1b[32;1H\n\n\n")
        time.sleep(0.5)
        replay.pump_once(recv_tcp=False)
        # Rows 5.. move to 2..; pixel (162, 2*16+2) should be red.
        hits = con.screen_pixels([(160, 33), (160, 34), (160, 35)])
        assert any(_is_red(p) for p in hits), hits
        _quit(con)
        assert con.send_line("PRINT 3+3") == "6"
    finally:
        replay.stop()
        con.stop()


# A solid red 16x48 block: two cells wide, three cell rows tall.
RED_BLOCK = (
    b'\x1bPq"1;1;16;48#0;2;100;0;0'
    + b"!16~"
    + b"-!16~" * 7
    + b"\x1b\\"
)


def _send_block(con, replay, cup=b"\x1b[1;1H"):
    replay._to_guest(cup + RED_BLOCK)
    time.sleep(0.5)
    replay.pump_once(recv_tcp=False)


def _red_points(con, coords):
    return any(_is_red(p) for p in con.screen_pixels(coords))


def test_sixel_raster_below_pane_top_draws(kernel_image):
    """A raster image lower in the pane is not clipped to its own height.

    The sixel clip box is image-relative, so an image starting at row 20 must
    still paint all 48 of its rows (#1488).
    """
    con = MMBasicConsole(kernel_image)
    con.start()
    replay = TermReplay(con, "127.0.0.1", 1)
    try:
        _session(con, replay)
        _send_block(con, replay, cup=b"\x1b[20;1H")
        assert _red_points(
            con, [(160, 306), (168, 320), (174, 350)]
        ), "raster image at row 20 was clipped away"
        _quit(con)
        assert con.send_line("PRINT 5+5") == "10"
    finally:
        replay.stop()
        con.stop()


def test_sixel_overwrite_and_menu_roundtrip(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    replay = TermReplay(con, "127.0.0.1", 1)
    try:
        _session(con, replay)
        _send_block(con, replay)
        assert _red_points(con, [(160, 0), (170, 2)])

        # A glyph over the image's first cell replaces it; the next cell
        # keeps its sixel pixels.
        replay._to_guest(b"\x1b[1;1HX")
        time.sleep(0.5)
        replay.pump_once(recv_tcp=False)
        assert _pane_grey_hits(con, 0, 0) > 0
        assert _red_points(con, [(170, 2)]), "neighbouring sixel pixels lost"

        # Opening and closing the menu repaints from the plane: the glyph and
        # the image both come back.
        replay.send_keys(b"\x1b[21~")
        time.sleep(0.4)
        replay.send_keys(b"\x1b\x1b")
        time.sleep(0.6)
        replay.pump_once(recv_tcp=False)
        assert _pane_grey_hits(con, 0, 0) > 0
        assert _red_points(con, [(170, 2)])

        _quit(con)
        assert con.send_line("PRINT 4+4") == "8"
    finally:
        replay.stop()
        con.stop()


def test_sixel_ed2_clears_pixels(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    replay = TermReplay(con, "127.0.0.1", 1)
    try:
        _session(con, replay)
        _send_block(con, replay)
        assert _red_points(con, [(160, 0), (170, 2), (160, 40)])
        replay._to_guest(b"\x1b[2J")
        time.sleep(0.6)
        replay.pump_once(recv_tcp=False)
        assert not _red_points(con, [(160, 0), (170, 2), (160, 40)])
        _quit(con)
        assert con.send_line("PRINT 5+5") == "10"
    finally:
        replay.stop()
        con.stop()


def test_sixel_insert_and_delete_line_move_pixels(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    replay = TermReplay(con, "127.0.0.1", 1)
    try:
        _session(con, replay)
        _send_block(con, replay)
        assert _red_points(con, [(160, 2)])

        # IL at row 0 pushes the image down one cell row.
        replay._to_guest(b"\x1b[1;1H\x1b[L")
        time.sleep(0.6)
        replay.pump_once(recv_tcp=False)
        assert _red_points(con, [(160, 18)]), "image did not move down"
        assert not _red_points(con, [(160, 2)])

        # DL pulls it back up.
        replay._to_guest(b"\x1b[1;1H\x1b[M")
        time.sleep(0.6)
        replay.pump_once(recv_tcp=False)
        assert _red_points(con, [(160, 2)]), "image did not move back up"

        _quit(con)
        assert con.send_line("PRINT 6+6") == "12"
    finally:
        replay.stop()
        con.stop()


def test_sixel_insert_and_delete_chars_shift_pixels(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    replay = TermReplay(con, "127.0.0.1", 1)
    try:
        _session(con, replay)
        _send_block(con, replay)
        assert _red_points(con, [(160, 2)])

        # ICH at column 0 shifts the row right one cell.
        replay._to_guest(b"\x1b[1;1H\x1b[@")
        time.sleep(0.6)
        replay.pump_once(recv_tcp=False)
        assert _red_points(con, [(168, 2)]), "image did not shift right"
        assert not _red_points(con, [(160, 2)])

        # DCH shifts it back.
        replay._to_guest(b"\x1b[1;1H\x1b[P")
        time.sleep(0.6)
        replay.pump_once(recv_tcp=False)
        assert _red_points(con, [(160, 2)]), "image did not shift back left"

        _quit(con)
        assert con.send_line("PRINT 7+7") == "14"
    finally:
        replay.stop()
        con.stop()


def _reply(replay, query, timeout=3.0):
    # Each query is isolated: clear the outbound buffer and wait for a
    # complete reply (CSI finals end in 'c', 't' or 'S').
    replay.pending_out = b""
    time.sleep(0.1)
    replay._to_guest(query)
    deadline = time.time() + timeout
    while time.time() < deadline:
        replay.pump_once(recv_tcp=False)
        data = replay.pending_out
        if data and data[-1:] in (b"c", b"t", b"S"):
            return data
        time.sleep(0.02)
    return replay.pending_out


def test_sixel_da_xtwinops_xtsmgraphics(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    replay = TermReplay(con, "127.0.0.1", 1)
    try:
        _session(con, replay)
        assert b"[=67;84;101;114;109;1;332c" in _reply(replay, b"\x1b[c")
        assert b"[4;512;640t" in _reply(replay, b"\x1b[14t")
        assert b"[6;16;8t" in _reply(replay, b"\x1b[16t")
        assert b"[8;32;80t" in _reply(replay, b"\x1b[18t")
        assert b"[?1;0;256S" in _reply(replay, b"\x1b[?1;1;0S")
        assert b"[?1;0;256S" in _reply(replay, b"\x1b[?1;2;0S")
        assert b"[?2;0;640;512S" in _reply(replay, b"\x1b[?2;1;0S")
        assert b"[?1;3;0S" in _reply(replay, b"\x1b[?1;3;0S")
        assert b"[?3;1;0S" in _reply(replay, b"\x1b[?3;1;0S")
        _quit(con)
        assert con.send_line("PRINT 8+8") == "16"
    finally:
        replay.stop()
        con.stop()


def test_sixel_decsdm_pins_image_and_keeps_cursor(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    replay = TermReplay(con, "127.0.0.1", 1)
    try:
        _session(con, replay)
        # DECSDM set: scrolling off, image at the pane origin, cursor stays.
        replay._to_guest(b"\x1b[?80h\x1b[6;1H" + RED_SIXEL)
        time.sleep(0.6)
        replay.pump_once(recv_tcp=False)
        assert _red_points(con, [(160, 0)]), "image not at pane origin"
        assert not _red_points(con, [(160, 80)])
        replay._to_guest(b"Z")
        time.sleep(0.5)
        replay.pump_once(recv_tcp=False)
        assert _pane_grey_hits(con, 0, 5) > 0, "cursor moved under DECSDM"

        # DECSDM reset: scrolling on again, image at the cursor, cursor moves.
        replay._to_guest(b"\x1b[?80l\x1b[6;1H" + RED_SIXEL)
        time.sleep(0.6)
        replay.pump_once(recv_tcp=False)
        assert _red_points(con, [(160, 80)]), "image not at the cursor row"
        replay._to_guest(b"W")
        time.sleep(0.5)
        replay.pump_once(recv_tcp=False)
        assert _pane_grey_hits(con, 0, 6) > 0, "cursor did not advance"
        _quit(con)
        assert con.send_line("PRINT 9+9") == "18"
    finally:
        replay.stop()
        con.stop()


def test_sixel_view_cycle_and_typing(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    replay = TermReplay(con, "127.0.0.1", 1)
    try:
        _session(con, replay)
        _send_block(con, replay)
        assert _red_points(con, [(160, 0)])

        # F10, then 'w' cycles Boxed -> 80x25: MODE changes and the plane is
        # freed.  The menu must close cleanly and the session must keep typing.
        replay.send_keys(b"\x1b[21~")
        time.sleep(0.4)
        replay.send_keys(b"w")
        time.sleep(0.9)
        replay.send_keys(b"\x1b\x1b")
        time.sleep(0.6)
        assert con.screen_size() == (640, 400)
        replay._to_guest(b"VIEWOK")
        more = replay.wait_serial(lambda s: "VIEWOK" in _plain(s), timeout=8.0)
        assert "VIEWOK" in _plain(more)
        _quit(con)
        assert con.send_line("PRINT 10+10") == "20"
    finally:
        replay.stop()
        con.stop()


def test_sixel_leave_and_reenter(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    replay = TermReplay(con, "127.0.0.1", 1)
    try:
        _session(con, replay)
        _send_block(con, replay)
        assert _red_points(con, [(160, 0)])
        _quit(con)
        assert con.send_line("PRINT 11+11") == "22"

        # A fresh session allocates a fresh plane; the old one was freed.
        _session(con, replay)
        _send_block(con, replay)
        assert _red_points(con, [(160, 0)])
        _quit(con)
        assert con.send_line("PRINT 12+12") == "24"
    finally:
        replay.stop()
        con.stop()


def test_sixel_scrollback_paging_does_not_fault(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    replay = TermReplay(con, "127.0.0.1", 1)
    try:
        _session(con, replay)
        _send_block(con, replay)
        assert _red_points(con, [(160, 0)])

        # Push the image off the top with plenty of scrollback lines.
        replay._to_guest(b"\x1b[32;1H" + b"SCROLLLINE\r\n" * 40)
        time.sleep(0.8)
        replay.pump_once(recv_tcp=False)
        assert not _red_points(con, [(160, 0)])

        # Page Up paints the text cells (not the freed plane); Page Down
        # returns to live without a fault.
        replay.send_keys(b"\x1b[5~")
        time.sleep(0.8)
        replay.pump_once(recv_tcp=False)
        assert not _red_points(con, [(160, 0), (160, 2)])
        replay.send_keys(b"\x1b[6~")
        time.sleep(0.6)
        replay.pump_once(recv_tcp=False)
        _quit(con)
        assert con.send_line("PRINT 13+13") == "26"
    finally:
        replay.stop()
        con.stop()


def _send_fixture(replay, name, cup=b"\x1b[1;1H"):
    data = open(os.path.join(FIXTURES, name), "rb").read()
    replay._to_guest(cup + data, chunk=24, delay=0.01)
    time.sleep(1.2)
    replay.pump_once(recv_tcp=False)


def test_sixel_fixture_img2sixel_red(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    replay = TermReplay(con, "127.0.0.1", 1)
    try:
        _session(con, replay)
        _send_fixture(replay, "red-img2sixel-p16.sixel")
        assert _red_points(con, [(160, 0), (168, 4), (174, 14)])
        _quit(con)
        assert con.send_line("PRINT 14+14") == "28"
    finally:
        replay.stop()
        con.stop()


def test_sixel_fixture_chafa_16_colours(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    replay = TermReplay(con, "127.0.0.1", 1)
    try:
        _session(con, replay)
        _send_fixture(replay, "pattern-chafa-16.sixel")
        coords = [(160 + x, y) for y in range(0, 72, 8) for x in range(0, 72, 8)]
        cols = con.screen_pixels(coords)
        distinct = {c for c in cols if c != (0, 0, 0)}
        assert len(distinct) >= 3, sorted(distinct)[:10]
        _quit(con)
        assert con.send_line("PRINT 15+15") == "30"
    finally:
        replay.stop()
        con.stop()


def test_sixel_fixture_img2sixel_256_colours(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    replay = TermReplay(con, "127.0.0.1", 1)
    try:
        _session(con, replay)
        _send_fixture(replay, "photo-img2sixel-p256.sixel")
        coords = [(160 + x, y) for y in range(0, 32) for x in range(0, 32)]
        cols = con.screen_pixels(coords)
        distinct = {c for c in cols if c != (0, 0, 0)}
        assert len(distinct) > 16, sorted(distinct)[:10]
        _quit(con)
        assert con.send_line("PRINT 16+16") == "32"
    finally:
        replay.stop()
        con.stop()


def test_sixel_fixture_lsix_montage(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    replay = TermReplay(con, "127.0.0.1", 1)
    try:
        _session(con, replay)
        _send_fixture(replay, "two-lsix.sixel")
        coords = [(x, y) for y in range(0, 136, 4) for x in range(160, 640, 4)]
        cols = con.screen_pixels(coords)
        distinct = {c for c in cols if c != (0, 0, 0)}
        assert len(distinct) >= 5, sorted(distinct)[:10]
        assert any(_is_red(c) for c in cols), "lsix montage did not render"
        _quit(con)
        assert con.send_line("PRINT 17+17") == "34"
    finally:
        replay.stop()
        con.stop()


@pytest.mark.skipif(
    os.environ.get("MMCORE_LIVE_NET") != "1",
    reason="set MMCORE_LIVE_NET=1 to run live img2sixel/chafa/lsix",
)
def test_sixel_live_tools(kernel_image):
    for tool in ("img2sixel", "chafa", "lsix"):
        if shutil.which(tool) is None:
            pytest.skip(f"{tool} not installed")

    red = subprocess.run(
        [
            "img2sixel", "-p", "16", "-w", "16", "-h", "16",
            os.path.join(FIXTURES, "red.png"),
        ],
        check=True, capture_output=True,
    ).stdout
    pattern = subprocess.run(
        [
            "chafa", "--format=sixels", "--colors=16", "--size=8x8",
            "--view-size=8x8", os.path.join(FIXTURES, "pattern.png"),
        ],
        check=True, capture_output=True,
    ).stdout
    lsix = subprocess.run(
        ["lsix", os.path.join(FIXTURES, "red.png"),
         os.path.join(FIXTURES, "pattern.png")],
        check=True, capture_output=True, stdin=subprocess.DEVNULL,
        env={**os.environ, "LSIX_FORCE_SIXEL_SUPPORT": "1"},
        timeout=30,
    ).stdout
    assert b"\x1bP" in red and b"\x1bP" in pattern and b"\x1bP" in lsix

    con = MMBasicConsole(kernel_image)
    con.start()
    replay = TermReplay(con, "127.0.0.1", 1)
    try:
        _session(con, replay)
        replay._to_guest(b"\x1b[1;1H" + red, chunk=24, delay=0.01)
        time.sleep(1.0)
        replay.pump_once(recv_tcp=False)
        assert _red_points(con, [(160, 0), (168, 4)])
        replay._to_guest(b"\x1b[1;1H" + pattern, chunk=24, delay=0.01)
        time.sleep(1.2)
        replay.pump_once(recv_tcp=False)
        cols = con.screen_pixels(
            [(160 + x, y) for y in range(0, 64, 8) for x in range(0, 64, 8)]
        )
        assert len({c for c in cols if c != (0, 0, 0)}) >= 3
        replay._to_guest(b"\x1b[1;1H" + lsix, chunk=24, delay=0.01)
        time.sleep(1.2)
        replay.pump_once(recv_tcp=False)
        assert any(
            c != (0, 0, 0)
            for c in con.screen_pixels([(x, y) for y in (0, 8) for x in (160, 300)])
        )
        _quit(con)
        assert con.send_line("PRINT 18+18") == "36"
    finally:
        replay.stop()
        con.stop()
