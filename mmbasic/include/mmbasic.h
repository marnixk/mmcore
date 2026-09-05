/*
 * Local MMBasic public API for the Circle console.
 *
 * Behavioural target: Colour Maximite 2. Sources live in this tree; the
 * picomite-fork submodule is a reference only.
 */
#ifndef MMBASIC_H
#define MMBASIC_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mmb_platform {
	void (*write_serial)(const char *s, unsigned n);
	void (*write_screen)(const char *s, unsigned n);
	void (*set_pixel)(int x, int y, unsigned rgb888);
	unsigned (*get_pixel)(int x, int y);
	void (*fill_screen)(unsigned rgb888);
	int (*hdmi_width)(void);
	int (*hdmi_height)(void);
	void *(*alloc)(unsigned n);
	void (*free)(void *p);
	unsigned (*millis)(void);
} mmb_platform;

void mmb_init(const mmb_platform *plat);
void mmb_reset(void);

/* Execute one immediate-mode or program line. Returns a NUL-terminated
 * response (empty if the command produced no text). The pointer is valid
 * until the next mmb_* call. */
const char *mmb_exec_line(const char *line);

/* True while the nano-style editor owns the keyboard. */
int mmb_in_editor(void);

/* Feed a keystroke to the editor. Returns text to emit (may be empty). */
const char *mmb_editor_key(char c);

/* Background work (audio decode/mix). */
void mmb_poll(void);

/* True once after CLS: caller should emit the prompt without leading newlines. */
int mmb_take_home_prompt(void);

#ifdef __cplusplus
}
#endif

#endif
