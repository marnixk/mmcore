"""Persistent INI settings, hidden dotfiles, FACTORY_RESET, and OPTION WIFI."""

from ihelp_util import dump_topic


def _settings_open(con):
    """Open SETTINGS as a dialog and return the raw serial frame text."""
    assert con._ser is not None
    con.drain(quiet=0.2, timeout=2.0)
    con._ser.sendall(b"SETTINGS\r")
    return con.drain(quiet=0.9).decode(errors="replace")


def _settings_keys(con, data, quiet=0.4):
    assert con._ser is not None
    con._ser.sendall(data)
    return con.drain(quiet=quiet).decode(errors="replace")


def _frame_rows(text):
    return [ln.rstrip() for ln in text.replace("\r", "\n").split("\n")]


def _has_inset_border(text):
    rows = _frame_rows(text)
    border = [ln for ln in rows if "+" in ln and "-" in ln]
    if not border:
        return False
    top = border[0]
    return top.startswith(" ") and top.index("+") > 0


def _ini_path(con):
    for path in ("A:/.mmbasic.ini", "C:/.mmbasic.ini"):
        out = con.send_line(f'OPEN "{path}" FOR INPUT AS #1')
        con.send_line("CLOSE #1")
        if out == "":
            return path
    raise AssertionError("settings INI not found on A: or C:")


def _read_ini(con, path=None):
    if not path:
        path = _ini_path(con)
    assert con.send_line("NEW") == ""
    assert con.send_line(f'10 OPEN "{path}" FOR INPUT AS #1') == ""
    assert con.send_line("20 IF EOF(#1) THEN GOTO 70") == ""
    assert con.send_line("30 LINE INPUT #1, A$") == ""
    assert con.send_line("40 PRINT A$") == ""
    assert con.send_line("50 GOTO 20") == ""
    assert con.send_line("70 CLOSE #1") == ""
    return con.send_line("RUN", timeout=8)


def test_factory_defaults_mode_prompt_slate(fresh_console):
    """Issue #239: new instance defaults MODE 11, PROMPT CWD, theme Slate."""
    con = fresh_console
    assert con.send_line("PRINT MM.HRES") == "1280"
    assert con.send_line("PRINT MM.VRES") == "720"
    listed = con.send_line("OPTION LIST")
    assert "DEFAULT MODE" not in listed
    assert "PROMPT" not in listed
    assert "EDIT THEME" not in listed
    all_listed = con.send_line("OPTION LIST ALL")
    assert "OPTION DEFAULT MODE 11" in all_listed
    assert "OPTION PROMPT CWD" in all_listed
    assert "SLATE" in all_listed.upper()
    assert con.send_line("FACTORY_RESET") == "Factory defaults restored"
    assert con.send_line("PRINT MM.HRES") == "1280"
    assert con.send_line("PRINT MM.VRES") == "720"
    ini = _read_ini(con)
    assert "default_mode=11" in ini
    assert "prompt=1" in ini
    assert "edit_theme=5" in ini


def test_eof_hash_file_number(console):
    assert console.send_line('OPEN "A:/EOFTEST.TXT" FOR OUTPUT AS #1') == ""
    assert console.send_line('PRINT #1, "hi"') == ""
    assert console.send_line("CLOSE #1") == ""
    assert console.send_line('OPEN "A:/EOFTEST.TXT" FOR INPUT AS #1') == ""
    assert console.send_line("PRINT EOF(#1)") == "0"
    assert console.send_line("LINE INPUT #1, A$") == ""
    assert console.send_line("PRINT A$") == "hi"
    assert console.send_line("PRINT EOF(#1)") == "1"
    assert console.send_line("CLOSE #1") == ""


def test_option_persists_to_hidden_ini(console):
    assert console.send_line("FACTORY_RESET") == "Factory defaults restored"
    assert console.send_line("OPTION TAB 4") == ""
    assert console.send_line("OPTION BREAK 4") == ""
    ini = _read_ini(console)
    assert "tab=4" in ini
    assert "break=4" in ini
    assert "[core]" in ini


