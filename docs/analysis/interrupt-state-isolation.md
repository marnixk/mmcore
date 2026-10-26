# Interrupt and nested execution state isolation

Analysis of mmCore (MMBasic on Circle) interpreter re-entrancy: how execution
state is stored, which entry points save or leak it, confirmed bugs, and
structural remediation options. Investigation date: 2026-10-26; tree at
`master` with built `console/kernel8.img` verified under QEMU harness.

**Scope:** design and inventory only. Point fixes for #1342 / #1343 are in
flight on `task/coord-if-shadow-end-1342-1343-0ccd`; this document targets the
underlying model.

---

## 1. Executive summary

The interpreter keeps **program position, control flow, and scratch state in the
per-console `mmb` global `G`** (`#define G (*g_cur)`). Nested work reuses that
single blob:

- **SUB/FUNCTION** calls push a **partial** snapshot into `gosub_*` side arrays.
- **SETTICK / ON KEY / mouse** handlers run a **second interpreter loop**
  (`run_gosub_body`) inside the first, saving only a few fields around the call.
- **Expression FUNCTION** calls use the same nested loop.

There is **no single execution frame**. Each path chooses its own save/restore
list. Omissions produce cross-talk between the interrupted program and the
handler. The two open regressions #1342 and #1343 are direct symptoms; the
matrix below lists further high-risk gaps (e.g. `ON n GOSUB` without
`gosub_save_ctrl`, process-global string arena).

**Root cause (structural):** one mutable execution context shared by all nested
runners, with ad hoc checkpointing instead of push/pop of a complete frame.

---

## 2. Mutable execution state inventory

Fields live in `mmb` (`mmbasic/include/mmb_priv.h`). “Readers/writers” name the
main subsystems; many fields are touched from `core.c`, `expr.c`, `vars.c`,
`cmd_*.c`, and command dispatch.

### 2.1 Program counter and line position

