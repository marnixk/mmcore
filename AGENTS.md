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

Linux SD-card install: `scripts/install-sdcard.sh --help`. Publishing a
GitHub Release: run the `github-release` skill (it looks up the last `vX.Y.Z`
and asks before building; default is a **minor** bump). Draining the GitHub
issue tracker: run the `issue-loop` skill (one issue, merge to `master`, minor
release, repeat; skip issues marked **not ready** in the title, a label, or
the description).