def test_dir_hides_dotfiles(console):
    assert console.send_line('CHDIR "A:/"') == ""
    assert console.send_line('OPEN "A:/.hidden.txt" FOR OUTPUT AS #1') == ""
    assert console.send_line('PRINT #1, "secret"') == ""
    assert console.send_line("CLOSE #1") == ""
    assert console.send_line('OPEN "A:VISIBLE.TXT" FOR OUTPUT AS #1') == ""
    assert console.send_line('PRINT #1, "ok"') == ""
    assert console.send_line("CLOSE #1") == ""
    listing = console.send_line('DIR "A:/"')
    assert "VISIBLE.TXT" in listing.upper()
    assert ".HIDDEN" not in listing.upper()
    assert ".MMBASIC.INI" not in listing.upper()
    # OPEN by exact path still works
    assert console.send_line('OPEN "A:/.hidden.txt" FOR INPUT AS #1') == ""
    assert console.send_line("LINE INPUT #1, A$") == ""
    assert console.send_line("PRINT A$") == "secret"
    assert console.send_line("CLOSE #1") == ""


def test_factory_reset_restores_options_and_ini(console):
    assert console.send_line("OPTION TAB 4") == ""
    assert console.send_line('OPTION WIFI "KeepMe","oldpass"') != "?SYNTAX ERROR"
    listing = console.send_line('DIR "A:/"')
    assert ".MMBASIC.INI" not in listing.upper()
    assert console.send_line('OPEN "A:/KEEP.BAS" FOR OUTPUT AS #1') == ""
    assert console.send_line('PRINT #1, "10 PRINT 1"') == ""
    assert console.send_line("CLOSE #1") == ""
    out = console.send_line("FACTORY_RESET")
    assert "Factory defaults restored" in out
    listed = console.send_line("OPTION LIST")
    assert "TAB 4" not in listed
    ini = _read_ini(console)
    assert "tab=2" in ini
    assert "ssid=" in ini
    assert "KeepMe" not in ini
    assert "oldpass" not in ini
    assert "psk=" in ini
    listing = console.send_line('DIR "A:/"')
    assert "KEEP.BAS" in listing.upper()


def test_option_wifi_persists_credentials(console):
    out = console.send_line('OPTION WIFI "TestSSID","secretpass"')
    assert "?SYNTAX ERROR" not in out
    assert "secretpass" not in out
    ini = _read_ini(console)
    assert "[wifi]" in ini
    assert "ssid=TestSSID" in ini
    assert "psk=secretpass" in ini
    assert "enabled=1" in ini


def test_option_wifi_bare_scans_or_reports_unavailable(console):
    assert console.send_line("FACTORY_RESET") == "Factory defaults restored"
    out = console.send_line("OPTION WIFI")
    assert "?WIFI not configured" not in out
    assert "?SYNTAX ERROR" not in out
    assert "Wi-Fi not available" in out or "not available" in out.lower()
    assert console.send_line("PRINT 6*7") == "42"


def test_option_wifi_status_is_on_a_new_line(console):
    """Enter after OPTION WIFI must LF before status, not only CR."""
    assert console.send_line("FACTORY_RESET") == "Factory defaults restored"
    console.drain(quiet=0.15)
    console._ser.sendall(b"OPTION WIFI\r")
    raw = console.drain(quiet=0.8).decode(errors="replace")
    low = raw.lower()
    assert "wi-fi not available" in low or "no networks found" in low
    mark = low.find("wi-fi")
    if mark < 0:
        mark = low.find("no networks")
    assert mark > 0
    assert "\n" in raw[:mark]
    assert console.send_line("PRINT 3") == "3"


def test_options_wifi_errors_without_credentials(console):
    assert console.send_line("FACTORY_RESET") == "Factory defaults restored"
    out = console.send_line("OPTIONS WIFI")
    assert "?WIFI not configured" in out
    assert console.send_line("PRINT 6*7") == "42"


