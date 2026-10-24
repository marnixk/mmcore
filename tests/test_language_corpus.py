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


def test_for_same_line_body_exact(console):
    """#997: a FOR body on the same line as the FOR must iterate."""
    assert console.send_line("NEW") == ""
    assert console.send_line("10 FOR I=1 TO 3: PRINT I;: NEXT I") == ""
    assert console.send_line("20 PRINT") == ""
    assert console.send_line("RUN") == "123"


def test_nested_for_same_line_exact(console):
    """#997: nested same-line FOR/NEXT must not report ?NEXT WITHOUT FOR."""
    assert console.send_line("NEW") == ""
    assert console.send_line("10 FOR I=1 TO 2: FOR J=1 TO 2: PRINT I;J;: NEXT J: NEXT I") == ""
    assert console.send_line("20 PRINT") == ""
    assert console.send_line("RUN") == "11122122"


def test_for_same_line_empty_body_and_trailing(console):
    """#997: empty same-line body and statements after NEXT on the line."""
    assert console.send_line("NEW") == ""
    assert console.send_line("10 FOR I=1 TO 2: NEXT I: PRINT I") == ""
    assert console.send_line("RUN") == "3"


def test_exit_for_same_line(console):
    """#1004: EXIT FOR in a FOR/NEXT loop that shares a line must work."""
    assert console.send_line("NEW") == ""
    assert console.send_line("10 FOR I=1 TO 5: IF I=3 THEN EXIT FOR: NEXT I") == ""
    assert console.send_line("20 PRINT \"z\"") == ""
    assert console.send_line("RUN") == "z"


def test_continue_for_same_line(console):
    """#1004: CONTINUE FOR in a FOR/NEXT loop that shares a line must work."""
    assert console.send_line("NEW") == ""
    assert (
        console.send_line(
            "10 FOR I=1 TO 3: IF I=2 THEN CONTINUE FOR: PRINT I;: NEXT I"
        )
        == ""
    )
    assert console.send_line("20 PRINT") == ""
    assert console.send_line("RUN") == "13"


