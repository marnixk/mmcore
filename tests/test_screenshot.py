"""SCREENSHOT to drive (#517/#522).

The SCREENSHOT command and the F12 global hotkey capture the displayed screen to
a PNG on A:. These tests verify the file exists, that its name is timestamped,
and that the image round-trips through LOAD PNG back to the same pixels.
"""

import re

from ihelp_util import dump_topic


def _is_green(rgb):
    r, g, b = rgb
    return g > 150 and r < 130 and b < 130


def _is_red(rgb):
    r, g, b = rgb
    return r > 150 and g < 130 and b < 130


def _shade(console, x, y):
    v = int(console.send_line(f"PRINT PIXEL({x},{y})").split()[0])
    return (v >> 16) & 255, (v >> 8) & 255, v & 255


def test_screenshot_explicit_path_round_trips(fresh_console):
    c = fresh_console
    assert c.send_line("MODE 7,16") == ""
    assert c.send_line("CLS") == ""
    assert c.send_line("PIXEL 100,100,RED") == ""
    out = c.send_line('SCREENSHOT "A:/CAPPED.PNG"')
    assert "CAPPED.PNG" in out.upper(), out
    assert "CAPPED.PNG" in c.send_line('DIR "A:/"').upper()
    assert c.send_line("CLS") == ""
    assert c.send_line('LOAD PNG "A:/CAPPED.PNG",0,0') == ""
    assert _is_red(_shade(c, 100, 100))


def test_screenshot_default_path_is_timestamped(fresh_console):
    c = fresh_console
    assert c.send_line("MODE 7,16") == ""
    assert c.send_line("CLS") == ""
    out = c.send_line("SCREENSHOT").strip()
    assert re.fullmatch(r"A:/SHOT_\d{8}_\d{6}(_\d+)?\.PNG", out, re.I), out
    listing = c.send_line('DIR "A:/SHOT_*.PNG"')
    assert "SHOT_" in listing.upper()
    # A second capture in the same second must not overwrite the first.
    again = c.send_line("SCREENSHOT").strip()
    assert again != out
    assert re.fullmatch(r"A:/SHOT_\d{8}_\d{6}(_\d+)?\.PNG", again, re.I), again


def test_screenshot_folder_argument_appends_name(fresh_console):
    c = fresh_console
    assert c.send_line("MODE 7,16") == ""
    assert c.send_line('MKDIR "SHOTS"') == ""
    out = c.send_line('SCREENSHOT "A:/SHOTS/"').strip()
    assert re.fullmatch(r"A:/SHOTS/SHOT_\d{8}_\d{6}(_\d+)?\.PNG", out, re.I), out
    assert "SHOT_" in c.send_line('DIR "A:/SHOTS"').upper()


def test_screenshot_hotkey_f12_globally(fresh_console):
    c = fresh_console
    assert c.send_line("MODE 7,16") == ""
    assert c.send_line("CLS") == ""
    assert c.send_line("PIXEL 50,50,RGB(0,255,0)") == ""
    c.drain(quiet=0.1)
    c._ser.sendall(b"\x1b[24~")
    c.drain(quiet=0.4, timeout=8.0)
    listing = c.send_line('DIR "A:/SHOT_*.PNG"')
    names = re.findall(r"(SHOT_[\w.]+\.PNG)", listing, re.I)
    assert names, listing
    path = "A:/" + names[0]
    assert c.send_line("CLS") == ""
    assert c.send_line(f'LOAD PNG "{path}",0,0') == ""
    assert _is_green(_shade(c, 50, 50))


def test_screenshot_missing_drive_fails_loudly(fresh_console):
    c = fresh_console
    assert c.send_line("MODE 7,16") == ""
    out = c.send_line('SCREENSHOT "Z:/NOPE.PNG"')
    assert out.startswith("?")


def test_screenshot_help_documents_hotkey(console):
    out = dump_topic(console, "SCREENSHOT")
    assert "SCREENSHOT" in out.upper()
    assert "F12" in out
    assert "A:/" in out
