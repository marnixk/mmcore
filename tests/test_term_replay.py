"""TERM replay host shuttles a live TCP service through QEMU serial."""

import re
import socket
import threading
import time

from harness import MMBasicConsole, TermReplay
from test_term import _plain, _quit, _pane_grey_hits


def _open_replay(con, replay, timeout=8.0, connect=True):
    con.drain(quiet=0.1)
    con._ser.sendall(b'TERM "replay", 23\r')
    deadline = time.time() + timeout
    acc = b""
    while time.time() < deadline:
        acc += replay.pump_once(recv_tcp=False)
        if b"Connected" in acc:
            break
        time.sleep(0.02)
    if connect:
        replay.connect()
    for _ in range(40):
        acc += replay.pump_once()
        time.sleep(0.03)
    return _plain(acc.decode(errors="replace"))


def _echo_server(banner: bytes):
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", 0))
    srv.listen(1)
    port = srv.getsockname()[1]
    held = {"sock": None}

    def run():
        try:
            srv.settimeout(20)
            c, _ = srv.accept()
            held["sock"] = c
            c.settimeout(0.2)
            c.sendall(banner)
            while True:
                try:
                    n = c.recv(64)
                except socket.timeout:
                    continue
                except OSError:
                    break
                if not n:
                    break
                c.sendall(n)
        finally:
            srv.close()

    th = threading.Thread(target=run, daemon=True)
    th.start()
    return port, held, th


