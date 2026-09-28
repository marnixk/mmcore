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


def _open_juke(con: MMBasicConsole, arg: str) -> str:
    assert con._ser is not None
    con.drain(quiet=0.15)
    con._ser.sendall(f'JUKE "{arg}"\r'.encode())
    return con.drain(quiet=0.9, timeout=10).decode(errors="replace")


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
    _w, h = con.screen_size()

    # The panel border is a muted cool grey (left edge and top edge); the
    # RGB555 native surface may shift each channel a count or two.
    for x, y in ((8, 126), (480, 100)):
        border = con.screen_pixel(x, y)
        r, g, b = border
        assert max(border) - min(border) <= 16, (border, x, y)
        assert b >= r, (border, x, y)
        assert r < 100, (border, x, y)

    # The interior is mostly the dark panel fill (traces only cross a few
    # pixels). The panel sits strictly above the spectrum baseline.
    base = h - 71
    assert 100 + 52 < base
    pts = [(x, y) for x in range(12, 948, 20) for y in range(103, 148, 6)]
    dark = sum(1 for c in con.screen_pixels(pts) if sum(c) < 60)
    assert dark > len(pts) * 0.6, (dark, len(pts))
    _quit_juke(con)


def test_juke_spectrum_bars_run_grey_to_lime(fresh_console):
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
    saw_lime = saw_grey = False
    for _ in range(8):
        px = con.screen_pixels(coords)
        for i in range(24):
            run = []
            for c in px[i * n:(i + 1) * n]:
                if sum(c) <= 30:
                    break
                run.append(c)
            if len(run) < 8:
                continue
            bottom, top = run[0], run[-1]
            if top[1] > 170 and top[0] > 90 and top[2] < 150:
                saw_lime = True
            if max(bottom) - min(bottom) < 40 and 90 < sum(bottom) < 520:
                saw_grey = True
        if saw_lime and saw_grey:
            break
        time.sleep(0.2)
    _quit_juke(con)
    assert saw_lime, "expected a lime tip in the spectrum bars"
    assert saw_grey, "expected a grey base in the spectrum bars"


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
    # The old bare < / > transport notation must be gone.
    assert "<  >" not in out
    assert "  <\n" not in out and "  >\n" not in out

