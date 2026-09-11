"""TUI Alt menus, prompt shortcuts, clock, and CMM2 language parity."""

import os
import tempfile
import time

from harness import MMBasicConsole

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def test_prompt_up_recalls_last(console):
    assert console.send_line("PRINT 41+1") == "42"
    console.drain(quiet=0.1)
    console._ser.sendall(b"\x1b[A\r")
    raw = console.drain(quiet=0.6).decode(errors="replace")
    assert "42" in raw
    assert "PRINT 41+1" in raw.replace("\r", "")


def test_prompt_up_down_walks_history(console):
    assert console.send_line("PRINT 101") == "101"
    assert console.send_line("PRINT 202") == "202"
    console.drain(quiet=0.1)
    console._ser.sendall(b"\x1b[A\x1b[A\r")
    older = console.drain(quiet=0.7).decode(errors="replace").replace("\r", "")
    assert "PRINT 101" in older
    assert "101" in older
    console.drain(quiet=0.1)
    console._ser.sendall(b"\x1b[A\x1b[A\x1b[B\r")
    newer = console.drain(quiet=0.7).decode(errors="replace").replace("\r", "")
    assert "PRINT 202" in newer
    assert "202" in newer


def test_prompt_down_restores_draft(console):
    console.drain(quiet=0.1)
    console._ser.sendall(b"PRINT 9")
    console.drain(quiet=0.2)
    console._ser.sendall(b"\x1b[A\x1b[B\r")
    raw = console.drain(quiet=0.7).decode(errors="replace").replace("\r", "")
    assert "PRINT 9" in raw
    assert "9" in raw


def test_prompt_left_inserts_in_place(console):
    console.drain(quiet=0.1)
    # "PRINT 8" then Left, insert "1" -> "PRINT 18"
    console._ser.sendall(b"PRINT 8\x1b[D1\r")
    raw = console.drain(quiet=0.7).decode(errors="replace")
    text = raw.replace("\r", "")
    assert "18" in text
    assert console.send_line("PRINT 0") == "0"


def test_prompt_drive_and_cd(console):
    assert console.send_line("D:") == "" or console.send_line("D:").startswith("?")
    # A: always exists
    assert console.send_line("A:") == ""
    cwd = console.send_line("PRINT CWD$")
    assert cwd.upper().startswith("A:")
    assert console.send_line('MKDIR "CDT"') == ""
    assert console.send_line("cd CDT") == ""
    cwd = console.send_line("PRINT CWD$")
    assert "CDT" in cwd.upper()
    assert console.send_line('CHDIR "A:/"') == ""


def test_numbered_line_no_extra_blank(console):
    console.drain(quiet=0.15)
    console._ser.sendall(b'10 print "some code"\r')
    raw = console.drain(quiet=0.5)
    text = raw.replace(b"\r", b"")
    # Enter already echoed CR; one LF then prompt. No blank line between.
    assert b"\n\n>" not in text
    assert b">" in text


def test_immediate_command_blank_before_prompt(console):
    console.drain(quiet=0.15)
    console._ser.sendall(b"PRINT 9\r")
    text = console.drain(quiet=0.5).replace(b"\r", b"")
    assert b"9" in text
    assert b"\n\n>" in text
    console.drain(quiet=0.1)
    console._ser.sendall(b"NEW\r")
    new_out = console.drain(quiet=0.5).replace(b"\r", b"")
    assert b"\n\n>" in new_out


def test_date_time_tick_with_clock(console):
    assert console.send_line('DATE$="28-7-26"') == ""
    assert console.send_line('TIME$="12:00:00"') == ""
    assert console.send_line("PRINT DATE$") == "28-7-26"
    t0 = console.send_line("PRINT TIME$")
    assert t0 == "12:00:00"
    assert console.send_line("PAUSE 1200") == ""
    t1 = console.send_line("PRINT TIME$")
    assert t1 != t0
    assert console.send_line("PRINT DATE$") == "28-7-26"


