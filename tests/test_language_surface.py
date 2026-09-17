"""CMM2 / MMBasic language surface from the official manuals."""

from ihelp_util import dump_topic, open_ihelp, close_ihelp, scroll_all


def test_dim_integer_prefix(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("DIM INTEGER N=21") == ""
    assert console.send_line("PRINT N*2") == "42"


def test_local_and_explicit(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("OPTION EXPLICIT") == ""
    assert console.send_line("LOCAL A") == ""
    assert console.send_line("A=7") == ""
    assert console.send_line("PRINT A") == "7"


def test_inc_dec_cat(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("OPTION EXPLICIT OFF") == ""
    assert console.send_line("A=10") == ""
    assert console.send_line("INC A,2") == ""
    assert console.send_line("PRINT A") == "12"
    assert console.send_line("DEC A") == ""
    assert console.send_line("PRINT A") == "11"
    assert console.send_line('A$="MM"') == ""
    assert console.send_line('CAT A$,"BASIC"') == ""
    assert console.send_line("PRINT A$") == "MMBASIC"


def test_error_command(console):
    out = console.send_line('ERROR "boom"')
    assert "boom" in out.lower() or "ERROR" in out.upper()


def test_memory_command(console):
    out = console.send_line("MEMORY")
    assert "Program" in out
    assert "Variables" in out


def test_randomize_and_rnd(console):
    assert console.send_line("RANDOMIZE 1") == ""
    a = console.send_line("PRINT RND")
    assert a != "?SYNTAX ERROR"
    assert console.send_line("RANDOMIZE 1") == ""
    b = console.send_line("PRINT RND")
    assert a == b


def test_if_then_linenumber(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("10 X=1") == ""
    assert console.send_line("20 IF X=1 THEN 40") == ""
    assert console.send_line("30 PRINT 0") == ""
    assert console.send_line("40 PRINT 42") == ""
    assert console.send_line("RUN") == "42"


def test_on_goto(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("10 N=2") == ""
    assert console.send_line("20 ON N GOTO 30,40,50") == ""
    assert console.send_line("30 PRINT 1") == ""
    assert console.send_line("35 END") == ""
    assert console.send_line("40 PRINT 42") == ""
    assert console.send_line("45 END") == ""
    assert console.send_line("50 PRINT 3") == ""
    assert console.send_line("RUN") == "42"


def test_exit_for(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("10 FOR I=1 TO 10") == ""
    assert console.send_line("20 IF I=3 THEN EXIT FOR") == ""
    assert console.send_line("30 PRINT I") == ""
    assert console.send_line("40 NEXT I") == ""
    assert console.send_line("50 PRINT 99") == ""
    assert console.send_line("RUN") == "1\n2\n99"


def test_continue_for(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("10 FOR I=1 TO 3") == ""
    assert console.send_line("20 IF I=2 THEN CONTINUE FOR") == ""
    assert console.send_line("30 PRINT I") == ""
    assert console.send_line("40 NEXT I") == ""
    assert console.send_line("RUN") == "1\n3"


def test_max_min_acos(console):
    assert console.send_line("PRINT MAX(1,5,3)") == "5"
    assert console.send_line("PRINT MIN(1,5,3)") == "1"
    assert console.send_line("OPTION ANGLE DEGREES") == ""
    assert console.send_line("PRINT INT(ACOS(1)+0.5)") == "0"


def test_help_new_topics(console):
    for topic in (
        "LOCAL",
        "STATIC",
        "ERROR",
        "MEMORY",
        "INC",
        "CAT",
        "ON",
        "CONTINUE",
        "EXIT",
        "CMM2",
        "LEN",
        "ACOS",
    ):
        out = dump_topic(console, topic)
        assert out != "?SYNTAX ERROR", topic
        assert "unknown" not in out.lower(), topic


def test_help_lists_new_commands(console):
    out = scroll_all(console, open_ihelp(console, "INDEX"))
    for cmd in ("MEMORY", "INC", "CAT", "ERROR", "CMM2", "SORT"):
        assert cmd in out, cmd
    close_ihelp(console)
    basic = dump_topic(console, "BASIC")
    assert "LOCAL" in basic
    assert "GOTO" in basic
    assert "<ON>" in basic or "ON GOTO" in basic


def test_const_max_not_function(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("CONST MAX=21") == ""
    assert console.send_line("PRINT MAX*2") == "42"
    # The parenthesised call still reaches the built-in MAX.
    assert console.send_line("PRINT MAX(3, 7)") == "7"


def test_case_to_and_label(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("10 N=3") == ""
    assert console.send_line("20 SELECT CASE N") == ""
    assert console.send_line("30 CASE 1") == ""
    assert console.send_line("40 PRINT 1") == ""
    assert console.send_line("50 CASE 2 TO 4") == ""
    assert console.send_line("60 PRINT 42") == ""
    assert console.send_line("70 CASE ELSE") == ""
    assert console.send_line("80 PRINT 0") == ""
    assert console.send_line("90 END SELECT") == ""
    assert console.send_line("RUN") == "42"
    assert console.send_line("NEW") == ""
    assert console.send_line("10 GOTO DONE") == ""
    assert console.send_line("20 PRINT 0") == ""
    assert console.send_line("30 DONE: PRINT 42") == ""
    assert console.send_line("RUN") == "42"


def test_mid_stmt_and_sort(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("OPTION EXPLICIT OFF") == ""
    assert console.send_line('A$="MMXXXX"') == ""
    assert console.send_line('MID$(A$,3)="BASIC"') == ""
    assert console.send_line("PRINT A$") == "MMBASIC"
    assert console.send_line("DIM Q(2)") == ""
    assert console.send_line("Q(0)=3") == ""
    assert console.send_line("Q(1)=1") == ""
    assert console.send_line("Q(2)=2") == ""
    assert console.send_line("SORT Q()") == ""
    assert console.send_line("PRINT Q(0);Q(1);Q(2)") == "123"


def test_format_bound_choice(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("PRINT FORMAT$(42)") == "42"
    assert console.send_line("PRINT CHOICE(1,9,8)") == "9"
    assert console.send_line("PRINT CHOICE(0,9,8)") == "8"
    assert console.send_line("DIM Z(4)") == ""
    assert console.send_line("PRINT BOUND(Z())") == "4"
    assert console.send_line("PRINT INKEY$") == ""
    out = console.send_line('DATE$="28-7-26"')
    assert out == ""
    assert console.send_line("PRINT DATE$") == "28-7-26"