def test_options_wifi_connects_with_stored_credentials(console):
    assert console.send_line("FACTORY_RESET") == "Factory defaults restored"
    stored = console.send_line('OPTION WIFI "TestSSID","secretpass"')
    assert "?SYNTAX ERROR" not in stored
    assert "?WIFI not configured" not in stored
    assert "secretpass" not in stored
    out = console.send_line("OPTIONS WIFI")
    assert "?WIFI not configured" not in out
    assert "?SYNTAX ERROR" not in out
    assert "secretpass" not in out
    assert "Wi-Fi not available" in out or "Connected to" in out or "Wi-Fi connected" in out or "connect failed" in out.lower()
    assert console.send_line("PRINT 1+1") == "2"
    assert "?SYNTAX ERROR" in console.send_line('OPTIONS WIFI "x","y"').upper()
    assert "?SYNTAX ERROR" in console.send_line("OPTIONS").upper()


def test_options_wifi_program_reconnects_or_errors(console):
    assert console.send_line("FACTORY_RESET") == "Factory defaults restored"
    assert console.send_line("NEW") == ""
    assert console.send_line("10 OPTIONS WIFI") == ""
    run = console.send_line("RUN")
    assert "?WIFI not configured" in run
    assert console.send_line("NEW") == ""
    assert console.send_line('10 OPTION WIFI "ProgNet","progpass"') == ""
    assert console.send_line("20 OPTIONS WIFI") == ""
    assert console.send_line("30 PRINT 42") == ""
    run = console.send_line("RUN")
    assert "?WIFI not configured" not in run
    assert "42" in run
    assert "progpass" not in run


def test_option_wifi_debug_default_off(console):
    assert console.send_line("FACTORY_RESET") == "Factory defaults restored"
    listed = console.send_line("OPTION LIST")
    assert "WIFI DEBUG" not in listed
    all_listed = console.send_line("OPTION LIST ALL")
    assert "OPTION WIFI DEBUG OFF" in all_listed
    ini = _read_ini(console)
    assert "debug=0" in ini


def test_option_term_log_persists(console):
    assert console.send_line("FACTORY_RESET") == "Factory defaults restored"
    listed = console.send_line("OPTION LIST")
    assert "TERM LOG" not in listed
    all_listed = console.send_line("OPTION LIST ALL")
    assert "OPTION TERM LOG OFF" in all_listed
    on = console.send_line("OPTION TERM LOG ON")
    assert "TERM log" in on
    listed = console.send_line("OPTION LIST")
    assert "OPTION TERM LOG ON" in listed
    ini = _read_ini(console)
    assert "term_log=1" in ini
    off = console.send_line("OPTION TERM LOG OFF")
    assert "in=" in off
    listed = console.send_line("OPTION LIST")
    assert "TERM LOG" not in listed
    ini = _read_ini(console)
    assert "term_log=0" in ini


def test_option_wifi_debug_on_persists(console):
    assert console.send_line("OPTION WIFI DEBUG ON") == ""
    listed = console.send_line("OPTION LIST")
    assert "OPTION WIFI DEBUG ON" in listed
    ini = _read_ini(console)
    assert "debug=1" in ini
    assert console.send_line("OPTION WIFI DEBUG OFF") == ""
    listed = console.send_line("OPTION LIST")
    assert "WIFI DEBUG" not in listed
    ini = _read_ini(console)
    assert "debug=0" in ini


def test_option_wifi_country_persists(console):
    assert console.send_line("FACTORY_RESET") == "Factory defaults restored"
    assert "?SYNTAX ERROR" in console.send_line('OPTION WIFI COUNTRY "USA"').upper()
    assert "?SYNTAX ERROR" in console.send_line('OPTION WIFI COUNTRY "00"').upper()
    assert "?SYNTAX ERROR" in console.send_line('OPTION WIFI COUNTRY "ZZ"').upper()
    assert "?SYNTAX ERROR" in console.send_line('OPTION WIFI COUNTRY "WW"').upper()
    assert console.send_line('OPTION WIFI COUNTRY "nz"') == ""
    listed = console.send_line("OPTION LIST")
    assert "WIFI COUNTRY" in listed
    assert "NZ" in listed
    ini = _read_ini(console)
    assert "country=NZ" in ini
    assert console.send_line('OPTION WIFI COUNTRY "uk"') == ""
    listed = console.send_line("OPTION LIST")
    assert "GB" in listed
    ini = _read_ini(console)
    assert "country=GB" in ini
    assert console.send_line("FACTORY_RESET") == "Factory defaults restored"
    all_listed = console.send_line("OPTION LIST ALL")
    assert "WIFI COUNTRY" in all_listed
    assert "US" in all_listed


