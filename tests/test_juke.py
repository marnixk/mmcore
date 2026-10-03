"""JUKE retro music player: visualiser, queue, and background playback."""

import subprocess
import time

from harness import MMBasicConsole
from ihelp_util import dump_topic


def _lit_fraction(con: MMBasicConsole) -> float:
    """Fraction of the framebuffer above a low brightness threshold."""
    png = con.capture_png()
    out = subprocess.run(
        [
            "convert", png, "-colorspace", "gray", "-threshold", "8%",
            "-format", "%[fx:mean]", "info:",
        ],
        check=True, capture_output=True, text=True,
    ).stdout.strip()
    return float(out)


def _peak_lit(con: MMBasicConsole, samples: int = 12, gap: float = 0.3) -> float:
    """Peak lit fraction across several captures (QEMU repaints are slow)."""
    best = 0.0
    for _ in range(samples):
        best = max(best, _lit_fraction(con))
        if best > 0.001:
            break
        time.sleep(gap)
    return best


def _open_juke(con: MMBasicConsole, arg: str, keep_mode: bool = False) -> str:
    """Open JUKE.

    Most layout tests pin the 960x540 baseline first so their geometry is
    stable; ``keep_mode=True`` instead starts JUKE in whatever mode the
    console is already in (the #1042 behaviour).
    """
    assert con._ser is not None
    if not keep_mode:
        con.send_line("MODE 12,32")
    con.drain(quiet=0.15)
    con._ser.sendall(f'JUKE "{arg}"\r'.encode())
    return con.drain(quiet=0.9, timeout=10).decode(errors="replace")


