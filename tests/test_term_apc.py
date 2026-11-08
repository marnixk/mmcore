"""TERM SyncTERM APC image verbs (#1464).

The APC framing, cache, capability replies and Store+Draw path are exercised
through the QEMU replay harness.  The heavy decoder vectors live in
tests/test_jxl_decode.py; these tests only check that a real session draws.
"""

import base64
import hashlib
import os
import re
import time

from harness import MMBasicConsole, TermReplay
from test_term import _plain, _quit
from test_term_replay import _open_replay

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FIX = os.path.join(REPO, "tests", "term", "jxl")


def _fixture(name: str) -> bytes:
    with open(os.path.join(FIX, name), "rb") as f:
        return f.read()


def _b64(data: bytes) -> bytes:
    return base64.b64encode(data)


def _session(con, replay):
    seen = _open_replay(con, replay, connect=False)
    return _plain(seen)


def _wait_out(replay, needle: bytes, timeout=4.0) -> bytes:
    deadline = time.time() + timeout
    while time.time() < deadline:
        replay.pump_once(recv_tcp=False)
        if needle in replay.pending_out:
            return replay.pending_out
        time.sleep(0.02)
    return replay.pending_out


def _is_red(rgb) -> bool:
    r, g, b = rgb
    return r > 170 and g < 90 and b < 90


def _is_blue(rgb) -> bool:
    r, g, b = rgb
    return b > 170 and r < 90 and g < 90


def _is_grey(rgb) -> bool:
    r, g, b = rgb
    return r > 100 and g > 100 and b > 100 and abs(r - g) < 40 and abs(g - b) < 40


RED_PPM = b"P6\n8 8\n255\n" + bytes([255, 0, 0]) * 64
BLUE_PPM = b"P6\n8 8\n255\n" + bytes([0, 0, 255]) * 64
RED_PPM_16 = b"P6\n16 8\n255\n" + bytes([255, 0, 0]) * 128


def _apc(cmd: bytes) -> bytes:
    return b"\x1b_SyncTERM:" + cmd + b"\x1b\\"


def _pbm_p4(width: int, rows) -> bytes:
    """Binary PBM: each row is a bytes object (1 = black = draw)."""
    return b"P4\n%d %d\n" % (width, len(rows)) + b"".join(rows)


# 8-wide mask with the left half set (draw) and right half clear.
HALF8 = bytes([0xF0]) * 8
# 16-wide mask: first cell drawn, second cell transparent (glyph shows).
LEFT_CELL16 = bytes([0xFF, 0x00]) * 8


def _apc_image_paste(opts: bytes) -> bytes:
    return _apc(b"P;Paste;" + opts)


def _plain_hits(con, coords):
    return con.screen_pixels(coords)