def test_option_prompt_cwd_is_default(console):
    assert console.send_line("FACTORY_RESET") == "Factory defaults restored"
    listed = console.send_line("OPTION LIST")
    assert "PROMPT" not in listed
    all_listed = console.send_line("OPTION LIST ALL")
    assert "OPTION PROMPT CWD" in all_listed
    help_opt = dump_topic(console, "OPTION")
    assert "PROMPT BARE|CWD" in help_opt or ("PROMPT" in help_opt and "CWD" in help_opt)
    help_prompt = dump_topic(console, "PROMPT")
    assert "BARE" in help_prompt
    assert "CWD" in help_prompt
    assert "$p$g" in help_prompt or "A:/>" in help_prompt
    via_option = dump_topic(console, "OPTION PROMPT")
    assert "BARE" in via_option and "CWD" in via_option
    console.drain(quiet=0.1)
    console._ser.sendall(b"PRINT 1\r")
    raw = console.drain(quiet=0.5).decode(errors="replace")
    assert "A:/>" in raw.replace("\r", "")


def test_option_prompt_cwd_shows_path(console):
    assert console.send_line("FACTORY_RESET") == "Factory defaults restored"
    listed = console.send_line("OPTION LIST ALL")
    assert "OPTION PROMPT CWD" in listed
    assert console.send_line('CHDIR "A:/"') == ""
    console.drain(quiet=0.1)
    console._ser.sendall(b"PRINT 7\r")
    raw = console.drain(quiet=0.5).decode(errors="replace")
    assert "A:/>" in raw.replace("\r", "")
    assert console.send_line('MKDIR "PRDIR"') == ""
    assert console.send_line("cd PRDIR") == ""
    console.drain(quiet=0.1)
    console._ser.sendall(b"PRINT 8\r")
    raw = console.drain(quiet=0.5).decode(errors="replace")
    assert "PRDIR>" in raw.replace("\r", "").upper()
    assert console.send_line('CHDIR "A:/"') == ""
    assert console.send_line("OPTION PROMPT BARE") == ""
    listed = console.send_line("OPTION LIST")
    assert "OPTION PROMPT BARE" in listed
    console.drain(quiet=0.1)
    console._ser.sendall(b"PRINT 9\r")
    raw = console.drain(quiet=0.5).decode(errors="replace")
    assert "A:/>" not in raw.replace("\r", "")


def test_option_prompt_cwd_persists(console):
    assert console.send_line("FACTORY_RESET") == "Factory defaults restored"
    ini = _read_ini(console)
    assert "prompt=1" in ini
    assert console.send_line("OPTION PROMPT BARE") == ""
    ini = _read_ini(console)
    assert "prompt=0" in ini
    assert console.send_line("FACTORY_RESET") == "Factory defaults restored"
    listed = console.send_line("OPTION LIST ALL")
    assert "OPTION PROMPT CWD" in listed
    ini = _read_ini(console)
    assert "prompt=1" in ini


def test_option_path_and_boot_persist(console):
    """#515/#520: app PATH and boot destination live in the settings INI."""
    assert console.send_line("FACTORY_RESET") == "Factory defaults restored"
    ini = _read_ini(console)
    assert "app_path=A:/APPS/" in ini
    assert "boot_mode=0" in ini
    assert console.send_line('OPTION PATH "A:/APPS2/"') == ""
    assert console.send_line("OPTION BOOT LAUNCHER") == ""
    ini = _read_ini(console)
    assert "app_path=A:/APPS2/" in ini
    assert "boot_mode=1" in ini
    assert console.send_line('OPTION BOOT "GAME"') == ""
    ini = _read_ini(console)
    assert "boot_mode=2" in ini
    assert "boot_app=GAME" in ini
    assert console.send_line("OPTION BOOT REPL") == ""
    ini = _read_ini(console)
    assert "boot_mode=0" in ini
    assert "boot_app=GAME" not in ini
    assert console.send_line('OPTION PATH "A:/APPS/"') == ""


