# Jetpack inner loop: where the interpreter spends time

Investigation only. Nothing here changes the interpreter unless
`MMB_STMTPROF` is set. The default build does not time statements.

The workload is [Jetpack](https://github.com/marnixk/mmcore-apps) at
`72bce5453ad8be55f6537d8e10b575827e6ea440` (`games/jetpack`). Level 0 is
WELCOME TO JETPACK!. Level 37 is GOLDEN SLUMBERS. Both ran at MODE 20
(683×384) for 48 logic frames of 55 ms (`TH_FAST = 0`, `FRAME_MS = 55`,
`PAUSE 4`). The drive copy forces `key.right = TRUE` at the end of
`key.logic` for the "holding Right" cases. The published game is not
modified.

Numbers are from this x86_64 Linux host (SDL dummy, `native/mmcore` and
`native/mmbasic` at `-O2 -g`), not from a Pi 400. Ratios that come from
`mmbasic/src` apply to the Pi 400, the Linux framebuffer build, SDL
desktop, and Windows, because those builds share that tree. Absolute
microseconds will not.

## Method

1. Opt-in exclusive timer in `mmbasic/src/stmt_prof.c`, armed only when
   `MMB_STMTPROF` is set (`1` writes `C:/.mmcore-stmtprof.csv`; any other
   value is a host path or a drive path). Unset, empty, `0`, and `off`
   leave `mmb_stmtprof_active` at 0. The hot path is that one load.
   Time is a cycle counter (`rdtscp` on x86_64, `cntvct_el0` on AArch64)
   converted with the run's `CLOCK_MONOTONIC` span. `busy_ns` subtracts
   the measured cost of the two timer reads. A `SUB`/`FUNCTION` body and
   `mmb_check_break` are their own rows, not part of the caller.
2. The same runs set `MMCORE_FRAME_PROFILE=1` (existing frame CSV:
   interp / draw / present / wait).
3. Isolated programs in `bench/hotspots/` (`interp_few.bas`,
   `interp.bas`, `gfx.bas`) run with the profiler **off**. `TIMER`
   milliseconds minus an empty `FOR`/`NEXT` are the ns/exec figures in
   `bench/hotspots/RESULTS.txt`. The game profile is used to decide
   which lines matter and what fraction of the run they are.
4. `perf record -e cycles:u -g --call-graph fp` on level 0 idle. This
   VM has no CPU PMU (`cpu pmu capabilities: not available`), so perf
   fell back to `task-clock:u` at 999 Hz: 726 samples over 2.82 s.
   That profile is still enough to name the C functions. It was taken
   **without** `MMB_STMTPROF`.

Driver: `scripts/jetpack_hotspot.py`. Benches: `scripts/hotspot_bench.py`.

Each run printed, then ended:

| run | result |
| --- | --- |
| level 0 idle | `JPRESULT 48 4 0 0 683 384 WELCOME TO JETPACK!` |
| level 0 Right | `JPRESULT 48 4 0 0 683 384 WELCOME TO JETPACK!` |
| level 37 idle | `JPRESULT 48 4 37 1 683 384 GOLDEN SLUMBERS` |
| level 37 Right | `JPRESULT 48 4 37 1 683 384 GOLDEN SLUMBERS` |

`4` is `GE_TITLE` (the 48-frame cap). Level 0 left `pl.dead = 0`. Level
37 finished with `pl.dead = 1`, so the tail of that run is the death
animation. It still executed the collision helpers: `en.tid` ran 672
times on level 0 and 3216 times on level 37.

Wall time was 2.80 s, of which `PAUSE IDLE_MS` was 2.41–2.55 s.
`gm.tick` fired 216 times and stepped the game 48 times. The other
~490 iterations of `gm.play` only drew and paused. Frame-profile sums
for level 0 idle: interp 91 ms, draw 0.4 ms, present 0.01 ms, wait
2.72 s. The frame profiler's present bucket does not wrap
`flush_present`; that work shows up under `PAUSE` and `check_break`.

## Baseline

Cached integer increment, trace cache on, profiler off, `OPTION DEFAULT INTEGER`:

```
a = a + 1
```

**145 ns/exec** (`bench/hotspots/interp.bas`, 1.5e6 trips). The `%`
form `a% = a% + 1` is 147 ns. A dotted name `pl.x = pl.x + 1` is 156 ns.
A float `f = f + 1` is 150 ns. Integer `\` and `MOD` are 149–150 ns.
A cached 2-D store `grid(x, y) = grid(x, y) + 1` is 215 ns.

Those are the same order of magnitude. Suffixes, dotted names, float
versus integer, and `\` / `MOD` are not the inner-loop problem once the
trace cache has compiled the statement. `mmb_int_val` does `memset` the
value and store `(double)i` (`util.c`), and cached compares go through
`mmb_as_float` (`tracecache.c` `tc_replay`, `TC_LT`..`TC_NE`). On this
host that extra work is lost in the 145 ns. It is not a useful target
next to the gaps below.

Variable lookup itself is a name hash (`vars.c` `hash_lookup` /
`mmb_find_var`), not a scan of every variable. `try_struct_path`
returns immediately when there are no struct types, so a dotted game
name is just a long identifier.

## What is actually slow

### 1. `PAUSE 4` re-walks the host on every spin

`mmb_cmd_pause` (`core.c`) busy-waits and calls `mmb_poll()` and
`mmb_break_events()` on every iteration. `mmb_poll` always calls
`mmb_storage_poll`, then WLAN, network, NTP, audio, TERM, settings,
and the rest.

On the host build, `mmb_storage_poll` (`native/storage_posix.c`) loops
every drive letter and calls `drive_dir_raw` → `root_dir()` →
`getenv("MMB_DRIVE_ROOT")`, `snprintf`, and `stat`. Perf on level 0
idle, as a share of process CPU (task-clock), all under
`mmb_cmd_pause` → `mmb_poll` → `mmb_storage_poll`:

| symbol | share of CPU |
| --- | ---: |
| `getenv` | 11.0% |
| `fstatat` | 8.8% |
| `snprintf` / printf buffer | 8.1% |
| `strncmp` inside `drive_dir_raw` | 4.3% |
| `mmb_storage_poll` itself | 2.5% |
| `flush_present` (also from `PAUSE`) | 4.3% |

About a third of CPU during the play loop is rebuilding and stating
drive paths inside a 4 ms pause. `mmb_check_break` already rate-limits
the device poll to ~1 ms (`core.c`, the `#988` gate). `PAUSE` does not
use that gate. `SETTICK` still needs `mmb_break_events` on the fast
path; the storage walk does not.

The Pi 400 build calls `CStorage::Poll` (`console/storage.cpp`) instead
of this `stat` loop. The same `mmb_poll` call still runs on every spin
(WLAN, network, audio, settings). The Linux framebuffer, SDL desktop,
and Windows builds use the POSIX/host storage poll.

A handful of `check_break` rows average 16 ms. Those are one SDL
present each (`flush_present` waits on the display grid and converts
RGB555→RGB565), booked against whichever line next polled: `MODE`,
`scr.init`, and the one-shot map blit. Steady `check_break` on tight
lines is ~90–140 ns. The line before `PAUSE` averages 2.2 µs because
4 ms has always elapsed, so it takes the slow poll. Do not read the
16 ms rows as the cost of testing for Ctrl-C.

### 2. Multi-line `IF` is re-parsed every time, and every name scans every `SUB`

The trace cache compiles a one-line `IF ... THEN <let|goto|gosub>`
(`tracecache.c` `tc_compile_if`). A `THEN` at end of line returns 0, so
a multi-line `IF` / `ELSEIF` / `END IF` never caches. A `THEN` that is
a `SUB` call does not cache either. On level 0, 70% of `IF` executions
were uncached (17712 of 25317). Only 12% of `LET`s were uncached, and
those are the ones whose right-hand side calls a user function or a
builtin the cache refuses.

Each uncached primary calls `mmb_try_user_function` (`core.c`), which
`sub_find`s the name. `sub_find` uppercases the name and walks
`G.subs` with `mmb_keyword_eq` (`util.c`). Jetpack has about 114
`SUB`/`FUNCTION`s. A miss scans the whole table. Perf attributes 3.4%
of process CPU to `mmb_keyword_eq` even while `PAUSE` dominates the
profile.

Isolated, profiler off, same ~100 unused functions:

| statement | ns/exec | vs 145 ns |
| --- | ---: | ---: |
| `a = a + 1`, cache on | 145 | 1× |
| `a = a + 1`, cache off, no extra functions | 380 | 2.6× |
| `a = a + 1`, cache off, ~100 functions | 580 | 4.0× |
| `grid(x,y) = grid(x,y)+1`, cache on | 215 | 1.5× |
| same 2-D store, cache off, ~100 functions | 2155 | 15× |
| 4-arm multi-line `IF` | 2085 | 14× |
| 4-arm `SELECT CASE` | 1575 | 11× |

The 200 ns between the two cache-off increments is the linear scan
(~1 ns per function per primary). The rest of the 4–15× is parsing the
statement again: `mmb_find_var` (hash, plus suffix / bare-type /
`strncpy`), `mmb_try_function`, and the expression walker. `SELECT CASE`
is the same kind of cost as a multi-line `IF`, not a special extra.

In the game, the hot conditions are exactly this shape. `en.tid` is a
few integer compares and one `\`:

```
if xu < 0 or yu < 0 then
  exit function
end if
```

That `IF` costs **1997 ns** (14×) and ran 672 times on level 0, 3216
times on level 37. `if p_c > 25 or p_r > 15 then` is 1602 ns.

### 3. A tiny user `FUNCTION` costs as much as a sprite read

Calling a user function does `sub_find` again, saves every argument's
previous global, binds a new local (`mmb_call_named_sub` in `core.c`),
runs the body through the full statement loop (`check_break` per line),
and copies the return value out. The trace cache will not compile a
call (`tc_compile_primary` bails on `tc_is_user_func`, which is itself
a linear scan, but only at compile time).

Isolated totals (body included, profiler off):

| call | ns/exec | vs 145 ns |
| --- | ---: | ---: |
| `GOSUB` / `RETURN` | 390 | 2.7× |
| empty `SUB` | 785 | 5.4× |
| `SUB` with one `LOCAL` | 1425 | 9.8× |
| `b = add1(b)` | 1405 | 9.7× |
| `key.isDown` clone (`KEYDOWN` loop, no key held) | 2145 | 15× |

`GOSUB` is cheap next to `SUB`/`FUNCTION`. The extra is argument
save/restore and the call setup, not the branch.

Level 0, exclusive statement time (body lines are separate rows, so
the real cost of one call is higher than the call-site number):

| line | ns/exec | vs 145 ns | times |
| --- | ---: | ---: | ---: |
| `key.left = key.isDown(k_left)` | 8263 | 57× | 48 |
| `p_id = en.tid(...)` | 7024 | 48× | 96 |
| `if fld.solid(p_c, p_r) then` | 7525 | 52× | 96 |
| `p_hit = bod.hit(pl.x, pl.y)` | 11084 | 76× | 48 |
| `p_hit = en.blocked(en.x(i), en.y(i))` | 12303 | 85× | 48 |
| `spr.put(...)` (the `CALL` only) | 11189 | 77× | 98 |
| `scr.mark(spr.x(n), ...)` | 9416 | 65× | 101 |

`key.isDown` ran 533 times (about 11 per logic frame: the directions,
jet, phaser, pause). Its body is `FOR key_i = 1 TO KEYDOWN(0)`, which
is another 1269 ns, and `KEYDOWN` itself cannot be cached
(`tc_parse_name` rejects a keyword token). Isolated `k = keydown(0)`
is 460 ns (3.2×), the same with the cache forced off. With no key
held the `FOR` does not iterate. Most of the 8 µs is the call.

`en.tid` / `fld.solid` / `bod.hit` are the collision inner loop. Level
37 calls `en.tid` 3216 times (67 per logic frame) and `fld.solid` 557
times. Holding Right on level 0 raised `fld.solid` from 144 to 228
calls and did not change which statements are hot. Both input cases
are the same shape.

`LOCAL` on the map blit (`local cell, id, dx, dy`, 416 times) is
1366 ns exclusive, in line with the `sub_local` bench.

### 4. `BLIT` / `SPRITE` argument setup, and one real present

Pixel work is not free, and it should not be compared with `a = a + 1`
as if it were. The isolated C-side costs at MODE 20 (profiler off):

| operation | time |
| --- | ---: |
| `PAGE COPY` of 683×384, including present of the display page | 202 µs |
| `BLIT` 32×32 | 1.61 µs |
| `BLIT` 160×96 | 16.1 µs |
| `SPRITE NEXT` + `SPRITE MOVE` | 8.3 µs |

`PAGE COPY` is `mmb_cmd_page` → `mmb_gfx_copy_page` (`gfx.c`, `memcpy`
or `dma_copy` of `w*h*2` bytes) → `mmb_gfx_present_if`. A 24×24 tile
blit should land near the 1.6 µs 32×32 figure. In the game the tile
blit is **3.7–6.0 µs**. The statement is a long expression list
(`id mod 20`, `id \ 20`, several names). It is not trace-cached, so
each name takes the `sub_find` path. The extra ~4 µs is parsing, not
pixels.

`SPRITE READ` of a sheet cell is ~10 µs (`cmd_sprite.c` `sprite_read`,
one `mmb_gfx_get_page` per pixel, 24×24). That is honest pixel work
(~17 ns/pixel) plus the call. `SPRITE SHOW` is ~7.5 µs.

Level 37's dirty-rect blit to the display page is the one graphics
line that dominates a run: **88 µs/exec, 635 times, 56 ms** on the idle
profile (18% of non-`PAUSE` CPU). A 160×96 blit in isolation is 16 µs,
so this row is a large rectangle and/or `flush_present`, not a
parse bug. Level 0's same statement is 8.8 µs × 195. Right versus idle
does not change that picture; level 37 dirties much more of the screen
(and ends in the death animation).

PNG load is real but once per run: about 16 ms for the five
`683x384` sheets (`LOAD PNG` in `scr.loadAssets`). The one-shot map
decode (`lv.hex` / `lv.nib` / `mid$` / `asc`, 416 cells = 26×16) is
the same class: `lv.hex = lv.nib(asc(mid$(...)))` is 6.6 µs × 416 ≈
2.7 ms, once. It is not the per-tick loop. `PEEK`/`POKE` are not
implemented.

### 5. Things that looked suspicious and are not

| guess | measurement |
| --- | --- |
| `%` suffix versus a bare integer | 147 ns vs 145 ns, cache on |
| dotted names (`pl.x`) | 156 ns cached; 730 ns only with the cache off and ~100 functions |
| float add versus integer add | 150 ns vs 145 ns, cache on |
| integer compare via `double` in the trace cache | 150 ns vs 145 ns, cache on |
| `\` and `MOD` | 149–150 ns, cache on |
| cached 1-D / 2-D index | 185 / 215 ns |
| string concatenate of two short literals | 280 ns |
| `MM.HRES` / `MM.INFO(MODE)` | 275 / 435 ns, and the game calls them a handful of times |
| `KEYDOWN(0)` by itself | 460 ns |
| steady per-line `check_break` | ~100 ns |

`MM.INFO` and `KEYDOWN` are keyword tokens, so the trace cache cannot
compile them (`tc_parse_name` / `tc_compile_primary`). At one call per
frame that does not matter. Inside `key.isDown`, eleven times a frame,
it is part of the function-call cost above, not a reason to special-case
`MM.INFO`.

## Top hot spots

Baseline **145 ns**. "Repeating CPU" for level 0 idle is non-`PAUSE`
time minus PNG load minus the one-shot ~16 ms presents: about **103 ms**
over 48 logic frames (~2.1 ms of interpreter and small blits per 55 ms
tick). The 55 ms tick is mostly the pause; the ratios are the part an
interpreter change can move.

| # | construct | example | ns/exec | vs baseline | notes |
| --- | --- | --- | ---: | ---: | --- |
| 1 | `PAUSE` poll | `pause IDLE_MS` in `gm.play` | the 4 ms wait | n/a | ~35% of process CPU is `getenv`/`stat`/`snprintf` in `mmb_storage_poll` |
| 2 | user `FUNCTION` | `p_hit = en.blocked(en.x(i), en.y(i))` | 12303 | 85× | `mmb_call_named_sub`: `sub_find`, save/rebind args |
| 3 | user `FUNCTION` | `p_hit = bod.hit(pl.x, pl.y)` | 11084 | 76× | same, body is a few integer `\` |
| 4 | `SUB` call | `spr.put(2+i, en.fr(i), ...)` | 11189 | 77× | exclusive of the body; 8-deep `IF` chain inside `spr.put` is also uncached |
| 5 | user `FUNCTION` | `key.left = key.isDown(k_left)` | 8263 | 57× | 533 calls/run; body is one `KEYDOWN` loop |
| 6 | user `FUNCTION` in `IF` | `if fld.solid(p_c, p_r) then` | 7525 | 52× | call plus uncached `IF` |
| 7 | user `FUNCTION` | `p_id = en.tid(...)` | 7024 | 48× | 14/frame on level 0, 67/frame on level 37 |
| 8 | multi-line `IF` | `if x < -w or y < -h or x > scr.W or y > scr.H then` | 8262 | 57× | four comparisons, re-parsed, each name scans `sub_find` |
| 9 | display `BLIT` | `blit dr.x(scr_i), ...` level 37 | 87611 | 600× | large rect + present; level 0 is 8.8 µs. Mostly pixels |
| 10 | multi-line `IF` | `if c < 0 or r < 0 or c > 25 or r > 15 then` | 4579 | 32× | `fld.solid` / bounds, uncached every time |
| 11 | single-line `IF` + call | `if fld.mark <> 0 then scr.mark(...)` | 3841 | 26× | `THEN` arm is a `SUB`, so the cache refuses it |
| 12 | tile `BLIT` | `blit (id mod 20)*lay.tile, ...` | 5972 | 41× | ~1.6 µs of that is pixels (32×32 bench); the rest is the argument list |
| 13 | `SPRITE READ` | `sprite read n, sx0, sy0, w, h, pg_sheet` | ~10700 | 74× | `sprite_read` per-pixel `mmb_gfx_get_page`, 24×24 |
| 14 | `SPRITE SHOW` | `sprite show n, x, y, layer` | ~7500 | 52× | `sprite_show` / `sprite_show_at` |
| 15 | multi-line `IF` | `if xu < 0 or yu < 0 then` | 1997 | 14× | hottest *count* in `en.tid` (672 and 3216) |
| 16 | multi-line `IF` | `if gm.drawing or gm.res <> ge_none then` | 4171 | 29× | `gm.tick`, uncached, 216 times |
| 17 | `SELECT`/`IF` chain | bench, not a hot Jetpack line | 1575–2085 | 11–14× | same re-parse class as row 15 |
| 18 | `FOR` + `KEYDOWN` | `for key_i = 1 to keydown(0)` | 1269 | 9× | builtin is 460 ns alone; cannot be cached |
| 19 | `LOCAL` | `local cell, id, dx, dy` | 1366 | 9× | matches the `sub_local` bench (1425 ns) |
| 20 | cache-off 2-D store | bench `grid(x,y)=grid(x,y)+1` | 2155 | 15× | cached form is 215 ns; Jetpack pays this only on lines the cache rejects |
| 21 | `SUB` vs `GOSUB` | bench empty `SUB` / `GOSUB` | 785 / 390 | 5.4× / 2.7× | call setup, not the branch |
| 22 | steady `check_break` | tight lines / the `PAUSE` line | ~100 / 2229 | 0.7× / 15× | `mmb_check_break`: events every line, device poll every ~1 ms; SDL present when the grid is due |

Rows 9 and 12–14 are graphics. Rows 2–8 and 10–11 and 15–16 are the
interpreter doing a lot of work for a few integer operations. Together
the uncached `IF`s are 39 ms on level 0 (about 38% of the 103 ms of
repeating CPU). `CALL` is another 11 ms.

## Proposed optimisations

Not implemented. Gain is against this host's level-0 play loop unless
noted. "All four" means Pi 400, Linux framebuffer, SDL desktop, and
Windows.

### 1. Rate-limit `mmb_poll` inside `PAUSE`

Keep `mmb_break_events` (so `SETTICK` / `gm.tick` stays on time). Call
the storage/network/settings walk on the same ~1 ms gate `mmb_check_break`
already uses. Cache `MMB_DRIVE_ROOT` instead of `getenv` + `snprintf`
+ `stat` per drive per spin (`storage_posix.c` `drive_dir_raw`,
`mmb_storage_poll`).

- Gain: about a third of process CPU on this host during play, with no
  change to the 55 ms step. Largest single item in the perf profile.
- Risk: low if `mmb_play_mix` stays on the fast spin. Hotplug and NTP
  move from "every spin" to ~1 ms, which is the check_break policy
  already. Medium if audio was implicitly depending on the tight spin;
  measure a long `PLAY`.
- Platforms: the `stat`/`getenv` win is Linux framebuffer, SDL desktop,
  and Windows. Pi 400 does not use this `stat` loop; it still benefits
  if `CStorage::Poll` and the other `mmb_poll` callees are skipped
  between 1 ms marks.

### 2. Stop the linear `sub_find` on every expression primary

Hash `G.subs` (or a perfect set of function names) in `sub_find`. Do
not call `mmb_try_user_function` until the name is in that set.
`mmb_keyword_eq` over ~114 entries is what turns a cache miss from
380 ns into 580 ns, and it runs on every primary of every uncached
`IF`.

- Gain: ~200 ns per uncached statement that mentions two names, more
  when the statement mentions more (`if x < -w or y < -h or ...` has
  many). On top of (3), this is most of the gap between a cached `IF`
  and `en.tid`'s 2 µs condition. A few tens of milliseconds per 48
  frames on level 37, where `en.tid` runs 3216 times.
- Risk: low. Lookup must stay case-insensitive and must still miss
  variables. `tc_is_user_func` is the same scan at compile time and
  can use the same table.
- Platforms: all four. Pure interpreter.

### 3. Trace-cache multi-line `IF` conditions

`tc_compile_if` already compiles a comparison. It bails when `THEN` is
at end of line, and when the arm is a `SUB` call. Caching the condition
and the branch target (which existing `END IF` / `ELSEIF` arm to run),
and accepting `THEN someSub`, would cover the 70% of `IF`s that miss.
Jetpack's hot arms are `EXIT FUNCTION`, a single assignment, or a call.

- Gain: the uncached-`IF` bucket is 39 ms of 103 ms repeating CPU on
  level 0. A cached condition should land near the 150 ns compare, not
  2–8 µs. That is the inner-loop interpreter win once (1) is set aside.
- Risk: medium. `EXIT FUNCTION`, `ELSE`, side effects in the condition,
  and `AND`/`OR` short-circuit have to match `mmb_cmd_if`. A wrong
  branch is a silent logic bug. Ship it behind the existing negative
  cache: shapes that do not compile stay on the interpreter, as now.
- Platforms: all four.

### 4. Fast path for user `FUNCTION` / `SUB` entry

Cache the `sub_find` hit on the call site. Rebinding every argument
with `mmb_find_var` + `mmb_local_save` + `bind_scalar_arg` is the
difference between `GOSUB` at 390 ns and `add1` at 1405 ns, and between
that bench and Jetpack's 7–12 µs call sites (more arguments, dotted
names, the return variable). A call site whose argument count and
types have not changed can reuse the saved slots.

- Gain: `key.isDown` 11 times a logic frame, `en.tid` 67 times on level
  37, plus `fld.solid` / `bod.hit` / `spr.put`. Cutting a 8 µs call
  toward 1 µs is several hundred microseconds per 55 ms tick on level
  37, and it stacks with (3) because the bodies are multi-line `IF`s.
- Risk: medium-high. MMBasic locals are saved globals. Recursion, a
  parameter that aliases the caller, `ON ERROR`, and `EXIT SUB` all
  restore that save. A stale slot is a wrong variable, not a slow one.
  The safe first slice is "cache the sub index only" (kills `sub_find`
  on the call, not the rebind).
- Platforms: all four.

### 5. Do not re-parse a repeated `BLIT` / `SPRITE` argument list

A 32×32 `BLIT` of literals is 1.6 µs. The game's tile `BLIT`, full of
names and `MOD`/`\`, is 4–6 µs. `SPRITE READ`/`SHOW` pay the same
parser. A statement cache for integer graphics commands (the trace
cache's shape, but for `BLIT`/`SPRITE`/`PAGE COPY`) removes the
`sub_find` on each argument. Do not try to make the pixel copy faster
until that is split out; the 24×24 read is already ~17 ns/pixel, and
level 37's 88 µs dirty blit is a real upload (`flush_present`).

- Gain: small on level 0 (tile blits are a few milliseconds). Larger
  as a fraction of CPU after (1)–(4), because the blits remain.
  SDL present itself (the 16 ms spikes and `flush_present` at 4% of
  CPU) is a separate, already-paced path (`#1367`); further wins there
  are SDL desktop and Linux framebuffer (software RGB555→565 upload),
  not the Pi's DMA `SetArea`.
- Risk: low for a cache that falls back to `mmb_cmd_blit` on any
  non-integer or wrong arity. Medium if the cache skips a dirty-rect
  update.
- Platforms: parse cache helps all four. Present upload helps SDL
  desktop and Linux framebuffer. Pi 400 present stays on the Circle
  DMA path described in `docs/analysis/pi400-vs-fbdev-perf.md`.

### Not recommended from this data

- Special-casing `%` versus a bare integer, or dotted names. Cached
  cost matches `a = a + 1`.
- Removing the `double` store in `mmb_int_val`, or an integer compare
  in `tc_replay`, ahead of (2)–(4). Measured gap is noise next to a
  2 µs uncached `IF`.
- A `PEEK`/`POKE` fast path. Those commands are not in this interpreter.

## How to reproduce

```bash
# profiler object is already in native/Makefile (wildcard) and console/Makefile
make -C native -j$(nproc) CC="cc -fno-omit-frame-pointer"

# isolated ns/exec, profiler off
python3 scripts/hotspot_bench.py

# 48 logic frames, MODE 20, levels 0 and 37, idle and Right
# requires a checkout of mmcore-apps (this run used /tmp/mmcore-apps @ 72bce54)
MMCORE_APPS=/path/to/mmcore-apps python3 scripts/jetpack_hotspot.py --work /tmp/jp-hotspot
```

`tests/test_stmtprof.py` checks that the flag is off by default and
that a cached `a% = a% + 1` is recorded when it is on.
