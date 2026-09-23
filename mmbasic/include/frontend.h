/*
 * Portable console front end: the interactive REPL (line editor, history,
 * ESC/CSI decoding) and routing of keystrokes to the full-screen apps.
 *
 * Backends feed raw console bytes and provide a byte sink for output; all the
 * language-level behaviour lives here so the Circle and native builds share
 * one implementation.
 */
#ifndef MMB_FRONTEND_H
#define MMB_FRONTEND_H

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*mmb_front_emit_fn)(void *ctx, const char *s, unsigned n);

/* Bind the output sink (serial + screen, as the backend defines it). */
void mmb_front_init(mmb_front_emit_fn emit, void *ctx);

/* Point the line editor at another virtual console's state (0-based). */
void mmb_front_select(int idx);

/* Clear every console's line editor/history (warm reset). */
void mmb_front_reset(void);

/* Feed console bytes (ESC sequences and 0x01-prefixed Alt in included). */
void mmb_front_feed(const char *s, unsigned n);
void mmb_front_feed_byte(char c);

/* True while a full-screen app owns the keyboard. */
int mmb_front_in_app(void);

/* True when the active console's REPL line editor holds no text. */
int mmb_front_line_empty(void);

/* Emit a fresh prompt (shows the hardware cursor). */
void mmb_front_prompt(void);

/* Sealed app sessions (CLI app-VM / TERM) never paint a REPL prompt: when the
 * app or session ends the process exits instead of dropping to `> `. */
void mmb_front_set_sealed(int on);

#ifdef __cplusplus
}
#endif

#endif
