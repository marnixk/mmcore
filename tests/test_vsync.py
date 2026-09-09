"""VSYNC_WAIT: 60 Hz frame cap (#202)."""

from ihelp_util import dump_topic


def test_vsync_wait_is_a_command(console):
    assert console.send_line("VSYNC_WAIT") == ""


def test_vsync_wait_caps_loop_to_60hz(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("10 TIMER=0") == ""
    assert console.send_line("20 FOR I=1 TO 6") == ""
    assert console.send_line("30 VSYNC_WAIT") == ""
    assert console.send_line("40 NEXT I") == ""
    assert console.send_line("50 PRINT TIMER") == ""
    out = console.send_line("RUN", timeout=8.0)
    parts = [p.strip() for p in out.replace("\r", "\n").split("\n") if p.strip()]
    ms = int(parts[0])
    assert 50 <= ms <= 250, out


def test_help_vsync_wait(console):
    out = dump_topic(console, "VSYNC_WAIT")
    assert out != "?SYNTAX ERROR"
    assert "unknown" not in out.lower()
    assert "VSYNC_WAIT" in out
    assert "60" in out
    basic = dump_topic(console, "CMM2")
    assert "VSYNC_WAIT" in basic
