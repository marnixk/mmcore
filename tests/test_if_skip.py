"""Block IF arm skipping (#1333).

Once a block IF has run its taken arm, the untaken ELSEIF/ELSE arms are
skipped without walking every line.  A false IF hops from arm to arm instead
of stepping through each arm body.  These tests pin the behaviour and the
speed-up; the older nesting suites cover the structure state itself.
"""


def _load(console, lines):
    assert console.send_line("NEW") == ""
    for line in lines:
        assert console.send_line(line) == "", line


def _lines(out):
    return [ln.strip() for ln in out.replace("\r", "\n").split("\n") if ln.strip()]


def _run(console, timeout=20.0):
    return _lines(console.send_line("RUN", timeout=timeout))


def test_elseif_chain_arms_and_nesting(console):
    """Every arm runs its own body, including nested IF/SELECT blocks."""
    _load(
        console,
        [
            "10 FOR V = 1 TO 4",
            "20 IF V = 1 THEN",
            "30   IF 1 THEN",
            '40     PRINT "a"',
            "50   ELSE",
            '60     PRINT "X"',
            "70   END IF",
            "80 ELSEIF V = 2 THEN",
            '90   IF V = 0 THEN PRINT "X" ELSE PRINT "b"',
            "100 ELSEIF V = 3 THEN",
            "110   SELECT CASE V",
            "120     CASE 3",
            '130       PRINT "c"',
            "140     CASE ELSE",
            '150       PRINT "X"',
            "160   END SELECT",
            "170 ELSE",
            '180   PRINT "d"',
            "190 END IF",
            "200 NEXT V",
        ],
    )
    assert _run(console) == ["a", "b", "c", "d"]


def test_false_if_hops_through_arms_to_else(console):
    """A false IF skips whole arm bodies and lands on the matching ELSE."""
    _load(
        console,
        [
            "10 FOR V = 1 TO 5",
            "20 IF V = 1 THEN",
            '30   PRINT "one"',
            "40 ELSEIF V = 2 THEN",
            '50   PRINT "two"',
            "60 ELSEIF V = 3 THEN",
            '70   PRINT "three"',
            "80 ELSEIF V = 4 THEN",
            '90   PRINT "four"',
            "100 ELSE",
            '110   PRINT "other"',
            "120 END IF",
            "130 NEXT V",
        ],
    )
    assert _run(console) == ["one", "two", "three", "four", "other"]


def test_skipped_arms_do_not_evaluate_conditions(console):
    """Only the conditions up to the first true arm may be evaluated."""
    _load(
        console,
        [
            "10 DIM INTEGER N",
            "20 FUNCTION BUMP()",
            "30 N = N + 1",
            "40 BUMP = 1",
            "50 END FUNCTION",
            "60 C = 1",
            "70 IF C = 1 THEN",
            '80   PRINT "one";N',
            "90 ELSEIF BUMP() THEN",
            '100   PRINT "two";N',
            "110 ELSEIF BUMP() THEN",
            '120   PRINT "three";N',
            "130 ELSE",
            '140   PRINT "else";N',
            "150 END IF",
            "160 C = 3",
            "170 IF C = 1 THEN",
            '180   PRINT "one";N',
            "190 ELSEIF C = 2 THEN",
            '200   PRINT "two";N',
            "210 ELSEIF BUMP() THEN",
            '220   PRINT "three";N',
            "230 ELSEIF BUMP() THEN",
            '240   PRINT "four";N',
            "250 ELSE",
            '260   PRINT "else";N',
            "270 END IF",
            "280 PRINT N",
        ],
    )
    assert _run(console) == ["one0", "three1", "1"]


def test_skipped_arm_nested_if_conditions_not_evaluated(console):
    """Nested block IF conditions inside a skipped arm never run (#1301)."""
    _load(
        console,
        [
            "10 DIM INTEGER N",
            "20 FUNCTION BUMP()",
            "30 N = N + 1",
            "40 BUMP = 1",
            "50 END FUNCTION",
            "60 IF 0 THEN",
            "70   IF BUMP() THEN",
            '80     PRINT "X"',
            "90   ELSEIF BUMP() THEN",
            '100     PRINT "Y"',
            "110   ELSE",
            '120     PRINT "Z"',
            "130   END IF",
            "140 ELSE",
            '150   PRINT "else";N',
            "160 END IF",
            "170 PRINT N",
        ],
    )
    assert _run(console) == ["else0", "0"]


