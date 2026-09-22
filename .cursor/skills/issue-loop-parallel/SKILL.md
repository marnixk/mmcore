---
name: issue-loop-parallel
description: Drain GitHub issues in parallel — fan out one worker thread/worktree per ticket bundle to implement and open PRs concurrently, then one coordinator thread merges them serially and cuts a single minor release. Use when asked to run the issue loop in parallel, fan out issues, bundle issues across threads/worktrees, or drain the tracker concurrently.
---

# Issue loop — parallel (fan-out workers → serial coordinator)

Same objective as `issue-loop` (read GitHub issues, implement, merge to
`master`, cut a minor release), but the **implement + PR** phase runs in
parallel across threads/worktrees while **merge + release** stays serial in one
coordinator.

**Rule:** never parallelize merges, tags, or releases. They race the `master`
ref, the version bump, and `gh release`. Parallelize only isolated
worktrees → PRs.

## Roles

Each parallel worker is its own T3 Code **thread** (which gets its own worktree
automatically, `defaultThreadEnvMode: worktree`) or an explicit
`git worktree`. Exactly one **coordinator** thread owns GitHub state.

### Bundling

Bundle to cut test-suite cost. A full run takes tens of minutes, so the full
suite runs **once, before the release** — never per issue or per worker. Group
every issue that can be validated by the same scoped test run, and prefer
fewer, larger bundles over many tiny ones when the work is local to one area.

- Same subsystem / same files / same test module → same bundle (one thread, one
  targeted run), e.g. #517 + #522 (both screenshot-to-drive).
- Different modules with no shared files → different bundles (true parallel).
- Unknown blast radius → its own bundle, or leave for the coordinator.
- Re-check `gh issue list --state open --json number,title,body,labels` and
  apply the `issue-loop` **not ready** skip rule (title, label, or body).

Each bundle gets a short lowercase slug used for its branch and thread title.

## Testing budget

- **Workers: scoped tests only.** Run the bundle's test module(s) plus
  immediate neighbours — not the full suite.
- **Full suite: exactly once, on `master`, before the release** (coordinator).
- Use `scripts/test-watch.py` for percentage progress on every run, per
  AGENTS.md.

## Coordinator (one thread)

1. List open issues, drop **not ready**, split into bundles, write the
   bundle → issue map.
2. Spawn one worker per bundle (see “Spawning workers”).
3. Collect worker PRs. For each ready PR, **one at a time** (serial):
   - `git fetch origin master`
   - If behind, rebase/merge `origin/master` into the PR branch **in that
     branch's own worktree**; resolve conflicts; push.
   - `GH_TOKEN="$GITHUB_PERSONAL_ACCESS_TOKEN" gh pr merge N --merge`
   - Confirm the issue closed: `gh issue view N --json state` is `CLOSED`.
4. Once all bundles in the batch are merged, run the **full suite once on
   `master`** — the only full run in the loop:

   ```bash
   scripts/build.sh
   .venv/bin/python scripts/test-watch.py start
   ```

   Do not publish unless it is green. Then cut **one** minor release (matches
   AGENTS.md “one minor release at the end”):

   ```bash
   GH_TOKEN="$GITHUB_PERSONAL_ACCESS_TOKEN" scripts/github-release.sh next-minor
   GH_TOKEN="$GITHUB_PERSONAL_ACCESS_TOKEN" scripts/github-release.sh publish X.Y.Z
   scripts/build.sh
   ```

   If the user wants a release per issue, still do them serially here — never
   concurrently.
5. Re-list issues and repeat until the `issue-loop` completion audit holds.

## Worker (one thread per bundle)

Do **not** merge or release. Stop at “PR ready”.

1. You are already on a branch in your own worktree — if not, branch from
   current `master`: `task/<slug>-0ccd` (lowercase, suffix `-0ccd`).
2. Implement the bundle's issues. Repo specifics live in `issue-loop` step 2
   (`mmbasic/`, `console/`, tests, QEMU harness, `OPTIONS WIFI`, Alt+X, etc.).
3. Test **scoped only** — the bundle's test module(s) plus immediate
   neighbours, via the `test-suite-progress` skill, e.g.
   `scripts/test-watch.py start -- tests/test_<area>.py`. Do **not** run the
   full suite here; that happens once, before the release.
4. Commit; `git push -u origin <branch>`.
5. Open the PR against `master` (ManagePullRequest, or `gh pr create` where it
   is unavailable — see `issue-done`). Body must include `Fixes #N` for every
   issue in the bundle. Mark ready when tests pass.
6. Register the PR with T3: `link_pull_request` (required for every PR).
7. Report to the coordinator: issue numbers, branch, PR URL, test status,
   blockers. Then stop.

## Spawning workers

**Manual (works today):** open N new threads in T3 Code; each auto-creates a
worktree. Paste the worker prompt below with the bundle filled in.

**Scripted (T3 host API):** the local server exposes
`POST /api/orchestration/dispatch` (environment-auth required):

- `thread.create` →
  `{ threadId, projectId, title, modelSelection, runtimeMode, branch, worktreePath, createdAt }`
- `thread.turn.start` →
  `{ threadId, message: { messageId, role: "user", text, attachments }, runtimeMode, interactionMode, createdAt }`

Create the branch + worktree first (the API does not), then dispatch
`thread.create` followed by `thread.turn.start` with the worker prompt. Only
worth wiring at scale; the manual path needs no plumbing.

**In-thread subagents (`task` tool):** read-only recon only. They share this
worktree, so they must not commit, branch, or push.

## Worker prompt template

```text
Run the issue-loop-parallel skill in WORKER mode for bundle: #<N>, #<M> (<slug>).

Scope: implement only these issues, in this worktree. Do NOT merge, do NOT cut
a release. Stop when the PR is ready.

- Branch task/<slug>-0ccd from current origin/master.
- Implement per the skill; run only the bundle's scoped tests (test-suite-progress),
  not the full suite.
- Open a PR against master with `Fixes #<N>` for every issue in the bundle;
  use ManagePullRequest or gh; mark ready when tests pass.
- Register the PR with link_pull_request.
- Report: issues, branch, PR URL, test status, blockers. Then stop.
```

## Hazards / do not

- Do not run the serial `issue-loop` inside a worker — it merges and releases.
- Do not run the full suite in a worker; it runs once, on `master`, before the
  release. Workers use scoped tests only.
- Do not advance `master` from a worker (no `git push origin <branch>:master`);
  only the coordinator merges.
- Do not move a branch another worktree has checked out (AGENTS.md). Rebase a
  PR branch only in that branch's own worktree.
- Do not force-push tags, and never publish two releases concurrently.
- Keep the *different bundles* file-disjoint. Issues that touch the same files
  belong in one bundle; split across bundles, they will conflict at merge and
  must be serialized by the coordinator.
