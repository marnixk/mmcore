"""Control-flow nesting regressions (#1299, #1300, #1301).

The cases come from the nesting audit suite; each program is loaded into the
interpreter and its printed lines compared exactly.
"""

import pytest


def _run(console, src):
    assert console.send_line("NEW") == ""
    for number, line in enumerate(src.split("\n"), 1):
        assert console.send_line(f"{number * 10} {line}") == ""
    out = console.send_line("RUN")
    return [ln.strip() for ln in out.replace("\r", "\n").split("\n") if ln.strip()]


CASES = {
    'c1_elseif_chain_nested_arms': (
        '''\
FOR v = 1 TO 4
  IF v = 1 THEN
    FOR i = 1 TO 2
      IF i = 2 THEN
        PRINT "a";v;i
      ENDIF
    NEXT i
  ELSEIF v = 2 THEN
    j = 0
    DO
      j = j + 1
      IF j = 2 THEN
        IF v = 2 THEN
          PRINT "b";v;j
        ELSE
          PRINT "X"
        ENDIF
      ENDIF
    LOOP UNTIL j = 3
  ELSEIF v = 3 THEN
    SELECT CASE v * 2
      CASE 6
        IF v = 3 THEN
          IF 1 THEN
            PRINT "c";v
          ENDIF
        ENDIF
      CASE ELSE
        PRINT "X"
    END SELECT
  ELSE
    k = 0
    WHILE k < 2
      k = k + 1
      IF k = 1 THEN
        PRINT "d";v;k
      ELSEIF k = 2 THEN
        PRINT "e";v;k
      ENDIF
    WEND
  ENDIF
NEXT v
PRINT "end"
''',
        """\
a12
b22
c3
d41
e42
end
""",
    ),
    'c2_select_cases_with_nested_if_no_nested_select': (
        '''\
FOR v = 0 TO 5
  SELECT CASE v
    CASE 0
      IF v = 0 THEN
        IF 1 THEN
          PRINT "zero"
        ELSE
          PRINT "X"
        ENDIF
      ENDIF
    CASE 1 TO 2
      FOR i = 1 TO v
        IF i = v THEN PRINT "r";v;i
      NEXT i
    CASE 3, 4
      IF v = 3 THEN
        PRINT "three"
      ELSEIF v = 4 THEN
        DO
          PRINT "four"
          EXIT DO
        LOOP
      ENDIF
    CASE ELSE
      IF v = 5 THEN PRINT "else5" ELSE PRINT "X"
  END SELECT
NEXT v
PRINT "end"
''',
        """\
zero
r11
r22
three
four
else5
end
""",
    ),
    'c3_select_in_taken_case': (
        '''\
FOR v = 1 TO 3
  SELECT CASE v
    CASE 1
      SELECT CASE v + 1
        CASE 2
          PRINT "in2"
        CASE ELSE
          PRINT "inE"
      END SELECT
      PRINT "after-inner"
    CASE 2
      PRINT "outer2"
    CASE ELSE
      PRINT "outerE"
  END SELECT
NEXT v
PRINT "end"
''',
        """\
in2
after-inner
outer2
outerE
end
""",
    ),
    'c4_select_in_skipped_case': (
        '''\
v = 2
SELECT CASE v
  CASE 1
    SELECT CASE v
      CASE 2
        PRINT "X-inner"
    END SELECT
    PRINT "X-outer1"
  CASE 2
    PRINT "ok2"
  CASE ELSE
    PRINT "X-else"
END SELECT
PRINT "end"
''',
        """\
ok2
end
""",
    ),
    'c5_select_3deep_taken': (
        '''\
FOR a = 1 TO 2
  SELECT CASE a
    CASE 1
      FOR b = 1 TO 2
        SELECT CASE b
          CASE 1
            SELECT CASE a + b
              CASE 2
                PRINT "abc";a;b
              CASE ELSE
                PRINT "X1"
            END SELECT
          CASE 2
            PRINT "b2";a
          CASE ELSE
            PRINT "X2"
        END SELECT
      NEXT b
    CASE 2
      PRINT "a2"
    CASE ELSE
      PRINT "X3"
  END SELECT
NEXT a
PRINT "end"
''',
        """\
abc11
b21
a2
end
""",
    ),
    'c6_select_in_if_in_case': (
        '''\
v = 1
SELECT CASE v
  CASE 1
    IF v = 1 THEN
      SELECT CASE v * 10
        CASE 10
          PRINT "ten"
      END SELECT
    ENDIF
    PRINT "c1-after"
  CASE 10
    PRINT "X-ten-outer"
  CASE ELSE
    PRINT "X-else"
END SELECT
PRINT "end"
''',
        """\
ten
c1-after
end
""",
    ),
    'c7_select_in_sub_called_from_case': (
        '''\
SUB k(n)
  SELECT CASE n
    CASE 5
      PRINT "k5"
    CASE ELSE
      PRINT "kE"
  END SELECT
END SUB
FOR v = 1 TO 3
  SELECT CASE v
    CASE 1
      k 5
      PRINT "c1"
    CASE 2
      k 2
      PRINT "c2"
    CASE ELSE
      PRINT "cE"
  END SELECT
NEXT v
PRINT "end"
''',
        """\
k5
c1
kE
c2
cE
end
""",
    ),
    'x1_exit_do_through_for_named_next': (
        '''\
FOR k = 1 TO 3
  DO
    FOR j = 1 TO 3
      IF j = 2 THEN EXIT DO
    NEXT j
  LOOP
  PRINT "k";k;j
NEXT k
PRINT "end"
''',
        """\
k12
k22
k32
end
""",
    ),
    'x2_exit_for_through_do_then_outer_loop': (
        '''\
c = 0
DO
  FOR i = 1 TO 3
    DO
      IF i = 2 THEN EXIT FOR
      EXIT DO
    LOOP
  NEXT i
  c = c + 1
  PRINT "c";c;i
LOOP UNTIL c >= 3
PRINT "end"
''',
        """\
c12
c22
c32
end
""",
    ),
    'x3_exit_for_through_while_then_outer_wend': (
        '''\
c = 0
WHILE c < 3
  FOR i = 1 TO 3
    w = 0
    WHILE w < 5
      w = w + 1
      IF i = 2 AND w = 2 THEN EXIT FOR
    WEND
  NEXT i
  c = c + 1
  PRINT "c";c;i;w
WEND
PRINT "end"
''',
        """\
c122
c222
c322
end
""",
    ),
    'x4_exit_do_through_while': (
        '''\
FOR r = 1 TO 40
  DO
    w = 0
    WHILE w < 5
      w = w + 1
      IF w = 3 THEN EXIT DO
    WEND
  LOOP
  t = t + w
NEXT r
PRINT "t";t
''',
        """\
t120
""",
    ),
    'g1_if_31_deep_taken': (
        '''\
IF 1 THEN
IF 1 THEN
IF 1 THEN
IF 1 THEN
IF 1 THEN
IF 1 THEN
IF 1 THEN
IF 1 THEN
IF 1 THEN
IF 1 THEN
IF 1 THEN
IF 1 THEN
IF 1 THEN
IF 1 THEN
IF 1 THEN
IF 1 THEN
IF 1 THEN
IF 1 THEN
IF 1 THEN
IF 1 THEN
IF 1 THEN
IF 1 THEN
IF 1 THEN
IF 1 THEN
IF 1 THEN
IF 1 THEN
IF 1 THEN
IF 1 THEN
IF 1 THEN
IF 1 THEN
IF 1 THEN
PRINT "bottom"
ENDIF
ENDIF
ENDIF
ENDIF
ENDIF
ENDIF
ENDIF
ENDIF
ENDIF
ENDIF
ENDIF
ENDIF
ENDIF
ENDIF
ENDIF
ENDIF
ENDIF
ENDIF
ENDIF
ENDIF
ENDIF
ENDIF
ENDIF
ENDIF
ENDIF
ENDIF
ENDIF
ENDIF
ENDIF
ENDIF
ENDIF
PRINT "end"
''',
        """\
bottom
end
""",
    ),
    'g2_if_40_deep_inside_skipped_branch': (
        '''\
IF 1 THEN
  IF 0 THEN
    IF 1 THEN
    IF 1 THEN
    IF 1 THEN
    IF 1 THEN
    IF 1 THEN
    IF 1 THEN
    IF 1 THEN
    IF 1 THEN
    IF 1 THEN
    IF 1 THEN
    IF 1 THEN
    IF 1 THEN
    IF 1 THEN
    IF 1 THEN
    IF 1 THEN
    IF 1 THEN
    IF 1 THEN
    IF 1 THEN
    IF 1 THEN
    IF 1 THEN
    IF 1 THEN
    IF 1 THEN
    IF 1 THEN
    IF 1 THEN
    IF 1 THEN
    IF 1 THEN
    IF 1 THEN
    IF 1 THEN
    IF 1 THEN
    IF 1 THEN
    IF 1 THEN
    IF 1 THEN
    IF 1 THEN
    IF 1 THEN
    IF 1 THEN
    IF 1 THEN
    IF 1 THEN
    IF 1 THEN
PRINT "X"
    ENDIF
    ENDIF
    ENDIF
    ENDIF
    ENDIF
    ENDIF
    ENDIF
    ENDIF
    ENDIF
    ENDIF
    ENDIF
    ENDIF
    ENDIF
    ENDIF
    ENDIF
    ENDIF
    ENDIF
    ENDIF
    ENDIF
    ENDIF
    ENDIF
    ENDIF
    ENDIF
    ENDIF
    ENDIF
    ENDIF
    ENDIF
    ENDIF
    ENDIF
    ENDIF
    ENDIF
    ENDIF
    ENDIF
    ENDIF
    ENDIF
    ENDIF
    ENDIF
    ENDIF
  ELSE
    PRINT "else-ok"
  ENDIF
ENDIF
PRINT "end"
''',
        """\
else-ok
end
""",
    ),
    'g3_comments_on_block_keywords': (
        '''\
IF 0 THEN ' c1
  PRINT "X1"
ELSEIF 0 THEN ' c2
  PRINT "X2"
ELSE ' c3
  IF 1 THEN ' c4
    PRINT "ok"
  END IF ' c5
END IF ' c6
PRINT "end"
''',
        """\
ok
end
""",
    ),
    'g4_block_if_after_colon_in_skipped_branch': (
        '''\
IF 0 THEN
  x = 1 : IF 1 THEN
    PRINT "X2"
  ENDIF
  PRINT "X3"
ELSE
  PRINT "ok"
ENDIF
PRINT "end"
''',
        """\
ok
end
""",
    ),
    'g5_next_with_two_vars': (
        '''\
FOR i = 1 TO 2
  FOR j = 1 TO 2
    PRINT i;j
NEXT j, i
PRINT "end"
''',
        """\
11
12
21
22
end
""",
    ),
}


@pytest.mark.parametrize("name", sorted(CASES))
def test_nesting_case(console, name):
    src, expected = CASES[name]
    assert _run(console, src) == expected.splitlines()
