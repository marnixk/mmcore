# raspberrypi-mmbasic

Bare-metal MMBasic for Raspberry Pi (Circle runtime). Compatibility target:
Colour Maximite 2 (CMM2).

## MMBasic sources are local

`picomite-fork/` is an upstream reference submodule only. Language code,
commands, codecs, and tests live in `mmbasic/` and `console/`. Copy anything
needed out of `picomite-fork/` rather than compiling that tree. Circle stays a
submodule.

## Build and test

```bash
scripts/build.sh
.venv/bin/python -m pytest                           # full suite (slow)
.venv/bin/python -m pytest -n 6 --dist loadscope     # parallel
.venv/bin/python -m pytest tests/test_strings.py     # a subset
```

The QEMU harness in `harness/` drives `console/kernel8.img`. To run the suite
with visible progress and poll it while it runs, use the `test-suite-progress`
skill (`scripts/test-watch.py start` / `status` / `wait`).

Live guest->host / external-network tests are opt-in: set `MMCORE_LIVE_NET=1`
to run them. QEMU SLIRP guest->host TCP is only transiently usable in CI, so
they are skipped by default to keep the parallel suite green (#389).

Linux SD-card install: `scripts/install-sdcard.sh --help`. Publishing a
GitHub Release: run the `github-release` skill (it looks up the last `vX.Y.Z`
and asks before building; default is a **minor** bump). Draining the GitHub
issue tracker: run the `issue-loop` skill (implement ready issues one at a
time, scoped tests, merge each to `master`, then one minor release at the end;
skip issues marked **not ready** in the title, a label, or the description, and
file out-of-scope bugs as `follow-up` tickets instead of fixing them). To drain
the tracker in parallel, run the `issue-loop-parallel` skill instead: bundle
issues that share a test area, fan out one worker thread/worktree per bundle to
implement and open PRs, and keep merges plus the single minor release serial in
one coordinator thread (the full suite runs only once, before the release).
Wrapping up a single finished branch: run the `issue-done` skill (commit
outstanding changes, open and merge a PR into `master`, then cut one minor
release).

## Decisions

- **Percentage test-run feedback** — every test-suite run (full or scoped)
  must show progress as a percentage while it runs: `n/N (pct%)` plus running
  pass/fail/skip counts. Use `scripts/test-watch.py start` / `status` / `wait`
  (or an equivalent progress reporter) rather than a bare `pytest -q`, so a
  long run can be polled and its progress reported. **Report that progress to
  the user in the conversation, not just to the log**: poll `status --sleep 60`
  in a loop and post an update each interval (never a single silent blocking
  `wait` that surfaces only at the end). See the `test-suite-progress` skill.

- **Never move a branch another worktree has checked out** — all worktrees
  share one ref store, so `git update-ref refs/heads/<b>`, `git branch -f <b>`,
  or `git push <remote> <b>:<b>` advances the ref without touching the other
  worktree's HEAD, index, or files. Its `git status` then shows a phantom diff
  (the whole commit gap as staged adds/deletes) and `git diff --cached` against
  the old commit comes back empty — the tell that the ref moved underneath a
  clean checkout. Sync locally with `git fetch origin` (updates `origin/*`
  only) or run the update in that worktree; recover elsewhere with
  `git reset --hard` there once you have confirmed there are no real local
  edits.
