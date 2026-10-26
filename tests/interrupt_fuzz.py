"""Deterministic program generator for the interrupt fuzz harness (#1352).

The harness checks one invariant: a handler that never touches program state
must not change what a program computes, no matter where ``SETTICK`` lands.

Each generated program:

* builds a checksum from control flow, so a line that runs when it should be
  skipped (or a loop that iterates one extra time) changes the printed result;
* uses the structured constructs that exercise the interpreter's control
  stacks: nested block IF / ELSEIF / ELSE, SELECT CASE, DO / WHILE / FOR
  (same-line and multi-line), SUB / FUNCTION calls, GOSUB and ``ON n GOSUB``;
* opens the outer false block IF on an ``ELSE :`` arm (with a nested IF in
  its skipped body) so the arm-to-arm jump in ``if_skip_pc`` cannot skip the
  body.  Those programs walk the skipped lines through ``if_shadow``, which is
  the state #1342 lost;
* never reads the tick counter.  The tick handler only increments a private
  ``TK`` variable, so the reference (timers off) and timed outputs must be
  byte-for-byte identical.

Generation is a pure function of the seed, program index, loop count and tick
period.  Call it twice with the same arguments (only the period differs) and
the emitted program is the same.
"""

from __future__ import annotations

import random

DEFAULT_SEED = 1352
DEFAULT_PROGRAMS = 3
DEFAULT_LOOP_ITERATIONS = 128
TICK_OFF = 0
TICK_ON = 1


class _Builder:
    """Emit MMBasic source lines with increasing line numbers."""

    def __init__(self) -> None:
        self.lines: list[str] = []
        self._number = 10

    def emit(self, text: str) -> None:
        self.lines.append(f"{self._number} {text}")
        self._number += 10

    def at(self, number: int, text: str) -> None:
        self.lines.append(f"{number} {text}")

    def text(self) -> str:
        return "\n".join(self.lines) + "\n"


def _pick(rng: random.Random) -> int:
    return rng.randrange(1, 9)


def _false_skip_block(b: _Builder, rng: random.Random) -> None:
    """A false block IF opened on an ``ELSE :`` arm with nested skipped IFs.

    The inner IF follows ``ELSE :`` on the arm line, so the jump table records
    its pc as -1 and ``if_skip_pc`` cannot hop over the body.  Every skipped
    line is then walked with ``if_shadow`` holding the nested-IF depth, which
    is what a tick handler used to lose (#1342).

    The outer IF deliberately has no END IF: this is the #1333 corner the
    regression test uses (``ELSE : IF ...`` is an arm at runtime but a label to
    the jump-table builder), so the enclosing FOR leaves the leftover frame on
    exit and the skipped arm is always walked.  Keep it inside the main FOR.
    """
    a = _pick(rng)
    m = rng.choice((2, 3, 4, 5))
    big = rng.randrange(1000, 9000)
    other = rng.randrange(1000, 9000)
    b.emit("IF 0 THEN")
    b.emit(f"  H = H + {big - 1}")
    b.emit("ELSE : IF 0 THEN")
    b.emit(f"  IF (K + {a}) MOD {m} = 0 THEN")
    for i in range(3):
        b.emit(f"    H = H + {big + i}")
        b.emit("    N = N + 1")
    b.emit("  END IF")
    b.emit(f"  H = H + {other}")
    b.emit("  N = N + 2")
    b.emit(f"  IF K > {rng.randrange(1, 20)} THEN")
    b.emit(f"    H = H + {other + 1}")
    b.emit("  END IF")
    b.emit("END IF")


def _nested_if_block(b: _Builder, rng: random.Random) -> None:
    a = _pick(rng)
    m = rng.choice((3, 4, 5, 6))
    b.emit(f"V = (K + {a}) MOD {m}")
    b.emit("IF V = 0 THEN")
    b.emit(f"  ACC({_pick(rng)})")
    b.emit("  IF (K MOD 2) = 0 THEN")
    b.emit(f"    H = H + {_pick(rng)}")
    b.emit("  ELSEIF (K MOD 3) = 0 THEN")
    b.emit(f"    ACC({_pick(rng)})")
    b.emit("  ELSE")
    b.emit(f"    H = H + {_pick(rng)}")
    b.emit("  END IF")
    b.emit("ELSEIF V = 1 THEN")
    b.emit(f"  H = H + {_pick(rng)}")
    b.emit("  IF (K MOD 5) = 0 THEN")
    b.emit(f"    ACC({_pick(rng)})")
    b.emit("  END IF")
    b.emit("ELSE")
    b.emit(f"  ACC({_pick(rng)})")
    b.emit("  IF (K MOD 4) = 0 THEN")
    b.emit(f"    H = H + {_pick(rng)}")
    b.emit("  ELSE")
    b.emit("    N = N + 1")
    b.emit("  END IF")
    b.emit("END IF")