| Field | Role | Primary readers/writers |
|-------|------|-------------------------|
| `run_pc` | Index into `prog` / tokenized `tprog` | `run_program`, `run_gosub_body`, branches, `DATA`, errors |
| `branch_pc` | Pending line jump (-1 none, -2 function end) | GOTO/GOSUB/RETURN, loops, SUB entry, `RESUME` |
| `branch_pos` | Mid-line resume pointer into tokenized line | Same-line SUB/FOR/NEXT (#997, #1004, #1276) |
| `G.p` | Current token pointer within a line | Tokenizer, `mmb_expr`, all commands |
| `in_sub` | Scanning past SUB/FUNCTION definition | `mmb_cmd_sub`, `mmb_cmd_function`, `mmb_cmd_end_sub` |
| `cur_sub` | Index of SUB/FUNCTION body executing | Call setup, `gosub_restore_top` |

**Interrupt note:** `mmb_run_events` saves/restores `run_pc`, `G.p`, and
`branch_pc` only; it does **not** save `branch_pos`.

### 2.2 Block IF / one-line IF

| Field | Role | Primary readers/writers |
|-------|------|-------------------------|
| `if_depth`, `if_stack[]` | Block IF nesting (#1273): `IF_BODY`, `IF_SEARCH`, `IF_DONE` | `process_line_structure`, `exec_stmt_loop` |
| `if_inline`, `if_armed`, `if_inline_keep` | One-line IF arm ownership (#1276, #1295) | IF parsing, `exec_stmt_loop`, `gosub_*` |
| `if_shadow` | Count of block IFs seen while skipping false arms (#1301) | `process_line_structure` only |

**Checkpointing:** `gosub_save_ctrl` / `gosub_restore_ctrl` save depth, stack,
and `if_inline`, but **`if_shadow` is zeroed on both save and restore** (not
stored in `gosub_if_*`). This is the confirmed #1342 defect.

### 2.3 SELECT CASE

| Field | Role | Primary readers/writers |
|-------|------|-------------------------|
| `sel_sp`, `sel_base`, `selstack[]` | Nested SELECT; `sel_base` hides caller frames in SUB (#1299) | `process_line_structure`, `gosub_save_ctrl` |

Saved per gosub frame: `gosub_sel_sp`, `gosub_sel_base`; callee bumps
`sel_base = sel_sp` without copying frames.

### 2.4 DO / WHILE / FOR loops

| Field | Role | Primary readers/writers |
|-------|------|-------------------------|
| `ctrl_sp`, `ctrlstack[]` | DO/WHILE; stores `if_depth`/`sel_depth` at entry (#1273, #1300) | `push_ctrl`, `mmb_cmd_wend`, EXIT DO |
| `for_sp`, `forstack[]` | FOR; same-line body `pos`, matching NEXT pcs (#992–#1004) | FOR/NEXT/EXIT/CONTINUE |
| `loop_seq` | Cross-loop ordering for EXIT (#1300) | `push_ctrl`, FOR |

`run_gosub_body` saves/restores `ctrl_sp` and `for_sp` locally; gosub frames
also store `gosub_ctrl_sp` / `gosub_for_sp` for callee cleanup on return.

### 2.5 GOSUB / call stack and bindings

| Field | Role | Primary readers/writers |
|-------|------|-------------------------|
| `gosub_sp`, `gosub_stack[]`, `gosub_pos[]` | Return line / mid-line | GOSUB, CALL, SUB, events |
| `gosub_sub[]`, `gosub_event[]` | Callee identity; `2` = FUNCTION | `mmb_call_named_sub`, `mmb_cmd_end_function` |
| `gosub_nsave`, `gosub_saven`, `gosub_savev`, `gosub_ss` | Argument / function-result save | `mmb_call_named_sub`, `gosub_restore_top` |
| `gosub_nlocal`, `gosub_local` | LOCAL shadow restore | `vars.c`, `gosub_restore_top` |
| `gosub_if_*`, `gosub_ctrl_sp`, `gosub_for_sp`, `gosub_sel_*` | Control-flow checkpoint | `gosub_save_ctrl`, `gosub_restore_ctrl` |

### 2.6 Run lifecycle and errors

| Field | Role | Primary readers/writers |
|-------|------|-------------------------|
| `running` | Main / nested loops continue | `run_program`, `run_gosub_body`, `mmb_cmd_end`, STOP |
| `run_suspended` | Virtual-console switch | `session.c`, `run_program` |
| `quit_requested` | Host exit | `mmb_cmd_quit` |
| `on_error_pc`, `error_active`, `err_resume_pc`, `run_errjmp` | ON ERROR GOTO | `mmb_error`, `run_program`, `mmb_cmd_resume` |
| `tick_busy` | Suppress nested event dispatch | `mmb_run_events`, mouse cursor |
| `prog_dirty`, `prog_ready`, `tok_ready`, `jmp_*` | Tokenize / jump tables | RUN, EDIT |
| `dim_local` | LOCAL in SUB | `cmd_lang.c`, `vars.c` |

**#1343:** `mmb_cmd_end` sets `running = 0`, but `run_gosub_body` restores a
saved `running` on exit, undoing END for FUNCTION / nested `run_gosub_body`
paths. There is no separate “program terminate” latch.

### 2.7 Expression and scratch (non–control-flow but re-entrancy sensitive)

| Field | Role | Primary readers/writers |
|-------|------|-------------------------|
| `func_ret`, `func_ret_s`, `func_ret_blob` | Single FUNCTION return slot | `mmb_try_user_function`, nested calls (#1275) |
| `acc_on`, `acc_sid`, `acc_mtype`, `acc_moff`, `acc_msize`, `acc_base_*` | Fast struct member access | `vars.c`, trace cache |
| `G.out`, `outn` | PRINT buffer | All output |
| `print_x`, `print_y`, `print_locate` | Text cursor | PRINT/LOCATE |
| `data_line`, `data_pos`, `data_off` | READ pointer | `mmb_read_data` |
| `tline`, `tprog`, `tcache[]` | Token buffer / trace cache | Tokenizer, RUN |
| **Process-global** `s_arena` (`util.c`) | Expression string temporaries | `mmb_expr`, `mmb_str_reset`, `mmb_arena_mark` |

`mmb_str_reset` runs **once per program line** in `run_program`; nested
`run_gosub_body` uses **arena marks** instead. The arena heap is **not**
per-console (file-static), so concurrent or switched consoles can theoretically
share temporaries if both run expressions without isolation.

### 2.8 Event registration (global to the running program)

| Field | Role |
|-------|------|
| `tick[]`, `tick_busy` | SETTICK periods and handlers |
| `on_key`, `on_key_ascii[]`, `inkey_*`, `inkey_gen`, `on_key_gen` | ON KEY |
| `on_mouseclick`, `on_mousemove`, `on_mouseup` | Mouse handlers |

Cleared on fresh RUN in `run_program`; handlers read/write program variables
(`G.vars`) shared with main code by design.

### 2.9 Intentionally shared (not frame-local)

Program text, variables, CONST, OPTION defaults (mostly), graphics, open
files, `timer_base`, `rnd_seed`, jump tables, labels, SUB table — these are
**meant** to be visible across handlers and main code.

---

## 3. Nested and re-entrant entry points

### 3.1 Main program driver: `run_program`

- Resets control stacks on new RUN (not on console resume).
- Loop: `mmb_check_break()` → line execution → handle `branch_pc` / FOR same-line.
- `mmb_check_break` → `mmb_break_events` → **`mmb_run_events`** (interrupts).
- ON ERROR: `setjmp(run_errjmp)` per line; `unwind_call_frames()` then jump to handler line.

### 3.2 Nested driver: `run_gosub_body`

Used by:

- SETTICK / ON KEY / per-key ON KEY / mouse handlers (`mmb_run_events`)
- `mmb_try_user_function` (expression FUNCTION calls)
- Not used for statement-level SUB/CALL (those use `branch_pc` in `run_program`)

**Saves/restores on entry/exit:**

- `ctrl_sp`, `for_sp`, `G.running`, `G.branch_pos`
- Arena watermark (`mmb_arena_mark` / `release`)

**Does not save:** `if_shadow`, `if_armed`, `if_inline_keep`, `branch_pos` of
caller (caller restores manually in events path only partially), `G.p` (caller
restores), ON ERROR jmp state, `data_*`, `acc_*`, `func_ret` (caller handles).

Sets `gosub_stack[sp-1] = -2` while running (function return protocol).

### 3.3 `mmb_call_named_sub`

Always pushes gosub frame, saves arguments/locals, calls **`gosub_save_ctrl`**
(clears IF/loop/SELECT visibility for callee; zeros **`if_shadow`**).

Paired with:

- `gosub_restore_top` + `gosub_restore_ctrl` on RETURN / END SUB
- Interrupt path: restore via handler RETURN; **`gosub_restore_ctrl` clears
  `if_shadow` again**

### 3.4 Statement SUB / CALL

`mmb_call_named_sub` + `stmt_call_resume`; body runs under **`run_program`**
via `branch_pc`, not `run_gosub_body`. END in SUB from main program stops
`run_program` correctly.

### 3.5 `mmb_cmd_gosub` vs `ON n GOSUB`

| Mechanism | `gosub_save_ctrl` | Argument save |
|-----------|-------------------|---------------|
| `GOSUB` (`mmb_cmd_gosub`) | Yes | N/A |
| `ON n GOSUB` (`mmb_cmd_on`) | **No** | **No** (`gosub_nsave = 0` only) |

`ON n GOSUB` only pushes return PC and sets `branch_pc`. Caller IF/SELECT/loop
state is **not** isolated from the gosub target — latent bug class.

### 3.6 `mmb_cmd_execute` / immediate mode

`exec_line_body` on a tokenized string with **`G.p` saved** around the call only.
No control-flow checkpoint. Running structured code via EXECUTE inside a false
IF arm or during a loop can desynchronize stacks (same class as pre-#1273 bugs).

### 3.7 ON ERROR

`mmb_error` → `longjmp(run_errjmp)` → `unwind_call_frames()` (full gosub unwind
+ restore) → handler line at outer `run_program` level. Does not use
`run_gosub_body`. Trapped errors and interrupt handlers are orthogonal.

### 3.8 Where interrupts can fire

| Site | Granularity |
|------|-------------|
| `run_program` / `run_gosub_body` start of each **line** | Line boundary |
| `PAUSE`, `VSYNC_WAIT` inner loops | Sub-line, during wait |
| `SORT` | Periodic during long sort |
| Not during arbitrary `mmb_expr` | No mid-statement events |

So handlers can interrupt **between lines** of skipped IF arms (where #1342
bites), and during PAUSE, but not between statements on the same line unless a
command explicitly polls.

### 3.9 Other

- **PLAY TTS** optional callback: `mmb_call_named_sub` only; body runs when
  the current statement finishes and `branch_pc` is honored — deferred like CALL.
- **Yield registry** (`yield.c`): separate C callbacks; not BASIC — uses
  `mmb_bg_console_enter` for console switching.
- **Virtual console suspend**: line-boundary `run_suspended` (safe checkpoint).

---

## 4. State × context matrix

Legend: **S** = saved/restored for this entry, **C** = cleared for callee start,
**L** = leaked / caller value not restored, **—** = not applicable.

| State | Stmt SUB/CALL (`run_program`) | `run_gosub_body` (tick/key/func) | `gosub_save_ctrl` frame | ON ERROR unwind |
|-------|------------------------------|----------------------------------|-------------------------|-----------------|
| `run_pc` | S via branch | Temporarily replaced; caller S in events | — | — |
| `G.p` | Unchanged mid-line | Replaced; events S | — | — |
| `branch_pc` | S | Replaced; events S | — | — |
| `branch_pos` | Own protocol | L in events | — | Cleared on trap |
| `if_depth` / `if_stack` | S in gosub_* | C on call; **L shadow** on return | S | Unwind restore |
| `if_shadow` | C on call; **L on restore** | **L (#1342)** | C/C | Not saved |
| `if_inline` / `if_armed` | Partial S | `if_armed` cleared on restore | partial | — |
| `sel_sp` / `sel_base` | S / hide | S | S | Unwind |
| `ctrl_sp` / `for_sp` | gosub_* + local in nested body | Local S in nested body | S on return | Unwind |
| `gosub` stack | Push/pop | Push/pop | Push/pop | Pop all |
| `running` | END works | **END undone (#1343)** | — | 0 at end |
| `func_ret` | Expr path copies out | Shared slot | — | — |
| Arena | Per-line reset (main) | Mark/release | Mark | — |
| Vars / OPTION | Shared | Shared | Shared | Shared |

---

## 5. Confirmed and likely bugs

### 5.1 Confirmed (reproduced on built kernel, 2026-10-26)

**#1342 — `if_shadow` lost across interrupt**

- Mechanism: `gosub_restore_ctrl` sets `G.if_shadow = 0` without restoring the
  caller’s count (`gosub_save_ctrl` also clears on entry).
- Repro: issue’s `scan` + `SETTICK 1, tick` loop → `bad=549` (expected 0).
- Fix direction: store `gosub_if_shadow[g]` (issue / PR #1344).

**#1343 — END inside FUNCTION / SUB under `run_gosub_body`**

- Repro:

```basic
FUNCTION f()
  PRINT "in f"
  END
  f = 1
END FUNCTION
x = f()
PRINT "AFTER x="; x
```

- Output: `in f` then `AFTER x=0` (program continues).
- Mechanism: `run_gosub_body` restores `saved_running`.

### 5.2 High-confidence latent issues

1. **`ON n GOSUB` without `gosub_save_ctrl`** — nested IF/SELECT in caller can
   interact with gosub target (compare `mmb_cmd_gosub` vs `mmb_cmd_on`).
2. **`branch_pos` not saved in `mmb_run_events`** — tick during same-line FOR /
   mid-line SUB resume (#1276 class) may resume wrong token.
3. **`if_armed` / `if_inline_keep` not in gosub checkpoint** — edge cases for
   one-line IF + interrupt (lower rate than #1342).
4. **Process-global `s_arena`** — multi-console / nested expression temporaries
   not isolated per `mmb` instance (documented risk for virtual consoles).
5. **`EXECUTE` without frame** — dynamic code can mutate control stacks in place.
6. **Nested `run_gosub_body` + END** — should terminate entire program, not only
   inner nested loop; needs `end_req` visible to `run_program`.

### 5.3 Related closed issues (context)

- #1276, #1295, #1297, #1299, #1300, #1301 — block IF, SELECT, loops, same-line
  resume; fixed by expanding ad hoc saves (same pattern as today’s gaps).
- #1329, #1332 — mid-line DO / EXIT SUB; `branch_pos` and gosub resume protocol.
- #261 — SELECT in handler must not see caller’s CASE ELSE (sel_base).

---

## 6. Structural design options

### Option A — Full execution frame stack (recommended direction)

Introduce `mmb_exec_frame` holding **all** control-flow and position fields:

- PC, `branch_pc`, `branch_pos`, `p` snapshot or “resume kind”
- IF, SELECT, ctrl, for stacks (or pointers into stack arenas)
- `if_shadow`, one-line IF flags
- Optional: `dim_local`, `acc_*`, arena mark, `error_active` snapshot

API:

```c
void mmb_frame_push(mmb_exec_frame *f, enum mmb_nested_kind kind);
void mmb_frame_pop(mmb_exec_frame *f);
void mmb_run_nested(void (*body)(void), enum mmb_nested_kind kind);
```

Every nested entry (`mmb_call_named_sub`, `run_gosub_body`, future ON ERROR
island) uses the same helper. **Interrupt handlers** push a frame, run, pop.

| Criterion | Assessment |
|-----------|------------|
| Correctness | Best: invariant “handler sees empty control; caller unchanged” |
| Performance | One memcpy or stack struct per interrupt (~ tens of words); hot on Pi 400 but far cheaper than tokenize/expr; tick at 1–5 ms is fine |
| Migration | Medium: refactor `gosub_*` arrays into frame or embed frame in gosub slot |
| Risk | Low if staged behind tests |

### Option B — Interrupts only at defined checkpoints

Defer `mmb_run_events` until `mmb_exec_checkpoint()` at statement boundaries
(and end of line). Remove line-start dispatch from `mmb_check_break` or gate it
on “structure idle” flag.

| Criterion | Assessment |
|-----------|------------|
| Correctness | Good if checkpoints are complete; breaks “key during PAUSE” timing unless PAUSE keeps polling |
| Performance | Slightly fewer re-entrant entries |
| Migration | Small code delta |
| Risk | **Semantic change** for games; CMM2-like “between statements” may differ from current between-lines + PAUSE |

### Option C — Split `mmb` into `mmb_program` + `mmb_thread`

Program store, vars, gfx, files = shared. One **thread** struct per nested runner
(main + interrupt stack). `G` macro points at merged view or `G.th->`.

| Criterion | Assessment |
|-----------|------------|
| Correctness | Excellent long-term |
| Performance | Pointer indirection; cache-friendly if thread struct is contiguous |
| Migration | Large (every `G.` field categorized) |
| Risk | High without incremental path |

### Comparison summary

| | A Frame stack | B Checkpoints only | C Split thread |
|---|---------------|--------------------|----------------|
| Fixes #1342/#1343 class | Yes | Partial | Yes |
| ON KEY during PAUSE | Unchanged | Needs care | Unchanged |
| Effort | Medium | Low | High |
| Testability | Frame invariants fuzzable | Checkpoint coverage | Module tests |

---

## 7. Recommendation

**Adopt Option A** with a thin migration from existing `gosub_save_ctrl`:

1. Define `mmb_exec_frame` as the union of everything `gosub_save_ctrl` plus
   `if_shadow`, `branch_pos`, `if_armed`, `if_inline_keep`, and arena mark.
2. Replace `run_gosub_body`’s hand-rolled saves with `mmb_frame_push/pop`.
3. Add **`G.end_program`** (or `end_req`) set by `END` / `STOP`, cleared only
   on new RUN; **never restored** by `mmb_frame_pop`. `run_program` and outer
   `run_gosub_body` exit when set.
4. Route **`mmb_run_events`** through `mmb_run_nested()` only.
5. Fix **`ON n GOSUB`** to use the same push as `GOSUB`.
6. Move **`s_arena`** into `mmb` (or per-frame mark) in a follow-up for
   multi-console correctness.

Option B can supplement A for **debug builds** (`ASSERT` structure idle at
interrupt) without changing release semantics.

---

## 8. Staged migration plan (mergeable PRs)

| Stage | PR | Content | Tests |
|-------|-----|---------|-------|
| 1 | `exec-frame-type` | Add `mmb_exec_frame`, populate from `G`, unit tests in host build | C tests or pytest on native if available |
| 2 | `if-shadow-gosub` | `gosub_if_shadow[g]` save/restore (#1344) | #1342 scan repro, nest corpus |
| 3 | `end-req-flag` | `end_program` + honor in `run_program` / nested pop (#1343) | FUNCTION END, SUB-from-FUNCTION END |
| 4 | `run-nested-helper` | `mmb_run_nested` wraps `run_gosub_body`; single pop path | Existing ON KEY / SETTICK tests |
| 5 | `on-n-gosub-frame` | `ON n GOSUB` calls `gosub_save_ctrl` / frame push | New ON GOSUB + nested IF test |
| 6 | `events-branch-pos` | Save/restore `branch_pos` in event wrapper | Same-line FOR + SETTICK regression |
| 7 | `arena-per-console` | Move `s_arena` into `mmb` | Two-console expression test |
| 8 | `interrupt-fuzz` | Harness: random program + tick at every line boundary | CI optional job |

Each stage keeps behaviour except the bug under fix; stages 2–3 are independent.

---

## 9. Testing strategy

1. **Regression corpus:** #1342 scan program, #1343 END tests, `tests/test_nesting.py`,
   `test_select_case_on_key_and_settick`, `test_inkey.py`.
2. **Invariant tests (new):** After every `mmb_frame_pop`, assert caller
   `if_shadow`, `if_depth`, `sel_sp`, `for_sp`, `ctrl_sp` unchanged from
   pre-push snapshot.
3. **Interrupt fuzz:** Generator for nested IF/SELECT/DO; SETTICK 1 ms no-op or
   counter SUB; compare output with interrupts disabled (same seed).
4. **Mid-line resume:** Colon-separated FOR/SUB/EXIT cases from #1329/#1332 with
   SETTICK enabled.
5. **Perf smoke:** Tick handler empty body, 1 ms period, measure scan loop time
   on Pi 400 vs baseline (should stay within few %).

---

## 10. Code anchors

- `gosub_save_ctrl` / `gosub_restore_ctrl`: `mmbasic/src/core.c` (~1146–1188)
- `run_gosub_body`: `mmbasic/src/core.c` (~5094–5164)
- `mmb_run_events`: `mmbasic/src/core.c` (~5268–5406)
- `process_line_structure` / `if_shadow`: `mmbasic/src/core.c` (~714–770)
- `mmb` fields: `mmbasic/include/mmb_priv.h` (~494–676)

---

## Appendix: event mechanisms inventory

| Mechanism | Config | Dispatch |
|-----------|--------|----------|
| SETTICK | `G.tick[]` | `mmb_run_events` → `run_gosub_body` |
| ON KEY (general) | `G.on_key` | Same; once per `inkey_gen` change |
| ON KEY code | `G.on_key_ascii[]` | Same; consumes key from buffer |
| ON MOUSECLICK / UP / MOVE | `G.on_mouse*` | Same; synthetic `sub(x,y[,btn])` |
| ON ERROR GOTO | `G.on_error_pc` | `longjmp` + `unwind_call_frames` |
| ON n GOTO / GOSUB | `mmb_cmd_on` | Branch in `run_program` (no nested loop) |
| BREAK / Ctrl-C | `mmb_break_events` | Error, not SUB |
| Virtual console switch | `run_suspended` | Line boundary |

No other BASIC-level asynchronous SUB dispatch found in `mmbasic/src` (yield
registry is native code only).