def test_else_arm_with_colon_statements(console):
    """``ELSE : ...`` is an arm, not a label; a block IF after it still nests."""
    _load(
        console,
        [
            "10 IF 0 THEN",
            '20 PRINT "X"',
            '30 ELSE : PRINT "e" : IF 1 THEN',
            '40 PRINT "b"',
            "50 ENDIF",
            "60 ENDIF",
            '70 PRINT "c"',
        ],
    )
    assert _run(console) == ["e", "b", "c"]


def test_keyword_before_colon_is_not_a_label(console):
    """#1341: ``ELSE :`` / ``ENDIF :`` are structure, not phantom labels, so
    the arm and ENDIF pairing survives."""
    _load(
        console,
        [
            "10 IF 0 THEN",
            '20 PRINT "X"',
            "30 ELSE : IF 1 THEN",
            '40 PRINT "b"',
            "50 ENDIF : IF 1 THEN",
            '60 PRINT "c"',
            "70 ENDIF",
            '80 PRINT "d"',
        ],
    )
    assert _run(console) == ["b", "c", "d"]


def test_block_if_state_across_sub_call_in_taken_arm(console):
    """A SUB's own block IF does not disturb the caller's arm (#1295)."""
    _load(
        console,
        [
            "10 SUB SHOW()",
            "20 IF 1 THEN",
            '30   PRINT "s";',
            "40 ELSE",
            '50   PRINT "X"',
            "60 END IF",
            "70 END SUB",
            "80 FOR V = 1 TO 2",
            "90 IF V = 1 THEN",
            "100   SHOW()",
            '110   PRINT "a"',
            "120 ELSEIF V = 2 THEN",
            '130   PRINT "b"',
            "140 END IF",
            "150 NEXT V",
        ],
    )
    assert _run(console) == ["sa", "b"]


def test_tick_during_skipped_arm_keeps_nested_if_count(console):
    """An interrupt inside a skipped arm must not lose the nested-IF count.

    The tick fires between lines of ``SCAN`` while a false block IF is skipping
    its body, which contains a nested block IF.  The outer IF is opened mid-line
    on an ``ELSE`` arm so the arm-to-arm jump cannot skip the body (#1333) and
    the count in ``if_shadow`` owns the walk.  The nested ``END IF`` must still
    pair with the nested block, so the statements after it stay skipped and the
    reference result is preserved (#1342).
    """
    _load(
        console,
        [
            "10 DIM A(8)",
            "20 TK = 0",
            "30 BAD = 0",
            "40 SUB TICK()",
            "50 TK = TK + 1",
            "60 END SUB",
            "70 SUB SCAN()",
            "80 W = 8",
            "90 C = 0",
            "100 FOR R = 8 TO 0 STEP -1",
            "110 IF 0 THEN",
            "120 X = 0",
            "130 ELSE : IF A(R) < 0 THEN",
            "140 IF R <> W THEN",
            "150 C = C + 1",
            "160 END IF",
            "170 W = W - 1",
            "180 END IF",
            "190 NEXT R",
            "200 END SUB",
            "210 FOR K = 0 TO 8 : A(K) = (K MOD 3) - 1 : NEXT K",
            "220 SCAN()",
            "230 REF = W * 100 + C",
            "240 SETTICK 1, TICK",
            "250 FOR K = 1 TO 20000",
            "260 SCAN()",
            "270 IF W * 100 + C <> REF THEN BAD = BAD + 1",
            "280 NEXT K",
            "290 SETTICK 0, TICK",
            '300 PRINT "bad=";BAD;" ref=";REF;" tk=";TK',
        ],
    )
    out = _run(console, timeout=120.0)
    assert len(out) == 1, out
    fields = dict(part.split("=", 1) for part in out[0].split())
    assert fields["ref"] == "503", out
    assert fields["bad"] == "0", out
    assert int(fields["tk"]) > 0, out


