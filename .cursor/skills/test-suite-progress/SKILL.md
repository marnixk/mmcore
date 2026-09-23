---
name: test-suite-progress
description: Run the QEMU pytest suite in parallel with visible minute-by-minute progress, and index the slow tests. Use when asked to run the tests, watch the suite, monitor test progress, speed up test runs, or run a subset of the QEMU tests.
---

# Test suite progress

The suite boots QEMU per test module (and per `fresh_console` test), so a full
run takes tens of minutes. Run it detached with `scripts/test-watch.py` and
poll `status` so long runs show visible activity instead of looking stuck.

## Reporting progress (required)

A detached run is **not** a progress update. You must surface progress to the
user in the conversation while the run continues:

- Do **not** call `wait` (or any single blocking command) and stay silent until
  it finishes. `wait` hides the run behind one long tool call.
- Instead, loop: `test-watch.py status --sleep 60` (or `--sleep 90`), then post a
  one-line update in your message — `n/N (pct%) · pass X fail Y skip Z · last
  <test>` — and call `status` again. Keep going until the run finishes, then
  post the summary and any failures.
- Example cadence: `status --sleep 60` → reply with the numbers → `status
  --sleep 60` → reply → … Each status call is cheap and safe.
- The same rule applies to scoped runs and to every wave/coordinator update: the
  user should never have to ask "how's the suite going?".

## Quick start

```bash
scripts/build.sh                                    # build once (idempotent)
.venv/bin/python scripts/test-watch.py start        # full suite, 6 workers
.venv/bin/python scripts/test-watch.py status       # one snapshot, cheap
.venv/bin/python scripts/test-watch.py status --sleep 55   # wait then snapshot
.venv/bin/python scripts/test-watch.py wait --interval 60  # heartbeat to the end
.venv/bin/python scripts/test-watch.py stop         # terminate the run
```

`status` prints `done/total`, `pass/fail/skip`, and the last test seen. State
files are `.pytest-watch.log`, `.pytest-watch.pid`, `.pytest-watch.started`.

## Running a subset

Pass pytest node ids or paths after `start --`:

```bash
.venv/bin/python scripts/test-watch.py start -- tests/test_strings_dynamic.py
.venv/bin/python scripts/test-watch.py start -- -p no:cacheprovider --timeout=180 tests/test_input.py
```

## Parallelism

The tests are parallel-safe: every console uses its own QEMU process and unix
sockets, and `tests/conftest.py` serialises `scripts/build.sh` behind a file
lock (and skips the build when the image is newer than its sources) so workers
do not race. `pytest-xdist` is required (see `requirements.txt`).

```bash
.venv/bin/python -m pytest -n 6 --dist loadscope
```

Use `--dist loadscope` so a module's tests stay on one worker and module-scoped
fixtures (one QEMU per module) are reused.

## Indexing slow tests

Add `--durations=25` to list the slowest tests:

```bash
.venv/bin/python scripts/test-watch.py start -- -p no:cacheprovider --durations=25
```

Most slowness is framebuffer capture: prefer
`MMBasicConsole.screen_pixels(coords)` (one screendump, one ImageMagick call)
over a loop of `screen_pixel(x, y)`, and keep OCR crops tight. Tests that use
`fresh_console` boot a fresh QEMU per test; only use it when a clean screen is
actually required.

## Telling regressions from pre-existing failures

Stash the working tree and run the same failing nodes on the baseline:

```bash
git stash push -m baseline
scripts/build.sh
.venv/bin/python -m pytest <failing nodes> -q --tb=short --timeout=180
git stash pop
scripts/build.sh
```

If a node fails on both, it is pre-existing and should not block the change
under test. XFER uploads and some OCR/theme tests are known-flaky; re-run them
serially before treating them as real failures.