def test_for_unnamed_next_and_step(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("10 FOR I=5 TO 1 STEP -2") == ""
    assert console.send_line("20 PRINT I;") == ""
    assert console.send_line("30 NEXT") == ""
    assert console.send_line("40 PRINT I") == ""
    assert console.send_line("RUN") == "531-1"


def test_for_empty_range_skips_body_issue_1277(console):
    """#1277: the termination test runs before the first pass, including for
    variable/expression bounds, a negative STEP and a float STEP."""
    _write_bas(
        console,
        "TZEROT.BAS",
        [
            "OPTION EXPLICIT",
            "OPTION DEFAULT INTEGER",
            "DIM I, N = 1",
            "FOR I = 1 TO 0",
            '  PRINT "ran body A", I',
            "NEXT I",
            "FOR I = 1 TO N - 1",
            '  PRINT "ran body B", I',
            "NEXT I",
            "SUB S(N)",
            "  LOCAL I",
            "  FOR I = 1 TO N - 1",
            '    PRINT "ran body C", I',
            "  NEXT I",
            '  PRINT "s done"',
            "END SUB",
            "S(1)",
            "FOR I = 9 TO 10 STEP -1",
            '  PRINT "ran body D", I',
            "NEXT I",
            'PRINT "end"',
        ],
    )
    assert console.send_line('RUN "TZEROT.BAS"') == "s done\nend"


def test_for_empty_range_zero_trip_corpus(console):
    """#1277: literal, variable and expression bounds, negative and float
    STEP all skip, while a non-empty control loop still runs."""
    assert console.send_line("NEW") == ""
    assert console.send_line("10 N = 1") == ""
    assert console.send_line("20 FOR I = 1 TO 0") == ""
    assert console.send_line('30 PRINT "A";') == ""
    assert console.send_line("40 NEXT I") == ""
    assert console.send_line("50 FOR I = 1 TO N - 1") == ""
    assert console.send_line('60 PRINT "B";') == ""
    assert console.send_line("70 NEXT I") == ""
    assert console.send_line("80 FOR I = 9 TO 10 STEP -1") == ""
    assert console.send_line('90 PRINT "C";') == ""
    assert console.send_line("100 NEXT I") == ""
    assert console.send_line("110 FOR I = 1 TO 0 STEP 0.5") == ""
    assert console.send_line('120 PRINT "D";') == ""
    assert console.send_line("130 NEXT I") == ""
    assert console.send_line("140 FOR I = 0 TO 1 STEP -0.5") == ""
    assert console.send_line('150 PRINT "E";') == ""
    assert console.send_line("160 NEXT I") == ""
    assert console.send_line("170 FOR I = 1 TO 2") == ""
    assert console.send_line('180 PRINT "x";I;') == ""
    assert console.send_line("190 NEXT I") == ""
    assert console.send_line("200 PRINT") == ""
    assert console.send_line('210 PRINT "done"') == ""
    assert console.send_line("RUN") == "x1x2\ndone"


def test_for_same_line_empty_range_zero_trip(console):
    """#1277/#997: an empty same-line FOR skips its body and resumes after
    the matching NEXT."""
    assert console.send_line("NEW") == ""
    assert console.send_line('10 FOR I = 1 TO 0: PRINT "x";: NEXT I: PRINT "y"') == ""
    assert console.send_line('20 PRINT "z"') == ""
    assert console.send_line("RUN") == "y\nz"


def test_for_fractional_step_advances_issue_1280(console):
    """#1280: a fractional STEP must advance the loop, not truncate to 0."""
    assert console.send_line("NEW") == ""
    assert console.send_line("10 FOR I = 0 TO 1 STEP 0.5") == ""
    assert console.send_line("20 PRINT I;") == ""
    assert console.send_line("30 NEXT I") == ""
    assert console.send_line("40 PRINT") == ""
    assert console.send_line("RUN") == "00.51"


def test_for_fractional_negative_step_issue_1280(console):
    """#1280: a fractional negative STEP counts down."""
    assert console.send_line("NEW") == ""
    assert console.send_line("10 FOR I = 1 TO 0 STEP -0.5") == ""
    assert console.send_line("20 PRINT I;") == ""
    assert console.send_line("30 NEXT I") == ""
    assert console.send_line("40 PRINT") == ""
    assert console.send_line("RUN") == "10.50"


def test_for_float_to_bounds_issue_1280(console):
    """#1280: fractional bounds advance in floating point."""
    assert console.send_line("NEW") == ""
    assert console.send_line("10 FOR I = 0.5 TO 2 STEP 0.5") == ""
    assert console.send_line("20 PRINT I;") == ""
    assert console.send_line("30 NEXT I") == ""
    assert console.send_line("40 PRINT") == ""
    assert console.send_line("RUN") == "0.511.52"


def test_for_variable_fractional_step_issue_1280(console):
    """#1280: a STEP held in a variable is not truncated either."""
    assert console.send_line("NEW") == ""
    assert console.send_line("10 S = 0.5") == ""
    assert console.send_line("20 FOR I = 0 TO 1 STEP S") == ""
    assert console.send_line("30 PRINT I;") == ""
    assert console.send_line("40 NEXT I") == ""
    assert console.send_line("50 PRINT") == ""
    assert console.send_line("RUN") == "00.51"


def test_for_integer_control_loop_issue_1280(console):
    """#1280: all-integer ranges keep the exact integer iteration path."""
    assert console.send_line("NEW") == ""
    assert console.send_line("10 FOR I = 1 TO 5") == ""
    assert console.send_line("20 PRINT I;") == ""
    assert console.send_line("30 NEXT I") == ""
    assert console.send_line("40 PRINT") == ""
    assert console.send_line("RUN") == "12345"


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
    # #1276: the THEN arm owns statements after the colon, so a false
    # condition skips PRINT 7 too; a quoted ELSE is not the keyword.
    assert console.send_line('IF 0 THEN PRINT "ELSE": PRINT 7') == ""
    assert console.send_line('IF 1 THEN PRINT "ELSE": PRINT 7') == "ELSE\n7"
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


def test_sub_defined_after_call(console):
    """#698: a SUB may be called before its textual definition."""
    assert console.send_line("NEW") == ""
    assert console.send_line('10 PRINT "top"') == ""
    assert console.send_line("20 LEAF") == ""
    assert console.send_line("30 END") == ""
    assert console.send_line("100 SUB LEAF") == ""
    assert console.send_line('110 PRINT "in sub"') == ""
    assert console.send_line("120 END SUB") == ""
    assert console.send_line("RUN") == "top\nin sub"


def test_function_used_before_definition(console):
    """#698: a FUNCTION may be evaluated before its textual definition."""
    assert console.send_line("NEW") == ""
    assert console.send_line("10 PRINT TWICE(21)") == ""
    assert console.send_line("20 END") == ""
    assert console.send_line("100 FUNCTION TWICE(N)") == ""
    assert console.send_line("110 TWICE = N * 2") == ""
    assert console.send_line("120 END FUNCTION") == ""
    assert console.send_line("RUN") == "42"


def test_sub_with_args_defined_after_call(console):
    """#698: pre-registration keeps the argument signature of a forward SUB."""
    assert console.send_line("NEW") == ""
    assert console.send_line("10 ADD 2, 3") == ""
    assert console.send_line("20 END") == ""
    assert console.send_line("100 SUB ADD(A, B)") == ""
    assert console.send_line('110 PRINT A + B') == ""
    assert console.send_line("120 END SUB") == ""
    assert console.send_line("RUN") == "5"


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


def test_string_expression_function_call_temporaries(console):
    """#1275: FUNCTION calls inside string expressions keep earlier operands."""
    _write_bas(
        console,
        "STREXPR.BAS",
        [
            "OPTION EXPLICIT",
            "OPTION DEFAULT INTEGER",
            "FUNCTION A$(N)",
            '  A$ = "A" + STR$(N)',
            "END FUNCTION",
            "FUNCTION B$(N)",
            '  B$ = "B" + STR$(N)',
            "END FUNCTION",
            "FUNCTION W(S$)",
            "  W = LEN(S$)",
            "END FUNCTION",
            "SUB SHOW(S$, X)",
            '  PRINT "show [" + S$ + "]"; X',
            "END SUB",
            "FUNCTION PAD2$(N)",
            '  PAD2$ = RIGHT$("0" + STR$(N), 2)',
            "END FUNCTION",
            "FUNCTION JOIN2$(P$, R$)",
            '  JOIN2$ = P$ + "|" + R$',
            "END FUNCTION",
            'PRINT A$(1) + "-" + B$(2)',
            'PRINT PAD2$(0) + ":" + PAD2$(2)',
            'DIM Q$ = "ESC PAUSE"',
            "SHOW Q$, 100 - W(Q$)",
            'PRINT JOIN2$(JOIN2$("x", "y"), A$(7))',
        ],
    )
    out = console.send_line('RUN "STREXPR.BAS"')
    assert out == "A1-B2\n00:02\nshow [ESC PAUSE]91\nx|y|A7"


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
    """Issue #243: LET/IF with CONST True/False under OPTION EXPLICIT (tcache)."""
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
            "DIM INTEGER g_started = False",
            "SUB keyb.Initialise",
            "  LOCAL INTEGER idx",
            "  FOR idx = 0 TO NumberOfKeys",
            "    keyboard(idx) = False",
            "  NEXT idx",
            "END SUB",
            "keyb.Initialise()",
            "IF g_started = False THEN g_started = True",
            "PRINT keyboard(0);keyboard(9);g_started",
        ],
    )
    out = console.send_line('RUN "KINIT.BAS"')
    assert "?UNDECLARED" not in out.upper(), out
    assert "?SYNTAX" not in out.upper(), out
    compact = out.replace(" ", "").replace("\n", "")
    assert "001" in compact, out


