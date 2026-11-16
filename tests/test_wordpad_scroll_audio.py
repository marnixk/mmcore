"""WORDPAD scrolling must keep the audio mixer fed (#1160).

Timing-sensitive: the MIXGAP cushion is only meaningful when the QEMU lane is
not shared with other QEMU instances, so the module is ``qemu_exclusive``
(#1495).  It has its own module because ``tests/test_wordpad.py`` keeps a
module-scoped shared ``console`` open for the whole module, and the lane lock
is re-entrant per process (``harness/qemu_harness.py``): an exclusive console
created inside that module would silently stay shared.
"""

import re
import time

import pytest

from harness import MMBasicConsole
from test_wordpad import _open, _plain, _quit, _seed_long_doc

# The marker makes fixtures exclusive; the console below is built by hand, so
# it passes ``exclusive=True`` itself.  Keep both so the intent is visible.
pytestmark = pytest.mark.qemu_exclusive


def test_wordpad_scroll_keeps_audio_mixing(kernel_image):
    """#1160: holding a cursor key scrolls the pane and music keeps playing.

    Mirrors the TERM repaint/audio test (#936).  WORDPAD streams every visible
    row to the serial console on each redraw, so the hold is drained like a
    serial reader (a saturated 115200-baud TX ring is what starves the mixer
    on hardware).  The drained rows prove the view really scrolled, and
    MIXGAP stays well under the queued cushion.

    The console holds the QEMU lane exclusively, so no other QEMU instance
    competes for CPU and a busy parallel host cannot fake an audio
    underrun (#1495).
    """
    con = MMBasicConsole(
        kernel_image, extra_qemu=["-device", "usb-kbd"], exclusive=True
    )
    con.start()
    try:
        _seed_long_doc(con)
        # A fast repeat drives many scroll frames in the hold window.
        assert con.send_line("OPTION USBKEYBOARD REPEAT 100, 30") == ""
        assert con.send_line('PLAY S3M "tests/TEST.S3M"') == ""
        assert con.send_line("PRINT PLAYING()") == "1"
        # MIXGAP is cumulative and load-sensitive, so this baseline only
        # sanity-checks that the mixer is running at all (#1170).
        base_gap = int(con.send_line('PRINT MM.INFO("MIXGAP")'))
        assert base_gap < 1000, base_gap
        assert "[WORDPAD]" in _open(con, 'WORDPAD "LONG.MD"', quiet=1.0)
        con.key_down("down")
        rows = _plain(con.drain(quiet=0.3, timeout=1.5).decode(errors="replace"))
        con.key_up("down")
        time.sleep(0.2)
        seen = {int(m) for m in re.findall(r"line (\d+)", rows)}
        # The pane started at line 1; the held key must have scrolled well past
        # the initial window (~23 text rows).
        assert seen and max(seen) >= 30, sorted(seen)
        _quit(con)
        gap = int(con.send_line('PRINT MM.INFO("MIXGAP")'))
        underrun = con.send_line('PRINT MM.INFO("UNDERRUN")')
        playing = con.send_line("PRINT PLAYING()")
        con.send_line("PLAY STOP")
        # Measure the gap the scroll *added* over the pre-scroll baseline
        # rather than an absolute bound: a busy parallel host pushes MIXGAP up
        # even when the fix works, while a real starvation still blows the
        # margin (#1170).
        assert gap < base_gap + 150, (base_gap, gap)
        assert underrun == "0", underrun
        assert playing == "1", playing
    finally:
        con.stop()
