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

### Periodic check-in (every ~30 min)

A batch can take a while and the tracker can grow while it runs. Re-check the
tracker at least every ~30 minutes and again whenever a batch finishes, so a
newly filed **ready** issue joins this run instead of waiting for the next one.

At each check-in:

1. List and diff:

   ```bash
   gh issue list --state open --limit 100 --json number,title,body,labels
   ```

   Ignore numbers already bundled, queued, merged, or skipped in this run.
2. Apply the `issue-loop` **not ready** skip rule (title/label/body `not ready`,
   or the `follow-up` label) to each new issue; skip and note those.
3. Place each new ready issue by how it interacts with in-flight work:
   - **File-disjoint** from every in-flight worker → start it now. Prefer
     attaching it to a running worker that already owns the same test module
     and has not opened its PR yet; otherwise spawn a new worker
     (`t3-orchestrate.mjs spawn --slug <slug> …`) with the bundle prompt.
   - **Overlaps an in-flight worker's files** → queue it for the next batch
     after those PRs merge, so two workers never edit the same files. Record it
     in the bundle map as `queued (after <slug>)`.
     Decide overlap from the files each worker reported, or check directly with
     `git -C <worktree> fetch origin master && git -C <worktree> diff
     --name-only origin/master...HEAD`.
   - **Unknown blast radius** → its own bundle, or leave it to the coordinator.
4. Update the T3 task list and the bundle → issue map, then log
   `[checkin] +<slug> #N,#M`, `[checkin] queued #N after <slug>`, or
   `[checkin] skipped #N (<reason>)`.

A check-in with nothing new logs `[checkin] no changes` and the coordinator
keeps watching the current workers.

**Release cutoff.** The single minor release still closes the run, so do not
start new workers once the batch's full-suite step has begun. A worker started
at the last check-in must have its PR merged before that step, or it is deferred
to the next run and listed under “queued” in the final summary.

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
   blockers, and the files/areas touched (`git diff --name-only
   origin/master...HEAD`). The file list is what lets the coordinator place new
   issues at a check-in without creating conflicts. Then stop.

## Spawning workers

**Scripted (works today) — preferred.** Drive the local T3 host API with the
bundled driver, `.cursor/skills/issue-loop-parallel/t3-orchestrate.mjs`:

```bash
# projects (id, title, repo root)
node .cursor/skills/issue-loop-parallel/t3-orchestrate.mjs projects

# one worker per bundle; the server creates BOTH the thread and the
# git worktree/branch (from origin/master) and starts the first turn
node .cursor/skills/issue-loop-parallel/t3-orchestrate.mjs spawn \
  --slug term --title "#528/#529 TERM scrollback + replay" \
  --prompt @/tmp/worker-term.txt

# watch / clean up
node .cursor/skills/issue-loop-parallel/t3-orchestrate.mjs threads
node .cursor/skills/issue-loop-parallel/t3-orchestrate.mjs rm-thread <threadId>
```

`spawn` creates the branch/worktree itself
(`git worktree add -b task/<slug>-0ccd <t3-worktrees>/<repo>/task-<slug>-0ccd origin/<base>`),
then dispatches `thread.create` and `thread.turn.start`. The new thread shows up
in the T3 UI so the user can watch it. `--project <name>` picks the project when
there are several (default: the only one, `mmcore`); `--prompt` takes literal
text or `@/path/file`; `--model`/`--instance` override the default model
selection.

Mechanics (so this does not need re-investigating):

- Endpoint `POST /api/orchestration/dispatch` on the server from
  `~/.t3/userdata/server-runtime.json` (default `http://127.0.0.1:3773`).
  `GET /api/orchestration/shell` lists projects/threads.
- The HTTP endpoint calls `orchestrationEngine.dispatch` (the decider) directly,
  so it does **not** process `bootstrap.createThread`/`bootstrap.prepareWorktree`
  — those are handled only on the WebSocket RPC path the desktop uses. Over HTTP
  the thread and worktree must exist first, hence the two-step `spawn`.
- Auth is **environment-auth**: `Authorization: Bearer <session JWT>` with
  scope `orchestration:operate`. A session token is
  `base64url(JSON(claims)) + "." + base64url(HMAC-SHA256(payload, key))`,
  verified against the `auth_sessions` row in `~/.t3/userdata/state.sqlite`;
  the HMAC key is `~/.t3/userdata/secrets/server-signing-key.bin`. There is no
  unauthenticated mint path (`/oauth/token` and `/api/auth/browser-session`
  need a bootstrap/pairing credential, and the desktop's token is not stored in
  plaintext).
- `spawn`/`snapshot`/`threads`/`rm-thread` **mint a 5-minute session per
  invocation and revoke it in a `finally`**, so no credential is left behind.
  Do not hand-mint a long-lived token. A spawned thread does not depend on the
  minting session, so revoking immediately after the dispatch is safe. The DB
  is shared with the live server, so writes use a busy timeout + retry. Sanity
  check: `sqlite3 ~/.t3/userdata/state.sqlite "select count(*) from
  auth_sessions where subject='t3-orchestrate-script';"` → `0`.
- `dispatch <file|->` sends a raw `ClientOrchestrationCommand`
  (`thread.create`, `thread.turn.start`, `thread.delete`, …) when you need
  something the wrapper does not cover.

**Manual fallback:** open N new threads in T3 Code; each auto-creates a
worktree. Paste the worker prompt below.

**In-thread subagents (`task` tool):** read-only recon only. They share this
worktree, so they must not commit, branch, or push.

## Worker prompt template

```text
Run the issue-loop-parallel skill in WORKER mode for bundle: #<N>, #<M> (<slug>).

Scope: implement only these issues, in this worktree. Do NOT merge, do NOT cut
a release. Stop when the PR is ready.

- You are on branch task/<slug>-0ccd in your own worktree (scripted spawn
  creates both); if not, branch from current origin/master.
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
- Do not start a worker for an issue that touches files an in-flight worker is
  editing; queue it for the next batch (see the periodic check-in rules).
- Do not extend the run indefinitely for issues that arrive mid-flight; respect
  the release cutoff and leave later arrivals for the next run.
