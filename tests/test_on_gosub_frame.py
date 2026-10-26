"""`ON n GOSUB` pushes the same control-flow frame as `GOSUB` (#1349).

`ON n GOSUB` used to push only the return PC, so `RETURN` restored the stale
gosub_* checkpoint instead of the caller's.  A caller that was inside an open
block IF, SELECT CASE or DO loop lost that structure, and the arm/loop it was
executing then behaved as if it had already ended.

Each case runs the same body twice: once with a plain `GOSUB` and once with
`ON 1 GOSUB`.  The two must print exactly the same lines, and the lines must be
the ones the enclosing structure implies.
"""

GOSUB_TARGET = [
    "1000 IF 1 THEN",
    '1010   PRINT "sub-if"',
    "1020 END IF",
    "1030 SELECT CASE 1",
    "1040   CASE 1",
    '1050     PRINT "sub-sel"',
    "1060   CASE ELSE",
    '1070     PRINT "sub-else"',
    "1080 END SELECT",
    "1090 RETURN",
]


def _run(console, caller):
    assert console.send_line("NEW") == ""
    program = [f"{n * 10} {line}" for n, line in enumerate(caller, 1)]
    for line in program + GOSUB_TARGET:
        assert console.send_line(line) == "", line
    out = console.send_line("RUN", timeout=20.0)
    return [ln.strip() for ln in out.replace("\r", "\n").split("\n") if ln.strip()]


def _compare(console, caller, expected):
    # Run ON first: a preceding plain GOSUB leaves the stale checkpoint arrays
    # populated and would mask the missing push this test is pinning.
    on = _run(console, [line.replace("{call}", "ON 1 GOSUB 1000") for line in caller])
    plain = _run(console, [line.replace("{call}", "GOSUB 1000") for line in caller])
    assert on == expected
    assert plain == expected


def test_on_gosub_inside_block_if(console):
    _compare(
        console,
        [
            "IF 1 THEN",
            "  {call}",
            '  PRINT "after"',
            "ELSE",
            '  PRINT "else"',
            "END IF",
            'PRINT "done"',
            "END",
        ],
        ["sub-if", "sub-sel", "after", "done"],
    )


def test_on_gosub_inside_select_case(console):
    _compare(
        console,
        [
            "SELECT CASE 1",
            "  CASE 1",
            "    {call}",
            '    PRINT "after"',
            "  CASE ELSE",
            '    PRINT "else"',
            "END SELECT",
            'PRINT "done"',
            "END",
        ],
        ["sub-if", "sub-sel", "after", "done"],
    )


def test_on_gosub_inside_do_loop(console):
    _compare(
        console,
        [
            "I = 0",
            "DO",
            "  I = I + 1",
            "  {call}",
            '  PRINT "after";I',
            "LOOP UNTIL I >= 2",
            'PRINT "done"',
            "END",
        ],
        ["sub-if", "sub-sel", "after1", "sub-if", "sub-sel", "after2", "done"],
    )


def test_on_gosub_out_of_range_pushes_no_frame(console):
    """No branch taken: the line falls through and a later RETURN finds no
    frame of its own (an extra `after` would mean one was left behind)."""
    assert console.send_line("NEW") == ""
    for line in [
        "10 ON ERROR GOTO 900",
        "20 IF 1 THEN",
        "30   ON 2 GOSUB 1000",
        '40   PRINT "after"',
        "50 END IF",
        "60 RETURN",
        "70 END",
        '900 PRINT "ERR"',
        "1000 END",
    ]:
        assert console.send_line(line) == "", line
    out = console.send_line("RUN", timeout=20.0)
    assert [ln.strip() for ln in out.replace("\r", "\n").split("\n") if ln.strip()] == [
        "after",
        "ERR",
    ]
