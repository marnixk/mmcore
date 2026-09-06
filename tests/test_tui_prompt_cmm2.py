"""TUI Alt menus, prompt shortcuts, clock, and CMM2 language parity."""

import os
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
        con._ser.sendall(bytes([24]))
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
    with open(host_path, "r", encoding="utf-8", errors="replace") as fh:
        lines = fh.read().splitlines()
    assert con.send_line(f'OPEN "{dest}" FOR OUTPUT AS #1') == ""
    for line in lines:
        _print_hash1_line(con, line)
    assert con.send_line("CLOSE #1") == ""


def _upload_basic_tree(con, host_dir, dest_prefix):
    _mkdir(con, dest_prefix)
    for dirpath, dirnames, filenames in os.walk(host_dir):
        rel = os.path.relpath(dirpath, host_dir)
        dest = dest_prefix if rel == "." else dest_prefix + "/" + rel.replace("\\", "/")
        if dest != dest_prefix:
            _mkdir(con, dest)
        for name in filenames:
            if not name.lower().endswith((".bas", ".inc")):
                continue
            _upload_text_file(
                con, os.path.join(dirpath, name), dest + "/" + name
            )


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


def _run_until_break(con, path, timeout=4.0):
    con.drain(quiet=0.1, timeout=0.4)
    con._ser.sendall(f'RUN "{path}"\r'.encode())
    time.sleep(timeout)
    con._ser.sendall(b"\x03")
    deadline = time.time() + 3.0
    buf = b""
    while time.time() < deadline:
        chunk = con._recv(con._ser)
        if chunk:
            buf += chunk
            if buf.rstrip().endswith(b">"):
                break
    return buf.decode(errors="replace")


def test_cmm2_compat_syntaxshock_runs(console):
    host = os.path.join(REPO, "tests", "cmm2_compat", "syntaxshock")
    _upload_basic_tree(console, host, "A:/syntaxshock")
    out = _run_until_break(console, "A:/syntaxshock/typing.bas")
    up = out.upper()
    assert "?SYNTAX" not in up
    assert "?TYPE MISMATCH" not in up
    assert "?UNDECLARED" not in up
    assert "?INVALID" not in up
    assert "?NOT AN ARRAY" not in up
    assert "?LABEL" not in up
    assert "?NO DATA" not in up


def test_cmm2_compat_xmas_runs(console):
    host = os.path.join(REPO, "tests", "cmm2_compat", "xmas")
    _upload_basic_tree(console, host, "A:/xmas")
    out = _run_until_break(console, "A:/xmas/main.bas", timeout=14.0)
    up = out.upper()
    assert "?SYNTAX" not in up
    assert "?TYPE MISMATCH" not in up
    assert "?UNDECLARED" not in up
    assert "?INVALID" not in up
    assert "?NOT AN ARRAY" not in up
    assert "?LABEL" not in up
    assert "?GOSUB" not in up