def _norm_lines(text: str) -> list[str]:
    return [" ".join(ln.split()) for ln in text.split("\n") if ln.strip()]


def test_nested_multiline_if_keeps_outer_else_1273(console):
    """#1273: a nested block IF must not make the outer ELSE/ELSEIF run."""
    _write_bas(
        console,
        "IFNEST.BAS",
        [
            'a = 1 : b = 1 : c = 0',
            'if a then',
            '  if b then',
            '    print "1 inner"',
            '  end if',
            'else',
            '  print "1 BAD else"',
            'end if',
            '',
            'if a then',
            '  if c then',
            '    print "2 BAD"',
            '  else',
            '    print "2 inner else ok"',
            '  end if',
            'elseif b then',
            '  print "2 BAD elseif"',
            'else',
            '  print "2 BAD else"',
            'end if',
            '',
            'sub s()',
            '  local k',
            '  for k = 1 to 2',
            '    if k = 1 then',
            '      if b then',
            '        print "3 k1"',
            '      end if',
            '    elseif k = 2 then',
            '      print "3 k2"',
            '    else',
            '      print "3 BAD"',
            '    end if',
            '  next k',
            'end sub',
            's',
            'print "done"',
        ],
    )
    assert _norm_lines(console.send_line('RUN "IFNEST.BAS"')) == [
        "1 inner",
        "2 inner else ok",
        "3 k1",
        "3 k2",
        "done",
    ]


