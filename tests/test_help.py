"""HELP / IHELP interactive help: index, topics, BASIC, unknowns."""

from ihelp_util import close_ihelp, dump_topic, keys, open_ihelp, scroll_all


def test_help_lists_commands(console):
    seen = scroll_all(console, open_ihelp(console))
    for cmd in (
        "CLS",
        "PRINT",
        "PIXEL",
        "DIR",
        "FILES",
        "OPEN",
        "MODE",
        "PLAY",
        "FACTORY_RESET",
        "CONNECT",
        "TERM",
        "WORDPAD",
        "CREDITS",
        "IPCONFIG",
    ):
        assert f"<{cmd}>" in seen, cmd
    for junk in ("DELETE", "GUI", "CAMERA", "MAP", "TILE"):
        assert f"<{junk}>" not in seen, junk
    assert "<SPRITE>" in seen
    assert "HELP BASIC" in seen
    close_ihelp(console)
    overview = dump_topic(console, "BASIC")
    assert "OPTION" in overview
    assert "WIFI" in overview
    assert "ssid" in overview.lower()
    assert "DEBUG" in overview
    assert ".mmbasic.ini" in overview


def test_help_cls(console):
    out = dump_topic(console, "CLS")
    assert "CLS" in out
    assert "clear" in out.lower()
    assert "screen" in out.lower()
    assert "[" in out or "colour" in out.lower() or "color" in out.lower()


def test_help_basic_lists_constructs(console):
    out = dump_topic(console, "BASIC")
    assert out
    for name in ("FOR", "WHILE", "DIM", "IF", "DO", "CONST"):
        assert name in out, name


def test_help_basic_for(console):
    out = dump_topic(console, "BASIC FOR")
    assert "FOR" in out
    assert "NEXT" in out
    assert "TO" in out
    assert "FOR I=" in out or "FOR I =" in out


def test_help_for_as_construct(console):
    out = dump_topic(console, "FOR")
    assert "NEXT" in out
    assert "FOR" in out


def test_help_unknown_topic(console):
    out = dump_topic(console, "NOSUCHTHING")
    assert "?SYNTAX ERROR" not in out
    assert "unknown" in out.lower()
    assert "NOSUCHTHING" in out.upper()


def test_help_case_insensitive(console):
    out = dump_topic(console, "print")
    assert out != "?SYNTAX ERROR"
    assert "PRINT" in out
    mixed = dump_topic(console, "CLS")
    assert "CLS" in mixed
    assert "screen" in mixed.lower()


def test_help_mode_resolutions(console):
    out = dump_topic(console, "MODE")
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
    out = dump_topic(console, "mode")
    assert out != "?SYNTAX ERROR"
    assert "384x240" in out
    assert "1024x768" in out
    mixed = dump_topic(console, "Mode")
    assert "1920x1080" in mixed
    assert "MM.HRES" in mixed


def test_help_files(console):
    out = dump_topic(console, "FILES")
    assert out != "?SYNTAX ERROR"
    assert "dual-pane" in out.lower() or "file manager" in out.lower()
    assert "DIR listing" in out or "not a DIR" in out or "<DIR>" in out
    listing = dump_topic(console, "DIR")
    assert "alias" not in listing.lower()


def test_help_kill_rm_del(console):
    out = dump_topic(console, "KILL")
    assert "Delete" in out or "delete" in out.lower()
    assert "RM" in out
    assert "DEL" in out
    assert "There is no DELETE" not in out
    assert "RM" in dump_topic(console, "RM")
    assert "DEL" in dump_topic(console, "DEL")


def test_help_rename_mv(console):
    out = dump_topic(console, "RENAME")
    assert "Rename" in out or "rename" in out.lower()
    assert "MV" in out
    mv = dump_topic(console, "MV")
    assert "RENAME" in mv or "Rename" in mv or "move" in mv.lower()


def test_help_factory_reset(console):
    out = dump_topic(console, "FACTORY_RESET")
    assert out != "?SYNTAX ERROR"
    assert "Factory" in out or "defaults" in out.lower()
    assert ".mmbasic.ini" in out
    alias = dump_topic(console, "FACTORY")
    assert "FACTORY_RESET" in alias or "defaults" in alias.lower()


def test_help_option_wifi(console):
    out = dump_topic(console, "OPTION")
    assert "WIFI" in out
    assert ".mmbasic.ini" in out
    assert "firmware" in out.lower()
    assert "WPA2" in out or "wpa" in out.lower()
    assert "beacon" in out.lower()
    assert "country=US" in out or "US" in out
    assert "COUNTRY" in out
    assert "[wifi]" in out
    assert "DEBUG" in out
    assert "default OFF" in out or "Default OFF" in out
    assert "PROMPT" in out
    assert "CWD" in out


def test_ihelp_has_no_menu_bar(console):
    seen = open_ihelp(console)
    assert "File  Edit  View  Search" not in seen
    assert "Debug  Options" not in seen
    assert "HELP: Index" in seen or "<Contents>" in seen
    close_ihelp(console)


def test_ihelp_enter_opens_link(console):
    seen = open_ihelp(console)
    assert "<Contents>" in seen
    assert "<Index>" in seen
    seen = keys(console, b"\r")
    seen = scroll_all(console, seen)
    assert "MMBasic Interactive Help" in seen or "Language constructs" in seen
    close_ihelp(console)


def test_ihelp_escape_quits_index(console):
    open_ihelp(console)
    keys(console, b"\x1b\x1b")
    assert console.send_line("PRINT 1") == "1"


def test_ihelp_deeplink_escape_returns_to_index(console):
    seen = open_ihelp(console, "CLS")
    assert "clear" in seen.lower()
    assert "HELP: CLS" in seen or "CLS [" in seen or "CLS" in seen
    seen = keys(console, b"\x1b\x1b")
    assert "<CLS>" in seen
    assert "HELP: Index" in seen or "[C]" in seen or "<Contents>" in seen
    keys(console, b"\x1b\x1b")
    assert console.send_line("PRINT 2") == "2"


def test_ihelp_alias_command(console):
    seen = open_ihelp(console)
    assert "<Contents>" in seen
    close_ihelp(console)
    out = dump_topic(console, "CLS")
    assert "clear" in out.lower()