def test_paste_inline_mask(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    replay = TermReplay(con, "127.0.0.1", 1)
    try:
        _session(con, replay)
        replay._to_guest(_apc(b"C;LoadPPMBlob;B=0;" + _b64(RED_PPM)))
        replay._to_guest(
            _apc_image_paste(
                b"B=0;DX=0;DY=0;MW=8;MH=8;MASK=" + _b64(HALF8)
            )
        )
        time.sleep(0.5)
        replay.pump_once(recv_tcp=False)
        hits = _plain_hits(con, [(160, 2), (163, 6), (166, 2), (174, 5)])
        assert _is_red(hits[0]) and _is_red(hits[1]), hits
        assert not _is_red(hits[2]) and not _is_red(hits[3]), hits
        replay._to_guest(b"AFTERMASK")
        more = replay.wait_serial(lambda s: "AFTERMASK" in _plain(s), timeout=8.0)
        assert "AFTERMASK" in _plain(more)
        _quit(con)
        assert con.send_line("PRINT 12+12") == "24"
    finally:
        replay.stop()
        con.stop()


def test_cache_load_and_mfile(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    replay = TermReplay(con, "127.0.0.1", 1)
    try:
        _session(con, replay)
        replay._to_guest(_apc(b"C;S;red.ppm;" + _b64(RED_PPM)))
        replay._to_guest(_apc(b"C;S;half.pbm;" + _b64(_pbm_p4(8, [HALF8[:1]] * 8))))
        replay._to_guest(_apc(b"C;LoadPPM;B=1;red.ppm"))
        replay._to_guest(
            _apc_image_paste(
                b"B=1;DX=64;DY=0;MFILE=half.pbm;MX=0;MY=0"
            )
        )
        time.sleep(0.5)
        replay.pump_once(recv_tcp=False)
        hits = _plain_hits(con, [(224, 2), (227, 6), (229, 2), (231, 5)])
        assert _is_red(hits[0]) and _is_red(hits[1]), hits
        assert not _is_red(hits[2]) and not _is_red(hits[3]), hits
        _quit(con)
        assert con.send_line("PRINT 13+13") == "26"
    finally:
        replay.stop()
        con.stop()


def test_load_jxl_cache_and_blob(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    replay = TermReplay(con, "127.0.0.1", 1)
    try:
        _session(con, replay)
        jxl = _fixture("red.jxl")
        replay._to_guest(_apc(b"C;S;red.jxl;" + _b64(jxl)))
        replay._to_guest(_apc(b"C;LoadJXL;B=0;red.jxl"))
        replay._to_guest(_apc_image_paste(b"B=0;DX=0;DY=0"))
        replay._to_guest(_apc(b"C;LoadJXLBlob;B=1;" + _b64(jxl)))
        replay._to_guest(_apc_image_paste(b"B=1;DX=64;DY=0"))
        time.sleep(0.5)
        replay.pump_once(recv_tcp=False)
        hits = _plain_hits(con, [(160, 2), (167, 6), (224, 2), (231, 6)])
        assert all(_is_red(p) for p in hits), hits
        _quit(con)
        assert con.send_line("PRINT 14+14") == "28"
    finally:
        replay.stop()
        con.stop()


def test_copy_and_paste_buffer(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    replay = TermReplay(con, "127.0.0.1", 1)
    try:
        _session(con, replay)
        replay._to_guest(
            _apc(b"C;DrawPPMBlob;DX=0;DY=0;" + _b64(RED_PPM))
        )
        replay._to_guest(_apc(b"P;Copy;B=0;X=0;Y=0;W=8;H=8"))
        replay._to_guest(
            _apc(b"C;LoadPPMBlob;B=1;" + _b64(BLUE_PPM))
        )
        replay._to_guest(_apc_image_paste(b"B=1;DX=0;DY=0"))
        replay._to_guest(_apc_image_paste(b"B=0;DX=64;DY=0"))
        time.sleep(0.5)
        replay.pump_once(recv_tcp=False)
        hits = _plain_hits(con, [(160, 2), (167, 5), (224, 2), (231, 5)])
        assert _is_blue(hits[0]) and _is_blue(hits[1]), hits
        assert _is_red(hits[2]) and _is_red(hits[3]), hits
        _quit(con)
        assert con.send_line("PRINT 15+15") == "30"
    finally:
        replay.stop()
        con.stop()


def test_mask_transparent_keeps_glyph(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    replay = TermReplay(con, "127.0.0.1", 1)
    try:
        _session(con, replay)
        # Two full white-bg cells at the pane origin, then let the pane paint
        # before the masked image primes its cells.
        replay._to_guest(b"\x1b[H\x1b[47m  \x1b[0m")
        time.sleep(0.5)
        replay.pump_once(recv_tcp=False)
        replay._to_guest(
            _apc(b"C;LoadPPMBlob;B=0;" + _b64(RED_PPM_16))
        )
        replay._to_guest(
            _apc_image_paste(
                b"B=0;DX=0;DY=0;MW=16;MH=8;MASK=" + _b64(LEFT_CELL16)
            )
        )
        time.sleep(0.5)
        replay.pump_once(recv_tcp=False)
        hits = _plain_hits(con, [(160, 2), (172, 2), (172, 8)])
        assert _is_red(hits[0]), hits
        assert _is_grey(hits[1]) and _is_grey(hits[2]), hits
        _quit(con)
        assert con.send_line("PRINT 16+16") == "32"
    finally:
        replay.stop()
        con.stop()


def test_masked_syncdoom_style_replay(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    replay = TermReplay(con, "127.0.0.1", 1)
    try:
        _session(con, replay)
        mask_pbm = _pbm_p4(8, [HALF8[:1]] * 8)
        # Frame 1: red through the loaded mask buffer.
        replay._to_guest(_apc(b"C;LoadPPMBlob;B=0;" + _b64(RED_PPM)))
        replay._to_guest(_apc(b"C;LoadPBMBlob;" + _b64(mask_pbm)))
        replay._to_guest(_apc_image_paste(b"B=0;DX=0;DY=0;MBUF"))
        # Frame 2: blue through the same mask at another location.
        replay._to_guest(_apc(b"C;LoadPPMBlob;B=1;" + _b64(BLUE_PPM)))
        replay._to_guest(_apc_image_paste(b"B=1;DX=64;DY=0;MBUF"))
        time.sleep(0.5)
        replay.pump_once(recv_tcp=False)
        hits = _plain_hits(
            con,
            [(160, 2), (166, 2), (224, 2), (230, 2)],
        )
        assert _is_red(hits[0]) and not _is_red(hits[1]), hits
        assert _is_blue(hits[2]) and not _is_blue(hits[3]), hits
        # Bad verb arguments must drop silently and leave the frame alone.
        replay._to_guest(_apc(b"C;LoadPPMBlob;B=2;" + _b64(RED_PPM)))
        replay._to_guest(_apc(b"C;LoadPBMBlob;!!!!"))
        replay._to_guest(_apc_image_paste(b"B=9;DX=0;DY=0"))
        replay._to_guest(_apc_image_paste(b"B=0;DX=0;DY=0;MASK=!!!!;MW=8;MH=8"))
        replay._to_guest(b"AFTERMASK")
        more = replay.wait_serial(lambda s: "AFTERMASK" in _plain(s), timeout=8.0)
        assert "AFTERMASK" in _plain(more)
        hits = _plain_hits(con, [(160, 2), (166, 2)])
        assert _is_red(hits[0]) and not _is_red(hits[1]), hits
        _quit(con)
        assert con.send_line("PRINT 17+17") == "34"
    finally:
        replay.stop()
        con.stop()



def test_apc_payload_is_swallowed(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    replay = TermReplay(con, "127.0.0.1", 1)
    try:
        seen = _session(con, replay)
        replay._to_guest(
            b"\x1b_SyncTERM:Q;JXL\x1b\\APCOK"
            b"\x1b_NotSyncTERM:PAYLOADMARK\x1b\\AFTER"
        )
        more = replay.wait_serial(lambda s: "AFTER" in _plain(s), timeout=8.0)
        text = _plain(seen + more)
        assert "APCOK" in text and "AFTER" in text
        assert "PAYLOADMARK" not in text
        assert "Q;JXL" not in text
        _quit(con)
        assert con.send_line("PRINT 1+1") == "2"
    finally:
        replay.stop()
        con.stop()


def test_apc_split_and_can_abort(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    replay = TermReplay(con, "127.0.0.1", 1)
    try:
        seen = _session(con, replay)
        replay._to_guest(b"\x1b")
        replay._to_guest(b"_SyncTERM:C;L\x1b")
        replay._to_guest(b"\\SPLITOK")
        replay._to_guest(b"\x1b_SyncTERM:C;L\x18CANOK")
        more = replay.wait_serial(lambda s: "CANOK" in _plain(s), timeout=8.0)
        text = _plain(seen + more)
        assert "SPLITOK" in text and "CANOK" in text
        assert "SyncTERM" not in text
        _quit(con)
        assert con.send_line("PRINT 2+2") == "4"
    finally:
        replay.stop()
        con.stop()


def test_apc_invalid_byte_aborts(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    replay = TermReplay(con, "127.0.0.1", 1)
    try:
        seen = _session(con, replay)
        replay._to_guest(b"\x1b_SyncTERM:BAD\x01INVALIDOK")
        more = replay.wait_serial(lambda s: "INVALIDOK" in _plain(s), timeout=8.0)
        text = _plain(seen + more)
        assert "INVALIDOK" in text
        assert "SyncTERM" not in text
        _quit(con)
        assert con.send_line("PRINT 3+3") == "6"
    finally:
        replay.stop()
        con.stop()


def test_apc_capability_replies(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    replay = TermReplay(con, "127.0.0.1", 1)
    try:
        _session(con, replay)
        replay.pending_out = b""
        replay._to_guest(b"\x1b[c")
        assert b"[=67;84;101;114;109;1;332c" in _wait_out(replay, b"1;332c")

        replay.pending_out = b""
        replay._to_guest(b"\x1b[<c")
        assert b"[<0;4c" in _wait_out(replay, b"[<0;4c")

        replay.pending_out = b""
        replay._to_guest(b"\x1b_SyncTERM:Q;JXL\x1b\\")
        assert b"[=1;1-n" in _wait_out(replay, b"[=1;1-n")
        _quit(con)
        assert con.send_line("PRINT 4+4") == "8"
    finally:
        replay.stop()
        con.stop()


def test_cterm_identity_can_be_disabled(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    replay = TermReplay(con, "127.0.0.1", 1)
    try:
        _session(con, replay)
        replay.pending_out = b""
        replay._to_guest(b"\x1b[c")
        assert b"[=67;84;101;114;109;1;332c" in _wait_out(replay, b"332c")
        _quit(con)

        assert con.send_line("OPTION TERM CTERM OFF") == ""
        replay2 = TermReplay(con, "127.0.0.1", 1)
        try:
            _session(con, replay2)
            replay2.pending_out = b""
            replay2._to_guest(b"\x1b[c")
            assert b"?1;2;4c" in _wait_out(replay2, b"?1;2;4c")
            _quit(con)
        finally:
            replay2.stop()
        assert con.send_line("OPTION TERM CTERM ON") == ""
        assert con.send_line("PRINT 5+5") == "10"
    finally:
        replay.stop()
        con.stop()


def test_deadmodemsociety_jxl_detection(kernel_image):
    """The DOOMDoor CTerm handshake behind the deadmodemsociety JXL menu.

    The door only probes JPEG XL after DA1 returns the CTerm identity, then
    requires ESC[=1;1-n from Q;JXL and version >= 1.329 to offer the mode.
    """
    con = MMBasicConsole(kernel_image)
    con.start()
    replay = TermReplay(con, "127.0.0.1", 1)
    try:
        _session(con, replay)
        replay.pending_out = b""
        replay._to_guest(b"\x1b[c")
        assert b"\x1b[=67;84;101;114;109;1;332c" in _wait_out(replay, b"332c")

        replay.pending_out = b""
        replay._to_guest(
            b"\x1b_SyncTERM:Q;JXL\x1b\\"
            b"\x1b_SyncTERM:Q;libsndfile\x1b\\"
            b"\x1b_SyncTERM:Q;libsndfileFormat;32;96\x1b\\"
            b"\x1b_SyncTERM:Q;libsndfileFormat;1;5\x1b\\"
            b"\x1b[<0c\x1b[?2;1S\x1b[5n"
        )
        out = _wait_out(replay, b"\x1b[0n")
        assert b"\x1b[=1;1-n" in out
        assert b"\x1b[<0;4c" in out
        assert b"\x1b[?2;0;" in out
        _quit(con)
        assert con.send_line("PRINT 6+6") == "12"
    finally:
        replay.stop()
        con.stop()


def test_store_list_and_draw_ppm(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    replay = TermReplay(con, "127.0.0.1", 1)
    try:
        _session(con, replay)
        payload = _b64(RED_PPM)
        replay._to_guest(b"\x1b_SyncTERM:C;S;red.ppm;" + payload + b"\x1b\\")
        time.sleep(0.3)

        replay.pending_out = b""
        replay._to_guest(b"\x1b_SyncTERM:C;L;*.ppm\x1b\\")
        listing = _wait_out(replay, b"\x1b\\", timeout=4.0)
        assert b"SyncTERM:C;L\n" in listing
        assert b"red.ppm\t" in listing
        md5 = hashlib.md5(RED_PPM).hexdigest().encode()
        assert b"red.ppm\t" + md5 + b"\n" in listing

        replay._to_guest(
            b"\x1b_SyncTERM:C;DrawPPM;DX=0;DY=0;ZX=2;ZY=2;red.ppm\x1b\\"
        )
        time.sleep(0.5)
        replay.pump_once(recv_tcp=False)
        hits = con.screen_pixels([(160, 0), (170, 4), (175, 15)])
        assert any(_is_red(p) for p in hits), hits

        replay._to_guest(b"AFTERDRAW")
        more = replay.wait_serial(
            lambda s: "AFTERDRAW" in _plain(s), timeout=8.0
        )
        assert "AFTERDRAW" in _plain(more)
        _quit(con)
        assert con.send_line("PRINT 6+6") == "12"
    finally:
        replay.stop()
        con.stop()


def test_draw_jxl_blob_and_negative_dx(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    replay = TermReplay(con, "127.0.0.1", 1)
    try:
        _session(con, replay)
        replay._to_guest(
            b"\x1b_SyncTERM:C;DrawJXLBlob;DX=-4;DY=-4;ZX=2;ZY=2;"
            + _b64(_fixture("red.jxl"))
            + b"\x1b\\"
        )
        time.sleep(0.6)
        replay.pump_once(recv_tcp=False)
        hits = con.screen_pixels([(160, 0), (168, 8), (175, 7)])
        assert any(_is_red(p) for p in hits), hits
        _quit(con)
        assert con.send_line("PRINT 7+7") == "14"
    finally:
        replay.stop()
        con.stop()


def test_bad_cache_names_are_rejected(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    replay = TermReplay(con, "127.0.0.1", 1)
    try:
        _session(con, replay)
        payload = _b64(RED_PPM)
        for name in (b"../escape.ppm", b"dir/file.ppm", b".hidden.ppm"):
            replay._to_guest(
                b"\x1b_SyncTERM:C;S;" + name + b";" + payload + b"\x1b\\"
            )
        time.sleep(0.3)
        replay.pending_out = b""
        replay._to_guest(b"\x1b_SyncTERM:C;L;*\x1b\\")
        listing = _wait_out(replay, b"\x1b\\", timeout=4.0)
        assert b"escape" not in listing
        assert b"file.ppm" not in listing
        assert b"hidden" not in listing
        _quit(con)
        assert con.send_line("PRINT 8+8") == "16"
    finally:
        replay.stop()
        con.stop()