def test_nested_multiline_if_depth_three_1273(console):
    """#1273: per-level state must also hold at nesting depth > 2."""
    _write_bas(
        console,
        "IFDEEP.BAS",
        [
            "a = 1 : b = 1 : c = 1 : d = 0",
            "if a then",
            "  if b then",
            "    if c then",
            '      print "deep"',
            "    else",
            '      print "bad c"',
            "    end if",
            "  else",
            '    print "bad b"',
            "  end if",
            "else",
            '  print "bad a"',
            "end if",
            "if a then",
            "  if b then",
            "    if d then",
            '      print "bad d"',
            "    else",
            '      print "deep else"',
            "    end if",
            "  elseif c then",
            '    print "bad b elseif"',
            "  end if",
            "elseif b then",
            '  print "bad a elseif"',
            "else",
            '  print "bad a else"',
            "end if",
            'print "end"',
        ],
    )
    assert _norm_lines(console.send_line('RUN "IFDEEP.BAS"')) == [
        "deep",
        "deep else",
        "end",
    ]


def test_exit_do_from_taken_nested_if_1273(console):
    """#1273: EXIT DO inside a taken branch must unwind its IF frames."""
    _write_bas(
        console,
        "IFEXITDO.BAS",
        [
            "n = 0",
            "do",
            "  n = n + 1",
            "  if n = 1 then",
            "    if n = 1 then",
            '      print "one"',
            "    end if",
            "  end if",
            "  if n = 2 then",
            "    if n = 2 then",
            '      print "two"',
            "      exit do",
            "    end if",
            '    print "bad"',
            "  end if",
            '  print "iter"; n',
            "loop",
            'print "after"',
            "if n = 2 then",
            "  if 1 then",
            '    print "clean"',
            "  else",
            '    print "bad inner"',
            "  end if",
            "else",
            '  print "bad outer"',
            "end if",
        ],
    )
    assert _norm_lines(console.send_line('RUN "IFEXITDO.BAS"')) == [
        "one",
        "iter1",
        "two",
        "after",
        "clean",
    ]


def test_exit_for_from_taken_nested_if_1273(console):
    """#1273: EXIT FOR inside a taken branch must unwind its IF frames."""
    _write_bas(
        console,
        "IFEXITFOR.BAS",
        [
            "for i = 1 to 3",
            "  if i = 2 then",
            "    if i = 2 then",
            '      print "out"',
            "      exit for",
            "    end if",
            '    print "bad"',
            "  end if",
            '  print "i"; i',
            "next i",
            'print "after"',
            "if i = 2 then",
            "  if 1 then",
            '    print "clean"',
            "  else",
            '    print "bad inner"',
            "  end if",
            "else",
            '  print "bad outer"',
            "end if",
        ],
    )
    assert _norm_lines(console.send_line('RUN "IFEXITFOR.BAS"')) == [
        "i1",
        "out",
        "after",
        "clean",
    ]


def test_skipped_block_does_not_evaluate_if_conditions_1274(console):
    """#1274: conditions inside a skipped block must not run or index arrays."""
    _write_bas(
        console,
        "IFSKIP.BAS",
        [
            "option explicit",
            "option default integer",
            "function f()",
            '  print "f called"',
            "  f = 1",
            "end function",
            "dim a(9), d = 10, bad = 0",
            "if bad then",
            '  if f() then print "x"',
            "end if",
            'print "A"',
            "if bad then",
            "  if f() = 1 then",
            '    print "y"',
            "  end if",
            "end if",
            'print "B"',
            "if bad then",
            '  if a(d) > 1 then print "never"',
            "end if",
            'print "C"',
        ],
    )
    out = console.send_line('RUN "IFSKIP.BAS"')
    assert "?INDEX OUT OF BOUNDS" not in out.upper(), out
    assert _norm_lines(out) == ["A", "B", "C"]


