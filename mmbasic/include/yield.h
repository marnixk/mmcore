#ifndef MMB_YIELD_H
#define MMB_YIELD_H

/*
 * Cooperative background yield registry (#858).
 *
 * A screen or app registers a callback for the virtual console it belongs to.
 * The host main loop runs mmb_yield_run() on every yield; each callback fires
 * at most once per its own rate limit, whichever console is active.
 *
 * The callback receives its owning console index so it can select that
 * console's interpreter/UI context itself (mmb_bg_console_enter()) and must
 * not paint unless it owns the active screen. Focus/key handling must never
 * run from a yield callback: keyboard input is only ever delivered to the
 * active console.
 *
 * Participants (see docs/help/consoles.txt):
 *   - JUKE: queue advance at <=1 Hz while a track plays, even if the JUKE
 *     screen is backgrounded.
 *   - TERM: drain and interpret an established TCP session on a background
 *     console (no painting; the grid is redrawn when the console returns).
 *   - CONNECT: drain the socket on a background console and buffer any
 *     screen output for replay when the console returns.
 */
typedef void (*mmb_yield_fn)(int console, void *ctx);

/* Register (or re-register) `fn` for `console`, due at most every `min_ms`
 * milliseconds. A NULL fn is ignored. Returns 0 on success, -1 if the table
 * is full. */
int mmb_yield_add(int console, mmb_yield_fn fn, void *ctx, unsigned min_ms);

/* Remove every registration of `fn`, on any console. */
void mmb_yield_remove(mmb_yield_fn fn);

/* Remove every registration owned by `console`. */
void mmb_yield_remove_console(int console);

/* Drop every registration (warm reset / app teardown). */
void mmb_yield_clear(void);

/* Run the callbacks whose rate limit has elapsed. `now_ms` is a monotonic
 * millisecond clock (mmb_now_ms()). */
void mmb_yield_run(unsigned now_ms);

/* Number of live registrations (diagnostics/tests). */
int mmb_yield_count(void);

#endif
