---
name: issue-done
description: Finish the work on the current branch in one pass — commit outstanding changes, open a PR, merge it into master, then cut a minor GitHub release. Use when the user says the issue is done, to ship a branch, wrap up a task, commit–PR–merge–release, or run the issue-done skill.
---

# Issue done (commit → PR → merge → minor release)

One-shot finish for a branch that already has the implementation. Unlike
`issue-loop`, this does **not** pick issues or loop — it flushes the current
branch through to a release exactly once.

**Objective:** commit whatever is outstanding, get it reviewed via a PR,
merge that PR into `master`, then publish a **minor** GitHub release.

## Preconditions

- The implementation is done and the relevant tests pass. If not, stop — the
  `issue-loop` per-issue cycle is the right skill.
- You are on a `task/...` (or `t3code/...`) branch, not `master`. If you are on
  `master`, create a branch first and move the work onto it.
- The release is a **minor** bump. Do not stop to ask which version, and do not
  use patch, unless the user names a different bump in the invoking message.

## 1. Commit outstanding changes

```bash
git status
git diff
git log --oneline -5
```

- Stage and commit every intended change with a descriptive message matching
  the repo style. Inspect the diff first; never commit secrets or junk.
- Leave `dist/` alone (gitignored). Do **not** stage a `picomite-fork`
  submodule pointer bump (`modified content`) unless the change actually
  belongs to this task — it is an upstream reference only.
- If the working tree is already clean, skip to step 2 (nothing to commit).

```bash
git add <paths>
git commit -m "<descriptive message>"
```

## 2. Push and open the PR

```bash
git fetch origin master
git push -u origin "$(git branch --show-current)"
```

Create or update the PR against `master` with **ManagePullRequest** (the repo
convention — not `gh pr create`). Set `branch_name` and `base_branch: master`.
Include `Fixes #N` in the body (add `Fixes #M` for extra issues the PR closes).
Mark it ready (`draft=false`) once its tests pass.

If the host exposes a pull-request linker (T3 Code `link_pull_request`) and the
ManagePullRequest tool is unavailable, create the PR with `gh pr create` and
then register its URL with the linker.

## 3. Merge into master

```bash
GH_TOKEN="$GITHUB_PERSONAL_ACCESS_TOKEN" gh pr merge N --merge
```

- If GraphQL says the PR is still a draft, set `draft=false` and retry.
- Do not squash unless the user asks.

Then sync `master`:

```bash
git fetch origin master
git checkout master
git pull origin master
```

Confirm the linked issue closed: `gh issue view N --json state` should be
`CLOSED` (close it if the keyword did not fire).

## 4. Minor release

This skill **already chose minor**. Do not stop to ask and do not use patch.

```bash
GH_TOKEN="$GITHUB_PERSONAL_ACCESS_TOKEN" scripts/github-release.sh next-minor
GH_TOKEN="$GITHUB_PERSONAL_ACCESS_TOKEN" scripts/github-release.sh publish X.Y.Z
```

This skips the `github-release` skill's "ask, then wait" step. Publish from
`master` with a clean tree (except `dist/`).

After `publish`, restore the QEMU kernel (`package-release.sh` reconfigures
Circle `--qemu` but may leave a hardware `kernel8.img`):

```bash
scripts/build.sh
```

## 5. Report

- Show the merged PR URL and the release URL (`gh release view vX.Y.Z`).
- Do not commit `dist/`.
- If packaging was interrupted, restore the QEMU config with:
  `(cd circle && ./configure -r 3 -p aarch64-none-elf- --qemu -f)`.