def test_option_keyboard_repeat(console):
    assert console.send_line("OPTION KEYBOARD REPEAT 400, 80") == ""
    listing = console.send_line("OPTION LIST ALL")
    assert "KEYBOARD REPEAT" in listing
    assert "400" in listing
    assert "80" in listing


def test_option_keyboard_repeat_default_is_faster(fresh_console):
    listing = fresh_console.send_line("OPTION LIST ALL")
    compact = listing.replace(" ", "")
    assert "KEYBOARDREPEAT300,75" in compact
    hidden = fresh_console.send_line("OPTION LIST")
    assert "KEYBOARD REPEAT" not in hidden


def test_editor_alt_prefix_opens_file_menu(kernel_image):
    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        con.drain(quiet=0.1)
        con._ser.sendall(b'EDIT "ALT.BAS"\r')
        con.drain(quiet=0.8)
        con._ser.sendall(bytes([1]) + b"f")
        seen = con.drain(quiet=0.6).decode(errors="replace")
        assert "Open" in seen or "Save" in seen or "Quit" in seen
        con._ser.sendall(bytes([1]) + b"x")
        con.drain(quiet=0.5)
    finally:
        con.stop()


def test_files_alt_file_menu_aligns(fresh_console):
    con = fresh_console
    assert con.send_line('CHDIR "A:/"') == ""
    con.drain(quiet=0.1)
    con._ser.sendall(b"FILES\r")
    con.drain(quiet=0.8)
    con._ser.sendall(bytes([1]) + b"f")
    seen = con.drain(quiet=0.6).decode(errors="replace")
    assert "View" in seen or "Edit" in seen or "Copy" in seen
    con._ser.sendall(b"q")
    con.drain(quiet=0.4)
    con._ser.sendall(b"q")
    con.drain(quiet=0.4)


