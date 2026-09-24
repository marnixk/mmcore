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


def test_juke_has_fixed_cyberpunk_palette(fresh_console):
    """JUKE must not follow the system theme, and must leave it alone."""
    con = fresh_console
    assert con.send_line('OPTION THEME "Snow"') == ""
    before = con.send_line('PRINT THEME("TEXT_BG")')
    _open_juke(con, "tests/TEST.MOD")
    # Header/backing panels stay near-black even under a light system theme.
    dark = [con.screen_pixel(2, 2), con.screen_pixel(480, 2),
            con.screen_pixel(480, 44), con.screen_pixel(2, 494)]
    assert all(r + g + b < 140 for r, g, b in dark), dark

    # The header rule is a neon magenta accent, not a theme colour.
    r, g, b = con.screen_pixel(480, 46)
    assert r > 120 and b > 100 and g < 120, (r, g, b)

    _quit_juke(con)
    assert con.send_line('PRINT THEME("TEXT_BG")') == before


def test_juke_visualiser_has_vertical_gradient(fresh_console):
    con = fresh_console
    # TEST.WAV is a three-second tone, so the bars stay tall long enough to
    # sample the gradient (the MOD fixture is only a brief blip).
    _open_juke(con, "tests/TEST.WAV")
    _w, h = con.screen_size()
    base = h - 66
    xs = [14 + i * 38 + 17 for i in range(24)]
    coords = [(x, y) for x in xs for y in range(base - 2, base - 150, -2)]
    cool = hot = False
    for _ in range(8):
        for r, g, b in con.screen_pixels(coords):
            if b > 180 and g > 170 and r < 90:
                cool = True  # electric cyan base
            if r > 180 and b > 180 and g < 120:
                hot = True  # neon magenta tip
        if cool and hot:
            break
        time.sleep(0.2)
    _quit_juke(con)
    assert cool, "expected a cool gradient base in the spectrum bars"
    assert hot, "expected a hot gradient tip in the spectrum bars"


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
    assert "cyberpunk" in low
    # The old bare < / > transport notation must be gone.
    assert "<  >" not in out
    assert "  <\n" not in out and "  >\n" not in out

