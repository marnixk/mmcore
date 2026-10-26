"""#1304: user identifiers shadowed by built-in constants/functions.

A declaration whose base name collides with a built-in function (RGB, LEN),
a named colour constant (GREY, RED, WHITE) or an already-defined SUB/FUNCTION
must fail at declaration/prescan time with a message naming the conflicting
built-in and the line, instead of failing at the call site or silently
binding to the built-in.

Uses the fast native stdio build (native/mmbasic) like test_linux_native.py:
same interpreter core, no QEMU boot per module.
"""
import os
import shutil
import subprocess

import pytest

from native_harness import build_native

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(REPO, "native", "mmbasic")

pytestmark = pytest.mark.skipif(
    shutil.which("cc") is None or shutil.which("make") is None,
    reason="needs a host C toolchain (cc/make)",
)


@pytest.fixture(scope="module")
def mmb():
    build_native()
    assert os.path.isfile(BIN), "native build produced no binary"
    return BIN


def _run(mmb, lines):
    proc = subprocess.run(
        [mmb],
        input="".join(line + "\n" for line in lines),
        text=True,
        capture_output=True,
        timeout=120,
    )
    return proc.stdout + proc.stderr


# ---- declaration-time conflicts -------------------------------------------

@pytest.mark.parametrize(
    "prog, expect",
    [
        # SUB parameter: fails at RUN (prescan), never calls the sub.
        (["10 SUB s(grey)", "20 PRINT \"BAD\"", "30 END SUB", "RUN"],
         "?NAME CONFLICT: GREY is a built-in constant @10:SUB s(grey)"),
        (["10 FUNCTION f(grey)", "20 f=1", "30 END FUNCTION", "RUN"],
         "?NAME CONFLICT: GREY is a built-in constant @10:FUNCTION f(grey)"),
        # multiple colour parameters: first conflict wins
        (["10 SUB t(red, white)", "20 END SUB", "RUN"],
         "?NAME CONFLICT: RED is a built-in constant @10:SUB t(red, white)"),
        # parameter named like a built-in function
        (["10 SUB s(len)", "20 END SUB", "RUN"],
         "?NAME CONFLICT: LEN is a built-in function @10:SUB s(len)"),
        # SUB/FUNCTION names vs built-in function
        (["10 SUB RGB a", "20 END SUB", "RUN"],
         "?NAME CONFLICT: RGB is a built-in function @10:SUB RGB a"),
        (["10 FUNCTION RGB(a)", "20 END FUNCTION", "RUN"],
         "?NAME CONFLICT: RGB is a built-in function @10:FUNCTION RGB(a)"),
        # duplicate SUB/FUNCTION definitions (second one silently never bound)
        (["10 SUB s", "20 PRINT \"first\"", "30 END SUB",
          "40 SUB s", "50 PRINT \"second\"", "60 END SUB", "RUN"],
         "?NAME CONFLICT: S is already a SUB @40:SUB s"),
        (["10 FUNCTION f", "20 f=1", "30 END FUNCTION",
          "40 FUNCTION f", "50 f=2", "60 END FUNCTION", "RUN"],
         "?NAME CONFLICT: F is already a FUNCTION @40:FUNCTION f"),
    ],
)
def test_declaration_conflict(mmb, prog, expect):
    out = _run(mmb, prog)
    assert expect in out, out


@pytest.mark.parametrize(
    "line, expect",
    [
        ("LOCAL grey = 0", "?NAME CONFLICT: GREY is a built-in constant"),
        ("DIM grey = 0", "?NAME CONFLICT: GREY is a built-in constant"),
        ("CONST grey = 0", "?NAME CONFLICT: GREY is a built-in constant"),
        ("STATIC grey = 1", "?NAME CONFLICT: GREY is a built-in constant"),
        # implicit variable creation
        ("grey = 0 : PRINT \"BAD\"", "?NAME CONFLICT: GREY is a built-in constant"),
        # built-in function names route through the same clearer message
        ("LOCAL len = 3", "?NAME CONFLICT: LEN is a built-in function"),
        ("DIM len = 5", "?NAME CONFLICT: LEN is a built-in function"),
        ("STATIC len = 1", "?NAME CONFLICT: LEN is a built-in function"),
    ],
)
def test_statement_conflict(mmb, line, expect):
    out = _run(mmb, [line, 'PRINT "SURVIVED"'])
    assert expect in out, out
    assert "SURVIVED" in out, "the error must not stop the rest of the session"


