"""HELP / IHELP interactive help: index, topics, BASIC, unknowns."""

from harness import MMBasicConsole
from ihelp_util import close_ihelp, dump_topic, keys, open_ihelp, scroll_all


def test_help_lists_commands(console):
    seen = scroll_all(console, open_ihelp(console))
    for cmd in (
        "CLS",
        "LOCATE",
        "PRINT",
        "PIXEL",
        "DIR",
        "FILES",
        "PACKAGE",
        "OPEN",
        "MODE",
        "PLAY",
        "FACTORY_RESET",
        "CONNECT",
        "TERM",
        "WORDPAD",
        "CREDITS",
        "IPCONFIG",
        "OPTIONS",
        "MATH",
        "STRUCT",
        "JSON_PARSE",
        "ETHERNET",
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


def test_help_locate(console):
    out = dump_topic(console, "LOCATE")
    assert out != "?SYNTAX ERROR"
    assert "LOCATE" in out
    assert "pixel" in out.lower()
    assert "MM.HPOS" in out
    assert "@(x,y)" in out
    seen = scroll_all(console, open_ihelp(console))
    assert "<LOCATE>" in seen
    close_ihelp(console)


def test_help_pixel_array_form(console):
    out = dump_topic(console, "PIXEL")
    assert out != "?SYNTAX ERROR"
    assert "x()" in out or "XX()" in out
    assert "array" in out.lower()
    assert "smallest" in out.lower()
    assert "pos().x" in out
    math = dump_topic(console, "MATH")
    assert "pts().x" in math or "pos().x" in math
    assert "arr().member" in math or "TYPE arrays" in math


def test_help_basic_lists_constructs(console):
    out = dump_topic(console, "BASIC")
    assert out
    for name in ("FOR", "WHILE", "DIM", "IF", "DO", "CONST", "TYPE"):
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
    assert "overscan" in out.lower()
    assert "hdmi_mode=82" in out
    assert "2 when larger" not in out.lower()
    assert "0-7" in out


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
    assert "theme" in out.lower() or "EDIT THEME" in out.upper()
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
    assert "beacon" in out.lower() or "scan" in out.lower()
    assert "OPTIONS WIFI" in out or "<OPTIONS> WIFI" in out
    assert "country=US" in out or "US" in out
    assert "COUNTRY" in out
    assert "[wifi]" in out
    assert "DEBUG" in out
    assert ".termlog" in out.lower() or "TERM LOG" in out
    assert "default OFF" in out or "Default OFF" in out
    assert "PROMPT" in out
    assert "CWD" in out


def test_help_options_wifi(console):
    listing = scroll_all(console, open_ihelp(console))
    assert "<OPTIONS>" in listing
    close_ihelp(console)
    out = dump_topic(console, "OPTIONS")
    assert out != "?SYNTAX ERROR"
    assert "OPTIONS WIFI" in out
    assert "not configured" in out.lower()
    assert "not an alias" in out.lower()
    via = dump_topic(console, "OPTIONS WIFI")
    assert "OPTIONS WIFI" in via
    assert "Connected to" in via


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
    keys(console, b"\x1b", quiet=0.6)
    assert console.send_line("PRINT 1") == "1"


def test_ihelp_escape_prints_prompt(console):
    """Esc from HELP should reprint the prompt without needing Enter."""
    open_ihelp(console)
    console.drain(quiet=0.15)
    assert console._ser is not None
    console._ser.sendall(b"\x1b")
    out = console.drain(quiet=0.6).decode(errors="replace")
    assert "> " in out
    assert out.count("> ") == 1
    assert console.send_line("PRINT 3") == "3"


def test_ihelp_ctrl_c_prints_prompt(console):
    open_ihelp(console)
    console.drain(quiet=0.15)
    assert console._ser is not None
    console._ser.sendall(b"\x03")
    out = console.drain(quiet=0.5).decode(errors="replace")
    assert "> " in out
    assert out.count("> ") == 1
    assert console.send_line("PRINT 4") == "4"


def test_ihelp_deeplink_escape_returns_to_prompt(console):
    seen = open_ihelp(console, "CLS")
    assert "clear" in seen.lower()
    assert "HELP: CLS" in seen or "CLS [" in seen or "CLS" in seen
    console.drain(quiet=0.15)
    assert console._ser is not None
    console._ser.sendall(b"\x1b")
    out = console.drain(quiet=0.6).decode(errors="replace")
    assert "> " in out
    assert "HELP: Index" not in out
    assert console.send_line("PRINT 2") == "2"


def test_help_colour_esc_returns_to_prompt(console):
    assert console._ser is not None
    console._ser.sendall(b"HELP colour\r")
    seen = console.drain(quiet=0.85).decode(errors="replace")
    assert "COLOUR" in seen or "COLOR" in seen or "colour" in seen.lower()
    console._ser.sendall(b"\x1b")
    out = console.drain(quiet=0.6).decode(errors="replace")
    assert "> " in out
    assert "HELP: Index" not in out
    assert console.send_line("PRINT 8") == "8"


def test_help_open_tcp_stream(console):
    out = dump_topic(console, "OPEN")
    assert out != "?SYNTAX ERROR"
    assert "TCP:" in out
    assert "WEB" not in out
    fns = dump_topic(console, "FUNCTIONS")
    assert "INPUT$(nbr" in fns or "nbr,#n" in fns.replace(" ", "")
    assert "TCP" in fns
    dollar = dump_topic(console, "INPUT$")
    assert dollar != "?SYNTAX ERROR"
    assert "nbr" in dollar.lower() or "count" in dollar.lower()


def test_help_json_dollar(console):
    out = dump_topic(console, "JSON$")
    assert out != "?SYNTAX ERROR"
    assert "path$" in out
    fns = dump_topic(console, "FUNCTIONS")
    assert "JSON$" in fns


def test_help_colour_ibm(console):
    out = dump_topic(console, "COLOUR")
    assert out != "?SYNTAX ERROR"
    low = out.lower()
    assert "lightred" in low
    assert "ibm" in low
    assert "31" in out
    alias = dump_topic(console, "COLOR")
    assert "COLOUR" in alias or "COLOR" in alias


def test_ihelp_alias_command(console):
    seen = open_ihelp(console)
    assert "<Contents>" in seen
    close_ihelp(console)
    out = dump_topic(console, "CLS")
    assert "clear" in out.lower()


def test_help_mentions_edit_theme_colours(console):
    out = dump_topic(console, "HELP")
    assert "theme" in out.lower()
    files = dump_topic(console, "FILES")
    assert "theme" in files.lower() or "EDIT THEME" in files.upper()


def test_ihelp_follows_editor_theme_phosphor(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        assert con.send_line("OPTION EDIT THEME PHOSPHOR") == ""
        open_ihelp(con)
        empty = [con.screen_pixel(x, 80) for x in (480, 520, 560)]
        assert all(r + g + b < 50 for r, g, b in empty), empty
        close_ihelp(con)
        assert con.send_line("OPTION EDIT THEME TURBO") == ""
    finally:
        con.stop()


def test_ihelp_default_body_is_slate(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        open_ihelp(con)
        empty = [con.screen_pixel(x, 80) for x in (480, 520, 560)]
        assert all(r < 50 and g < 50 and b < 55 for r, g, b in empty), empty
        close_ihelp(con)
    finally:
        con.stop()
