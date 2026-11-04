# Interpreter render optimisations for sprite-heavy games

Analysis for #1431. **Nothing here is implemented.** No interpreter behaviour,
option or test for an unimplemented feature was added. The proposals are filed
as separate issues, listed at the end.

Scope: the interpreter's graphics/render path (`mmbasic/` and `console/`), not
game code. The motivating game is Patient Zero; its source is not in this
repository (see below), so measured numbers come from the repository's own
benches and graphics tests.

## Where the current code presents and syncs

There is no single "present" call. Four paths matter:

| path | entry point | what it uploads |
|---|---|---|
| full/dirty AABB | `mmb_gfx_present()` `mmbasic/src/gfx.c:590-639` | union dirty AABB (or the whole frame if nothing was tracked) |
| one rectangle | `mmb_gfx_present_rect()` `gfx.c:641-691` | the given rect, plus that rect's console-cache sync and cursor overlay |
| arbitrary native rect | `mmb_gfx_present_native()` `gfx.c:570-588` | software cursor / overlay scratch |
| deferred dirty AABB | `mmb_gfx_dirty_flush()` `gfx.c:407-426` → `mmb_gfx_flush_pending()` `gfx.c:431-439` | holds the AABB until an interpreter boundary (`#1402`) |

`mmb_gfx_present()` itself waits for any DMA, takes the display page with
`page_buf(display_page)`, intersects the dirty AABB with the HDMI size, uploads
through `present_native_or_rgb()` (`gfx.c:530-563`), then re-applies the software
cursor (`gfx.c:633-637`). The deferred path exists only when the platform sets
`coalesce_presents`; Circle does (`console/platform.cpp:1783`), the SDL/native
hosts do not.

The console hides an important extra cost. A present writes the framebuffer
directly, so the bare-metal console's cached text/pixel buffer still holds the
old rows and would stamp them back on its next flush. The page-copy/page-display
and rect-present paths therefore also reconcile the terminal cache:
`mmb_gfx_sync_console()` (`gfx.c:481-497`) or the rect hook in
`mmb_gfx_present_rect()` (`gfx.c:685-688`, `gfx.c:399-404`). On Circle that
reconcile is a per-pixel `SetRawPixel` loop
(`console/platform.cpp:1392-1455`). This is the #1403 area, already narrowed to
the presented rect; it is not repeated here.

Presents are forced at:

- `PAGE DISPLAY`: full-frame dirty, present and full console sync
  (`mmbasic/src/cmd_graphics.c:766-784`).
- `PAGE COPY`: full-page copy, full-page dirty, then `mmb_gfx_present_if(dst)`
  (`cmd_graphics.c:785-821`, `gfx.c:499-528`).
- display-page opaque/transparent `BLIT`: dirty rect then
  `mmb_gfx_dirty_flush()` (`mmbasic/src/gfx_cmm2.c:262-267`, `332-337`).
- `SPRITE SCROLL`: full-frame dirty then one present (`cmd_sprite.c:429-437`).
- interpreter boundaries: `mmb_check_break()` (`core.c:4651`),
  `mmb_poll_fast()` (`core.c:6909`) and `mmb_console_write()` (`util.c:966`).

### Sprites already suppress presents on hidden pages

`SPRITE SHOW/MOVE/HIDE/CLOSE` collect rectangles in `s_rects` and `rect_flush()`
presents them only when `sprite_page_visible()` is true, i.e. the write page is
the display page (`cmd_sprite.c:174-179`, `247-258`). Drawing sprites to page 1
while page 0 is displayed does **not** touch HDMI today.

That matters for the #1431 premise: deferring hidden-page sprite draws cannot
save HDMI back-and-forth, because there is none. It can only save page-buffer
work. On the visible page the present cost is real, because `rect_flush()`
calls `mmb_gfx_present_rect()` once per rectangle.

## Measured evidence

Host: macOS arm64 (Apple Silicon), `make -C native -j8` with the default
`-O2 -g`, `native/mmcore` under `SDL_VIDEODRIVER=dummy`. These are **not** Pi
400 numbers; use them as ratios. Commands:

```bash
make -C native -j8
python3 scripts/hotspot_bench.py
# plus a one-page sprite probe, same env:
# MMB_DRIVE_ROOT=... SDL_VIDEODRIVER=dummy native/mmcore --drive ... 'RUN "D:/RENDER.BAS"'
```

From `scripts/hotspot_bench.py` (MODE 20, 683x384, 16-bit, empty `FOR`
subtracted):

| operation | measured |
|---|---:|
| `PAGE COPY 1 TO 0`, including present | 517.5 µs |
| `BLIT` 32x32 | 2.8 µs |
| `BLIT` 160x96 | 28.3 µs |
| `SPRITE SHOW 1, x, y, 1` (`sprite_show_expr`) | 6.6 µs |
| `SPRITE NEXT` + `SPRITE MOVE` (`sprite_next_move`) | 9.6 µs |