def test_sub_param_conflict_never_runs_body(mmb):
    out = _run(mmb, [
        "10 SUB s(grey)",
        "20 PRINT \"BAD param: \", grey",
        "30 END SUB",
        "40 s 0",
        "RUN",
    ])
    assert "?NAME CONFLICT: GREY is a built-in constant @10:SUB s(grey)" in out, out
    assert "BAD param" not in out, out


def test_function_named_builtin_never_defers(mmb):
    out = _run(mmb, [
        "10 FUNCTION RGB(a)",
        "20 RGB = 42",
        "30 END FUNCTION",
        "40 PRINT RGB(1)",
        "RUN",
    ])
    assert "?NAME CONFLICT: RGB is a built-in function @10:FUNCTION RGB(a)" in out, out
    # neither the built-in call result nor a silent user function may run
    assert "170" not in out, out


# ---- exemptions: legit code must keep working ------------------------------

def test_suffixed_names_stay_allowed(mmb):
    out = _run(mmb, [
        "DIM grey% = 5",
        "grey% = 7",
        "DIM red$ LENGTH 4",
        'red$ = "hi"',
        'PRINT "SUFFIX OK"',
    ])
    assert "SUFFIX OK" in out, out
    assert "NAME CONFLICT" not in out, out


def test_struct_members_stay_allowed(mmb):
    out = _run(mmb, [
        "10 TYPE pt",
        "20 red AS INTEGER",
        "30 END TYPE",
        "40 DIM p AS pt",
        "50 p.red = 7",
        '60 PRINT "STRUCT OK"',
        "RUN",
    ])
    assert "STRUCT OK" in out, out
    assert "NAME CONFLICT" not in out, out


def test_labels_stay_allowed(mmb):
    out = _run(mmb, [
        "10 grey:",
        '20 PRINT "LABEL OK"',
        "30 END",
        "RUN",
    ])
    assert "LABEL OK" in out, out
    assert "NAME CONFLICT" not in out, out


def test_command_only_keywords_stay_allowed(mmb):
    # Statement-only keywords (DIR, DRAW, COLOR) yield no value in an
    # expression; real apps use them as parameter/variable names
    # (mmcore-apps tetris `sub gm.rotate(dir)`, sudoku `local draw`).
    out = _run(mmb, [
        "10 SUB gm.rotate(dir)",
        "20 PRINT \"DIR OK: \", dir",
        "30 END SUB",
        "40 gm.rotate 5",
        "RUN",
    ])
    assert "DIR OK" in out, out
    out = _run(mmb, ["LOCAL i, y, draw", "LOCAL near_x, far_x, color",
                     'PRINT "DRAW COLOR OK"'])
    assert "DRAW COLOR OK" in out, out
    assert "NAME CONFLICT" not in out, out


# ---- #1334: terminating command keywords cannot be variable names ---------

@pytest.mark.parametrize(
    "line",
    [
        "LOCAL quit",
        "DIM quit",
        "STATIC quit",
        "LOCAL end",
        "DIM stop",
        "DIM reboot",
        "DIM restart",
        "DIM shutdown",
    ],
)
def test_terminating_command_rejected_as_name(mmb, line):
    out = _run(mmb, [line, 'PRINT "SURVIVED"'])
    assert "?NAME CONFLICT: " in out, out
    assert "is a reserved command" in out, out
    assert "SURVIVED" in out, "the interpreter must survive the error"


def test_quit_local_does_not_run_quit(mmb):
    out = _run(mmb, [
        "10 SUB s2()",
        "20 LOCAL quit",
        "30 PRINT \"before\"",
        "40 quit = 0",
        "50 PRINT \"after\"",
        "60 END SUB",
        "70 s2()",
        "RUN",
        'PRINT "SURVIVED"',
    ])
    assert "?NAME CONFLICT: QUIT is a reserved command @20:" in out, out
    assert "before" not in out, out
    assert "after" not in out, out
    assert "SURVIVED" in out, out


# ---- #1337: bare command assignments must not run the command -------------

def test_bare_quit_assignment_exact_repro(mmb):
    out = _run(mmb, [
        "10 quit = 0",
        '20 PRINT "after"',
        "RUN",
        'PRINT "SURVIVED"',
    ])
    assert "?NAME CONFLICT: QUIT is a reserved command @10:" in out, out
    assert "after" not in out, out
    assert "SURVIVED" in out, "the interpreter must survive the error"


