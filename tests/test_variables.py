"""QuickBasic/PicoMite variable and type gaps: SWAP, REDIM [PRESERVE],
DIM SHARED, COMMON."""

from ihelp_util import dump_topic


def test_swap_numbers_and_strings(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("A = 1 : B = 2") == ""
    assert console.send_line("SWAP A, B") == ""
    assert console.send_line("PRINT A; B") == "21"
    assert console.send_line('S$ = "L" : T$ = "R"') == ""
    assert console.send_line("SWAP S$, T$") == ""
    assert console.send_line("PRINT S$; T$") == "RL"


def test_swap_array_elements(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("DIM N$(1)") == ""
    assert console.send_line('N$(0) = "left"') == ""
    assert console.send_line('N$(1) = "right"') == ""
    assert console.send_line("SWAP N$(0), N$(1)") == ""
    assert console.send_line("PRINT N$(0); N$(1)") == "rightleft"


def test_swap_type_mismatch(console):
    assert console.send_line("NEW") == ""
    assert console.send_line('A = 1 : B$ = "x"') == ""
    err = console.send_line("SWAP A, B$")
    assert "TYPE MISMATCH" in err.upper()


def test_redim_discards_without_preserve(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("DIM A(2)") == ""
    assert console.send_line("A(0) = 5") == ""
    assert console.send_line("REDIM A(4)") == ""
    assert console.send_line("PRINT A(0)") == "0"
    assert console.send_line("PRINT A(4)") == "0"


def test_redim_creates_new_array(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("REDIM B(3)") == ""
    assert console.send_line("B(2) = 9") == ""
    assert console.send_line("PRINT B(2)") == "9"


def test_redim_preserve_numeric(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("DIM A(2)") == ""
    assert console.send_line("A(0) = 1 : A(1) = 2 : A(2) = 3") == ""
    assert console.send_line("REDIM PRESERVE A(4)") == ""
    assert console.send_line("PRINT A(0) + A(1) + A(2)") == "6"
    assert console.send_line("PRINT A(3); A(4)") == "00"
    assert console.send_line("REDIM PRESERVE A(1)") == ""
    assert console.send_line("PRINT A(0); A(1)") == "12"
    assert console.send_line("PRINT BOUND(A())") == "1"


def test_redim_preserve_strings(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("DIM S$(2)") == ""
    assert console.send_line('S$(0) = "a" : S$(1) = "bb" : S$(2) = "ccc"') == ""
    assert console.send_line("REDIM PRESERVE S$(4)") == ""
    assert console.send_line("PRINT S$(0); S$(1); S$(2)") == "abbccc"
    assert console.send_line('PRINT "["; S$(4); "]"') == "[]"


def test_redim_preserve_2d_last_dim(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("DIM M(1, 2)") == ""
    assert console.send_line("M(0, 0) = 1 : M(0, 1) = 2 : M(0, 2) = 3") == ""
    assert console.send_line("M(1, 0) = 4 : M(1, 1) = 5 : M(1, 2) = 6") == ""
    assert console.send_line("REDIM PRESERVE M(1, 4)") == ""
    assert console.send_line("PRINT M(0, 0); M(0, 1); M(0, 2); M(1, 0); M(1, 3)") == "12340"


def test_redim_preserve_other_dim_errors(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("DIM M(1, 2)") == ""
    err = console.send_line("REDIM PRESERVE M(3, 2)")
    assert "DIMENSION" in err.upper()


def test_dim_and_redim_shared(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("DIM SHARED X(2)") == ""
    assert console.send_line("X(0) = 7") == ""
    assert console.send_line("PRINT X(0)") == "7"
    assert console.send_line("REDIM SHARED Y(3)") == ""
    assert console.send_line("Y(3) = 8") == ""
    assert console.send_line("PRINT Y(3)") == "8"


def test_common_declares_variables(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("COMMON FOO, BAR") == ""
    assert console.send_line("FOO = 5 : BAR$ = \"hi\"") == ""
    assert console.send_line("PRINT FOO") == "5"
    assert console.send_line("PRINT BAR$") == "hi"
    assert console.send_line("COMMON SHARED BAZ") == ""
    assert console.send_line("BAZ = 3") == ""
    assert console.send_line("PRINT BAZ") == "3"


def test_local_is_scoped_to_sub(console):
    """#658: a callee's LOCAL must not clobber the caller's same-named local."""
    assert console.send_line("NEW") == ""
    assert console.send_line("10 SUB INNER") == ""
    assert console.send_line("20 LOCAL X") == ""
    assert console.send_line("30 X=99") == ""
    assert console.send_line("40 END SUB") == ""
    assert console.send_line("50 SUB OUTER") == ""
    assert console.send_line("60 LOCAL X") == ""
    assert console.send_line("70 X=1") == ""
    assert console.send_line("80 INNER") == ""
    assert console.send_line("90 PRINT X") == ""
    assert console.send_line("100 END SUB") == ""
    assert console.send_line("110 OUTER") == ""
    assert console.send_line("RUN") == "1"


def test_local_does_not_clobber_argument(console):
    """#658: a callee's LOCAL must not overwrite the caller's argument slot."""
    assert console.send_line("NEW") == ""
    assert console.send_line("10 SUB INNER(P$)") == ""
    assert console.send_line("20 LOCAL N%") == ""
    assert console.send_line("30 N%=14") == ""
    assert console.send_line("40 END SUB") == ""
    assert console.send_line("50 SUB OUTER(P$, N%)") == ""
    assert console.send_line("60 INNER(P$)") == ""
    assert console.send_line("70 PRINT N%") == ""
    assert console.send_line("80 END SUB") == ""
    assert console.send_line('90 OUTER "x", 7') == ""
    assert console.send_line("RUN") == "7"


def test_local_string_scoped_to_sub(console):
    """#658: string locals are restored too."""
    assert console.send_line("NEW") == ""
    assert console.send_line("10 SUB INNER") == ""
    assert console.send_line("20 LOCAL S$") == ""
    assert console.send_line('30 S$="inner"') == ""
    assert console.send_line("40 END SUB") == ""
    assert console.send_line("50 SUB OUTER") == ""
    assert console.send_line("60 LOCAL S$") == ""
    assert console.send_line('70 S$="outer"') == ""
    assert console.send_line("80 INNER") == ""
    assert console.send_line("90 PRINT S$") == ""
    assert console.send_line("100 END SUB") == ""
    assert console.send_line("110 OUTER") == ""
    assert console.send_line("RUN") == "outer"


def test_local_array_scoped_to_sub(console):
    """#658: a LOCAL array gets its own dimensions and is restored on return."""
    assert console.send_line("NEW") == ""
    assert console.send_line("10 DIM A(2)") == ""
    assert console.send_line("20 A(0)=5") == ""
    assert console.send_line("30 SUB INNER") == ""
    assert console.send_line("40 LOCAL A(3)") == ""
    assert console.send_line("50 A(0)=11") == ""
    assert console.send_line("60 END SUB") == ""
    assert console.send_line("70 INNER") == ""
    assert console.send_line("80 PRINT A(0)") == ""
    assert console.send_line("RUN") == "5"


def test_local_in_loop_resets_between_calls(console):
    """#658: a LOCAL re-declared in a frame keeps its value within the call,
    but starts fresh on the next call."""
    assert console.send_line("NEW") == ""
    assert console.send_line("10 SUB COUNT") == ""
    assert console.send_line("20 FOR I=1 TO 3") == ""
    assert console.send_line("30 LOCAL T") == ""
    assert console.send_line("40 T=T+1") == ""
    assert console.send_line("50 NEXT I") == ""
    assert console.send_line("60 PRINT T") == ""
    assert console.send_line("70 END SUB") == ""
    assert console.send_line("80 COUNT") == ""
    assert console.send_line("90 COUNT") == ""
    assert console.send_line("RUN") == "3\n3"


def test_error_in_sub_restores_caller_local(console):
    """#692: a trapped error unwinds the SUB frame, so the caller's LOCAL
    binding comes back before the ON ERROR handler runs."""
    assert console.send_line("NEW") == ""
    assert console.send_line("10 ON ERROR GOTO 500") == ""
    assert console.send_line("20 SUB LEAF") == ""
    assert console.send_line("30 LOCAL X") == ""
    assert console.send_line("40 X = 99") == ""
    assert console.send_line("50 DIM A(2)") == ""
    assert console.send_line("60 A(5) = 1") == ""
    assert console.send_line("70 END SUB") == ""
    assert console.send_line("100 X = 5") == ""
    assert console.send_line("110 LEAF") == ""
    assert console.send_line('120 PRINT "after X="; X') == ""
    assert console.send_line("130 END") == ""
    assert console.send_line('500 PRINT "handler X="; X') == ""
    assert console.send_line("510 RESUME 120") == ""
    out = console.send_line("RUN")
    lines = [ln.strip() for ln in out.replace("\r", "\n").split("\n") if ln.strip()]
    assert lines == ["handler X=5", "after X=5"], out


def test_error_in_sub_restores_caller_argument(console):
    """#692: a trapped error restores the caller's argument slot too, so a
    SUB that overwrote a global of the same name leaves it intact."""
    assert console.send_line("NEW") == ""
    assert console.send_line("10 ON ERROR GOTO 500") == ""
    assert console.send_line("20 SUB LEAF(P$)") == ""
    assert console.send_line("30 DIM A(2)") == ""
    assert console.send_line("40 A(5) = 1") == ""
    assert console.send_line("50 END SUB") == ""
    assert console.send_line('100 P$ = "orig"') == ""
    assert console.send_line('110 LEAF "changed"') == ""
    assert console.send_line('120 PRINT "after P$="; P$') == ""
    assert console.send_line("130 END") == ""
    assert console.send_line('500 PRINT "handler P$="; P$') == ""
    assert console.send_line("510 RESUME 120") == ""
    out = console.send_line("RUN")
    lines = [ln.strip() for ln in out.replace("\r", "\n").split("\n") if ln.strip()]
    assert lines == ["handler P$=orig", "after P$=orig"], out


def test_error_in_nested_sub_restores_every_frame(console):
    """#692: nested SUB calls are all unwound, not just the innermost one."""
    assert console.send_line("NEW") == ""
    assert console.send_line("10 ON ERROR GOTO 500") == ""
    assert console.send_line("20 SUB OUTER") == ""
    assert console.send_line("30 LOCAL X") == ""
    assert console.send_line("40 X = 77") == ""
    assert console.send_line("50 INNER") == ""
    assert console.send_line("60 END SUB") == ""
    assert console.send_line("70 SUB INNER") == ""
    assert console.send_line("80 DIM A(2)") == ""
    assert console.send_line("90 A(5) = 1") == ""
    assert console.send_line("100 END SUB") == ""
    assert console.send_line("110 X = 5") == ""
    assert console.send_line("120 OUTER") == ""
    assert console.send_line('130 PRINT "after X="; X') == ""
    assert console.send_line("140 END") == ""
    assert console.send_line('500 PRINT "handler X="; X') == ""
    assert console.send_line("510 RESUME 130") == ""
    out = console.send_line("RUN")
    lines = [ln.strip() for ln in out.replace("\r", "\n").split("\n") if ln.strip()]
    assert lines == ["handler X=5", "after X=5"], out


def test_error_in_function_restores_caller_local(console):
    """#692: a FUNCTION frame is unwound on a trapped error as well."""
    assert console.send_line("NEW") == ""
    assert console.send_line("10 ON ERROR GOTO 500") == ""
    assert console.send_line("20 FUNCTION FN(N)") == ""
    assert console.send_line("30 LOCAL X") == ""
    assert console.send_line("40 X = N * 10") == ""
    assert console.send_line("50 DIM A(2)") == ""
    assert console.send_line("60 A(5) = 1") == ""
    assert console.send_line("70 FN = 0") == ""
    assert console.send_line("80 END FUNCTION") == ""
    assert console.send_line("100 X = 5") == ""
    assert console.send_line("110 R = FN(3)") == ""
    assert console.send_line('120 PRINT "after X="; X') == ""
    assert console.send_line("130 END") == ""
    assert console.send_line('500 PRINT "handler X="; X') == ""
    assert console.send_line("510 RESUME 120") == ""
    out = console.send_line("RUN")
    lines = [ln.strip() for ln in out.replace("\r", "\n").split("\n") if ln.strip()]
    assert lines == ["handler X=5", "after X=5"], out


def test_sub_argument_as_string(console):
    """#682: an unsuffixed SUB argument declared AS STRING is a string."""
    assert console.send_line("NEW") == ""
    assert console.send_line("10 SUB SHOW(A AS STRING)") == ""
    assert console.send_line("20 PRINT A") == ""
    assert console.send_line("30 END SUB") == ""
    assert console.send_line('40 SHOW "hello"') == ""
    assert console.send_line("RUN") == "hello"


def test_sub_argument_as_string_with_parens(console):
    """#682: the same binding works for a parenthesised call."""
    assert console.send_line("NEW") == ""
    assert console.send_line("10 SUB SHOW(A AS STRING)") == ""
    assert console.send_line('20 PRINT A + "!"') == ""
    assert console.send_line("30 END SUB") == ""
    assert console.send_line('40 SHOW("hi")') == ""
    assert console.send_line("RUN") == "hi!"


def test_sub_argument_as_integer(console):
    """#682: AS INTEGER on an unsuffixed SUB argument keeps the value."""
    assert console.send_line("NEW") == ""
    assert console.send_line("10 SUB SHOW(A AS INTEGER)") == ""
    assert console.send_line("20 PRINT A * 2") == ""
    assert console.send_line("30 END SUB") == ""
    assert console.send_line("40 SHOW 21") == ""
    assert console.send_line("RUN") == "42"


def test_sub_argument_as_float(console):
    """#682: AS FLOAT on an unsuffixed SUB argument keeps the value."""
    assert console.send_line("NEW") == ""
    assert console.send_line("10 SUB SHOW(A AS FLOAT)") == ""
    assert console.send_line("20 PRINT A + 0.5") == ""
    assert console.send_line("30 END SUB") == ""
    assert console.send_line("40 SHOW 5") == ""
    assert console.send_line("RUN") == "5.5"


def test_function_argument_as_string(console):
    """#682: AS STRING also binds FUNCTION arguments, not just SUBs."""
    assert console.send_line("NEW") == ""
    assert console.send_line("10 FUNCTION LEN2(A AS STRING)") == ""
    assert console.send_line("20 LEN2 = LEN(A)") == ""
    assert console.send_line("30 END FUNCTION") == ""
    assert console.send_line('40 PRINT LEN2("abcd")') == ""
    assert console.send_line("RUN") == "4"


def test_sub_argument_as_string_restores_caller(console):
    """#682: a typed argument is restored after the call returns."""
    assert console.send_line("NEW") == ""
    assert console.send_line("10 SUB SHOW(A AS STRING)") == ""
    assert console.send_line("20 PRINT A") == ""
    assert console.send_line("30 END SUB") == ""
    assert console.send_line('40 A$ = "outer"') == ""
    assert console.send_line('50 SHOW "inner"') == ""
    assert console.send_line("60 PRINT A$") == ""
    assert console.send_line("RUN") == "inner\nouter"


def test_help_variable_gaps(console):
    out = dump_topic(console, "REDIM")
    assert "PRESERVE" in out
    out = dump_topic(console, "SWAP")
    assert "SWAP" in out
    out = dump_topic(console, "COMMON")
    assert "COMMON" in out
    dim = dump_topic(console, "DIM")
    assert "SHARED" in dim
