"""MMBasic language, OPTION, types, files, loaders, audio, editor tests."""

import subprocess

import pytest


def test_integer_type(console):
    assert console.send_line("A% = 40") == ""
    assert console.send_line("PRINT A%+2") == "42"


def test_string_type(console):
    assert console.send_line('A$ = "MM"') == ""
    assert console.send_line('PRINT A$+"BASIC"') == "MMBASIC"


def test_float_default(console):
    assert console.send_line("X = 1.5") == ""
    assert console.send_line("PRINT X+X") == "3"


def test_array(console):
    assert console.send_line("DIM A(2)") == ""
    assert console.send_line("A(0)=10") == ""
    assert console.send_line("A(1)=20") == ""
    assert console.send_line("A(2)=12") == ""
    assert console.send_line("PRINT A(0)+A(1)+A(2)") == "42"


def test_option_base_1(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("OPTION BASE 1") == ""
    assert console.send_line("DIM B(2)") == ""
    assert console.send_line("B(1)=5") == ""
    assert console.send_line("B(2)=7") == ""
    assert console.send_line("PRINT B(1)+B(2)") == "12"


def test_option_default_integer(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("OPTION DEFAULT INTEGER") == ""
    assert console.send_line("N = 21") == ""
    assert console.send_line("PRINT N*2") == "42"


def test_option_explicit(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("OPTION EXPLICIT") == ""
    assert console.send_line("Z = 1") == "?UNDECLARED"


def test_option_list(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("OPTION RESET") == ""
    out = console.send_line("OPTION LIST")
    assert "No options set" in out or "OPTION" in out


def test_option_angle_degrees(console):
    assert console.send_line("OPTION ANGLE DEGREES") == ""
    # SIN 90 degrees is 1
    assert console.send_line("PRINT INT(SIN(90)+0.5)") == "1"


def test_option_tab_and_break(console):
    assert console.send_line("OPTION TAB 4") == ""
    assert console.send_line("OPTION BREAK 4") == ""
    assert console.send_line("OPTION LIST") != "?SYNTAX ERROR"


def test_remaining_option_subcommands(console):
    cmds = [
        "OPTION AUTORUN ON",
        "OPTION COLOURCODE ON",
        "OPTION CONSOLE BOTH",
        "OPTION CRLF CRLF",
        "OPTION BAUDRATE 115200",
        "OPTION CASE UPPER",
        "OPTION LEGACY OFF",
        "OPTION MILLISECONDS OFF",
        "OPTION STATUS ON",
        "OPTION VCC 3.3",
        "OPTION SLEEP 0",
        "OPTION SD TIMING NORMAL",
        "OPTION SERIAL PULLUP DISABLE",
        "OPTION PIN 0",
        "OPTION SEARCH PATH \"/\"",
        "OPTION F1 \"LIST\"",
        "OPTION DEFAULT MODE 8",
        "OPTION Y_AXIS DOWN",
        "OPTION EDIT FONT NORMAL",
        "OPTION USBKEYBOARD US",
        "OPTION RESET",
    ]
    for cmd in cmds:
        assert console.send_line(cmd) != "?SYNTAX ERROR", cmd


def test_mode_and_resolution(console):
    modes = [
        (1, 8, 800, 600),
        (2, 8, 640, 400),
        (3, 8, 320, 200),
        (4, 8, 480, 432),
        (5, 8, 240, 216),
        (6, 8, 256, 240),
        (7, 8, 320, 240),
        (8, 16, 640, 480),
        (10, 8, 848, 480),
        (13, 8, 400, 300),
        (17, 8, 384, 240),
    ]
    for mode, bits, w, h in modes:
        assert console.send_line(f"MODE {mode},{bits}") == ""
        assert console.send_line("PRINT MM.HRES") == str(w)
        assert console.send_line("PRINT MM.VRES") == str(h)
    assert console.send_line("MODE 8,16") == ""


def test_graphics_bitdepths(console):
    for bits in (8, 12, 16, 32):
        assert console.send_line(f"MODE 8,{bits}") == ""
        assert console.send_line("CLS") == ""
        assert console.send_line("PIXEL 4,4,RGB(255,0,0)") == ""
        pix = int(console.send_line("PRINT PIXEL(4,4)"))
        assert ((pix >> 16) & 255) > 80
    assert console.send_line("MODE 8,16") == ""


def test_rgb_and_pixel_function(fresh_console):
    fresh_console.send_line("CLS")
    fresh_console.send_line("PIXEL 10,10,RGB(255,0,0)")
    out = fresh_console.send_line("PRINT PIXEL(10,10)")
    assert out != "?SYNTAX ERROR"
    assert int(out) != 0


def test_box_fill_and_rbox(fresh_console):
    fresh_console.send_line("CLS")
    assert fresh_console.send_line("BOX 20,20,40,40,1,RGB(0,255,0),1") == ""
    assert fresh_console.send_line("RBOX 80,20,40,40,6,RGB(0,0,255)") == ""
    assert fresh_console.send_line("TRIANGLE 10,80,40,80,25,50,RGB(255,255,0)") == ""


def test_files_mkdir_copy_rename(console):
    assert console.send_line('MKDIR "DATA"') == ""
    assert console.send_line('CHDIR "DATA"') == ""
    assert console.send_line('OPEN "A.TXT" FOR OUTPUT AS #1') == ""
    assert console.send_line('PRINT #1, "HELLO"') == ""
    assert console.send_line("CLOSE #1") == ""
    listing = console.send_line("DIR")
    assert "A.TXT" in listing
    assert console.send_line('COPY "A.TXT" TO "B.TXT"') == ""
    assert console.send_line('RENAME "B.TXT" AS "C.TXT"') == ""
    listing = console.send_line("DIR")
    assert "C.TXT" in listing
    assert console.send_line('MV "C.TXT" TO "D.TXT"') == ""
    listing = console.send_line("DIR")
    assert "D.TXT" in listing
    assert "C.TXT" not in listing
    assert console.send_line('CHDIR "/"') == ""


def test_load_png(fresh_console):
    assert fresh_console.send_line("CLS") == ""
    assert fresh_console.send_line('LOAD PNG "TEST.PNG"') == ""
    pix = int(fresh_console.send_line("PRINT PIXEL(0,0)"))
    r = (pix >> 16) & 255
    assert r > 150


def test_load_jpeg(fresh_console):
    assert fresh_console.send_line("CLS") == ""
    assert fresh_console.send_line('LOAD JPG "TEST.JPG"') == ""
    pix = int(fresh_console.send_line("PRINT PIXEL(1,1)"))
    r = (pix >> 16) & 255
    assert r > 80


def test_play_mp3(console):
    assert console.send_line('PLAY MP3 "TEST.MP3"') == ""
    playing = console.send_line("PRINT PLAYING()")
    assert playing in ("0", "1")
    console.send_line("PLAY STOP")
    assert console.send_line("PRINT PLAYING()") == "0"


def test_play_mod(console):
    assert console.send_line('PLAY MODFILE "TEST.MOD"') == ""
    assert console.send_line("PRINT PLAYING()") == "1"
    console.send_line("PLAY STOP")


def test_play_xm(console):
    assert console.send_line('PLAY XM "TEST.XM"') == ""
    assert console.send_line("PRINT PLAYING()") == "1"
    console.send_line("PLAY STOP")
    assert console.send_line("PRINT PLAYING()") == "0"


def test_for_next_numbered(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("10 FOR I=1 TO 3") == ""
    assert console.send_line("20 PRINT I;") == ""
    assert console.send_line("30 NEXT I") == ""
    out = console.send_line("RUN")
    assert "1" in out and "2" in out and "3" in out


def test_print_newline_unless_semicolon(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("10 FOR I=0 TO 2") == ""
    assert console.send_line("20 PRINT I") == ""
    assert console.send_line("30 NEXT I") == ""
    assert console.send_line("RUN") == "0\n1\n2"

    assert console.send_line("NEW") == ""
    assert console.send_line("10 FOR I=0 TO 2") == ""
    assert console.send_line("20 PRINT I;") == ""
    assert console.send_line("30 NEXT I") == ""
    assert console.send_line("RUN") == "012"


def test_data_read_restore(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("10 DATA 10,20,12") == ""
    assert console.send_line("20 READ A,B,C") == ""
    assert console.send_line("30 PRINT A+B+C") == ""
    assert console.send_line("RUN") == "42"


def test_goto_gosub(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("10 GOTO 30") == ""
    assert console.send_line('20 PRINT "NO"') == ""
    assert console.send_line("30 GOSUB 50") == ""
    assert console.send_line("40 END") == ""
    assert console.send_line('50 PRINT 6*7') == ""
    assert console.send_line("60 RETURN") == ""
    assert console.send_line("RUN") == "42"


def test_while_wend(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("10 I=0") == ""
    assert console.send_line("20 WHILE I<3") == ""
    assert console.send_line("30 I=I+1") == ""
    assert console.send_line("40 PRINT I;") == ""
    assert console.send_line("50 WEND") == ""
    out = console.send_line("RUN")
    assert "1" in out and "2" in out and "3" in out


def test_const_and_dim_as(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("CONST MAX=21") == ""
    assert console.send_line("PRINT MAX*2") == "42"
    assert console.send_line("DIM N AS INTEGER") == ""
    assert console.send_line("N=6") == ""
    assert console.send_line("PRINT N*7") == "42"


def test_string_functions(console):
    assert console.send_line('PRINT LEFT$("MMBASIC",2)') == "MM"
    assert console.send_line('PRINT MID$("ABCDEF",3,2)') == "CD"
    assert console.send_line('PRINT INSTR("HELLO","LL")') == "3"
    assert console.send_line("PRINT HEX$(255)") == "FF"
    assert console.send_line("PRINT MM.DEVICE$") == "Colour Maximite 2"


def test_option_list_shows_settings(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("OPTION RESET") == ""
    assert console.send_line("OPTION AUTORUN ON") == ""
    assert console.send_line("OPTION TAB 4") == ""
    out = console.send_line("OPTION LIST")
    assert "AUTORUN" in out
    assert "TAB" in out
    assert console.send_line("OPTION LIST ALL") != "?SYNTAX ERROR"
    assert console.send_line("OPTION RESET") == ""


def test_option_hardware_parse(console):
    cmds = [
        "OPTION ERROR CONTINUE",
        "OPTION ERROR ABORT",
        "OPTION AUDIO ON",
        "OPTION PROMPT CWD",
        "OPTION PROMPT BARE",
        "OPTION HEARTBEAT OFF",
        "OPTION CPUSPEED 504000",
        "OPTION RESOLUTION 640x480",
        "OPTION DISPLAY DISABLE",
        "OPTION WIFI \"x\",\"y\"",
        "OPTION WIFI DEBUG ON",
        "OPTION WIFI DEBUG OFF",
        "OPTION TOUCH DISABLE",
        "OPTION LCDPANEL ILI9341",
        "OPTION CLOCK DS3231",
        "OPTION SDCARD GP22,GP26,GP27,GP28",
    ]
    for cmd in cmds:
        assert console.send_line(cmd) != "?SYNTAX ERROR", cmd


def test_remaining_modes(console):
    extras = [
        (9, 8, 1024, 768),
        (11, 8, 1280, 720),
        (12, 8, 960, 540),
        (14, 8, 960, 540),
        (15, 8, 1280, 1024),
        (16, 8, 1920, 1080),
    ]
    for mode, bits, w, h in extras:
        assert console.send_line(f"MODE {mode},{bits}") == ""
        assert console.send_line("PRINT MM.HRES") == str(w)
        assert console.send_line("PRINT MM.VRES") == str(h)
    assert console.send_line("MODE 8,16") == ""


def test_page_copy_and_colour(fresh_console):
    c = fresh_console
    assert c.send_line("CLS") == ""
    assert c.send_line("COLOUR RGB(255,0,0)") == ""
    assert c.send_line("PIXEL 12,12") == ""
    pix = int(c.send_line("PRINT PIXEL(12,12)"))
    assert ((pix >> 16) & 255) > 150
    assert c.send_line("PAGE COPY 0 TO 1") == ""
    assert c.send_line("PAGE WRITE 1") == ""
    pix2 = int(c.send_line("PRINT PIXEL(12,12)"))
    assert ((pix2 >> 16) & 255) > 150
    assert c.send_line("PAGE WRITE 0") == ""


def test_colour_rgb_red_print_text(fresh_console):
    c = fresh_console
    assert c.send_line("NEW") == ""
    assert c.send_line("10 CLS") == ""
    assert c.send_line("20 COLOR RGB(RED)") == ""
    assert c.send_line('30 PRINT "XXXX"') == ""
    out = c.send_line("RUN")
    assert "XXXX" in out
    png = c.capture_png()
    txt = subprocess.run(
        ["convert", png, "-crop", "48x20+0+0", "+repage", "txt:-"],
        check=True,
        capture_output=True,
        text=True,
    ).stdout
    found = False
    for line in txt.splitlines():
        if "(" not in line:
            continue
        nums = line[line.find("(") + 1 : line.find(")")].split(",")
        if len(nums) < 3:
            continue
        r, g, b = (int(float(n)) for n in nums[:3])
        if r > 150 and g < 130 and b < 130:
            found = True
            break
    assert found, txt[:800]


def test_text_draws_pixels(fresh_console):
    c = fresh_console
    c.send_line("CLS")
    assert c.send_line('TEXT 8,8,"A",RGB(255,255,255)') == ""
    pix = int(c.send_line("PRINT PIXEL(10,8)"))
    if pix == 0:
        pix = int(c.send_line("PRINT PIXEL(11,8)"))
    assert pix != 0


def test_box_fill_pixels(fresh_console):
    c = fresh_console
    c.send_line("CLS")
    assert c.send_line("BOX 10,10,20,20,1,RGB(0,255,0),RGB(0,255,0)") == ""
    pix = int(c.send_line("PRINT PIXEL(20,20)"))
    assert ((pix >> 8) & 255) > 150


def test_files_kill_and_cwd(console):
    assert console.send_line('CHDIR "/"') == ""
    assert console.send_line('MKDIR "TMPDIR"') == ""
    assert console.send_line('CHDIR "TMPDIR"') == ""
    cwd = console.send_line("PRINT CWD$")
    assert "TMPDIR" in cwd.upper()
    assert console.send_line('OPEN "Z.TXT" FOR OUTPUT AS #2') == ""
    assert console.send_line('PRINT #2, "X"') == ""
    assert console.send_line("CLOSE #2") == ""
    listing = console.send_line("DIR")
    assert "Z.TXT" in listing
    assert console.send_line('KILL "Z.TXT"') == ""
    listing = console.send_line("DIR")
    assert "Z.TXT" not in listing
    assert console.send_line('OPEN "R.TXT" FOR OUTPUT AS #2') == ""
    assert console.send_line('PRINT #2, "Y"') == ""
    assert console.send_line("CLOSE #2") == ""
    assert console.send_line('RM "R.TXT"') == ""
    listing = console.send_line("DIR")
    assert "R.TXT" not in listing
    assert console.send_line('OPEN "D.TXT" FOR OUTPUT AS #2') == ""
    assert console.send_line('PRINT #2, "Z"') == ""
    assert console.send_line("CLOSE #2") == ""
    assert console.send_line('DEL "D.TXT"') == ""
    listing = console.send_line("DIR")
    assert "D.TXT" not in listing
    assert console.send_line('CHDIR "/"') == ""
    assert console.send_line('RMDIR "TMPDIR"') == ""


def test_save_and_input_hash(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("10 PRINT 42") == ""
    assert console.send_line('SAVE "P.BAS"') == ""
    listing = console.send_line("DIR")
    assert "P.BAS" in listing
    assert console.send_line('OPEN "N.TXT" FOR OUTPUT AS #1') == ""
    assert console.send_line('PRINT #1, "42"') == ""
    assert console.send_line("CLOSE #1") == ""
    assert console.send_line('OPEN "N.TXT" FOR INPUT AS #1') == ""
    assert console.send_line("INPUT #1, N") == ""
    assert console.send_line("PRINT N") == "42"
    assert console.send_line("CLOSE #1") == ""


def test_editor_write_and_run(kernel_image):
    from harness import MMBasicConsole

    con = MMBasicConsole(kernel_image)
    con.start()
    try:
        con.drain(quiet=0.2)
        # enter editor
        con._ser.sendall(b'EDIT "HI.BAS"\r')
        seen = con.drain(quiet=0.8).decode(errors="replace")
        assert "nano" in seen.lower() or "EDIT" in seen or "^O" in seen or "File:" in seen
        # type a one-line program
        con._ser.sendall(b'PRINT 6*7')
        con._ser.sendall(bytes([15]))  # Ctrl+O write
        con.drain(quiet=0.4)
        con._ser.sendall(bytes([24]))  # Ctrl+X exit
        con.drain(quiet=0.4)
        result = con.send_line('RUN "HI.BAS"')
        assert "42" in result
    finally:
        con.stop()