def test_sub_args_include_settick_sprite(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("OPTION EXPLICIT OFF") == ""
    src = [
        'OPEN "INC.INC" FOR OUTPUT AS #1',
        'PRINT #1, "CONST K = 7"',
        "CLOSE #1",
        'OPEN "GAME.BAS" FOR OUTPUT AS #1',
        'PRINT #1, "OPTION DEFAULT INTEGER"',
        'PRINT #1, "#INCLUDE ""INC.INC"""',
        'PRINT #1, "CONST True = 1, False = 0"',
        'PRINT #1, "DIM INTEGER A(3) = (1, 2, 3, 4)"',
        'PRINT #1, "SUB Add(X, Y)"',
        'PRINT #1, "  PRINT X+Y+K"',
        'PRINT #1, "END SUB"',
        'PRINT #1, "SUB Tick"',
        'PRINT #1, "END SUB"',
        'PRINT #1, "Add(3, 4)"',
        'PRINT #1, "PRINT A(0);A(3)"',
        'PRINT #1, "PRINT RGB(1,2,3,15)"',
        'PRINT #1, "BOX 0,0,8,8, , 255, 255"',
        'PRINT #1, "PAGE COPY 0, 1, B"',
        'PRINT #1, "SPRITE LOADPNG 1, ""NO.PNG"""',
        'PRINT #1, "SPRITE SHOW 1, 0, 0, 1"',
        'PRINT #1, "SPRITE HIDE 1"',
        'PRINT #1, "PLAY WAV ""NO.WAV"""',
        'PRINT #1, "SETTICK 0, Tick"',
        'PRINT #1, "PRINT KEYDOWN(0)"',
        'PRINT #1, "PRINT INKEY$"',
        "CLOSE #1",
    ]
    for line in src:
        assert console.send_line(line) == ""
    out = console.send_line('RUN "GAME.BAS"', timeout=8.0)
    assert "?SYNTAX" not in out
    assert "14" in out
    assert "14" in out.split("\n")[0] or "14" in out
    assert "0" in out  # KEYDOWN(0)


def test_cmm2_compat_commands_from_games(console):
    """Commands used by tests/cmm2_compat games run without syntax errors."""
    assert console.send_line("NEW") == ""
    assert console.send_line("OPTION EXPLICIT OFF") == ""
    lines = [
        "OPTION DEFAULT INTEGER",
        "OPTION EXPLICIT ON",
        "CONST True = 1, False = 0",
        "DIM INTEGER glove.pos.x(3) = (130, 210, 150, 170)",
        "PRINT glove.pos.x(0);glove.pos.x(3)",
        "SUB fnt.draw(T$, X, Y, N)",
        '  PRINT T$;" ";X',
        "END SUB",
        "SUB fnt.init",
        '  PRINT "init"',
        "END SUB",
        "fnt.init()",
        'fnt.draw("hi", 9, 2, 4)',
        'fnt.draw "bare", 3, 0, 1',
        "CIRCLE 10, 10, 3, , , RGB(WHITE), RGB(WHITE)",
        "RBOX 1, 1, 10, 10, 2, RGB(WHITE), RGB(WHITE)",
        'LOAD PNG "missing.png", 0, 0, 4',
        'LOAD JPG "missing.jpg", 0, 0',
        'SPRITE LOADPNG 2, "gfx/santa-whack-sm", 4',
        "SPRITE SHOW 2, 10, 10, 2",
        "SPRITE MOVE",
        "IMAGE ROTATE 0, 0, 8, 8, 0, 8, 90, 0",
        "PAGE COPY 2, 1, B",
        "PAGE XOR_PIXELS 4, 1, 1",
        "PAGE AND_PIXELS 2, 1, 1",
        'PLAY EFFECT "sound/bonk.wav"',
        'PLAY TTS "a"',
        "PRINT KEYDOWN(0)",
        'PRINT INSTR(1, "abc", "b")',
    ]
    assert console.send_line('OPEN "CMM2.BAS" FOR OUTPUT AS #1') == ""
    for line in lines:
        esc = line.replace('"', '""')
        assert console.send_line(f'PRINT #1, "{esc}"') == ""
    assert console.send_line("CLOSE #1") == ""
    out = console.send_line('RUN "CMM2.BAS"', timeout=15.0)
    assert "?SYNTAX" not in out.upper()
    assert "?TYPE" not in out.upper()
    assert "130" in out
    assert "170" in out
    assert "hi" in out.lower()
    assert "init" in out.lower()
    assert "bare" in out.lower()


def test_restore_label_and_instr_start(console):
    assert console.send_line("NEW") == ""
    console.drain(quiet=0.2)
    src = [
        "RESTORE W",
        "READ A$",
        "PRINT A$",
        'PRINT INSTR(1, "hello", "ll")',
        "W:",
        'DATA "xyz"',
    ]
    assert console.send_line('OPEN "RST.BAS" FOR OUTPUT AS #1') == ""
    for line in src:
        esc = line.replace('"', '""')
        assert console.send_line(f'PRINT #1, "{esc}"') == ""
    assert console.send_line("CLOSE #1") == ""
    out = console.send_line('RUN "RST.BAS"')
    assert out.split("\n")[0].strip() == "xyz"
    assert "3" in out.split("\n")[1]


def test_pixel_array_form(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("DIM FLOAT PX(1) = (4, 5)") == ""
    assert console.send_line("DIM FLOAT PY(1) = (6, 7)") == ""
    assert console.send_line("PIXEL PX(), PY()") == ""


def test_pixel_colour_array(fresh_console):
    c = fresh_console
    assert c.send_line("NEW") == ""
    assert c.send_line("CLS") == ""
    assert c.send_line("DIM INTEGER XX(2) = (80, 90, 100)") == ""
    assert c.send_line("DIM INTEGER YY(2) = (300, 310, 320)") == ""
    assert c.send_line("DIM INTEGER CC(2)") == ""
    assert c.send_line("CC(0) = RGB(255,0,0)") == ""
    assert c.send_line("CC(1) = RGB(0,255,0)") == ""
    assert c.send_line("CC(2) = RGB(0,0,255)") == ""
    assert c.send_line("PIXEL XX(), YY(), CC()") == ""
    red = int(c.send_line("PRINT PIXEL(80,300)"))
    green = int(c.send_line("PRINT PIXEL(90,310)"))
    blue = int(c.send_line("PRINT PIXEL(100,320)"))
    assert ((red >> 16) & 255) > 150 and ((red >> 8) & 255) < 80
    assert ((green >> 8) & 255) > 150 and ((green >> 16) & 255) < 80
    assert (blue & 255) > 150 and ((blue >> 16) & 255) < 80
    assert c.send_line("PIXEL XX(), YY(), RGB(255,255,255)") == ""
    white = int(c.send_line("PRINT PIXEL(80,300)"))
    assert ((white >> 16) & 255) > 200 and (white & 255) > 200


def _mkdir(con, path):
    out = con.send_line(f'MKDIR "{path}"')
    assert out == "" or "DIRECTORY" in out.upper()


def _print_hash1_line(con, line):
    """Write one program line to #1 without embedding a fake '> ' prompt."""
    parts = []
    buf = []

    def flush():
        if buf:
            s = "".join(buf).replace('"', '""')
            parts.append(f'"{s}"')
            buf.clear()

    for ch in line:
        if ch in ">'":
            flush()
            parts.append(f"CHR$({ord(ch)})")
        else:
            buf.append(ch)
    flush()
    expr = "+".join(parts) if parts else '""'
    assert con.send_line(f"PRINT #1, {expr}") == ""


def _upload_text_file(con, host_path, dest):
    _upload_binary_file(con, host_path, dest)


def _wait_contains(con, token: bytes, timeout: float = 10.0) -> bytes:
    deadline = time.time() + timeout
    seen = b""
    while time.time() < deadline:
        chunk = con._recv(con._ser)
        if chunk:
            seen += chunk
            if token in seen:
                return seen
        else:
            time.sleep(0.01)
    raise AssertionError(f"timeout waiting for {token!r}, saw {seen!r}")


def _wait_prompt(con, timeout=20.0) -> str:
    deadline = time.time() + timeout
    buf = b""
    while time.time() < deadline:
        chunk = con._recv(con._ser)
        if chunk:
            buf += chunk
            if buf.rstrip().endswith(b">"):
                return buf.decode(errors="replace")
        else:
            time.sleep(0.02)
    raise AssertionError(f"timeout waiting for prompt, saw {buf!r}")


def _upload_binary_file(con, host_path, dest):
    with open(host_path, "rb") as fh:
        data = fh.read()
    con.drain(quiet=0.05)
    con._ser.sendall(f'XFER "{dest}", {len(data)}\r'.encode())
    _wait_contains(con, b"<<XFER>>")
    if data:
        con._ser.sendall(data)
    raw = _wait_prompt(con, timeout=max(20.0, len(data) / 2000.0 + 10.0))
    up = raw.upper()
    assert "?SYNTAX" not in up, raw
    assert "?FILE" not in up, raw
    assert "?ERROR" not in up, raw
    assert "?UNSUPPORTED" not in up, raw


_TEXT_EXT = (".bas", ".inc")
_ASSET_EXT = (".png", ".jpg", ".jpeg", ".wav", ".mod")


def _upload_compat_tree(con, host_dir, dest_prefix):
    _mkdir(con, dest_prefix)
    for dirpath, dirnames, filenames in os.walk(host_dir):
        dirnames[:] = [d for d in dirnames if d not in (".git",)]
        rel = os.path.relpath(dirpath, host_dir)
        dest = dest_prefix if rel == "." else dest_prefix + "/" + rel.replace("\\", "/")
        if dest != dest_prefix:
            _mkdir(con, dest)
        for name in filenames:
            lower = name.lower()
            if lower.endswith(".bak"):
                continue
            src = os.path.join(dirpath, name)
            out = dest + "/" + name
            if lower.endswith(_TEXT_EXT):
                _upload_text_file(con, src, out)
            elif lower.endswith(_ASSET_EXT):
                _upload_binary_file(con, src, out)


def _upload_basic_tree(con, host_dir, dest_prefix):
    _upload_compat_tree(con, host_dir, dest_prefix)


def test_upload_binary_includes_nul(console):
    blob = bytes([0, 1, 255, 65, 0, 10])
    fd, tmp = tempfile.mkstemp(suffix=".bin")
    os.close(fd)
    try:
        with open(tmp, "wb") as fh:
            fh.write(blob)
        _upload_binary_file(console, tmp, "A:/X.BIN")
    finally:
        os.unlink(tmp)
    assert console.send_line('OPEN "A:/X.BIN" FOR INPUT AS #1') == ""
    assert console.send_line("PRINT LOF(#1)") == str(len(blob))
    assert console.send_line("CLOSE #1") == ""


def test_typing_startup_fragment(console):
    """LoadWords, dotted CONST/SUB, and DIM from array used by typing.bas."""
    assert console.send_line("NEW") == ""
    lines = [
        "option explicit",
        "option default integer",
        "dim total_words = 2",
        "dim words$(total_words - 1)",
        "sub LoadWords()",
        "  local word_idx",
        "  restore WordList",
        "  for word_idx = 0 to total_words - 1",
        "    read words$(word_idx)",
        "  next word_idx",
        "end sub",
        "LoadWords()",
        "WordList:",
        'data "cat", "dog"',
        "const fnt.page = 6",
        "dim integer fnt.width(5) = (5, 8, 8, 8, 8, 16)",
        "sub fnt.init",
        "  page write fnt.page",
        "  page write 0",
        "end sub",
        "sub fnt.draw text$, x%, y%, fntNr%",
        "  print text$;x%",
        "end sub",
        "fnt.init()",
        "dim word$ = words$(0)",
        "print word$",
        'fnt.draw "hi", 1, 2, 3',
    ]
    assert console.send_line('OPEN "TSTART.BAS" FOR OUTPUT AS #1') == ""
    for line in lines:
        _print_hash1_line(console, line)
    assert console.send_line("CLOSE #1") == ""
    out = console.send_line('RUN "TSTART.BAS"', timeout=6.0)
    assert "?SYNTAX" not in out.upper()
    assert "?UNDECLARED" not in out.upper()
    assert "cat" in out.lower()
    assert "hi" in out.lower()


def test_sub_nine_args(console):
    assert console.send_line("NEW") == ""
    src = [
        "SUB sprite.configure(spriteNr%, file$, pageNr%, offsetX%, offsetY%, cols%, rows%, width%, height%)",
        "  PRINT height%;file$",
        "END SUB",
        'sprite.configure 1, "a", 2, 3, 4, 5, 6, 7, 8',
    ]
    assert console.send_line('OPEN "S9.BAS" FOR OUTPUT AS #1') == ""
    for line in src:
        _print_hash1_line(console, line)
    assert console.send_line("CLOSE #1") == ""
    out = console.send_line('RUN "S9.BAS"')
    assert "?SYNTAX" not in out.upper()
    assert "8" in out
    assert "a" in out.lower()


def test_then_return_from_sub(console):
    assert console.send_line("NEW") == ""
    src = [
        "SUB T",
        "  IF 1 THEN RETURN",
        "  PRINT 9",
        "END SUB",
        "T",
        "PRINT 42",
    ]
    assert console.send_line('OPEN "RET.BAS" FOR OUTPUT AS #1') == ""
    for line in src:
        _print_hash1_line(console, line)
    assert console.send_line("CLOSE #1") == ""
    out = console.send_line('RUN "RET.BAS"')
    assert "9" not in out.split()
    assert "42" in out


def test_local_string_array_assign_under_explicit(console):
    """GH-274: Menu.Screen dies on label$ = items$(menu_idx)."""
    assert console.send_line("NEW") == ""
    src = [
        "OPTION EXPLICIT",
        "OPTION DEFAULT INTEGER",
        "SUB Menu.Screen()",
        "  LOCAL pressed$",
        "  LOCAL last_pressed$",
        "  LOCAL active_item = 0",
        "  LOCAL label$",
        "  LOCAL menu_idx = 0",
        '  LOCAL items$(3) = ("start kids mode", "start", "instructions", "quit")',
        "  FOR menu_idx = 0 TO 3",
        "    label$ = items$(menu_idx)",
        "    PRINT label$",
        "  NEXT menu_idx",
        "END SUB",
        "Menu.Screen()",
    ]
    assert console.send_line('OPEN "MENU.BAS" FOR OUTPUT AS #1') == ""
    for line in src:
        _print_hash1_line(console, line)
    assert console.send_line("CLOSE #1") == ""
    out = console.send_line('RUN "MENU.BAS"')
    assert "?UNDECLARED" not in out.upper(), out
    assert "?SYNTAX" not in out.upper(), out
    low = out.lower()
    assert "start kids mode" in low, out
    assert "quit" in low, out


def test_restore_data_after_sub_and_include(console):
    """GH-274: SyntaxShock's words_long.inc pattern — RESTORE label, READ
    into a string array, DATA after the SUB and the LoadWords() call."""
    assert console.send_line("NEW") == ""
    _mkdir(console, "A:/ss")
    inc = [
        "DIM total_words = 3",
        "DIM words$(total_words - 1)",
        "SUB LoadWords()",
        "  LOCAL word_idx",
        "  RESTORE WordList",
        "  FOR word_idx = 0 TO total_words - 1",
        "    READ words$(word_idx)",
        "  NEXT word_idx",
        "END SUB",
        "LoadWords()",
        "WordList:",
        'DATA "ant", "bat", "rabbit"',
    ]
    assert console.send_line('OPEN "A:/ss/words.inc" FOR OUTPUT AS #1') == ""
    for line in inc:
        _print_hash1_line(console, line)
    assert console.send_line("CLOSE #1") == ""
    src = [
        "OPTION EXPLICIT",
        "OPTION DEFAULT INTEGER",
        '#include "A:/ss/words.inc"',
        "PRINT words$(0)",
        "PRINT words$(2)",
        "PRINT total_words",
    ]
    assert console.send_line('OPEN "A:/ss/dump.bas" FOR OUTPUT AS #1') == ""
    for line in src:
        _print_hash1_line(console, line)
    assert console.send_line("CLOSE #1") == ""
    out = console.send_line('RUN "A:/ss/dump.bas"', timeout=10.0)
    lines = [ln.strip() for ln in out.replace("\r", "").split("\n") if ln.strip()]
    assert "?NO DATA" not in out.upper(), out
    assert "?OUT OF DATA" not in out.upper(), out
    assert "?LABEL" not in out.upper(), out
    assert "?SYNTAX" not in out.upper(), out
    assert lines[0] == "ant", out
    assert lines[1] == "rabbit", out
    assert lines[2] == "3", out


def test_syntaxshock_words_long_inc(console):
    """GH-274: load the real 200-word DATA list the game includes."""
    host = os.path.join(REPO, "tests", "cmm2_compat", "syntaxshock")
    _mkdir(console, "A:/syntaxshock")
    _mkdir(console, "A:/syntaxshock/libs")
    _upload_text_file(
        console,
        os.path.join(host, "libs", "words_long.inc"),
        "A:/syntaxshock/libs/words_long.inc",
    )
    src = [
        "OPTION EXPLICIT",
        "OPTION DEFAULT INTEGER",
        '#include "libs/words_long.inc"',
        "PRINT words$(0)",
        "PRINT words$(199)",
        "PRINT total_words",
    ]
    assert console.send_line('OPEN "A:/syntaxshock/dump.bas" FOR OUTPUT AS #1') == ""
    for line in src:
        _print_hash1_line(console, line)
    assert console.send_line("CLOSE #1") == ""
    out = console.send_line('RUN "A:/syntaxshock/dump.bas"', timeout=15.0)
    lines = [ln.strip() for ln in out.replace("\r", "").split("\n") if ln.strip()]
    assert "?NO DATA" not in out.upper(), out
    assert "?OUT OF DATA" not in out.upper(), out
    assert "?LABEL" not in out.upper(), out
    assert lines[0] == "ant", out
    assert lines[1] == "rabbit", out
    assert lines[2] == "200", out


def test_unnumbered_file_leading_digit_is_line_number(console):
    """typing.bas has `1  local char_x, char_y` inside Draw_Word; a leading
    digit plus space is stored as that line number, not as body text."""
    assert console.send_line("NEW") == ""
    src = [
        'PRINT "first"',
        '1 PRINT "line1"',
        'PRINT "after"',
    ]
    assert console.send_line('OPEN "LN.BAS" FOR OUTPUT AS #1') == ""
    for line in src:
        _print_hash1_line(console, line)
    assert console.send_line("CLOSE #1") == ""
    out = console.send_line('RUN "LN.BAS"')
    lines = [ln.strip() for ln in out.replace("\r", "").split("\n") if ln.strip()]
    assert lines[0] == "line1", out
    assert "first" in lines
    assert "after" in lines


def test_cmm2_compat_syntaxshock_runs(console):
    host = os.path.join(REPO, "tests", "cmm2_compat", "syntaxshock")
    _upload_compat_tree(console, host, "A:/syntaxshock")
    listing = console.send_line('DIR "A:/syntaxshock/gfx"')
    assert "FONTS.PNG" in listing.upper()
    os.makedirs("/opt/cursor/artifacts", exist_ok=True)
    console.drain(quiet=0.1, timeout=0.4)
    console._ser.sendall(b'RUN "A:/syntaxshock/typing.bas"\r')
    time.sleep(6.0)
    menu_png = console.capture_png("/opt/cursor/artifacts/issue274_syntaxshock_menu.png")
    console._ser.sendall(b"\r")
    time.sleep(3.0)
    play_png = console.capture_png("/opt/cursor/artifacts/issue274_syntaxshock_play.png")
    console._ser.sendall(b"\x03")
    deadline = time.time() + 4.0
    buf = b""
    while time.time() < deadline:
        chunk = console._recv(console._ser)
        if chunk:
            buf += chunk
            if buf.rstrip().endswith(b">"):
                break
    out = buf.decode(errors="replace")
    log_path = "/opt/cursor/artifacts/issue274_syntaxshock_run.log"
    with open(log_path, "w") as fh:
        fh.write(out)
    up = out.upper()
    assert "?SYNTAX" not in up, out
    assert "?FILE" not in up, out
    assert "?PNG" not in up, out
    assert "?TYPE MISMATCH" not in up, out
    assert "?UNDECLARED" not in up, out
    assert "?INVALID" not in up, out
    assert "?NOT AN ARRAY" not in up, out
    assert "?LABEL" not in up, out
    assert "?NO DATA" not in up, out
    assert "?OUT OF DATA" not in up, out
    w0 = console.send_line("PRINT words$(0)")
    w199 = console.send_line("PRINT words$(199)")
    with open(log_path, "a") as fh:
        fh.write("\n--- after break ---\n")
        fh.write("words$(0)=" + w0 + "\n")
        fh.write("words$(199)=" + w199 + "\n")
    assert os.path.isfile(menu_png)
    assert os.path.isfile(play_png)


def test_cmm2_compat_xmas_runs(console):
    host = os.path.join(REPO, "tests", "cmm2_compat", "xmas")
    _upload_compat_tree(console, host, "A:/xmas")
    gfx = console.send_line('DIR "A:/xmas/gfx"')
    snd = console.send_line('DIR "A:/xmas/sound"')
    assert "FONTS.PNG" in gfx.upper()
    assert "HOME.PNG" in gfx.upper()
    assert "BONK.WAV" in snd.upper()
    console.drain(quiet=0.1, timeout=0.4)
    console._ser.sendall(b'RUN "A:/xmas/main.bas"\r')
    time.sleep(12.0)
    png = console.capture_png("/opt/cursor/artifacts/issue243_xmas_menu.png")
    console._ser.sendall(b"\x03")
    deadline = time.time() + 4.0
    buf = b""
    while time.time() < deadline:
        chunk = console._recv(console._ser)
        if chunk:
            buf += chunk
            if buf.rstrip().endswith(b">"):
                break
    out = buf.decode(errors="replace")
    up = out.upper()
    assert "?SYNTAX" not in up, out
    assert "?FILE" not in up, out
    assert "?PNG" not in up, out
    assert "?UNDECLARED" not in up, out
    assert "?SUBSCRIPT" not in up, out
    assert "?NOT AN ARRAY" not in up, out
    assert os.path.isfile(png)


def test_cmm2_compat_xmas_font_blit_png(fresh_console):
    """Issue #263: uploaded fonts.png loads and BLITs without ?PNG/?FILE."""
    c = fresh_console
    host = os.path.join(REPO, "tests", "cmm2_compat", "xmas")
    _mkdir(c, "A:/xmas")
    _mkdir(c, "A:/xmas/gfx")
    _upload_binary_file(c, os.path.join(host, "gfx", "fonts.png"), "A:/xmas/gfx/fonts.png")
    src = [
        'CHDIR "A:/xmas"',
        "MODE 7,12",
        "PAGE WRITE 6",
        "CLS",
        'LOAD PNG "gfx/fonts.png"',
        "PAGE WRITE 0",
        "CLS RGB(0,0,40)",
        "BLIT 0,0,16,16,48,48,6,4",
        "PAUSE 1500",
    ]
    assert c.send_line("NEW") == ""
    assert c.send_line('OPEN "FNT.BAS" FOR OUTPUT AS #1') == ""
    for line in src:
        _print_hash1_line(c, line)
    assert c.send_line("CLOSE #1") == ""
    c.drain(quiet=0.1)
    c._ser.sendall(b'RUN "FNT.BAS"\r')
    time.sleep(0.6)
    png = c.capture_png("/opt/cursor/artifacts/issue263_xmas_fonts.png")
    c.send_keys(b"\x03", timeout=6.0)
    pix = int(c.send_line("PRINT PIXEL(6,0,6)").split()[0])
    assert pix != 0
    assert os.path.isfile(png)
    assert c.send_line("PRINT 1+1") == "2"


def test_cmm2_load_png_trans_keeps_write_page(fresh_console):
    """Issue #243: LOAD PNG x,y,colour is CMM2 transparency, not a page."""
    c = fresh_console
    host = os.path.join(REPO, "tests", "cmm2_compat", "xmas", "gfx", "fonts.png")
    _mkdir(c, "A:/xmas")
    _mkdir(c, "A:/xmas/gfx")
    _upload_binary_file(c, host, "A:/xmas/gfx/fonts.png")
    src = [
        'CHDIR "A:/xmas"',
        "MODE 7,12",
        "PAGE WRITE 6",
        "CLS",
        'LOAD PNG "gfx/fonts.png", 0, 0, 4',
        "PAGE WRITE 0",
        "CLS RGB(0,0,40)",
        "BLIT 0,0,16,16,64,48,6,4",
        "PAUSE 1500",
    ]
    assert c.send_line("NEW") == ""
    assert c.send_line('OPEN "FNT4.BAS" FOR OUTPUT AS #1') == ""
    for line in src:
        _print_hash1_line(c, line)
    assert c.send_line("CLOSE #1") == ""
    c.drain(quiet=0.1)
    c._ser.sendall(b'RUN "FNT4.BAS"\r')
    time.sleep(0.6)
    png = c.capture_png("/opt/cursor/artifacts/issue243_xmas_fonts.png")
    c.send_keys(b"\x03", timeout=6.0)
    pix = int(c.send_line("PRINT PIXEL(6,0,6)").split()[0])
    assert pix != 0
    assert os.path.isfile(png)
