/*
 * Virtual consoles (Ctrl+Alt+F1..F4 style). Each console owns a complete
 * interpreter context (g_mmb[i]) plus its own front-end line editor state and
 * screen snapshot. The host loop runs the interpreter against the active
 * console; switching suspends one and resumes the other.
 */
#ifndef MMB_SESSION_H
#define MMB_SESSION_H

#include "mmbasic.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Mark console 0 (already brought up by mmb_init + the boot banner) as the
 * active console. Call once at boot, before mmb_print_startup(). */
void mmb_console_init(void);

/* Number of consoles wired up (MMB_MAX_CONSOLES). */
int mmb_console_count(void);

/* Active console index, 0-based. */
int mmb_console_active(void);

/* Switch to console idx (0-based). Returns 1 when the active console
 * changed. A request is ignored when idx is out of range or already active.
 * While a program is running the switch is deferred; the host must call
 * mmb_console_poll() from its loop to carry it out. */
int mmb_console_switch(int idx);

/* Host loop hook. Performs a switch that was deferred because a program was
 * running. Returns 1 when a switch happened. */
int mmb_console_poll(void);

/* Tear down every session context and make console 0 active again. Frees the
 * other interpreters; console 0 is reset so it can be re-initialised. */
void mmb_console_reset(void);

/* In-place warm reset (Ctrl+Alt+Del): re-initialise the interpreter on console
 * 0 and land at a ready prompt, without resetting the SoC. The hardware reset
 * can leave the USB controller dead on a Pi, which is why CAD does not use it. */
void mmb_warm_reset(void);

#ifdef __cplusplus
}
#endif

#endif