@pytest.mark.parametrize(
    "keyword",
    ["quit", "end", "stop", "reboot", "restart", "shutdown"],
)
def test_bare_terminating_assignment_rejected(mmb, keyword):
    out = _run(mmb, [
        "10 %s = 0" % keyword,
        '20 PRINT "after"',
        "RUN",
        'PRINT "SURVIVED"',
    ])
    assert "?NAME CONFLICT: %s is a reserved command" % keyword.upper() in out, out
    assert "after" not in out, out
    assert "SURVIVED" in out, out


@pytest.mark.parametrize(
    "line, name",
    [
        ("quit = 0", "QUIT"),
        ("quit=0", "QUIT"),
        ("quit  =  0", "QUIT"),
        # non-terminating command-only keywords that hijack an assignment
        ("run = 0", "RUN"),
        ("new = 0", "NEW"),
        ("chain = 0", "CHAIN"),
    ],
)
def test_bare_command_assignment_rejected_immediately(mmb, line, name):
    out = _run(mmb, [line, 'PRINT "SURVIVED"'])
    assert "?NAME CONFLICT: %s is a reserved command" % name in out, out
    assert "SURVIVED" in out, out


def test_multiword_end_forms_still_run(mmb):
    out = _run(mmb, [
        "10 IF 1 THEN",
        '20 PRINT "IFOK"',
        "30 END IF",
        "40 TYPE pt",
        "50 x AS INTEGER",
        "60 END TYPE",
        "70 SELECT CASE 1",
        "80 CASE 1",
        '90 PRINT "SELOK"',
        "100 END SELECT",
        "110 SUB s",
        '120 PRINT "SUBOK"',
        "130 END SUB",
        "140 s",
        "150 FUNCTION f1(a)",
        "160 f1 = a + 1",
        "170 END FUNCTION",
        "180 PRINT f1(41)",
        "RUN",
    ])
    assert "IFOK" in out, out
    assert "SELOK" in out, out
    assert "SUBOK" in out, out
    assert "42" in out, out
    assert "NAME CONFLICT" not in out, out


def test_run_command_forms_still_run(mmb):
    out = _run(mmb, ['10 PRINT "RAN"', "RUN"])
    assert "RAN" in out, out
    assert "NAME CONFLICT" not in out, out
    out = _run(mmb, ['RUN "no-such-file.bas"', 'PRINT "SURVIVED"'])
    assert "NAME CONFLICT" not in out, out
    assert "SURVIVED" in out, out


# ---- #1335: SUB/FUNCTION parameter colliding with a global CONST -----------

def test_sub_param_const_clash_named_at_definition(mmb):
    out = _run(mmb, [
        "10 CONST BIG$ = \"abc\"",
        "20 SUB s1(big$)",
        "30 PRINT big$",
        "40 END SUB",
        "50 s1(\"x\")",
        "RUN",
        'PRINT "SURVIVED"',
    ])
    assert "?CONST BIG$ used as parameter of S1 @20:" in out, out
    assert "SURVIVED" in out, out


def test_function_param_const_clash_named_at_definition(mmb):
    out = _run(mmb, [
        "10 CONST BIG$ = \"abc\"",
        "20 FUNCTION s1(big$)",
        "30 s1 = big$",
        "40 END FUNCTION",
        "50 PRINT s1(\"x\")",
        "RUN",
        'PRINT "SURVIVED"',
    ])
    assert "?CONST BIG$ used as parameter of S1 @20:" in out, out
    assert "SURVIVED" in out, out


def test_sub_param_const_clash_named_at_call_after_prescan(mmb):
    # CONST is declared after the SUB, so prescan and the definition line
    # cannot see it; the call-time check must still name the parameter.
    out = _run(mmb, [
        "10 SUB s1(big$)",
        "20 PRINT big$",
        "30 END SUB",
        "40 CONST BIG$ = \"abc\"",
        "50 s1(\"x\")",
        "RUN",
        'PRINT "SURVIVED"',
    ])
    assert "?CONST BIG$ used as parameter of S1 @50:" in out, out
    assert "SURVIVED" in out, out


def test_duplicate_dim_message_unchanged(mmb):
    out = _run(mmb, ["DIM x=1 : DIM x=2"])
    assert "?ALREADY DECLARED" in out, out


def test_normal_declarations_unaffected(mmb):
    out = _run(mmb, [
        "10 FUNCTION f(a)",
        "20 f = a * 2",
        "30 END FUNCTION",
        "40 PRINT f(21)",
        "RUN",
    ])
    assert "> 42" in out, out
    assert "NAME CONFLICT" not in out, out


def test_colour_use_still_resolves(mmb):
    # Using (not declaring) a colour constant must keep returning its value.
    out = _run(mmb, ["PRINT grey"])
    assert "8421504" in out, out
