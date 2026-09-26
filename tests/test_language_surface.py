"""CMM2 / MMBasic language surface from the official manuals."""

import os
import shutil
import subprocess
import sys

import pytest

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


# ---- identifier predicates (host unit test) --------------------------------
#
# #797: mmb_is_ident() is the *continuation* predicate (digits and dots keep an
# identifier going), so it must not be used to decide whether a new identifier
# may start. mmb_is_ident_start() is that guard. Compile the real util.c so the
# contract stays pinned: a leading digit is rejected by mmb_ident(), a letter or
# underscore starts one, and digits/dots after the first char still parse.

DRIVER_C = r"""
#include "mmb_priv.h"
#include <stdio.h>
#include <string.h>

mmb g_state;
mmb *g_cur = &g_state;

int mmb_tok_expand(char *dst, int dstsz)
{
	(void)dst;
	(void)dstsz;
	return 0;
}

void mmb_play_stop(void) {}

static int syntax_hits;
static int fails;

static void check(int cond, const char *what)
{
	if (!cond)
	{
		printf("FAIL %s\n", what);
		fails++;
	}
}

/* Returns 0 on success, -1 if mmb_ident() raised a syntax error, and leaves
 * the parse cursor in *rest. */
static int try_parse(const char *src, char *out, int cap, const char **rest)
{
	if (setjmp(g_state.errjmp) != 0)
	{
		syntax_hits++;
		return -1;
	}
	g_state.p = src;
	mmb_ident(out, cap);
	*rest = g_state.p;
	return 0;
}

int main(void)
{
	char buf[MMB_MAX_NAME];
	const char *rest;

	/* ident-start accepts a letter or underscore and rejects a digit. */
	check(mmb_is_ident_start('A'), "start_upper");
	check(mmb_is_ident_start('z'), "start_lower");
	check(mmb_is_ident_start('_'), "start_underscore");
	check(!mmb_is_ident_start('0'), "start_digit_rejected");
	check(!mmb_is_ident_start('.'), "start_dot_rejected");

	/* ident-continue still accepts digits and dots. */
	check(mmb_is_ident('0'), "continue_digit");
	check(mmb_is_ident('.'), "continue_dot");
	check(mmb_is_ident('A'), "continue_letter");
	check(!mmb_is_ident(' '), "continue_space");

	/* identifiers with digits/dots after the first char parse whole. */
	rest = NULL;
	check(try_parse("A1$+2", buf, (int)sizeof(buf), &rest) == 0, "parse_A1_ok");
	check(!strcmp(buf, "A1$"), "parse_A1_suffix");
	check(rest && *rest == '+', "parse_A1_stop");

	rest = NULL;
	check(try_parse("_X9", buf, (int)sizeof(buf), &rest) == 0, "parse_under_ok");
	check(!strcmp(buf, "_X9"), "parse_underscore_digits");
	check(rest && *rest == 0, "parse_underscore_end");

	rest = NULL;
	check(try_parse("V1.5", buf, (int)sizeof(buf), &rest) == 0, "parse_dot_ok");
	check(!strcmp(buf, "V1.5"), "parse_dot_continue");
	check(rest && *rest == 0, "parse_dot_end");

	/* A leading digit must still make mmb_ident() raise a syntax error. */
	rest = NULL;
	syntax_hits = 0;
	check(try_parse("5+1", buf, (int)sizeof(buf), &rest) == -1, "parse_digit_raises");
	check(syntax_hits == 1, "parse_digit_syntax_once");

	printf("%s\n", fails ? "FAILURES" : "ALL OK");
	return fails ? 1 : 0;
}
"""


@pytest.fixture(scope="module")
def ident_host_run(tmp_path_factory):
    if shutil.which("cc") is None:
        pytest.skip("needs a host C toolchain (cc)")
    repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    tmp = tmp_path_factory.mktemp("ident_start")
    (tmp / "mmb_version.h").write_text('#define MMB_VERSION "test"\n')
    (tmp / "driver.c").write_text(DRIVER_C)
    exe = tmp / "driver"
    gcflags = ["-Wl,-dead_strip"] if sys.platform == "darwin" else ["-Wl,--gc-sections"]
    subprocess.run(
        [
            "cc", "-std=c11", "-O0", "-Wall", "-Wextra",
            "-DMMB_PLATFORM_POSIX",
            "-ffunction-sections", "-fdata-sections",
            "-I", str(tmp),
            "-I", os.path.join(repo, "mmbasic", "include"),
            "-o", str(exe),
            str(tmp / "driver.c"),
            os.path.join(repo, "mmbasic", "src", "util.c"),
            *gcflags,
        ],
        check=True,
        cwd=repo,
    )
    return subprocess.run([str(exe)], check=False, capture_output=True, text=True)


def test_ident_start_predicate(ident_host_run):
    out = ident_host_run.stdout + ident_host_run.stderr
    assert ident_host_run.returncode == 0, out
    assert "ALL OK" in out, out
    assert "FAIL" not in out, out
