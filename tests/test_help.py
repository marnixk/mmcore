"""HELP command: command list, per-topic text, BASIC constructs, unknowns."""


def test_help_lists_commands(console):
    out = console.send_line("HELP")
    assert out
    for cmd in ("CLS", "PRINT", "PIXEL", "DIR", "OPEN", "MODE", "PLAY"):
        assert cmd in out, cmd
    for junk in ("DELETE", "SPRITE", "GUI", "CAMERA", "MAP", "TILE"):
        assert junk not in out, junk
    assert "HELP BASIC" in out


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


def test_help_mode_case_insensitive(console):
    out = console.send_line("help mode", timeout=8.0)
    assert out != "?SYNTAX ERROR"
    assert "384x240" in out
    assert "1024x768" in out
    mixed = console.send_line("Help Mode", timeout=8.0)
    assert "1920x1080" in mixed
    assert "MM.HRES" in mixed