def _juke_scale(w: int, h: int) -> int:
    """Mirror JUKE's layout scale: percent of the 960x540 design (see cmd_juke.c)."""
    s = min(w * 100 // 960, h * 100 // 540)
    return max(60, min(200, s))


def _quit_juke(con: MMBasicConsole, key: bytes = b"\x1b") -> None:
    assert con._ser is not None
    con._ser.sendall(key)
    con.drain(quiet=0.7, timeout=8)


def _prep_queue(con: MMBasicConsole, copies) -> None:
    assert con.send_line('CHDIR "A:/"') == ""
    for folder in {dst.split("/")[0] for _, dst in copies}:
        con.send_line(f'MKDIR "{folder}"')
    for src, dst in copies:
        assert con.send_line(f'COPY "{src}" TO "{dst}"') == ""


def _bar_layout(w: int) -> tuple[int, int, int]:
    """Mirror JUKE's centred spectrum-bar geometry (see cmd_juke.c)."""
    gap = 3
    bw = (w - 28) // 24 - gap
    if bw < 2:
        bw = 2
    span = 24 * bw + 23 * gap
    x0 = 14 + (w - 28 - span) // 2
    return max(x0, 14), bw, gap


def _bar_centres(w: int) -> list[int]:
    x0, bw, gap = _bar_layout(w)
    return [x0 + i * (bw + gap) + bw // 2 for i in range(24)]


def _gap_after(w: int, band: int) -> int:
    """An x coordinate inside the gap between `band` and the next bar."""
    x0, bw, gap = _bar_layout(w)
    return x0 + band * (bw + gap) + bw + gap // 2


def test_help_juke(console):
    out = dump_topic(console, "JUKE")
    assert out != "?SYNTAX ERROR"
    low = out.lower()
    assert "mp3" in low
    assert "folder" in low
    assert "subfolder" in low
    assert "id3" in low
    assert "visualis" in low or "spectrum" in low
    assert "mod" in low


def test_juke_plays_mod_and_keeps_playing_after_exit(fresh_console):
    con = fresh_console
    _open_juke(con, "tests/TEST.MOD")
    assert _peak_lit(con) > 0.001
    # Quitting the UI must not stop the music (background playback).
    _quit_juke(con)
    assert con.send_line("PRINT PLAYING()") == "1"
    con.send_line("PLAY STOP")
    assert con.send_line("PRINT PLAYING()") == "0"
    assert con.send_line("PRINT 1+1") == "2"


def test_juke_plays_mp3(fresh_console):
    con = fresh_console
    _open_juke(con, "tests/TEST.MP3")
    assert _peak_lit(con) > 0.001
    _quit_juke(con)
    playing = con.send_line("PRINT PLAYING()")
    assert playing in ("0", "1")  # the seeded MP3 is very short
    con.send_line("PLAY STOP")


def test_juke_plays_s3m(fresh_console):
    con = fresh_console
    _open_juke(con, "tests/TEST.S3M")
    assert _peak_lit(con) > 0.001
    _quit_juke(con)
    assert con.send_line("PRINT PLAYING()") == "1"
    con.send_line("PLAY STOP")
    assert con.send_line("PRINT PLAYING()") == "0"


def test_juke_folder_queue_starts_a_track(fresh_console):
    con = fresh_console
    _open_juke(con, "tests")
    assert _peak_lit(con) > 0.001
    _quit_juke(con)
    # The queue starts with TEST.MOD, which loops.
    assert con.send_line("PRINT PLAYING()") == "1"
    con.send_line("PLAY STOP")


def test_juke_queue_advances_to_next_track(fresh_console):
    con = fresh_console
    _prep_queue(con, [("tests/TEST.MP3", "JQ/A.MP3"), ("tests/TEST.MOD", "JQ/B.MOD")])
    _open_juke(con, "JQ")
    time.sleep(1.6)  # A.MP3 is tiny; the queue should reach B.MOD
    _quit_juke(con)
    assert con.send_line("PRINT PLAYING()") == "1"
    con.send_line("PLAY STOP")


def test_juke_single_file_finishes(fresh_console):
    con = fresh_console
    _prep_queue(con, [("tests/TEST.MP3", "JQ1/ONLY.MP3")])
    _open_juke(con, "JQ1")
    _quit_juke(con)
    samples = []
    for _ in range(5):
        samples.append(con.send_line("PRINT PLAYING()"))
        time.sleep(0.25)
    assert "1" not in samples, samples  # single file must not loop


def test_juke_single_module_finishes_once(fresh_console):
    """#924: a MOD queue must end after one play-through, not loop forever."""
    con = fresh_console
    _prep_queue(con, [("tests/TEST.MOD", "JQM/ONLY.MOD")])
    _open_juke(con, "JQM")
    _quit_juke(con)
    for _ in range(40):  # the fixture is ~8 s; allow headroom
        if con.send_line("PRINT PLAYING()") == "0":
            break
        time.sleep(0.5)
    else:
        raise AssertionError("module queued alone must end, not loop forever")
    assert con.send_line("PRINT 1+1") == "2"


def test_juke_pause_and_resume(fresh_console):
    con = fresh_console
    _open_juke(con, "tests/TEST.MOD")
    assert con._ser is not None
    con._ser.sendall(b" ")
    con.drain(quiet=0.4)
    _quit_juke(con)
    assert con.send_line("PRINT PLAYING()") == "0"  # paused
    con.send_line("PLAY STOP")


def test_juke_not_available_in_run(fresh_console):
    con = fresh_console
    assert con.send_line('OPEN "J.BAS" FOR OUTPUT AS #1') == ""
    assert con.send_line('PRINT #1, "JUKE"') == ""
    assert con.send_line("CLOSE #1") == ""
    out = con.send_line('RUN "J.BAS"').upper()
    assert "NOT AVAILABLE IN RUN" in out, out


def _footer_row_y(con: MMBasicConsole, dy: int) -> int:
    _w, h = con.screen_size()
    return h - 48 + dy


def _shuffle_lit(con: MMBasicConsole) -> tuple[int, int, int]:
    """Colour of the shuffle chip swatch (left of the SHUF label)."""
    return con.screen_pixel(21, _footer_row_y(con, 34))


def _volume_lit(con: MMBasicConsole) -> int:
    """How many pixels of the volume bar are filled."""
    y = _footer_row_y(con, 34)
    row = con.screen_pixels([(x, y) for x in range(148, 364, 2)])
    return sum(1 for r, g, b in row if r + g + b > 150)


def test_juke_has_fixed_grey_palette(fresh_console):
    """JUKE must not follow the system theme, and must leave it alone."""
    con = fresh_console
    assert con.send_line('OPTION THEME "Snow"') == ""
    before = con.send_line('PRINT THEME("TEXT_BG")')
    _open_juke(con, "tests/TEST.MOD")
    # The canvas/backing stays near-black even under a light system theme.
    # (gap after band 12) is the space between bars, so it is never a bar.
    w, _h = con.screen_size()
    dark = [con.screen_pixel(2, 2), con.screen_pixel(480, 2),
            con.screen_pixel(_gap_after(w, 12), 300), con.screen_pixel(2, 494)]
    assert all(r + g + b < 140 for r, g, b in dark), dark

    _quit_juke(con)
    assert con.send_line('PRINT THEME("TEXT_BG")') == before


def test_juke_header_shows_graffiti_logo(fresh_console):
    """#914: the header carries the colourful graffiti wordmark, not text."""
    con = fresh_console
    _open_juke(con, "tests/TEST.MOD")
    coords = [(x, y) for x in range(12, 128, 2) for y in range(1, 50, 2)]
    colourful = 0
    for _ in range(6):
        for r, g, b in con.screen_pixels(coords):
            if max(r, g, b) - min(r, g, b) > 50 and max(r, g, b) > 90:
                colourful += 1
        if colourful > 40:
            break
        time.sleep(0.2)
    _quit_juke(con)
    assert colourful > 40, "expected the colourful graffiti logo in the header"


def test_juke_scope_lives_in_its_own_panel(fresh_console):
    """#914: the oscilloscope sits in a bordered panel under the title."""
    con = fresh_console
    _open_juke(con, "tests/TEST.MOD")
    w, h = con.screen_size()

    # Muted cool-grey frame: left, right, and the top edge including its
    # right corner.
    frame = [(8, 126), (w - 9, 126), (480, 100), (w - 9, 100)]
    for (x, y), border in zip(frame, con.screen_pixels(frame)):
        assert _is_panel_border(border), (border, x, y)

    # The interior is mostly the dark panel fill (traces only cross a few
    # pixels). The panel sits strictly above the spectrum baseline.
    base = h - 71
    assert 100 + 52 < base
    pts = [(x, y) for x in range(12, 948, 20) for y in range(103, 148, 6)]
    dark = sum(1 for c in con.screen_pixels(pts) if sum(c) < 60)
    assert dark > len(pts) * 0.6, (dark, len(pts))
    _quit_juke(con)


def test_juke_spectrum_bars_run_black_grey_lime(fresh_console):
    """#952: the per-bar ramp starts black, passes through grey, peaks lime."""
    con = fresh_console
    # TEST.WAV is a three-second tone, so the bars stay tall long enough to
    # sample the gradient (the MOD fixture is only a brief blip).
    _open_juke(con, "tests/TEST.WAV")
    _w, h = con.screen_size()
    base = h - 71
    maxh = base - 190
    xs = _bar_centres(_w)
    ylist = list(range(base - 2, base - maxh, -2))
    coords = [(x, y) for x in xs for y in ylist]
    n = len(ylist)
    saw_black = saw_grey = saw_lime = False
    for _ in range(8):
        px = con.screen_pixels(coords)
        for i in range(24):
            col = px[i * n:(i + 1) * n]
            # The bar's lowest pixels fade to the black canvas, so skip them
            # for the grey/lime checks but record the black base.
            run = [c for c in col if sum(c) > 30]
            if len(run) < 8:
                continue
            first = next(k for k, c in enumerate(col) if sum(c) > 30)
            if first > 0:
                saw_black = True
            top = run[-1]
            if top[1] > 170 and top[0] > 90 and top[2] < 150:
                saw_lime = True
            if any(max(c) - min(c) < 40 and 90 < sum(c) < 520 for c in run):
                saw_grey = True
        if saw_black and saw_grey and saw_lime:
            break
        time.sleep(0.2)
    _quit_juke(con)
    assert saw_lime, "expected a lime tip in the spectrum bars"
    assert saw_grey, "expected a grey middle in the spectrum bars"
    assert saw_black, "expected the spectrum bar base to fade to black"


def test_juke_spectrum_has_no_guide_lines(fresh_console):
    """#914 removed the midfield guide lines; the bar field is plain black."""
    con = fresh_console
    _open_juke(con, "tests/TEST.MOD")
    _w, h = con.screen_size()
    base = h - 71
    maxh = base - 190
    # The gap between bands 0 and 1 has no bar or cap.
    ys = [base - maxh * i // 4 for i in (1, 2, 3)]
    got = con.screen_pixels([(_gap_after(_w, 0), y) for y in ys])
    assert all(sum(c) < 30 for c in got), got
    _quit_juke(con)


def test_juke_shuffle_toggle_lights_chip(fresh_console):
    con = fresh_console
    _prep_queue(con, [("tests/TEST.MOD", "JS/A.MOD"),
                      ("tests/TEST.MP3", "JS/B.MP3")])
    _open_juke(con, "JS")
    off = _shuffle_lit(con)
    assert sum(off) < 200, off
    con._ser.sendall(b"r")
    con.drain(quiet=0.4)
    on = _shuffle_lit(con)
    assert sum(on) > sum(off) + 120, (off, on)
    con._ser.sendall(b"r")
    con.drain(quiet=0.4)
    back = _shuffle_lit(con)
    assert sum(back) < 200, back
    _quit_juke(con)


def test_juke_volume_keys_change_level(fresh_console):
    con = fresh_console
    _open_juke(con, "tests/TEST.MOD")
    full = _volume_lit(con)
    assert full > 60, full  # defaults to an audible 100%
    for _ in range(4):
        con._ser.sendall(b"-")
    con.drain(quiet=0.5)
    lower = _volume_lit(con)
    assert lower < full, (full, lower)
    for _ in range(4):
        con._ser.sendall(b"+")
    con.drain(quiet=0.5)
    raised = _volume_lit(con)
    assert raised > lower, (lower, raised)
    _quit_juke(con)


def test_help_juke_documents_shuffle_volume_transport(console):
    out = dump_topic(console, "JUKE")
    low = out.lower()
    assert "shuffle" in low
    assert "volume" in low
    assert "prev" in low and "next" in low
    assert "grey" in low or "gray" in low
    assert "oscilloscope" in low
    assert "playlist" in low
    assert "list" in low
    # The old bare < / > transport notation must be gone.
    assert "<  >" not in out
    assert "  <\n" not in out and "  >\n" not in out


_LIST_Y0 = 160
_LIST_ROW = 18


def _list_rows(h: int) -> int:
    y0 = _LIST_Y0
    y1 = h - 56
    if y1 < y0 + 24:
        y1 = y0 + 24
    rows = (y1 - y0 - 8) // _LIST_ROW
    return rows if rows > 0 else 1


def _list_row_y(h: int, vis: int) -> int:
    return _LIST_Y0 + 4 + vis * _LIST_ROW + 8


def _is_list_sel(rgb: tuple[int, int, int]) -> bool:
    r, g, b = rgb
    return r + g + b > 140 and abs(r - b) < 50 and g < r + 40


def _is_panel_border(rgb: tuple[int, int, int]) -> bool:
    """Muted cool grey of the JUKE panel frame (col_panel2)."""
    r, _g, b = rgb
    return max(rgb) - min(rgb) <= 16 and b >= r and 30 < r < 100 and sum(rgb) > 80


def _is_lime_mark(rgb: tuple[int, int, int]) -> bool:
    r, g, b = rgb
    return g > 140 and g > r and g > b + 20


def _keys(con: MMBasicConsole, data: bytes, quiet: float = 0.45) -> None:
    assert con._ser is not None
    con._ser.sendall(data)
    con.drain(quiet=quiet, timeout=8)


def test_juke_list_toggles_midfield(fresh_console):
    """#933: L swaps the midfield; header and footer stay put."""
    con = fresh_console
    _prep_queue(con, [("tests/TEST.MOD", "JT/A.MOD"),
                      ("tests/TEST.MOD", "JT/B.MOD")])
    _open_juke(con, "JT")
    _w, h = con.screen_size()
    y0 = _list_row_y(h, 0)
    assert not _is_list_sel(con.screen_pixel(16, y0))
    logo = con.screen_pixel(40, 20)
    shuf = _shuffle_lit(con)
    _keys(con, b"l")
    assert _is_list_sel(con.screen_pixel(16, y0)), con.screen_pixel(16, y0)
    assert con.screen_pixel(40, 20) == logo
    assert _shuffle_lit(con) == shuf
    _keys(con, b"\x1b[B")
    assert not _is_list_sel(con.screen_pixel(16, y0))
    assert _is_list_sel(con.screen_pixel(16, _list_row_y(h, 1)))
    _keys(con, b"l")
    assert not _is_list_sel(con.screen_pixel(16, y0))
    assert not _is_list_sel(con.screen_pixel(16, _list_row_y(h, 1)))
    _quit_juke(con)
    con.send_line("PLAY STOP")


def test_juke_list_keeps_right_border(fresh_console):
    """The playlist frame's right edge stays visible on rows and between them."""
    con = fresh_console
    _prep_queue(con, [("tests/TEST.MOD", "JB/A.MOD"),
                      ("tests/TEST.MOD", "JB/B.MOD")])
    _open_juke(con, "JB")
    w, h = con.screen_size()
    _keys(con, b"l")
    y_sel = _list_row_y(h, 0)
    y_gap = _LIST_Y0 + 4 + 16
    edge = w - 9
    samples = [
        (edge, y_sel),
        (edge, y_gap),
        (edge, _list_row_y(h, 1)),
        (edge, 300),
        (8, y_sel),
        (w - 11, y_sel),
        (w - 10, y_sel),
        (w - 8, y_sel),
    ]
    got = con.screen_pixels(samples)
    for (x, y), rgb in zip(samples[:5], got[:5]):
        assert _is_panel_border(rgb), (rgb, x, y)
    # Selection stops one pixel inside the frame; the margin past it is black.
    assert _is_list_sel(got[5]), got[5]
    assert sum(got[6]) < 60, got[6]
    assert sum(got[7]) < 40, got[7]
    _quit_juke(con)
    con.send_line("PLAY STOP")


def test_juke_list_keeps_waveform_visible(fresh_console):
    """#950: with the playlist open the scope stays put and the list sits below."""
    con = fresh_console
    _prep_queue(con, [("tests/TEST.MOD", "JW/A.MOD"),
                      ("tests/TEST.MOD", "JW/B.MOD")])
    _open_juke(con, "JW")
    w, h = con.screen_size()
    _keys(con, b"l")
    # The oscilloscope panel is still drawn while the playlist is showing.
    frame = [(8, 126), (w - 9, 126), (480, 100), (w - 9, 100)]
    for (x, y), border in zip(frame, con.screen_pixels(frame)):
        assert _is_panel_border(border), (border, x, y)
    # The playlist selection begins below the scope panel, not over it.
    assert _list_row_y(h, 0) > 100 + 52
    assert _is_list_sel(con.screen_pixel(16, _list_row_y(h, 0)))
    _quit_juke(con)
    con.send_line("PLAY STOP")


def test_juke_arrows_on_visualiser_do_not_quit(fresh_console):
    con = fresh_console
    _open_juke(con, "tests/TEST.MOD")
    _keys(con, b"\x1b[A\x1b[B")
    assert _peak_lit(con) > 0.001
    _quit_juke(con)
    assert con.send_line("PRINT 1+1") == "2"
    con.send_line("PLAY STOP")


def test_juke_enter_on_visualiser_quits(fresh_console):
    con = fresh_console
    _open_juke(con, "tests/TEST.MOD")
    _quit_juke(con, b"\r")
    assert con.send_line("PRINT PLAYING()") == "1"
    con.send_line("PLAY STOP")


def test_juke_list_enter_starts_selection(fresh_console):
    con = fresh_console
    _prep_queue(con, [("tests/TEST.MOD", "JL/A.MOD"),
                      ("tests/TEST.MOD", "JL/B.MOD")])
    _open_juke(con, "JL")
    _keys(con, b" ")
    assert con._ser is not None
    _w, h = con.screen_size()
    _keys(con, b"l\x1b[B")
    assert _is_list_sel(con.screen_pixel(16, _list_row_y(h, 1)))
    _keys(con, b"\r")
    _quit_juke(con)
    assert con.send_line("PRINT PLAYING()") == "1"
    con.send_line("PLAY STOP")


def test_juke_list_scrolls_and_keeps_now_playing(fresh_console):
    con = fresh_console
    copies = [("tests/TEST.MOD", "JSQ/%02d.MOD" % i) for i in range(22)]
    _prep_queue(con, copies)
    _open_juke(con, "JSQ")
    _w, h = con.screen_size()
    rows = _list_rows(h)
    _keys(con, b"l", quiet=0.6)
    assert _is_list_sel(con.screen_pixel(16, _list_row_y(h, 0)))
    assert _is_lime_mark(con.screen_pixel(12, _list_row_y(h, 0)))
    _keys(con, b"\x1b[B" * (rows + 4), quiet=1.2)
    assert not _is_list_sel(con.screen_pixel(16, _list_row_y(h, 0)))
    assert _is_list_sel(con.screen_pixel(16, _list_row_y(h, rows - 1)))
    _quit_juke(con)
    con.send_line("PLAY STOP")


def test_juke_list_selection_follows_shuffle(fresh_console):
    """The highlighted row is the selected item after shuffle, and Enter plays it."""
    con = fresh_console
    _prep_queue(con, [("tests/TEST.MOD", "JSH/A.MOD"),
                      ("tests/TEST.MOD", "JSH/B.MOD"),
                      ("tests/TEST.MOD", "JSH/C.MOD"),
                      ("tests/TEST.MOD", "JSH/D.MOD")])
    _open_juke(con, "JSH")
    _keys(con, b" ")
    _w, h = con.screen_size()
    _keys(con, b"l\x1b[B")
    assert _is_list_sel(con.screen_pixel(16, _list_row_y(h, 1)))
    _keys(con, b"r", quiet=0.6)
    hits = []
    for vis in range(4):
        if _is_list_sel(con.screen_pixel(16, _list_row_y(h, vis))):
            hits.append(vis)
    assert hits, "selection highlight missing after shuffle"
    assert len(hits) == 1
    assert _is_lime_mark(con.screen_pixel(12, _list_row_y(h, 0)))
    _keys(con, b"\r")
    _quit_juke(con)
    assert con.send_line("PRINT PLAYING()") == "1"
    con.send_line("PLAY STOP")


def _mkdir_path(con: MMBasicConsole, path: str) -> None:
    acc = ""
    for part in path.split("/"):
        if not part:
            continue
        acc = part if not acc else acc + "/" + part
        assert con.send_line(f'MKDIR "{acc}"') == ""


def test_juke_queues_nested_folder_before_siblings(fresh_console):
    """#934: a subfolder's track is queued, and it plays before files beside it."""
    con = fresh_console
    _mkdir_path(con, "JY/SUB")
    assert con.send_line('COPY "tests/TEST.MOD" TO "JY/SUB/NEST.MOD"') == ""
    assert con.send_line('COPY "tests/TEST.MOD" TO "JY/TOP.MOD"') == ""
    out = _open_juke(con, "JY")
    assert "FILE" not in out.upper()
    screen = con.wait_ocr("NEST", timeout=8.0, crop="960x90+0+0")
    assert "NEST" in screen.upper(), screen
    _quit_juke(con)
    assert con.send_line("PRINT PLAYING()") == "1"
    con.send_line("PLAY STOP")


def test_juke_nested_only_file_still_plays(fresh_console):
    con = fresh_console
    _mkdir_path(con, "JZ/SUB")
    assert con.send_line('COPY "tests/TEST.MOD" TO "JZ/SUB/ONLY.MOD"') == ""
    out = _open_juke(con, "JZ")
    assert "FILE" not in out.upper() and "DIRECTORY" not in out.upper()
    _quit_juke(con)
    assert con.send_line("PRINT PLAYING()") == "1"
    con.send_line("PLAY STOP")


def test_juke_deep_tree_reaches_deep_file(fresh_console):
    """#966: the scan is not depth-capped, so a deep track is queued and plays."""
    con = fresh_console
    acc = "JX"
    _mkdir_path(con, acc)
    for _ in range(9):
        acc = acc + "/L"
        assert con.send_line(f'MKDIR "{acc}"') == ""
    assert con.send_line(f'COPY "tests/TEST.MOD" TO "{acc}/DEEP.MOD"') == ""
    assert con.send_line('COPY "tests/TEST.MOD" TO "JX/TOP.MOD"') == ""
    out = _open_juke(con, "JX")
    assert "FILE" not in out.upper()
    # Subfolders come first, so the deepest track is the one now playing.
    title = con.wait_ocr("DEEP", timeout=8.0, crop="960x40+0+50")
    assert "DEEP" in title.upper(), title
    # The truncation hint is gone from the header (#965).
    header = con.ocr_screen(crop="960x40+400+0")
    assert "more" not in header.lower(), header
    _quit_juke(con)
    con.send_line("PLAY STOP")


def _title_ocr(con: MMBasicConsole, needle: str) -> str:
    return con.wait_ocr(needle, timeout=8.0, crop="960x24+0+56")


def test_juke_shows_module_song_titles(fresh_console):
    con = fresh_console
    for path, needle in (
        ("tests/TEST.MOD", "TESTMOD"),
        ("tests/TEST.XM", "TESTXM"),
        ("tests/TEST.S3M", "TESTS3M"),
    ):
        _open_juke(con, path)
        screen = _title_ocr(con, needle)
        assert needle in screen.upper().replace(" ", ""), screen
        _quit_juke(con)
        con.send_line("PLAY STOP")


def test_juke_keeps_current_mode(fresh_console):
    """#1042: JUKE draws in the mode the console is already in and leaves it."""
    con = fresh_console
    before_mode = con.send_line("PRINT MM.INFO(MODE)")
    w0, h0 = con.screen_size()
    assert (w0, h0) == (1280, 720)  # the boot default, not JUKE's 960x540

    _open_juke(con, "tests/TEST.MOD", keep_mode=True)

    # The framebuffer is still the console's mode, not a forced 960x540.
    assert con.screen_size() == (w0, h0), "JUKE switched video mode"
    assert _peak_lit(con) > 0.001

    _quit_juke(con)
    assert con.send_line("PRINT MM.INFO(MODE)") == before_mode
    con.send_line("PLAY STOP")


def test_juke_layout_scales_with_current_mode(fresh_console):
    """#1041: panels scale from the framebuffer rather than a fixed mode."""
    con = fresh_console
    _open_juke(con, "tests/TEST.MOD", keep_mode=True)
    w, h = con.screen_size()
    s = _juke_scale(w, h)
    assert s > 100  # 1280x720 must enlarge the 960x540 design

    # The oscilloscope inset is anchored at the scaled offset, so it sits
    # well below where the 960x540 layout would have put it (y = 100).
    pad = 8 * s // 100
    top = 100 * s // 100
    bh = 52 * s // 100
    left = pad
    right = pad + (w - 16 * s // 100) - 1
    assert top > 100 and bh > 52
    frame = [(left + 40, top), (left + 40, top + bh - 1),
             (left, top + 5), (right, top + 5)]
    for (x, y), rgb in zip(frame, con.screen_pixels(frame)):
        assert _is_panel_border(rgb), (rgb, x, y)
    _quit_juke(con)
    con.send_line("PLAY STOP")


def test_juke_keeps_chromebook_modes(fresh_console):
    """#1042: JUKE runs in MODE 19 and MODE 20 and leaves each one in place.

    Pixel-exact layout isn't asserted here: QEMU's HDMI pitch for these
    non-16-pixel-aligned widths skews presented graphics independent of
    JUKE (#1047), so the check is that the mode survives and the player
    actually renders.
    """
    con = fresh_console
    for mode, size, info in ((19, (1366, 768), "19.32"),
                             (20, (683, 384), "20.32")):
        assert con.send_line(f"MODE {mode},32") == ""
        _open_juke(con, "tests/TEST.MOD", keep_mode=True)
        assert con.screen_size() == size
        assert _peak_lit(con) > 0.001, f"MODE {mode} rendered nothing"
        _quit_juke(con)
        assert con.send_line("PRINT MM.INFO(MODE)") == info
        con.send_line("PLAY STOP")


def test_juke_shows_id3_titles_and_basename_fallback(fresh_console):
    con = fresh_console
    _open_juke(con, "tests/ZV2.MP3")
    screen = _title_ocr(con, "TWOSONG")
    assert "TWOSONG" in screen.upper().replace(" ", ""), screen
    _quit_juke(con)
    con.send_line("PLAY STOP")
    _open_juke(con, "tests/ZV1.MP3")
    screen = _title_ocr(con, "ONESONG")
    assert "ONESONG" in screen.upper().replace(" ", ""), screen
    _quit_juke(con)
    con.send_line("PLAY STOP")
    _open_juke(con, "tests/ZBAD.MP3")
    screen = _title_ocr(con, "ZBAD")
    assert "ZBAD" in screen.upper().replace(" ", ""), screen
    _quit_juke(con)
    con.send_line("PLAY STOP")
    assert con.send_line('COPY "tests/TEST.WAV" TO "ZTONE.WAV"') == ""
    _open_juke(con, "ZTONE.WAV")
    screen = _title_ocr(con, "ZTONE")
    assert "ZTONE" in screen.upper().replace(" ", ""), screen
    _quit_juke(con)
    con.send_line("PLAY STOP")


def test_juke_list_rows_use_song_titles(fresh_console):
    con = fresh_console
    _prep_queue(con, [("tests/TEST.MOD", "JTIT/A.MOD"),
                      ("tests/TEST.XM", "JTIT/B.XM")])
    _open_juke(con, "JTIT")
    _keys(con, b"l", quiet=0.6)
    screen = con.wait_ocr("TESTXM", timeout=8.0, crop="960x220+0+100")
    assert "TESTXM" in screen.upper().replace(" ", ""), screen
    assert "TESTMOD" in screen.upper().replace(" ", ""), screen
    _quit_juke(con)
    con.send_line("PLAY STOP")


def test_juke_folder_beyond_64_files_is_fully_reachable(fresh_console):
    """#966: a >64-file folder queues every file, not the first 64/256.

    WAV fixtures fall back to their basename as the title, so the last track's
    name proves the playlist reached the 70th entry rather than clamping.
    """
    con = fresh_console
    total = 70
    copies = [("tests/TEST.WAV", "JLG/%02d.WAV" % i) for i in range(total)]
    _prep_queue(con, copies)
    out = _open_juke(con, "JLG")
    assert "FILE" not in out.upper()
    _w, h = con.screen_size()
    _keys(con, b"l", quiet=0.6)
    assert _is_list_sel(con.screen_pixel(16, _list_row_y(h, 0)))
    # Walk the selection to the end of the queue and play it.
    _keys(con, b"\x1b[B" * (total - 1), quiet=2.5)
    _keys(con, b"\r", quiet=0.6)
    title = _title_ocr(con, "69")
    assert "69" in title.upper(), title
    _quit_juke(con)
    con.send_line("PLAY STOP")

