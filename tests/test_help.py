"""HELP command: command list, per-topic text, BASIC constructs, unknowns."""


def test_help_lists_commands(console):
    out = console.send_line("HELP")
    assert out
    for cmd in ("CLS", "PRINT", "PIXEL", "DIR", "FILES", "OPEN", "MODE", "PLAY", "FACTORY_RESET", "CONNECT"):
        assert cmd in out, cmd
    for junk in ("DELETE", "GUI", "CAMERA", "MAP", "TILE"):
        assert junk not in out, junk
    assert "SPRITE" in out
    assert "HELP BASIC" in out
    assert "OPTION WIFI" in out
    assert "ssid" in out.lower()
    assert ".mmbasic.ini" in out


def test_help_cls(console):
    out = console.send_line("HELP CLS")
    assert "CLS" in out
    assert "clear" in out.lower()
    assert "screen" in out.lower()
    assert "[" in out or "colour" in out.lower() or "color" in out.lower()


def test_help_basic_lists_constructs(console):
    out = console.send_line("HELP BASIC")
    assert out
    for name in ("FOR", "WHILE", "DIM", "IF", "DO", "CONST"):
        assert name in out, name


def test_help_basic_for(console):
    out = console.send_line("HELP BASIC FOR")
    assert "FOR" in out
    assert "NEXT" in out
    assert "TO" in out
    assert "FOR I=" in out or "FOR I =" in out


def test_help_for_as_construct(console):
    out = console.send_line("HELP FOR")
    assert "NEXT" in out
    assert "FOR" in out


def test_help_unknown_topic(console):
    out = console.send_line("HELP NOSUCHTHING")
    assert "?SYNTAX ERROR" not in out
    assert "unknown" in out.lower()
    assert "NOSUCHTHING" in out.upper()


def test_help_case_insensitive(console):
    out = console.send_line("help print")
    assert out != "?SYNTAX ERROR"
    assert "PRINT" in out
    mixed = console.send_line("Help CLS")
    assert "CLS" in mixed
    assert "screen" in mixed.lower()


def test_help_mode_resolutions(console):
    out = console.send_line("HELP MODE", timeout=8.0)
    assert out != "?SYNTAX ERROR"
    assert "MODE n" in out
    assert "MM.HRES" in out
    assert "MM.VRES" in out
    for size in (
        "384x240",
        "1024x768",
        "1920x1080",
        "640x480",
        "800x600",
        "1280x720",
        "1280x1024",
    ):
        assert size in out, size
    assert "MODE 8,16" in out
    assert "HDMI" in out
    assert "retune" in out.lower()


def test_help_mode_case_insensitive(console):
    out = console.send_line("help mode", timeout=8.0)
    assert out != "?SYNTAX ERROR"
    assert "384x240" in out
    assert "1024x768" in out
    mixed = console.send_line("Help Mode", timeout=8.0)
    assert "1920x1080" in mixed
    assert "MM.HRES" in mixed


def test_help_files(console):
    out = console.send_line("HELP FILES")
    assert out != "?SYNTAX ERROR"
    assert "dual-pane" in out.lower() or "file manager" in out.lower()
    assert "DIR listing" in out or "not a DIR" in out
    listing = console.send_line("HELP DIR")
    assert "alias" not in listing.lower()


def test_help_factory_reset(console):
    out = console.send_line("HELP FACTORY_RESET")
    assert out != "?SYNTAX ERROR"
    assert "Factory" in out or "defaults" in out.lower()
    assert ".mmbasic.ini" in out
    alias = console.send_line("HELP FACTORY")
    assert "FACTORY_RESET" in alias or "defaults" in alias.lower()


def test_help_option_wifi(console):
    out = console.send_line("HELP OPTION")
    assert "WIFI" in out
    assert ".mmbasic.ini" in out
    assert "firmware" in out.lower()
    assert "WPA2" in out or "wpa" in out.lower()
    assert "beacon" in out.lower()
    assert "country=US" in out or "US" in out
    assert "[wifi]" in out
