"""Frozen language corpus for interpreter migrations (#144).

These tests freeze printed MMBasic text and error transcripts on the
current ASCII/`mmb_match` interpreter. Tokenize (#132), jump tables (#133),
cheaper values (#134), and any later compiler must keep this suite green.

Assertions compare printed text, not QEMU duration.

Gaps on today’s interpreter (not failing tests): STATIC is DIM (no
persist-across-CALL); LOAD is graphics (`?PNG`), so program reload is
`RUN "file.BAS"` after SAVE; DATA on the same line as a label is not
READ (RESTORE label then the next DATA line).
"""

import time

from harness import MMBasicConsole


def _write_bas(console: MMBasicConsole, path: str, lines: list[str]) -> None:
    assert console.send_line(f'OPEN "{path}" FOR OUTPUT AS #1') == ""
    for line in lines:
        esc = line.replace('"', '""')
        assert console.send_line(f'PRINT #1, "{esc}"') == ""
    assert console.send_line("CLOSE #1") == ""


def test_immediate_and_run_same_print(console):
    assert console.send_line("PRINT 1+1") == "2"
    assert console.send_line("NEW") == ""
    assert console.send_line("10 PRINT 1+1") == ""
    assert console.send_line("RUN") == "2"


def test_for_print_semicolon_exact(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("10 FOR I=1 TO 3") == ""
    assert console.send_line("20 PRINT I;") == ""
    assert console.send_line("30 NEXT I") == ""
    assert console.send_line("RUN") == "123"


def test_while_print_semicolon_exact(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("10 I=0") == ""
    assert console.send_line("20 WHILE I<3") == ""
    assert console.send_line("30 I=I+1") == ""
    assert console.send_line("40 PRINT I;") == ""
    assert console.send_line("50 WEND") == ""
    assert console.send_line("RUN") == "123"


def test_do_loop_until_exact(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("10 I=0") == ""
    assert console.send_line("20 DO") == ""
    assert console.send_line("30 I=I+1") == ""
    assert console.send_line("40 PRINT I;") == ""
    assert console.send_line("50 LOOP UNTIL I=3") == ""
    assert console.send_line("RUN") == "123"


def test_nested_for_exact(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("10 FOR I=1 TO 2") == ""
    assert console.send_line("20 FOR J=1 TO 2") == ""
    assert console.send_line("30 PRINT I;J;") == ""
    assert console.send_line("40 NEXT J") == ""
    assert console.send_line("50 NEXT I") == ""
    assert console.send_line("RUN") == "11122122"


def test_for_unnamed_next_and_step(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("10 FOR I=5 TO 1 STEP -2") == ""
    assert console.send_line("20 PRINT I;") == ""
    assert console.send_line("30 NEXT") == ""
    assert console.send_line("40 PRINT I") == ""
    assert console.send_line("RUN") == "531-1"


def test_array_let_mul_add_and_wrap_if(console):
    _write_bas(
        console,
        "TCARR.BAS",
        [
            "DIM POS.X(4), SPEED(4)",
            "RATIO=2",
            "FOR I=0 TO 4",
            "POS.X(I)=I",
            "SPEED(I)=3",
            "NEXT I",
            "FOR I=0 TO 4",
            "POS.X(I)=POS.X(I)+(SPEED(I)*RATIO)",
            "IF POS.X(I)>7 THEN POS.X(I)=POS.X(I)-7",
            "NEXT I",
            "PRINT INT(POS.X(0));INT(POS.X(1));INT(POS.X(2))",
        ],
    )
    assert console.send_line('RUN "TCARR.BAS"') == "671"


def test_nested_if_elseif_else_end_if(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("10 N=2") == ""
    assert console.send_line("20 IF N=1 THEN") == ""
    assert console.send_line("30 PRINT 1") == ""
    assert console.send_line("40 ELSEIF N=2 THEN") == ""
    assert console.send_line("50 PRINT 2") == ""
    assert console.send_line("60 ELSE") == ""
    assert console.send_line("70 PRINT 3") == ""
    assert console.send_line("80 END IF") == ""
    assert console.send_line("90 PRINT 9") == ""
    assert console.send_line("RUN") == "2\n9"
    _write_bas(
        console,
        "IFNE.BAS",
        [
            "N=2",
            "IF N=1 THEN",
            "PRINT 1",
            "ELSEIF N=2 THEN",
            "PRINT 2",
            "ELSE",
            "PRINT 3",
            "END IF",
            "PRINT 9",
        ],
    )
    assert console.send_line('RUN "IFNE.BAS"') == "2\n9"


def test_false_if_does_not_skip_next(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("10 FOR I=1 TO 3") == ""
    assert console.send_line("20 IF 0 THEN") == ""
    assert console.send_line("30 PRINT 0") == ""
    assert console.send_line("40 END IF") == ""
    assert console.send_line("50 PRINT I") == ""
    assert console.send_line("60 NEXT I") == ""
    assert console.send_line("RUN") == "1\n2\n3"


def test_same_line_if_else_elseif_and_string(console):
    assert console.send_line("IF 0 THEN PRINT 1") == ""
    assert console.send_line("IF 0 THEN PRINT 1 ELSE PRINT 2") == "2"
    assert console.send_line("IF 1 THEN PRINT 1 ELSE PRINT 2") == "1"
    assert console.send_line("IF 0 THEN PRINT 1 ELSEIF 1 THEN PRINT 3") == "3"
    assert console.send_line("IF 1 THEN PRINT 4 ELSEIF 1 THEN PRINT 5") == "4"
    assert console.send_line('IF 0 THEN PRINT "ELSE" ELSE PRINT 9') == "9"
    assert console.send_line('IF 0 THEN PRINT "ELSE": PRINT 7') == "7"
    _write_bas(
        console,
        "IFSL.BAS",
        [
            "X=0",
            "IF X<0 THEN X=X+384",
            "IF X=0 THEN PRINT 1 ELSE PRINT 2",
            'IF 0 THEN PRINT "ELSE" ELSE PRINT 8',
        ],
    )
    assert console.send_line('RUN "IFSL.BAS"') == "1\n8"


def test_while_and_for_mixed(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("10 I=0") == ""
    assert console.send_line("20 WHILE I<2") == ""
    assert console.send_line("30 I=I+1") == ""
    assert console.send_line("40 FOR J=1 TO 2") == ""
    assert console.send_line("50 PRINT I;J;") == ""
    assert console.send_line("60 NEXT J") == ""
    assert console.send_line("70 WEND") == ""
    assert console.send_line("RUN") == "11122122"


def test_exit_for_exact(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("10 FOR I=1 TO 10") == ""
    assert console.send_line("20 IF I=3 THEN EXIT FOR") == ""
    assert console.send_line("30 PRINT I") == ""
    assert console.send_line("40 NEXT I") == ""
    assert console.send_line("50 PRINT 99") == ""
    assert console.send_line("RUN") == "1\n2\n99"


def test_continue_for_exact(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("10 FOR I=1 TO 3") == ""
    assert console.send_line("20 IF I=2 THEN CONTINUE FOR") == ""
    assert console.send_line("30 PRINT I") == ""
    assert console.send_line("40 NEXT I") == ""
    assert console.send_line("RUN") == "1\n3"


def test_exit_do_exact(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("10 N=0") == ""
    assert console.send_line("20 DO") == ""
    assert console.send_line("30 N=N+1") == ""
    assert console.send_line("40 IF N=4 THEN EXIT DO") == ""
    assert console.send_line("50 LOOP") == ""
    assert console.send_line("60 PRINT N") == ""
    assert console.send_line("RUN") == "4"


def test_sub_local_and_caller(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("10 A=1") == ""
    assert console.send_line("20 SUB Bump(X)") == ""
    assert console.send_line("30 LOCAL L") == ""
    assert console.send_line("40 L=X+10") == ""
    assert console.send_line("50 PRINT L") == ""
    assert console.send_line("60 END SUB") == ""
    assert console.send_line("70 Bump(5)") == ""
    assert console.send_line("80 PRINT A") == ""
    assert console.send_line("RUN") == "15\n1"


def test_static_is_dim_today(console):
    """STATIC currently aliases DIM. CMM2 persist-across-CALL is not implemented."""
    assert console.send_line("NEW") == ""
    assert console.send_line("STATIC N") == ""
    assert console.send_line("N=3") == ""
    assert console.send_line("PRINT N") == "3"


def test_function_local_return_by_name(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("10 FUNCTION Mul(A,B)") == ""
    assert console.send_line("20 LOCAL T") == ""
    assert console.send_line("30 T=A*B") == ""
    assert console.send_line("40 Mul=T") == ""
    assert console.send_line("50 END FUNCTION") == ""
    assert console.send_line("60 PRINT Mul(6,7)") == ""
    assert console.send_line("RUN") == "42"


def test_data_restore_label_order(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("10 DATA 1,2") == ""
    assert console.send_line("20 More:") == ""
    assert console.send_line("30 DATA 9,8") == ""
    assert console.send_line("40 READ A,B") == ""
    assert console.send_line("50 PRINT A;B") == ""
    assert console.send_line("60 RESTORE More") == ""
    assert console.send_line("70 READ C,D") == ""
    assert console.send_line("80 PRINT C;D") == ""
    assert console.send_line("RUN") == "12\n98"


def test_select_case_to_else_skipped(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("10 N=5") == ""
    assert console.send_line("20 SELECT CASE N") == ""
    assert console.send_line("30 CASE 1") == ""
    assert console.send_line("40 PRINT 1") == ""
    assert console.send_line("50 CASE 2 TO 4") == ""
    assert console.send_line("60 PRINT 24") == ""
    assert console.send_line("70 CASE ELSE") == ""
    assert console.send_line("80 PRINT 9") == ""
    assert console.send_line("90 END SELECT") == ""
    assert console.send_line("RUN") == "9"


def test_strings_concat_mid_left_right_len(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("OPTION EXPLICIT OFF") == ""
    assert console.send_line('A$="MM"') == ""
    assert console.send_line('PRINT A$+"BASIC"') == "MMBASIC"
    assert console.send_line('A$="MMXXXX"') == ""
    assert console.send_line('MID$(A$,3)="BASIC"') == ""
    assert console.send_line("PRINT A$") == "MMBASIC"
    assert console.send_line('PRINT LEFT$("MMBASIC",2)') == "MM"
    assert console.send_line('PRINT RIGHT$("MMBASIC",5)') == "BASIC"
    assert console.send_line('PRINT LEN("MMBASIC")') == "7"


def test_implied_let_goto_gosub_label(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("10 A=1") == ""
    assert console.send_line("20 GOTO Skip") == ""
    assert console.send_line('30 PRINT "NO"') == ""
    assert console.send_line("40 Skip: GOSUB 70") == ""
    assert console.send_line("50 PRINT A") == ""
    assert console.send_line("60 END") == ""
    assert console.send_line('70 PRINT "G"') == ""
    assert console.send_line("80 RETURN") == ""
    assert console.send_line("RUN") == "G\n1"


def test_include_inlined_at_load(console):
    _write_bas(console, "CHILD.INC", ['PRINT "C"'])
    _write_bas(console, "PARENT.BAS", ['#include "CHILD.INC"', 'PRINT "P"'])
    assert console.send_line('RUN "PARENT.BAS"') == "C\nP"
    _write_bas(console, "MISS.BAS", ['#include "NOPE.INC"', 'PRINT 1'])
    out = console.send_line('RUN "MISS.BAS"')
    assert "FILE NOT FOUND" in out.upper() or "INCLUDE" in out.upper()


def test_error_line_number_format(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("10 PRINT 1") == ""
    assert console.send_line("20 FOO") == ""
    err = console.send_line("RUN")
    assert "?SYNTAX ERROR" in err
    assert "@20:" in err or "@20" in err
    assert console.send_line("NEW") == ""
    assert console.send_line("10 ERROR \"boom\"") == ""
    boom = console.send_line("RUN")
    assert "boom" in boom
    assert "@10:" in boom or "@10" in boom


def test_undeclared_error_format(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("OPTION EXPLICIT") == ""
    assert console.send_line("Z = 1") == "?UNDECLARED"
    assert console.send_line("NEW") == ""
    assert console.send_line("10 OPTION EXPLICIT") == ""
    assert console.send_line("20 Z = 1") == ""
    err = console.send_line("RUN")
    assert "?EXPLICIT" in err or "?UNDECLARED" in err
    assert "@20" in err


def test_list_save_load_run_roundtrip(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("10 FOR I=1 TO 2") == ""
    assert console.send_line("20 PRINT I") == ""
    assert console.send_line("30 NEXT I") == ""
    listed = console.send_line("LIST")
    assert listed == "10 FOR I=1 TO 2\n20 PRINT I\n30 NEXT I"
    assert console.send_line('SAVE "RT.BAS"') == ""
    assert console.send_line("NEW") == ""
    assert console.send_line("LIST") == ""
    assert console.send_line('RUN "RT.BAS"') == "1\n2"
    assert console.send_line("LIST") == "10 FOR I=1 TO 2\n20 PRINT I\n30 NEXT I"


def test_select_case_colon_after_match(console):
    """Issue #261: CASE value: stmt after a match must not SYNTAX or run."""
    _write_bas(
        console,
        "SELCOL.BAS",
        [
            "SELECT CASE 2",
            '  CASE 1: PRINT "a"',
            '  CASE 2: PRINT "b"',
            '  CASE 3: PRINT "c"',
            '  CASE ELSE: PRINT "d"',
            "END SELECT",
        ],
    )
    out = console.send_line('RUN "SELCOL.BAS"')
    assert "?SYNTAX" not in out.upper()
    assert out.strip() == "b"


def test_select_case_nested_sub_keeps_outer(console):
    """Issue #261: a SUB SELECT CASE must not fire the caller's CASE ELSE."""
    _write_bas(
        console,
        "SELSUB.BAS",
        [
            "HIT=0",
            "SUB Inner",
            "  SELECT CASE 2",
            "    CASE 1: PRINT \"no\"",
            "    CASE 2: HIT=HIT+1",
            "    CASE ELSE: PRINT \"ibad\"",
            "  END SELECT",
            "END SUB",
            "SELECT CASE 5",
            "  CASE 1",
            '    PRINT "bad1"',
            "  CASE 5",
            "    Inner",
            '    PRINT "ok"',
            "  CASE ELSE",
            '    PRINT "badelse"',
            "END SELECT",
            "PRINT HIT",
        ],
    )
    out = console.send_line('RUN "SELSUB.BAS"')
    assert "?SYNTAX" not in out.upper()
    assert "ok" in out
    assert "badelse" not in out
    assert "bad1" not in out
    assert "no" not in out
    assert "ibad" not in out
    assert "1" in out.splitlines()[-1]


def test_select_case_gatherkeyinfo_colon_cases(console):
    """Issue #261: keyboard.inc CASE SC_V: keyboard(K_V)=1 inside a SUB."""
    _write_bas(
        console,
        "KEYB.BAS",
        [
            "CONST True=1, False=0",
            "CONST SC_V=118, SC_Z=122, SC_Enter=10",
            "CONST K_Enter=0, K_Z=5, K_V=6",
            "DIM INTEGER keyboard(9)",
            "DIM INTEGER codes(4)",
            "codes(1)=118",
            "codes(2)=122",
            "codes(3)=10",
            "n=3",
            "SUB Gather",
            "  LOCAL INTEGER idx",
            "  FOR idx=0 TO 9",
            "    keyboard(idx)=False",
            "  NEXT idx",
            "  FOR idx=1 TO n",
            "    SELECT CASE codes(idx)",
            "      CASE SC_V: keyboard(K_V)=True",
            "      CASE SC_Z: keyboard(K_Z)=True",
            "      CASE SC_Enter: keyboard(K_Enter)=True",
            "    END SELECT",
            "  NEXT idx",
            "END SUB",
            "SELECT CASE 1",
            "  CASE 1",
            "    Gather",
            "    PRINT keyboard(K_V);keyboard(K_Z);keyboard(K_Enter)",
            "  CASE ELSE",
            '    PRINT "fail"',
            "END SELECT",
        ],
    )
    out = console.send_line('RUN "KEYB.BAS"')
    assert "?SYNTAX" not in out.upper()
    assert "fail" not in out
    assert "111" in out


def test_select_case_on_key_and_settick(console):
    """Issue #261: ON KEY / SETTICK SELECT CASE must not take the caller's ELSE."""
    _write_bas(
        console,
        "SELIRQ.BAS",
        [
            "K=0",
            "T=0",
            "SUB Drain",
            "  LOCAL C$",
            "  C$=INKEY$",
            "  SELECT CASE 1",
            "    CASE 1: K=1",
            "    CASE ELSE: PRINT \"drainelse\"",
            "  END SELECT",
            "END SUB",
            "SUB Tick",
            "  SELECT CASE 1",
            "    CASE 1: T=1",
            "    CASE ELSE: PRINT \"tickelse\"",
            "  END SELECT",
            "END SUB",
            "ON KEY Drain",
            "SETTICK 20, Tick",
            "SELECT CASE 5",
            "  CASE 5",
            "    PAUSE 500",
            '    PRINT "outer"',
            "  CASE ELSE",
            '    PRINT "else"',
            "END SELECT",
            "PRINT K;T",
        ],
    )
    assert console.send_line("NEW") == ""
    console.drain(quiet=0.1)
    console._ser.sendall(b'RUN "SELIRQ.BAS"\r')
    time.sleep(0.15)
    console._ser.sendall(b"x")
    raw = console.drain(quiet=1.2).decode(errors="replace")
    assert "?SYNTAX" not in raw.upper()
    assert "outer" in raw
    assert "drainelse" not in raw
    assert "tickelse" not in raw
    lines = [ln.strip() for ln in raw.replace("\r", "\n").split("\n") if ln.strip()]
    assert "else" not in lines
    assert "1" in raw


def test_explicit_const_false_array_in_sub(console):
    """Issue #243: keyboard(idx)=False with OPTION EXPLICIT inside a SUB."""
    _write_bas(
        console,
        "KINIT.BAS",
        [
            "OPTION DEFAULT INTEGER",
            "OPTION EXPLICIT ON",
            "CONST True = 1",
            "CONST False = 0",
            "CONST NumberOfKeys = 9",
            "DIM INTEGER keyboard(NumberOfKeys)",
            "SUB keyb.Initialise",
            "  LOCAL INTEGER idx",
            "  FOR idx = 0 TO NumberOfKeys",
            "    keyboard(idx) = False",
            "  NEXT idx",
            "END SUB",
            "keyb.Initialise()",
            "PRINT keyboard(0);keyboard(9)",
        ],
    )
    out = console.send_line('RUN "KINIT.BAS"')
    assert "?UNDECLARED" not in out.upper(), out
    assert "?SYNTAX" not in out.upper(), out
    assert "00" in out.replace(" ", "")