def _select_block(b: _Builder, rng: random.Random) -> None:
    a = _pick(rng)
    b.emit(f"SELECT CASE (K + {a}) MOD 5")
    b.emit("  CASE 0")
    b.emit(f"    ACC({_pick(rng)})")
    b.emit("    IF (K MOD 2) = 0 THEN")
    b.emit(f"      H = H + {_pick(rng)}")
    b.emit("    END IF")
    b.emit("  CASE 1 TO 3")
    b.emit(f"    H = H + {_pick(rng)}")
    b.emit("  CASE ELSE")
    b.emit(f"    ACC({_pick(rng)})")
    b.emit("END SELECT")


def _loops_block(b: _Builder, rng: random.Random) -> None:
    a = _pick(rng)
    b.emit(f"FOR J = 1 TO 3 : H = H + J * {a} : NEXT J")
    b.emit("FOR J = 1 TO 2")
    b.emit(f"  H = H + {a} * J")
    b.emit("NEXT J")
    b.emit(f"L = 0 : DO : L = L + 1 : H = H + L : LOOP UNTIL L >= {rng.choice((2, 3))}")
    b.emit("L = 0")
    b.emit(f"WHILE L < {rng.choice((2, 3))}")
    b.emit("  L = L + 1")
    b.emit(f"  H = H + {a}")
    b.emit("WEND")


def _calls_block(b: _Builder, rng: random.Random) -> None:
    b.emit(f"ACC({_pick(rng)}) : H = H + 1")
    b.emit(f"H = H + FN({_pick(rng)})")
    b.emit("GOSUB 9000")
    b.emit("ON (K MOD 3) + 1 GOSUB 9000, 9100, 9200")


def _colon_block(b: _Builder, rng: random.Random) -> None:
    a = _pick(rng)
    b.emit(f"W = W + {a} : L = 1 : FOR J = 1 TO 2 : H = H + L * {a} : L = L + 1 : NEXT J")
    b.emit(f"H = H + 1 : IF (K MOD 2) = 0 THEN H = H + {a} ELSE H = H + {a + 1}")
    b.emit("IF (K MOD 7) = 0 THEN PAUSE 1")


def generate_program(
    seed: int = DEFAULT_SEED,
    index: int = 0,
    iterations: int = DEFAULT_LOOP_ITERATIONS,
    tick_period: int = TICK_ON,
) -> str:
    """Return a deterministic MMBasic program exercising interrupt boundaries.

    ``tick_period`` is the only argument that differs between the two runs of
    one program: 0 disables SETTICK, 1 fires the no-op handler as often as the
    interpreter checks for events.
    """
    rng = random.Random((seed * 1_000_003) ^ (index * 7_919))
    b = _Builder()

    b.emit("DIM INTEGER TK, H, N, K, W, V, J, L")
    b.emit("TK = 0 : H = 0 : N = 0 : W = 0")
    b.emit("SUB TICK()")
    b.emit("  TK = TK + 1")
    b.emit("END SUB")
    b.emit("SUB ACC(P)")
    b.emit("  H = H + P")
    b.emit("  N = N + 1")
    b.emit("END SUB")
    b.emit("FUNCTION FN(P)")
    b.emit("  FN = P * 2 + 1")
    b.emit("END FUNCTION")

    # Always include the false-arm walkers; they are the #1342 trigger.
    fragments = [
        lambda: _false_skip_block(b, rng),
        lambda: _false_skip_block(b, rng),
        lambda: _nested_if_block(b, rng),
        lambda: _select_block(b, rng),
        lambda: _loops_block(b, rng),
        lambda: _calls_block(b, rng),
        lambda: _colon_block(b, rng),
    ]
    rng.shuffle(fragments)

    b.emit(f"SETTICK {tick_period}, TICK")
    b.emit(f"FOR K = 1 TO {iterations}")
    for fragment in fragments:
        fragment()
    b.emit("NEXT K")
    b.emit("SETTICK 0, TICK")
    b.emit('PRINT "H="; H; " N="; N; " W="; W')
    b.emit("END")

    g0, g1, g2 = (_pick(rng) for _ in range(3))
    b.at(9000, f"H = H + {g0} : N = N + 1")
    b.at(9010, "RETURN")
    b.at(9100, f"H = H + {g1 + 100} : N = N + 1")
    b.at(9110, "RETURN")
    b.at(9200, f"H = H + {g2 + 200} : N = N + 1")
    b.at(9210, "RETURN")
    return b.text()