def test_files_hides_dotfiles(fresh_console):
    con = fresh_console
    assert con.send_line('CHDIR "A:/"') == ""
    assert con.send_line('OPEN "A:/.secret.txt" FOR OUTPUT AS #1') == ""
    assert con.send_line('PRINT #1, "secret"') == ""
    assert con.send_line("CLOSE #1") == ""
    assert con.send_line('OPEN "A:/VISIBLE.TXT" FOR OUTPUT AS #1') == ""
    assert con.send_line('PRINT #1, "ok"') == ""
    assert con.send_line("CLOSE #1") == ""
    assert con.send_line("OPTION TAB 4") == ""
    listing = con.send_line('DIR "A:/"')
    assert "VISIBLE.TXT" in listing.upper()
    assert ".SECRET" not in listing.upper()
    assert con._ser is not None
    con.drain(quiet=0.15)
    con._ser.sendall(b"FILES\r")
    seen = con.drain(quiet=0.8).decode(errors="replace").upper()
    assert "VISIBLE.TXT" in seen
    assert ".SECRET" not in seen
    assert ".MMBASIC" not in seen
    con._ser.sendall(b"q")


# --- #590/#591: SETTINGS dialog + hub ------------------------------------


def test_settings_dialog_is_inset_not_fullscreen(console):
    """#590: SETTINGS opens a framed inset panel, not a full-screen takeover."""
    con = console
    seen = _settings_open(con)
    assert "SETTINGS" in seen.upper()
    assert _has_inset_border(seen), seen
    _settings_keys(con, b"\x1b")
    assert con.send_line("PRINT 6*7") == "42"


def test_settings_hub_lists_categories(console):
    """#591: the hub has peer category slots, Appearance first."""
    con = console
    seen = _settings_open(con).upper()
    for name in ("APPEARANCE", "NETWORK", "SYSTEM", "SOUND"):
        assert name in seen, seen
    assert "THEME" in seen, seen
    _settings_keys(con, b"\x1b")
    assert con.send_line("PRINT 1+1") == "2"


def test_settings_network_section_shows_status(console):
    """#591: Network is a real peer section wired to existing status/config."""
    con = console
    assert con.send_line("FACTORY_RESET") == "Factory defaults restored"
    _settings_open(con)
    _settings_keys(con, b"\x1b[B")  # Appearance -> Network
    seen = _settings_keys(con, b"\r", quiet=0.6).upper()
    assert "NETWORK" in seen, seen
    assert "WI-FI" in seen, seen
    assert "NTP" in seen, seen
    _settings_keys(con, b"\x1b")  # back to hub
    _settings_keys(con, b"\x1b")  # close
    assert con.send_line("PRINT 2+2") == "4"


def test_settings_appearance_theme_applies_and_persists(console):
    """#509 moves under Appearance: pick a theme, Enter saves it."""
    con = console
    assert con.send_line("OPTION THEME SLATE") == ""
    _settings_open(con)
    _settings_keys(con, b"\r")       # open Appearance
    _settings_keys(con, b"\x1b[B")   # Slate -> Forest (live preview)
    _settings_keys(con, b"\r")       # apply and close
    listed = con.send_line("OPTION LIST ALL").upper()
    assert "FOREST" in listed, listed
    assert con.send_line("FACTORY_RESET") == "Factory defaults restored"

    con.drain(quiet=0.3)


# --- #611: SETTINGS legibility across every shipped theme -----------------

SHIPPED_THEMES = [
    "Paper", "Cloud", "Snow", "Night", "Nord",
    "Slate", "Forest", "Violet", "Turbo", "Phosphor",
]


def _srgb_channel(c):
    c = c / 255.0
    return c / 12.92 if c <= 0.03928 else ((c + 0.055) / 1.055) ** 2.4


def _contrast(fg, bg):
    def luma(p):
        return (
            0.2126 * _srgb_channel(p[0])
            + 0.7152 * _srgb_channel(p[1])
            + 0.0722 * _srgb_channel(p[2])
        )

    a, b = luma(fg), luma(bg)
    if a < b:
        a, b = b, a
    return (a + 0.05) / (b + 0.05)


