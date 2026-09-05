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
	/* Retune the HDMI framebuffer. Returns 1 on success, 0 on failure. */
	int (*resize_hdmi)(int w, int h);
	void *(*alloc)(unsigned n);
	void (*free)(void *p);
	unsigned (*millis)(void);
	/* Optional blocking console line. hide!=0 echoes '*'. 0=ok, -1=none. */
	int (*read_line)(char *buf, unsigned maxn, int hide);
	/* Drain serial/USB while a program is running (sets break on PrtScr / BREAK key). */
	void (*poll_input)(void);
	/* 1 if a break was requested since the last call (clears the flag). */
	int (*take_break)(void);
	/* Hardware reset. Does not return. */
	void (*reboot)(void);
	/* 0 = analogue jack (PWM), 1 = HDMI. */
	void (*audio_set_target)(int target);
	void (*audio_enable)(int on);
	/* Write interleaved stereo s16le frames. Returns frames consumed. */
	int (*audio_write)(const short *stereo_s16, unsigned nframes);
	unsigned (*audio_free_frames)(void);
} mmb_platform;

void mmb_init(const mmb_platform *plat);
void mmb_reset(void);

/* Execute one immediate-mode or program line. Returns a NUL-terminated
 * response (empty if the command produced no text). The pointer is valid
 * until the next mmb_* call. */
const char *mmb_exec_line(const char *line);

/* True while the full-screen editor owns the keyboard. */
int mmb_in_editor(void);

/* Feed a keystroke to the editor. Returns text to emit (may be empty). */
const char *mmb_editor_key(char c);

/* True while the FILES dual-pane TUI owns the keyboard. */
int mmb_in_files(void);

/* Feed a keystroke to FILES. Returns text to emit (may be empty). */
const char *mmb_files_key(char c);

/* Redraw FILES after the editor returns, or close it if EDIT ran a program. */
const char *mmb_files_on_editor_exit(void);

/* True while CONNECT owns the keyboard (telnet-style session). */
int mmb_in_connect(void);

/* Feed a keystroke to CONNECT. Returns text to emit (may be empty). */
const char *mmb_connect_key(char c);

/* Background work (audio decode/mix). */
void mmb_poll(void);

/* 1 while RUN is executing a program. */
int mmb_is_running(void);

/* OPTION BREAK key (ASCII). 0 disables the cooked break key; PrtScr still breaks. */
int mmb_break_key(void);

/* True once after CLS: caller should emit the prompt without leading newlines. */
int mmb_take_home_prompt(void);

#ifdef __cplusplus
}
#endif

#endif
