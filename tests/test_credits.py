"""CREDITS command: MMBasic/PicoMite copyright and Pi port line."""

import os
import subprocess

from ihelp_util import close_ihelp, dump_topic, open_ihelp, scroll_all

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def _mmb_version():
    return subprocess.check_output(
        ["git", "describe", "--tags", "--always"],
        cwd=REPO,
        text=True,
    ).strip()


def test_credits_shows_holders_and_port(console):
    out = console.send_line("CREDITS", timeout=20)
    low = out.lower()
    assert "geoff graham" in low
    assert "peter mather" in low
    assert "mmbasic" in low
    assert "marnix kok" in low
    assert "raspberry" in low
    assert "circle" in low
    assert f"build {_mmb_version()}".lower() in low
    assert console.send_line("PRINT 2+2") == "4"


def test_credits_ascii_homage(console):
    out = console.send_line("CREDITS")
    low = out.lower()
    assert "maximite" in low
    assert "mmbasic" in out or "MM" in out
    assert "_" in out or "|" in out


def test_help_credits(console):
    listing = scroll_all(console, open_ihelp(console, "INDEX"))
    assert "CREDITS" in listing
    close_ihelp(console)
    out = dump_topic(console, "CREDITS")
    assert out != "?SYNTAX ERROR"
    low = out.lower()
    assert "copyright" in low or "geoff" in low or "picomite" in low
