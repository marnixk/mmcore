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
