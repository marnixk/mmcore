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
	unsigned (*audio_queued_frames)(void);
	int (*audio_have_device)(void);
	void (*audio_kick)(void);
	void (*audio_flush)(void);
	/* Character-cell TUI: 8x16 cells covering the current HDMI mode. */
	int (*video_cols)(void);
	int (*video_rows)(void);
	void (*tui_prepare)(void);
	void (*tui_glyph)(int col, int row, unsigned ch, unsigned fg_rgb, unsigned bg_rgb);
	/* Copy dirty pixel rows [y0, y1] from the offscreen buffer to HDMI. */
	void (*tui_present)(int y0, int y1);
	/* Scroll a pixel rectangle by dy pixels (positive = content up) and fill. */
	void (*tui_scroll)(int x, int y, int w, int h, int dy, unsigned fill_rgb);
	/* 8x16 glyph drawn at 2x (16x32 pixels, two cells wide and tall). */
	void (*tui_glyph2x)(int col, int row, unsigned ch, unsigned fg_rgb, unsigned bg_rgb);
	void (*tui_glyph_n)(int col, int row, unsigned ch, unsigned fg_rgb, unsigned bg_rgb,
			     int scale);
	void (*tui_set_font)(const unsigned char *font256x16);
	int (*alt_held)(void);
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
int mmb_files_take_prompt(void);

/* Feed a keystroke to FILES. Returns text to emit (may be empty). */
const char *mmb_files_key(char c);

/* Redraw FILES after the editor returns, or close it if EDIT ran a program. */
const char *mmb_files_on_editor_exit(void);

/* Restore the editor TUI after nested interactive HELP closes. */
void mmb_editor_on_ihelp_exit(void);

/* True while interactive HELP owns the keyboard. */
int mmb_in_ihelp(void);

/* Feed a keystroke to IHELP. Returns text to emit (may be empty). */
const char *mmb_ihelp_key(char c);

/* True while CONNECT owns the keyboard (telnet-style session). */
int mmb_in_connect(void);

/* Feed a keystroke to CONNECT. Returns text to emit (may be empty). */
const char *mmb_connect_key(char c);

/* True while TERM fullscreen terminal owns the keyboard. */
int mmb_in_term(void);

/* Feed a keystroke to TERM. Returns text to emit (may be empty). */
const char *mmb_term_key(char c);

/* True while WORDPAD markdown editor owns the keyboard. */
int mmb_in_wordpad(void);

/* Feed a keystroke to WORDPAD. Returns text to emit (may be empty). */
const char *mmb_wordpad_key(char c);

/* Background work (audio decode/mix). */
void mmb_poll(void);

/* 1 while RUN is executing a program. */
int mmb_is_running(void);

/* OPTION BREAK key (ASCII). 0 disables the cooked break key; PrtScr still breaks. */
int mmb_break_key(void);

/* True once after CLS: caller should emit the prompt without leading newlines. */
int mmb_take_home_prompt(void);

/* Immediate HDMI+serial write (Wi-Fi diagnostics, etc.). */
void mmb_console_write(const char *s);

/* PicoMite/MMBasic copyright plus HELP hint. Call once at boot. */
void mmb_print_startup(void);

/* Stop audio, save settings, unmount disks, then hardware reset. */
void mmb_reboot(void);

/* Apply COLOUR / OPTION DEFAULT COLOURS to HDMI text (ANSI). */
void mmb_console_apply_colour(void);

/* OPTION KEYBOARD REPEAT first [, next]  (milliseconds). */
#define MMB_REPEAT_FIRST_DEFAULT 300
#define MMB_REPEAT_NEXT_DEFAULT  75
int mmb_opt_repeat_first(void);
int mmb_opt_repeat_next(void);

/* OPTION WIFI DEBUG ON|OFF (default OFF). */
int mmb_opt_wifi_debug(void);

/* OPTION CONSOLE: serial and/or HDMI. */
int mmb_opt_console_serial(void);
int mmb_opt_console_screen(void);

/* Immediate-mode prompt. BARE is "> "; CWD is DOS $p$g, e.g. "A:/> ". */
const char *mmb_prompt(void);

/* OPTION WIFI COUNTRY "XX" (ISO 3166-1 alpha-2 Circle accepts; default US; UK→GB). */
const char *mmb_opt_wifi_country(void);

/* Host input: typeahead / INKEY$ FIFO and KEYDOWN() scan codes. */
void mmb_inkey_push(int c);
void mmb_keydown_set(const int *codes, int n);

#ifdef __cplusplus
}
#endif

#endif