The committed Linux run of the same bench (`bench/hotspots/RESULTS.txt`) is
`PAGE COPY` 202 µs, `BLIT` 32x32 1.61 µs, `BLIT` 160x96 16.1 µs and
`SPRITE NEXT`+`MOVE` 8.3 µs. The shapes agree; absolutes do not.

A small one-sprite probe (24x24, 20,000 `SPRITE NEXT`+`SPRITE MOVE`) isolates
the present tax from the compositing work:

| write target | elapsed | per iteration |
|---|---:|---:|
| page 1 while page 0 is displayed (hidden) | 26 ms | 1.3 µs |
| page 0 (displayed) | 128 ms | 6.4 µs |
| empty `FOR` (20,000) | 2 ms | 0.1 µs |

So a visible sprite move costs about 5 µs more than the same move on a hidden
page. That difference is the per-rectangle upload. A hidden-page move still
costs ~1.3 µs of background restore and compositing.

Prior interpretation work (`docs/analysis/jetpack-inner-loop-hotspots.md`)
already found that Jetpack's display-page BLIT/present line dominated its
non-`PAUSE` graphics time (88 µs/exec, 635 calls, 56 ms of a level-37 run) and
that a 24x24 `SPRITE READ`/`SHOW` is mostly parsing plus real pixel work. That
run also showed `PAUSE` and uncached `IF`/`SUB` calls dominate the interpreter
as a whole, so render work should not be credited for more than the frame
profile shows.

## Patient Zero

**The game is not in this repository.** Search performed:

```bash
grep -rni "patient zero|patientzero|patient_zero|patient-zero" \
  . --exclude-dir=.git --exclude-dir=circle --exclude-dir=picomite-fork
find bench tests ramdisk docs assets -type f \( -iname '*.bas' -o -iname '*.png' \)
```

There is no game source, asset, or test under `bench/`, `tests/`, `ramdisk/`,
`docs/` or `assets/`. The available `/tmp/mmcore-apps` checkout contains only
`games/snake`, `games/sudoku` and `games/tetris`; it has no Patient Zero. No
Patient Zero measurement is claimed.

A sprite-heavy game of that kind exercises exactly the paths above: a
`MODE`/`CLS`/`PAGE WRITE`, `SPRITE READ`/`LOADPNG`, a per-frame
`SPRITE NEXT`/`SPRITE MOVE` (or one no-arg `SPRITE MOVE`), optional
`SPRITE SCROLL`, and a `PAGE COPY` from the composing page to the display page.
The repository's evidence for those paths is `bench/hotspots/gfx.bas`,
`tests/test_sprite.py` (background restore, ordering, layers, scroll) and
`tests/test_cmm2_gfx.py:734` (visible BLIT coalescing).

To profile the real game, obtain its `mmcore-apps`-style checkout and run it
with:

- `OPTION FRAMEPROFILE ON` or `MMCORE_FRAME_PROFILE=1`: the CSV gives
  `interp_us`, `draw_us`, `present_us`, `wait_us` and one row per present epoch,
  so "frames per game tick" is the coalescing signal
  (`mmbasic/src/frame_prof.c:20-32`, `138-155`).
- Statement-level profiling as described in
  `docs/analysis/jetpack-inner-loop-hotspots.md` (`MMB_STMTPROF`; check that the
  opt-in profiler exists on the branch you profile).
- A temporary counter for `mmb_gfx_present_rect()` calls and uploaded area, plus
  sprite `blit_restore`/`blit_show` counts, to separate present from compositing.
- `perf record` on host, or the Circle frame profiler on Pi, before choosing a
  fix.

Without that, the ranked proposals below are hypotheses grounded in the code
and the repository benches, not claims about Patient Zero.

## Proposed optimisations

### `OPTION FASTDRAW ON` plumbing (part of #1433)

Deferred/coalesced drawing changes *when* pixels become visible even when the
final page is identical, so it should be opt-in. The switch would be:

- declared as `int fastdraw;` in `mmb_options` (`mmb_priv.h:178-259`);
- parsed beside `OPTION PROFILING` (`cmd_option.c:810-814`) using the existing
  `onoff()` helper, and printed by `mmb_option_list()` (`cmd_option.c:1424-1425`);
- defaulted off in `mmb_option_reset()` (`core.c:3029-3031`);
- not loaded from or written to `.mmbasic.ini`: it is a per-session switch,
  since a later boot must not silently defer drawing (see `settings.c`);
- documented in `docs/help/option.txt` (the `OPTION` block, around line 29),
  then `mmbasic/src/help_data.c` regenerated from the help sources.

