"""Interrupt fuzz/stress invariant (#1352).

With a handler that never touches program state, turning interrupts on must
not change what a program computes.  Each generated program is run twice with
the same seed: once with timers off, once with ``SETTICK 1`` and a handler
that only increments a counter the program never reads.  The printed checksum
must match exactly.

The generator lives in ``tests/interrupt_fuzz.py``.  It emits the structured
constructs that exercise the interpreter's control stacks, and opens false
block IFs mid-line so their skipped bodies are walked through ``if_shadow``
(the state #1342 lost) instead of being hopped over by the arm-to-arm jump.

Configuration (environment variables):

* ``MMCORE_INTERRUPT_FUZZ_PROGRAMS`` - number of generated programs (default 3)
* ``MMCORE_INTERRUPT_FUZZ_SEED`` - base seed (default 1352)
* ``MMCORE_INTERRUPT_FUZZ_ITERATIONS`` - BASIC loop count per program (default 128)
* ``MMCORE_INTERRUPT_FUZZ_TIMEOUT`` - seconds per RUN (default 45)

A failure writes the offending program and both outputs to
``tests/artifacts/interrupt_fuzz/`` so it can be reproduced by hand.
"""

from __future__ import annotations

import os
import time

import pytest

from interrupt_fuzz import (
    DEFAULT_LOOP_ITERATIONS,
    DEFAULT_PROGRAMS,
    DEFAULT_SEED,
    TICK_OFF,
    TICK_ON,
    generate_program,
)

PROGRAMS = int(os.environ.get("MMCORE_INTERRUPT_FUZZ_PROGRAMS", DEFAULT_PROGRAMS))
SEED = int(os.environ.get("MMCORE_INTERRUPT_FUZZ_SEED", DEFAULT_SEED))
LOOP_ITERATIONS = int(
    os.environ.get("MMCORE_INTERRUPT_FUZZ_ITERATIONS", DEFAULT_LOOP_ITERATIONS)
)
RUN_TIMEOUT = float(os.environ.get("MMCORE_INTERRUPT_FUZZ_TIMEOUT", "45"))
ARTIFACT_DIR = os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "artifacts", "interrupt_fuzz"
)


def _lines(out: str) -> list[str]:
    return [ln.strip() for ln in out.replace("\r", "\n").split("\n") if ln.strip()]


def _load(console, source: str) -> None:
    """Enter a program quickly in small serial chunks.

    ``send_line`` waits for a quiet line before every command (0.25s), which
    makes a 100-line generated program take ~25s to enter.  The program is
    written in chunks small enough for the guest's serial input buffer; the
    console processes the numbered lines in order and echoes them back.
    """
    assert console.send_line("NEW") == ""
    chunk: list[str] = []
    size = 0
    for line in source.splitlines():
        cost = len(line) + 1
        if chunk and size + cost > 400:
            console._ser.sendall(("\r".join(chunk) + "\r").encode())
            console.drain(quiet=0.15, timeout=10.0)
            chunk, size = [], 0
        chunk.append(line)
        size += cost
    if chunk:
        console._ser.sendall(("\r".join(chunk) + "\r").encode())
    console.drain(quiet=0.4, timeout=20.0)


def _run(console, source: str) -> list[str]:
    _load(console, source)
    started = time.monotonic()
    out = console.send_line("RUN", timeout=RUN_TIMEOUT)
    elapsed = time.monotonic() - started
    if elapsed >= RUN_TIMEOUT * 0.95:
        console.send_keys(b"\x03", timeout=5.0)
        pytest.fail(f"generated program did not finish within {RUN_TIMEOUT:.0f}s")
    return _lines(out)


def _save_failure(index: int, source: str, reference: list[str], timed: list[str]):
    os.makedirs(ARTIFACT_DIR, exist_ok=True)
    stem = os.path.join(ARTIFACT_DIR, f"seed{SEED}_program{index}")
    with open(stem + ".bas", "w", encoding="utf-8") as fh:
        fh.write(source)
    with open(stem + ".txt", "w", encoding="utf-8") as fh:
        fh.write("timers off:\n")
        fh.write("\n".join(reference) + "\n\n")
        fh.write("SETTICK 1:\n")
        fh.write("\n".join(timed) + "\n")
    return stem + ".bas", stem + ".txt"


def test_generator_is_deterministic():
    """The only difference between the two runs is the SETTICK period."""
    on = generate_program(SEED, 0, LOOP_ITERATIONS, TICK_ON)
    off = generate_program(SEED, 0, LOOP_ITERATIONS, TICK_OFF)
    assert generate_program(SEED, 0, LOOP_ITERATIONS, TICK_ON) == on
    assert on.replace("SETTICK 1, TICK", "SETTICK 0, TICK") == off


def test_tick_handler_fires_with_pause(console):
    """Sanity: SETTICK 1 actually dispatches the handler under the harness."""
    program = [
        "10 DIM INTEGER TK",
        "20 SUB TICK()",
        "30 TK = TK + 1",
        "40 END SUB",
        "50 SETTICK 1, TICK",
        "60 FOR I = 1 TO 10 : PAUSE 2 : NEXT I",
        "70 SETTICK 0, TICK",
        '80 PRINT "tk"; TK > 0',
    ]
    assert _run(console, "\n".join(program) + "\n") == ["tk1"]


@pytest.mark.parametrize("index", range(PROGRAMS))
def test_generated_program_is_interrupt_invariant(console, index):
    """Timers off and SETTICK 1 must produce exactly the same output."""
    reference = _run(console, generate_program(SEED, index, LOOP_ITERATIONS, TICK_OFF))
    assert reference, "generated program printed nothing"
    assert not any(line.startswith("?") for line in reference), reference

    timed = _run(console, generate_program(SEED, index, LOOP_ITERATIONS, TICK_ON))

    if timed != reference:
        timed_source = generate_program(SEED, index, LOOP_ITERATIONS, TICK_ON)
        bas_path, txt_path = _save_failure(
            index, timed_source, reference, timed
        )
        pytest.fail(
            "interrupt changed the program result\n"
            f"seed={SEED} program={index} iterations={LOOP_ITERATIONS}\n"
            f"program: {bas_path}\noutput: {txt_path}\n"
            f"timers off: {reference}\nSETTICK 1:  {timed}\n"
            f"program source:\n{timed_source}"
        )
