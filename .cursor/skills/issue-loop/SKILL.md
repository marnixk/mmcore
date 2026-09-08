---
name: issue-loop
description: Drain GitHub issues one at a time — implement, merge to master, then cut a minor GitHub release — until no actionable open issues remain. Use when the user asks to drain issues, implement open issues, restart the issue goal, run the issue loop, or ship until the tracker is empty.
---

# Issue loop (implement → merge → minor release)

Standing process for this repository. Do not shrink it to “just open a PR”
or “fix one bug and stop” unless the user names a single issue.

**Objective:** read GitHub issues and implement them. After **each** issue:
commit, push, **merge into `master`**, then cut a **minor** GitHub release.
Loop until no **actionable** open issues remain.

## Persistent goal

If Cursor goals are available (`CreateGoal` / `UpdateGoal`):

- Keep **one** active goal whose objective is the paragraph above.
- On “restart goal” / “resume”, set that goal **active** and continue from
  the current tracker and git state, not from memory.
- Do **not** mark the goal complete because a turn is ending, a PR exists,
  or tests are green for one issue.
- Mark complete only after the completion audit below passes.

## Skip

Skip an issue when it is marked **not ready** in **any** of these places
(case insensitive). Leave it open. It does not block the loop.

- **Title** — contains `not ready` (including `[not ready]`).
- **Label** — any label named `not ready` or `not-ready`.
- **Description** — the issue body contains `not ready`.

Check with `gh issue list --state open --json number,title,body,labels`
before picking work. Treat every other `OPEN` issue as work.

## Per-issue cycle

Work **one** issue at a time (two only when they are the same change, e.g.
one font fix for two tickets). After that issue is on `master` and released,
re-list issues and pick the next.

### 1. Sync and branch

```bash
git fetch origin master
git checkout master
git pull origin master
```

Branch from current `master`:

```text
task/<short-descriptive-name>-0ccd
```

Lowercase. Prefix `task/`, suffix `-0ccd`.

### 2. Implement

- Language, commands, codecs, fonts, tests: `mmbasic/` and `console/`.
- `picomite-fork/` is reference only — copy out, do not compile it.
- Circle (`circle/`) stays a submodule.
- `OPTIONS WIFI` is the stored-credential connect command (not `OPTION WIFI`).
- Prefer tests in `tests/` plus `scripts/build.sh` and
  `.venv/bin/python -m pytest` (targeted first, then relevant neighbours).
- QEMU harness: `harness/` drives `console/kernel8.img`.
- Fullscreen apps (TERM, editor, FILES, WORDPAD): raw serial, not
  `send_line` (that hangs on `>`). USB Alt in tests is `bytes([1]) + b"…"`.
- Editor quit is **Alt+X**. Ctrl+X is cut.
- `send_line()` strips leading/trailing whitespace; treat `> ` as the
  prompt.
- HDMI `screen_pixel()` can be overwritten by the QEMU console; prefer
  `PIXEL(x,y)` for graphics-page assertions.

### 3. Commit and PR

Commit with a descriptive message. Push:

```bash
git push -u origin <branch-name>
```

Create or update the PR with **ManagePullRequest** (not `gh pr create`).
Set `branch_name` and `base_branch` (`master`). Draft while testing; mark
ready (`draft=false`) when the issue’s tests pass.

PR body must include `Fixes #N` (and extra `Fixes #M` when one PR closes
several).

### 4. Merge

The user asked to merge. After the PR is ready:

```bash
GH_TOKEN="$GITHUB_PERSONAL_ACCESS_TOKEN" gh pr merge N --merge
```

If GraphQL says the PR is still a draft, set `draft=false` and retry.
Do not squash unless the user asks.

Then:

```bash
git fetch origin master
git checkout master
git pull origin master
```

Confirm `gh issue view N --json state` is `CLOSED` (or close it if the
keyword did not fire).

### 5. Minor release

This loop **already chose minor**. Do not stop to ask. Do not use patch.

```bash
GH_TOKEN="$GITHUB_PERSONAL_ACCESS_TOKEN" scripts/github-release.sh next-minor
GH_TOKEN="$GITHUB_PERSONAL_ACCESS_TOKEN" scripts/github-release.sh publish X.Y.Z
```

That skips the `github-release` skill’s “ask, then wait” step. Publish from
`master` with a clean tree (except `dist/`).

After `publish`, restore the QEMU kernel:

```bash
scripts/build.sh
```

(`package-release.sh` reconfigures Circle `--qemu` but may leave a hardware
`kernel8.img`.)

## Completion audit

Before declaring the loop done, verify **current** state:

1. `gh issue list --state open --json number,title,body,labels` — every
   remaining issue is marked **not ready** (title, label, or body), or
   the list is empty.
2. Each implemented issue has a merged PR and a `vX.Y.Z` release that
   includes its merge (or a later minor that includes it).
3. Local `master` matches `origin/master`.

If any actionable issue is still `OPEN`, keep going. Do not mark the
Cursor goal complete until this audit holds.