The interpreter reads `G.opt.fastdraw` in the sprite flush and page-copy paths.
Most programs should never need it; it exists so a title that observes the page
between draw commands can keep today's immediate behaviour.

### 1. Coalesce visible-page sprite presents — filed as #1433

`rect_flush()` (`cmd_sprite.c:247-258`) uploads every sprite rectangle with
`mmb_gfx_present_rect()`. A no-arg `SPRITE MOVE` can queue 2 x 64 rectangles
(`MMB_MAX_SPRITE`, `mmb_priv.h:39`), and each one is its own `SetArea`, rect
sync and cursor overlay. BLIT already coalesces through the deferred dirty AABB
(`#1402`); sprites should use the same machinery, with a fallback to per-rect
presents when the union AABB is much larger than the sum of the rects.

Expected win: removes most of the ~5 µs per visible sprite move measured above,
and turns a multi-sprite `SPRITE MOVE` from up to 2N presents into one.
Risks: deferral must flush before text, `PAGE COPY`/`DISPLAY`, screenshots,
program end and mode changes; a wide AABB may over-upload. Gate with an opt-in
option because it changes when the frame appears.

### 2. `OPTION FASTDRAW` gate (part of #1433) and hidden-page draw recorder — filed as #1434

A recorder for sprites drawn to a hidden page can collapse repeated
`blit_restore`/`blit_show` work into one replay. It must capture the ordered op
log (SHOW/NEXT/MOVE/HIDE/CLOSE/SCROLL), the layer and stable show sequence, and
sprite content generations. It must replay before any read of the page
(`PIXEL`, `SPRITE READ`, `BLIT READ`, snapshot), any non-sprite write (`CLS`,
shapes, `TEXT`, `IMAGE`, `PAGE SCROLL`/`LOGIC`, framebuffer window), `PAGE COPY`
when the recorded page is the source, and `PAGE DISPLAY`.

Honest limit: this saves page-buffer CPU, not HDMI uploads. It is worthwhile
only if a game moves sprites several times before showing the page. The
measured hidden-page baseline is ~1.3 µs per move, so the win per removed
intermediate move is on that order; `PAGE COPY` (517 µs/frame on this host)
dominates a hidden-compose-then-copy game.

### 3. Per-pixel colour round-trip on live drawing — filed as #1435

`span_target_pixel()` and `mmb_gfx_plot()` hold a native pixel, convert it to
RGB888 for `G.plat->set_pixel()` (`gfx.c:844-866`, `948-1010`), and
`plat_set_pixel()` converts it straight back with `rgb_to_raw()`
(`console/platform.cpp:200-218`), once per lit pixel. An optional
`set_pixel_native()` hook removes two conversions and two indirect calls per
pixel for LINE/BOX/CIRCLE/TEXT. The win scales with lit-pixel count; it is not
measured on this host and should be measured on a Pi before prioritising.

### 4. PAGE COPY damage tracking — filed as #1436

`mmb_gfx_copy_page()` always copies the whole page and marks the whole display
page dirty (`gfx.c:499-528`), so `PAGE COPY` uploads a full frame even if only a
small part of the source changed. Only the display page has a dirty AABB today.
Per-page damage plus a source/destination generation would let a copy upload
only the changed rectangle, and skip the copy entirely when the source has not
changed since the last copy. This is the highest-risk proposal: a missed damage
mark or a stale generation leaves the display one frame behind.

## Considered but not proposed

- **Full-frame console-cache reconcile**: already reduced to the presented rect
  by #1403 (`gfx.c:399-404`, `685-688`). A bulk terminal-cache copy would need a
  Circle API change and is not justified by the current evidence.
- **Double buffering**: already implemented for Pi ≤ 4 full-frame presents via
  the virt-offset flip (`docs/graphics-acceleration.md`, `console/platform.cpp:91-109`),
  and for TERM via async coalescing. No new proposal.
- **Occlusion-aware sprite redraw**: `SPRITE MOVE`/`SHOW` redraw every sprite
  above the insertion point (`cmd_sprite.c:307-332`, `334-387`), not only those
  whose bounds overlap. This is a plausible CPU win on the hidden path, but it
  overlaps #1434's recorder and carries the same ordering risk. Fold it into
  #1434 if the recorder proves out.
- **Faster sprite argument parsing**: already covered by the graphics
  statement-cache proposal in the Jetpack analysis, and it is parser work rather
  than render work.

## Filed proposals

| issue | proposal |
|---|---|
| #1433 | Coalesce per-rect sprite presents into one deferred AABB (`OPTION FASTDRAW`) |
| #1434 | Record sprite draws to a hidden page and replay on visibility |
| #1435 | Native-pixel live-write hook (no per-pixel colour round-trip) |
| #1436 | Per-page damage and delta `PAGE COPY` |