def _timing_program(iterations=20000):
    """Three loops with the same taken arm but 2, 21 and 0 arms total."""
    lines = []
    number = 10

    def emit(text):
        nonlocal number
        lines.append(f"{number} {text}")
        number += 10

    emit(f"N = {iterations}")
    for name, arms in (("TS", 2), ("TL", 21), ("TE", 0)):
        emit("T = TIMER")
        emit("FOR I = 1 TO N")
        emit("C = 1")
        emit("IF C <= 4 THEN")
        for k in range(1, arms):
            emit(f"ELSEIF C = {4 + k} THEN")
            emit("X = 1 : Y = 2 : Z = 3 : W = 4")
        emit("END IF")
        emit("NEXT I")
        emit(f"{name} = TIMER - T")
    emit('PRINT "TS="; TS; " TL="; TL; " TE="; TE')
    return lines


def _timings(out):
    text = " ".join(out)
    values = {}
    for part in text.split():
        key, _, value = part.partition("=")
        if key in ("TS", "TL", "TE"):
            values[key] = int(value)
    assert set(values) == {"TS", "TL", "TE"}, out
    return values["TS"], values["TL"], values["TE"]


def _one_line_if_interrupt_program(tick_period):
    """One-line IF arms with calls and PAUSE, run with and without ticks.

    ``tick_period`` is 1 for the interrupt run and 0 for the timers-off
    reference.  Every PAUSE leaves the tick plenty of time to fire, so the
    gosub checkpoint is exercised while a one-line arm is in progress
    (#1353).  The output must not depend on the interrupt.
    """
    return [
        "10 TK = 0",
        "20 SUB TICK()",
        "30 TK = TK + 1",
        "40 END SUB",
        "50 SUB S()",
        '60 PRINT "s";',
        "70 PAUSE 40",
        '80 PRINT "S";',
        "90 END SUB",
        "100 FUNCTION F()",
        "110 PAUSE 40",
        "120 F = 7",
        "130 END FUNCTION",
        "140 X = 0 : Y = 0",
        f"150 SETTICK {tick_period}, TICK",
        '160 IF 1 THEN S() : PRINT "t1" : X = 1 ELSE PRINT "e1" : X = 2',
        '170 IF 1 THEN Y = F() : PRINT "t2";Y : X = 3 ELSE PRINT "e2" : X = 4',
        '180 IF 1 THEN PAUSE 40 : PRINT "t3" : X = 5 ELSE PRINT "e3" : X = 6',
        '190 IF 0 THEN PRINT "t4" : X = 7 ELSE S() : PRINT "e4" : X = 8',
        '200 IF 0 THEN X = 9 ELSE Y = F() : PRINT "e5";Y : X = 10',
        '210 IF 0 THEN S() : PRINT "bad" : X = 11',
        "220 SETTICK 0, TICK",
        '230 PRINT "out";X;Y',
        '240 PRINT "tk";TK > 0',
    ]


def test_one_line_if_arm_survives_settick_interrupt(console):
    """A tick during a one-line IF arm must not change which arm owns the line.

    The arm calls a SUB and a FUNCTION and also polls events with PAUSE while
    SETTICK runs at 1 ms, then continues with more statements on the same line.
    The taken and untaken branches are covered, including an ELSE on the same
    line.  The same program with timers off must print exactly the same arm
    output (#1353).
    """
    expected = ["sSt1", "t27", "t3", "sSe4", "e57", "out107"]
    _load(console, _one_line_if_interrupt_program(1))
    timed = _run(console, timeout=30.0)
    assert timed[-1] == "tk1", timed
    assert timed[:-1] == expected, timed

    _load(console, _one_line_if_interrupt_program(0))
    untimed = _run(console, timeout=30.0)
    assert untimed[-1] == "tk0", untimed
    assert untimed[:-1] == expected, untimed


def test_long_chain_costs_like_short_chain(console):
    """Skipping 19 extra arms must not cost several times the short chain.

    Both loops take the same empty first arm, so a correct arm-to-arm jump
    gives nearly equal times.  Before #1333 the long chain stepped over every
    skipped line and cost ~5x the short one on the native build.  The 3x
    margin (plus a small absolute allowance) keeps QEMU/desktop variance from
    flaking while still failing the old behaviour.
    """
    _load(console, _timing_program())
    ts, tl, te = _timings(_run(console, timeout=120.0))
    assert ts > 0 and te > 0, (ts, tl, te)
    assert tl < ts * 3 + 25, f"long chain too slow: short={ts}ms long={tl}ms empty={te}ms"