def _cell(col, row):
    """A pixel at the vertical middle of a text cell."""
    return (col * 8 + 2, row * 16 + 8)


def _hub_rect(con):
    """Mirror tui_dialog_geom for the 68x13 SETTINGS hub, in text cells."""
    w, h = con.screen_size()
    cols, rows = w // 8, h // 16
    pw = min(68, cols - 2)
    ph = min(13, rows - 2)
    px = max(1, (cols - pw) // 2)
    py = max(1, (rows - ph) // 2)
    return px, py, pw, ph


def test_settings_legible_in_every_theme(console):
    """#611: the hub's body text, focus and hints stay legible in every theme.

    Turbo previously drew the list with edit_fg (near-white) on dlg_bg (grey)
    and the hint keys in hot (blue) on edit_bg (blue), so the dialog washed out
    and the <key> tags vanished. Walk every shipped theme with the hub open and
    check the rendered pixels instead of trusting the role names.
    """
    con = console
    x, y, w, h = _hub_rect(con)
    row = y + 5          # Network, the second hub row
    hint_row = y + h - 2  # last interior row holds the hints
    body_at = _cell(x + w - 5, row)
    focus_at = _cell(x + w - 5, row - 1)
    name_at = [_cell(x + 5 + i, row) for i in range(8)]
    hint_bg_at = _cell(x + w - 4, hint_row)
    hint_at = [_cell(x + 2 + i, hint_row) for i in range(w - 5)]
    samples = [body_at, focus_at] + name_at + [hint_bg_at] + hint_at
    try:
        for theme in SHIPPED_THEMES:
            assert con.send_line(f"OPTION THEME {theme}") == ""
            _settings_open(con)
            # One screendump serves the whole theme.
            vals = con.screen_pixels(samples)
            body, focus = vals[0], vals[1]
            name = max(_contrast(p, body) for p in vals[2:10])
            hint_bg = vals[10]
            hint = max(_contrast(p, hint_bg) for p in vals[11:])
            assert name >= 4.5, (theme, "body text", name)
            assert focus != body, (theme, "selection")
            assert hint >= 4.5, (theme, "hints", hint)
            _settings_keys(con, b"\x1b")
            assert con.send_line("PRINT 0") == "0"
    finally:
        assert con.send_line("OPTION THEME SLATE") == ""


# --- #623: Appearance preview fits the panel and clears the hint row -------


def test_settings_appearance_preview_fits_above_hint(console):
    """The live PREVIEW swatch must sit inside the panel with its bottom
    border above the hint row, not overlapping it (#623)."""
    import re

    con = console
    _settings_open(con)
    frame = _settings_keys(con, b"\r", quiet=0.6)
    rows = _frame_rows(frame)

    title = next(i for i, ln in enumerate(rows) if "Appearance - theme" in ln)
    prev = next(i for i, ln in enumerate(rows) if "PREVIEW" in ln)
    hint = next(i for i, ln in enumerate(rows) if "<Up/Down> Preview" in ln)

    # Panel top border: the last border-only line before the section title
    # (the hub's own border precedes it in the same frame).
    top = None
    for i in range(title - 1, -1, -1):
        stripped = rows[i].strip()
        if stripped and set(stripped) <= {"+", "-"} and "+-" in stripped:
            top = i
            break
    assert top is not None, rows
    panel_right = rows[top].rindex("+")

    # Preview bottom border: first horizontal run after the title that sits
    # clear of the theme list.
    bottom = None
    for i in range(prev + 1, hint):
        m = re.search(r"\+-{10,}\+", rows[i])
        if m and m.start() >= 22:
            bottom = i
            break
    assert bottom is not None, rows
    assert bottom < hint, ("preview overlaps hint row", rows)
    assert rows[bottom].rindex("+") < panel_right, rows
    # Nothing of the swatch is drawn on the hint row.
    assert not re.search(r"\+-{2,}", rows[hint][22:]), rows

    _settings_keys(con, b"\x1b")  # back to hub
    _settings_keys(con, b"\x1b")  # close
    assert con.send_line("PRINT 6*7") == "42"