def test_one_line_if_owns_rest_of_line_1276(console):
    """#1276: THEN/ELSE arms own every colon-separated statement."""
    _write_bas(
        console,
        "IFLINE.BAS",
        [
            "option explicit",
            "option default integer",
            "dim a = 0, b = 0, c = 0",
            "if a then b = 1 : c = 1",
            'print "false:", b, c',
            "a = 1 : b = 0 : c = 0",
            "if a then b = 1 : c = 1",
            'print "true:", b, c',
            "sub q(x)",
            '  print "q"; x;',
            "end sub",
            'q(1) : print " after-colon"',
            "a = 0 : b = 0 : c = 0",
            "if a then b = 1 : c = 1 else b = 2 : c = 2",
            'print "else form:", b, c',
            "a = 1 : b = 0 : c = 0",
            "if a then b = 1 : c = 1 else b = 2 : c = 2",
            'print "then form:", b, c',
        ],
    )
    out = console.send_line('RUN "IFLINE.BAS"')
    assert "?SYNTAX" not in out.upper(), out
    assert _norm_lines(out) == [
        "false: 0 0",
        "true: 1 1",
        "q1 after-colon",
        "else form: 2 2",
        "then form: 1 1",
    ]


def test_one_line_if_exit_do_owns_colon_statements_1276(console):
    """#1276: `if c then a = 1 : exit do` must not exit when c is false."""
    _write_bas(
        console,
        "IFLINEEXIT.BAS",
        [
            "n = 0",
            "do",
            "  n = n + 1",
            "  if n = 2 then flag = 1 : exit do",
            "  if n > 5 then exit do",
            "loop",
            'print "n"; n; "f"; flag',
        ],
    )
    assert _norm_lines(console.send_line('RUN "IFLINEEXIT.BAS"')) == ["n2f1"]


def test_one_line_if_sub_call_runs_only_taken_branch_1295(console):
    """#1295: a one-line IF whose THEN arm calls a SUB/FUNCTION must not run
    the ELSE arm (or fail to parse the ELSE), for every call form."""
    _write_bas(
        console,
        "IFCALL.BAS",
        [
            "option explicit",
            "option default integer",
            "sub show(a$, n)",
            '  print "show:"; a$; n',
            "end sub",
            "sub hi()",
            '  print "hi"',
            "end sub",
            "function f(x)",
            "  f = x",
            "end function",
            "dim i = 8, a",
            'if i < 9 then show "A", 1 else show "B", 2',
            'if i < 9 then show("A", 1) else show("B", 2)',
            "if i < 9 then hi() else a = 5",
            'print "a:"; a',
            "if i < 9 then a = f(1) else a = 2",
            'print "a:"; a',
            "if i < 9 then hi else a = 5",
            'print "a:"; a',
            'if i > 9 then show "A", 1 else show "B", 2',
        ],
    )
    out = console.send_line('RUN "IFCALL.BAS"')
    assert "?SYNTAX" not in out.upper(), out
    assert "?UNDECLARED" not in out.upper(), out
    assert _norm_lines(out) == [
        "show:A1",
        "show:A1",
        "hi",
        "a:0",
        "a:1",
        "hi",
        "a:1",
        "show:B2",
    ]


def test_one_line_if_multi_statement_branches_with_calls_1295(console):
    """#1295: colon-separated THEN/ELSE parts keep owning their statements,
    including function calls in assignments and SUB calls as statements."""
    _write_bas(
        console,
        "IFCALLMULTI.BAS",
        [
            "option explicit",
            "option default integer",
            "sub show(a$, n)",
            '  print "show:"; a$; n',
            "end sub",
            "function f(x)",
            "  f = x",
            "end function",
            "dim a = 0, b = 0",
            "if 1 then a = f(1) : b = f(2) else a = 9 : b = 9",
            'print "t:"; a; b',
            "a = 0 : b = 0",
            "if 0 then a = f(1) : b = f(2) else a = f(3) : b = f(4)",
            'print "f:"; a; b',
            "a = 0",
            'if 1 then show "T", 1 : a = 1 else show "E", 2 : a = 2',
            'print "t2:"; a',
            "a = 0",
            'if 0 then show "T", 1 : a = 1 else show "E", 2 : a = 2',
            'print "f2:"; a',
        ],
    )
    out = console.send_line('RUN "IFCALLMULTI.BAS"')
    assert "?SYNTAX" not in out.upper(), out
    assert _norm_lines(out) == ["t:12", "f:34", "show:T1", "t2:1", "show:E2", "f2:2"]