def test_term_replay_local_banner_and_echo(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    replay = TermReplay(con, "127.0.0.1", 0)
    try:
        port, held, _th = _echo_server(b"HELLO-REPLAY\r\n")
        replay.port = port
        replay.host = "127.0.0.1"
        seen = _open_replay(con, replay, connect=True)
        more = replay.wait_serial(lambda s: "HELLO-REPLAY" in s, timeout=6.0)
        text = seen + more
        assert "HELLO-REPLAY" in text
        replay.send_keys(b"Z")
        echoed = replay.wait_serial(lambda s: "Z" in s, timeout=4.0)
        assert "Z" in echoed
        _quit(con)
        assert con.send_line("PRINT 2+2") == "4"
    finally:
        replay.stop()
        if held.get("sock"):
            try:
                held["sock"].close()
            except OSError:
                pass
        con.stop()


def test_term_replay_charset_esc_b_not_printed(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    replay = TermReplay(con, "127.0.0.1", 1)
    try:
        seen = _open_replay(con, replay, connect=False)
        replay._to_guest(b"\x1b(BKEEP")
        time.sleep(0.2)
        more = replay.wait_serial(lambda s: "KEEP" in s, timeout=4.0)
        text = seen + more
        assert "KEEP" in text
        assert "BKEEP" not in text
        _quit(con)
        assert con.send_line("PRINT 1") == "1"
    finally:
        replay.stop()
        con.stop()


def test_term_replay_csi_split_across_frames(kernel_image):
    """ESC then CSI payload in the next UART frame must still erase, not print '['."""
    con = MMBasicConsole(kernel_image)
    con.start()
    replay = TermReplay(con, "127.0.0.1", 1)
    try:
        seen = _open_replay(con, replay, connect=False)
        replay._to_guest(b"NOISE")
        replay._to_guest(b"\x1b")
        replay._to_guest(b"[2J\x1b[1;1HSPLITOK")
        more = replay.wait_serial(lambda s: "SPLITOK" in s, timeout=6.0)
        text = _plain(seen + more)
        assert "SPLITOK" in text
        assert "[2J" not in text
        _quit(con)
        assert con.send_line("PRINT 3+3") == "6"
    finally:
        replay.stop()
        con.stop()


def test_term_replay_remote_close_reports_reason(kernel_image):
    """A hangup renders the pending text, then names the reason on its own line."""
    con = MMBasicConsole(kernel_image)
    con.start()
    replay = TermReplay(con, "127.0.0.1", 1)
    try:
        seen = _open_replay(con, replay, connect=False)
        replay._to_guest(b"Enter your ")
        replay.wait_serial(lambda s: "Enter your" in s, timeout=6.0)
        replay.close_from_host("reset by peer")
        more = replay.wait_serial(
            lambda s: "\nConnection closed: reset by peer" in _plain(s), timeout=6.0
        )
        text = _plain(seen + more)
        assert "!NET Connection closed: reset by peer" in text
        assert re.search(r"Enter your +\r?\n\r?Connection closed: reset by peer", text), text
        _quit(con)
        assert con.send_line("PRINT 6+1") == "7"
    finally:
        replay.stop()
        con.stop()


def test_term_replay_iac_split_across_frames(kernel_image):
    """IAC at end of a chunk is held; WILL ECHO in the next chunk is not a glyph."""
    con = MMBasicConsole(kernel_image)
    con.start()
    replay = TermReplay(con, "127.0.0.1", 1)
    try:
        seen = _open_replay(con, replay, connect=False)
        replay._to_guest(bytes([255]))
        replay._to_guest(bytes([251, 1]) + b"AFTERIAC")
        more = replay.wait_serial(lambda s: "AFTERIAC" in s, timeout=6.0)
        text = seen + more
        assert "AFTERIAC" in text
        _quit(con)
        assert con.send_line("PRINT 4+1") == "5"
    finally:
        replay.stop()
        con.stop()


def test_term_replay_cursor_on_then_host_hides(kernel_image):
    """Cursor is on at connect; CSI ?25l from the host turns it off."""
    con = MMBasicConsole(kernel_image)
    con.start()
    replay = TermReplay(con, "127.0.0.1", 1)
    try:
        _open_replay(con, replay, connect=False)
        on_hits = _pane_grey_hits(con, 0, 1)
        con.capture_png("/opt/cursor/artifacts/term_cursor_on_connect.png")
        assert on_hits >= 12, on_hits
        replay._to_guest(b"\x1b[?25l")
        time.sleep(0.25)
        replay.pump_once(recv_tcp=False)
        time.sleep(0.15)
        off_hits = _pane_grey_hits(con, 0, 1)
        con.capture_png("/opt/cursor/artifacts/term_cursor_hidden_by_host.png")
        assert off_hits <= 2, off_hits
        replay._to_guest(b"\x1b[?25h")
        time.sleep(0.25)
        replay.pump_once(recv_tcp=False)
        time.sleep(0.15)
        shown = _pane_grey_hits(con, 0, 1)
        con.capture_png("/opt/cursor/artifacts/term_cursor_shown_by_host.png")
        assert shown >= 12, shown
        _quit(con)
        assert con.send_line("PRINT 5") == "5"
    finally:
        replay.stop()
        con.stop()


def _is_vga_cyan(r: int, g: int, b: int) -> bool:
    return g > 80 and b > 80 and g > r + 20 and b > r + 20


def test_term_replay_newline_burst_paints_latest_line(kernel_image):
    """A burst of newlines must redraw from cells, keeping the last line on HDMI."""
    con = MMBasicConsole(kernel_image)
    con.start()
    replay = TermReplay(con, "127.0.0.1", 1)
    try:
        _open_replay(con, replay, connect=False)
        burst = b"\x1b[?25l"
        for i in range(80):
            burst += f"FILL{i:03d}\r\n".encode()
        burst += b"\x1b[1;36mCYANMARK"
        replay._to_guest(burst)
        more = replay.wait_serial(lambda s: "CYANMARK" in s, timeout=10.0)
        assert "CYANMARK" in more
        time.sleep(0.3)
        found_cyan = False
        for x in (164, 172, 180, 188, 196, 204):
            for y in range(400, 520, 8):
                if _is_vga_cyan(*con.screen_pixel(x, y)):
                    found_cyan = True
                    break
            if found_cyan:
                break
        con.capture_png("/opt/cursor/artifacts/term_newline_burst_cyanmark.png")
        assert found_cyan, "expected cyan last line after a newline burst"
        _quit(con)
        assert con.send_line("PRINT 8") == "8"
    finally:
        replay.stop()
        con.stop()


def _is_blue(rgb) -> bool:
    r, g, b = rgb
    return b > 100 and b > r + 40 and b > g + 40


def _blue_count(con, row: int, cols, pane_left: int = 20) -> int:
    coords = [((pane_left + c) * 8 + 4, row * 16 + 8) for c in cols]
    return sum(1 for rgb in con.screen_pixels(coords) if _is_blue(rgb))


def _outbound(replay) -> bytes:
    return b"".join(p for k, _t, p in list(replay.log) if k == "out")


def _outbound_until(replay, pred, timeout: float = 6.0) -> bytes:
    """Pump until ``pred(out)`` holds, or timeout; return the outbound bytes."""
    deadline = time.time() + timeout
    out = _outbound(replay)
    while time.time() < deadline:
        replay.pump_once(recv_tcp=False)
        out = _outbound(replay)
        if pred(out):
            return out
        time.sleep(0.02)
    return out


def test_term_replay_ctrl_a_forwarded_to_remote(kernel_image):
    """#1218: Ctrl+A is screen's prefix and must reach the remote.

    Over a connected session Ctrl+A (byte 1) is forwarded instead of opening
    TERM's menu; a real USB Alt still takes the local menu/exit path.
    """
    con = MMBasicConsole(kernel_image)
    con.start()
    replay = TermReplay(con, "127.0.0.1", 1)
    try:
        _open_replay(con, replay, connect=False)
        # Ask for SGA so typed bytes are forwarded immediately (raw mode).
        base = _outbound(replay).count(bytes([255, 253, 3]))
        replay._to_guest(bytes([255, 251, 3]))  # IAC WILL SGA
        # Wait for this request's answer so the guest is in char mode before
        # we type; otherwise the command key would be line-buffered.
        _outbound_until(
            replay, lambda out: out.count(bytes([255, 253, 3])) > base
        )

        serial = ""
        replay.send_keys(b"\x01c")
        deadline = time.time() + 6.0
        while time.time() < deadline:
            serial += replay.pump_once(recv_tcp=False).decode(errors="replace")
            out = _outbound(replay)
            if b"\x01" in out and b"c" in out:
                break
            time.sleep(0.02)
        out = _outbound(replay)
        assert b"\x01" in out, out
        assert b"c" in out, out
        assert "Bookmarks" not in _plain(serial), "Ctrl+A must not open the menu"

        _quit(con)
        assert con.send_line("PRINT 2+2") == "4"
    finally:
        replay.stop()
        con.stop()


def _red_count(con, row: int, cols, pane_left: int = 20) -> int:
    coords = [((pane_left + c) * 8 + 4, row * 16 + 8) for c in cols]
    return sum(1 for rgb in con.screen_pixels(coords) if _is_red(rgb))


def _is_red(rgb) -> bool:
    r, g, b = rgb
    return r > 100 and r > g + 40 and r > b + 40


def _replay_settle(replay):
    time.sleep(0.35)
    replay.pump_once(recv_tcp=False)
    time.sleep(0.15)


def test_term_replay_il_dl_shift_lines(kernel_image):
    """#1223: IL (CSI L) and DL (CSI M) insert/delete lines in the pane."""
    con = MMBasicConsole(kernel_image)
    con.start()
    replay = TermReplay(con, "127.0.0.1", 1)
    try:
        _open_replay(con, replay, connect=False)
        all_cols = list(range(0, 80, 4))

        # A blue row 1, then reset to the default pen.
        replay._to_guest(b"\x1b[0m\x1b[2J")
        _replay_settle(replay)
        replay._to_guest(b"\x1b[?25l\x1b[2;1H\x1b[44m\x1b[K\x1b[0m")
        _replay_settle(replay)
        assert _blue_count(con, 1, all_cols) == len(all_cols), "setup blue row 1"

        # IL 1 at row 0 pushes the blue row down to row 2, blanks rows 0-1.
        replay._to_guest(b"\x1b[1;1H\x1b[1L")
        _replay_settle(replay)
        assert _blue_count(con, 2, all_cols) == len(all_cols), "IL must shift down"
        assert _blue_count(con, 0, all_cols) == 0, "IL inserts a blank line"
        assert _blue_count(con, 1, all_cols) == 0, "IL inserts a blank line"
        con.capture_png("/opt/cursor/artifacts/term_csi_il.png")

        # DL 1 at row 0 pulls the blue row up to row 0; row 1 becomes blank.
        replay._to_guest(b"\x1b[0m\x1b[2J")
        _replay_settle(replay)
        replay._to_guest(b"\x1b[?25l\x1b[2;1H\x1b[44m\x1b[K\x1b[0m")
        _replay_settle(replay)
        replay._to_guest(b"\x1b[1;1H\x1b[1M")
        _replay_settle(replay)
        assert _blue_count(con, 0, all_cols) == len(all_cols), "DL must shift up"
        assert _blue_count(con, 1, all_cols) == 0, "DL blanks the freed row"
        con.capture_png("/opt/cursor/artifacts/term_csi_dl.png")

        _quit(con)
        assert con.send_line("PRINT 2+2") == "4"
    finally:
        replay.stop()
        con.stop()


def test_term_replay_ich_dch_edit_row(kernel_image):
    """#1223: ICH (CSI @) and DCH (CSI P) shift row cells; inserted cells BCE."""
    con = MMBasicConsole(kernel_image)
    con.start()
    replay = TermReplay(con, "127.0.0.1", 1)
    try:
        _open_replay(con, replay, connect=False)

        # ICH 2 at col 0: blue cols 2-3 shift right to 4-5, insertion is red.
        replay._to_guest(b"\x1b[0m\x1b[2J")
        _replay_settle(replay)
        replay._to_guest(b"\x1b[?25l\x1b[1;3H\x1b[44m  \x1b[0m")
        _replay_settle(replay)
        assert _blue_count(con, 0, [2, 3]) == 2, "setup blue cols 2-3"
        replay._to_guest(b"\x1b[1;1H\x1b[41m\x1b[2@")
        _replay_settle(replay)
        assert _red_count(con, 0, [0, 1]) == 2, "ICH inserts BCE cells"
        assert _blue_count(con, 0, [4, 5]) == 2, "ICH shifts content right"
        assert _blue_count(con, 0, [2, 3]) == 0, "ICH clears the insertion point"
        con.capture_png("/opt/cursor/artifacts/term_csi_ich.png")

        # DCH 2 at col 0: blue cols 2-3 pull left to 0-1, row tail turns red.
        replay._to_guest(b"\x1b[0m\x1b[2J")
        _replay_settle(replay)
        replay._to_guest(b"\x1b[?25l\x1b[1;3H\x1b[44m  \x1b[0m")
        _replay_settle(replay)
        replay._to_guest(b"\x1b[1;1H\x1b[41m\x1b[2P")
        _replay_settle(replay)
        assert _blue_count(con, 0, [0, 1]) == 2, "DCH pulls content left"
        assert _blue_count(con, 0, [2, 3]) == 0, "DCH clears the vacated cells"
        assert _red_count(con, 0, [78, 79]) == 2, "DCH blanks the tail with BCE"
        con.capture_png("/opt/cursor/artifacts/term_csi_dch.png")

        _quit(con)
        assert con.send_line("PRINT 3+3") == "6"
    finally:
        replay.stop()
        con.stop()


def test_term_replay_rep_repeats_last_char(kernel_image):
    """#1223: REP (CSI b) repeats the preceding graphic character."""
    con = MMBasicConsole(kernel_image)
    con.start()
    replay = TermReplay(con, "127.0.0.1", 1)
    try:
        _open_replay(con, replay, connect=False)
        replay._to_guest(b"\x1b[0m\x1b[2J")
        _replay_settle(replay)
        replay._to_guest(b"\x1b[?25l\x1b[1;1HH\x1b[5b")
        _replay_settle(replay)
        for col in range(0, 6):
            assert _pane_grey_hits(con, col, 0) > 0, f"REP should fill col {col}"
        assert _pane_grey_hits(con, 6, 0) == 0, "REP must stop after n repeats"
        con.capture_png("/opt/cursor/artifacts/term_csi_rep.png")

        _quit(con)
        assert con.send_line("PRINT 4+4") == "8"
    finally:
        replay.stop()
        con.stop()


def test_term_replay_bce_erases_with_background(kernel_image):
    """#1219: EL/ECH/ED erase with the current SGR background (BCE)."""
    con = MMBasicConsole(kernel_image)
    con.start()
    replay = TermReplay(con, "127.0.0.1", 1)
    try:
        _open_replay(con, replay, connect=False)
        all_cols = list(range(0, 80, 4))

        def settle():
            time.sleep(0.35)
            replay.pump_once(recv_tcp=False)
            time.sleep(0.15)

        # EL (ESC[K): fill row 0 from column 0 with the blue background.
        replay._to_guest(b"\x1b[?25l\x1b[1;1H\x1b[44m\x1b[K")
        settle()
        assert _blue_count(con, 0, all_cols) == len(all_cols), "EL must BCE"
        con.capture_png("/opt/cursor/artifacts/term_bce_el.png")

        # ECH (ESC[10X): erased cells keep the background, later cells do not.
        replay._to_guest(b"\x1b[0m\x1b[2J")
        settle()
        replay._to_guest(b"\x1b[?25l\x1b[1;1H\x1b[44m\x1b[10X")
        settle()
        assert _blue_count(con, 0, list(range(0, 10))) == 10, "ECH must BCE"
        assert _blue_count(con, 0, list(range(12, 80, 4))) == 0, "ECH must stop"

        # ED 2 (ESC[2J): the whole screen takes the background.
        replay._to_guest(b"\x1b[0m\x1b[44m\x1b[2J")
        settle()
        assert _blue_count(con, 0, all_cols) == len(all_cols), "ED must BCE"
        assert _blue_count(con, 3, all_cols) == len(all_cols), "ED rows below must BCE"
        con.capture_png("/opt/cursor/artifacts/term_bce_ed.png")

        _quit(con)
        assert con.send_line("PRINT 6*7") == "42"
    finally:
        replay.stop()
        con.stop()


def _is_green(rgb) -> bool:
    r, g, b = rgb
    return g > 100 and g > r + 40 and g > b + 40


def _green_count(con, row: int, cols, pane_left: int = 20) -> int:
    coords = [((pane_left + c) * 8 + 4, row * 16 + 8) for c in cols]
    return sum(1 for rgb in con.screen_pixels(coords) if _is_green(rgb))


def _decstbm_paint(replay):
    """Paint row 1 blue (above region), rows 2..4 red/green/blue, row 5 blue."""
    replay._to_guest(b"\x1b[0m\x1b[2J")
    _replay_settle(replay)
    replay._to_guest(
        b"\x1b[?25l"
        b"\x1b[1;1H\x1b[44m\x1b[K"
        b"\x1b[2;1H\x1b[41m\x1b[K"
        b"\x1b[3;1H\x1b[42m\x1b[K"
        b"\x1b[4;1H\x1b[44m\x1b[K"
        b"\x1b[5;1H\x1b[44m\x1b[K"
        b"\x1b[0m"
    )
    _replay_settle(replay)


def test_term_replay_decstbm_il_dl_respect_region(kernel_image):
    """#1225: IL/DL are bounded by the DECSTBM bottom margin."""
    con = MMBasicConsole(kernel_image)
    con.start()
    replay = TermReplay(con, "127.0.0.1", 1)
    try:
        _open_replay(con, replay, connect=False)
        all_cols = list(range(0, 80, 4))

        # Region rows 2..4.  IL at row 2 blanks row 2 and shifts red row 2 -> 3
        # and green row 3 -> 4; blue at row 5 (below the margin) must not move.
        _decstbm_paint(replay)
        replay._to_guest(b"\x1b[2;4r\x1b[2;1H\x1b[1L")
        _replay_settle(replay)
        assert _red_count(con, 1, all_cols) == 0, "IL blanks the cursor row"
        assert _red_count(con, 2, all_cols) == len(all_cols), "IL shifts red down"
        assert _green_count(con, 3, all_cols) == len(all_cols), "IL shifts green down"
        assert _blue_count(con, 0, all_cols) == len(all_cols), "row above region fixed"
        assert _blue_count(con, 4, all_cols) == len(all_cols), "row below margin fixed"
        con.capture_png("/opt/cursor/artifacts/term_csi_decstbm_il.png")

        # DL at row 2 pulls green -> row 2 and blue -> row 3, blanks row 4.
        _decstbm_paint(replay)
        replay._to_guest(b"\x1b[2;4r\x1b[2;1H\x1b[1M")
        _replay_settle(replay)
        assert _green_count(con, 1, all_cols) == len(all_cols), "DL pulls green up"
        assert _blue_count(con, 2, all_cols) == len(all_cols), "DL pulls blue up"
        assert _red_count(con, 1, all_cols) == 0, "DL removes red"
        assert _green_count(con, 3, all_cols) == 0, "DL blanks the freed row"
        assert _blue_count(con, 0, all_cols) == len(all_cols), "row above region fixed"
        assert _blue_count(con, 4, all_cols) == len(all_cols), "row below margin fixed"
        con.capture_png("/opt/cursor/artifacts/term_csi_decstbm_dl.png")

        _quit(con)
        assert con.send_line("PRINT 2+2") == "4"
    finally:
        replay.stop()
        con.stop()


def test_term_replay_decstbm_su_sd_respect_region(kernel_image):
    """#1225: SU/SD scroll only the DECSTBM region."""
    con = MMBasicConsole(kernel_image)
    con.start()
    replay = TermReplay(con, "127.0.0.1", 1)
    try:
        _open_replay(con, replay, connect=False)
        all_cols = list(range(0, 80, 4))

        # SU 1 on rows 2..4 lifts green -> 2 and blue -> 3, blanking row 4.
        _decstbm_paint(replay)
        replay._to_guest(b"\x1b[2;4r\x1b[1S")
        _replay_settle(replay)
        assert _green_count(con, 1, all_cols) == len(all_cols), "SU lifts green up"
        assert _blue_count(con, 2, all_cols) == len(all_cols), "SU lifts blue up"
        assert _red_count(con, 1, all_cols) == 0, "SU drops red"
        assert _blue_count(con, 3, all_cols) == 0, "SU blanks the region bottom"
        assert _blue_count(con, 0, all_cols) == len(all_cols), "SU leaves row above"
        assert _blue_count(con, 4, all_cols) == len(all_cols), "SU leaves row below"
        con.capture_png("/opt/cursor/artifacts/term_csi_decstbm_su.png")

        # SD 1 on rows 2..4 drops red -> 3 and green -> 4, blanking row 2.
        _decstbm_paint(replay)
        replay._to_guest(b"\x1b[2;4r\x1b[1T")
        _replay_settle(replay)
        assert _red_count(con, 2, all_cols) == len(all_cols), "SD drops red down"
        assert _green_count(con, 3, all_cols) == len(all_cols), "SD drops green down"
        assert _red_count(con, 1, all_cols) == 0, "SD blanks the region top"
        assert _green_count(con, 1, all_cols) == 0, "SD blanks the region top"
        assert _blue_count(con, 0, all_cols) == len(all_cols), "SD leaves row above"
        assert _blue_count(con, 4, all_cols) == len(all_cols), "SD leaves row below"
        con.capture_png("/opt/cursor/artifacts/term_csi_decstbm_sd.png")

        _quit(con)
        assert con.send_line("PRINT 3+3") == "6"
    finally:
        replay.stop()
        con.stop()


def test_term_replay_decstbm_reset_to_full_pane(kernel_image):
    """#1225: CSI r resets the region to the whole pane."""
    con = MMBasicConsole(kernel_image)
    con.start()
    replay = TermReplay(con, "127.0.0.1", 1)
    try:
        _open_replay(con, replay, connect=False)
        all_cols = list(range(0, 80, 4))

        replay._to_guest(b"\x1b[0m\x1b[2J")
        _replay_settle(replay)
        replay._to_guest(
            b"\x1b[?25l"
            b"\x1b[1;1H\x1b[44m\x1b[K"
            b"\x1b[3;1H\x1b[41m\x1b[K"
            b"\x1b[0m"
        )
        _replay_settle(replay)
        # Set a sub-region, then reset it; IL at row 1 must now push the red
        # row 3 down to row 4 across the whole pane.
        replay._to_guest(b"\x1b[2;4r\x1b[r\x1b[1;1H\x1b[1L")
        _replay_settle(replay)
        assert _blue_count(con, 0, all_cols) == 0, "blue shifted off row 1"
        assert _blue_count(con, 1, all_cols) == len(all_cols), "blue moves to row 2"
        assert _red_count(con, 2, all_cols) == 0, "region reset before IL"
        assert _red_count(con, 3, all_cols) == len(all_cols), "red shifts to row 4"
        con.capture_png("/opt/cursor/artifacts/term_csi_decstbm_reset.png")

        _quit(con)
        assert con.send_line("PRINT 4+4") == "8"
    finally:
        replay.stop()
        con.stop()


def test_term_replay_decstbm_homes_cursor(kernel_image):
    """#1225: CSI r moves the cursor to home (xterm/DEC behaviour)."""
    con = MMBasicConsole(kernel_image)
    con.start()
    replay = TermReplay(con, "127.0.0.1", 1)
    try:
        _open_replay(con, replay, connect=False)
        replay._to_guest(b"\x1b[0m\x1b[2J")
        _replay_settle(replay)
        replay._to_guest(b"\x1b[?25l\x1b[3;5H\x1b[2;4rX")
        _replay_settle(replay)
        assert _pane_grey_hits(con, 0, 0) > 0, "DECSTBM homes the cursor"
        assert _pane_grey_hits(con, 4, 2) == 0, "old cursor column stays empty"
        con.capture_png("/opt/cursor/artifacts/term_csi_decstbm_home.png")

        _quit(con)
        assert con.send_line("PRINT 5+5") == "10"
    finally:
        replay.stop()
        con.stop()
