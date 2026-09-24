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

/* Number of virtual consoles (Ctrl+Alt+1..4 sessions on every platform). */
#define MMB_MAX_CONSOLES 4

/* Drive letters A..H (see the VFS). Physical drives are C..H. */
#define MMB_MAX_DRIVES 8

/* Pointer state for the full-screen apps (currently PAINT). Coordinates are
 * in screen pixels with (0,0) at the top-left, matching hdmi_width()/
 * hdmi_height(); buttons is a bitmask (1 left, 2 right, 4 middle) and wheel
 * is the accumulated wheel ticks since the last read. */
typedef struct mmb_mouse_state {
	int present; /* non-zero when a pointer device is attached */
	int x, y;
	int buttons;
	int wheel;
} mmb_mouse_state;

/* One entry from a structured directory listing (see mmb_vfs_list_entries).
 * Shared between the C interpreter and the C++ Circle storage backend. `name`
 * carries no trailing slash; `size` is -1 for directories or when a backend
 * cannot tell without an extra lookup. The width covers the longest name a
 * backend can produce (FatFs FF_MAX_LFN is 255) so callers never see a name
 * silently shortened. */
#define MMB_DIRENT_NAME 256
typedef struct mmb_dirent {
	char name[MMB_DIRENT_NAME];
	int is_dir;
	int size;
} mmb_dirent;

/* Ordering for a structured listing: folders first, then case-insensitively by
 * name. Shared by the interpreter and every storage backend so a bounded scan
 * keeps exactly the entries a full listing would show first (#676). */
int mmb_dirent_cmp(const mmb_dirent *a, const mmb_dirent *b);
/* Bounded selection for a structured listing (#676): offer each matching entry
 * as the backend enumerates it and `heap`/`*n` (capacity `max`) retains the
 * `max` smallest under mmb_dirent_cmp. This makes the kept set independent of
 * the backend's raw scan order (readdir/FatFs/node order) instead of an
 * arbitrary first-N cut. `*n` grows to at most `max`; the caller sorts the
 * result and compares the number of candidates seen against `max` to decide
 * whether it was cut. */
void mmb_dirent_offer(mmb_dirent *heap, int *n, int max, const mmb_dirent *e);

typedef struct mmb_platform {
	void (*write_serial)(const char *s, unsigned n);
	void (*write_screen)(const char *s, unsigned n);
	void (*set_pixel)(int x, int y, unsigned rgb888);
	/* Draw a pixel that must survive console text flushes (boot-splash
	 * logo). A bare-metal console keeps its own pixel buffer and repaints
	 * changed rows over the framebuffer, which erases pixels written with
	 * set_pixel alone. Optional; falls back to set_pixel when NULL (native
	 * hosts share one surface for text and graphics). */
	void (*console_pixel)(int x, int y, unsigned rgb888);
	unsigned (*get_pixel)(int x, int y);
	void (*fill_screen)(unsigned rgb888);
	int (*hdmi_width)(void);
	int (*hdmi_height)(void);
	/* Retune the HDMI framebuffer. Returns 1 on success, 0 on failure. */
	int (*resize_hdmi)(int w, int h);
	/* Resolution of the host display the graphics mode is shown on. On a
	 * bare Pi this tracks the HDMI framebuffer; on a native host it can be
	 * larger than the mode, which is integer-scaled into it (letterboxed). */
	int (*host_width)(void);
	int (*host_height)(void);
	/* Build target for MM.RUNTIME: "pi", "linux", "mac", or "windows". */
	const char *(*runtime)(void);
	void *(*alloc)(unsigned n);
	void (*free)(void *p);
	unsigned (*millis)(void);
	/* Optional blocking console line. hide!=0 echoes '*'. On success sets
	 * *out to a malloc'd NUL-terminated line (caller frees via free) and
	 * returns 0; -1 no console; -2 break. */
	int (*read_line)(char **out, int hide);
	/* Drain serial/USB while a program is running (sets break on PrtScr / BREAK key). */
	void (*poll_input)(void);
	/* Read the current pointer position/buttons. Returns 1 and fills *out
	 * when a device is attached, 0 when no pointer is available (a bare
	 * Pi with no mouse, or a backend without one). Optional; may be NULL. */
	int (*mouse_state)(mmb_mouse_state *out);
	/* 1 if a break was requested since the last call (clears the flag). */
	int (*take_break)(void);
	/* Hardware reset. Does not return. */
	void (*reboot)(void);
	/* Non-zero when QUIT should end the host application (native builds).
	 * A bare Pi has no application to close, so QUIT only stops the program. */
	int can_quit;
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
	void (*tui_glyph_n_px)(int x_px, int y_px, unsigned ch, unsigned fg_rgb,
			       unsigned bg_rgb, int scale, int ink_only);
	void (*tui_fill_px)(int x_px, int y_px, int w, int h, unsigned rgb);
	/* Read a pixel from the TUI composition buffer the tui_* helpers and the
	 * pixel helpers draw into (RGB888). Distinct from get_pixel, which reads
	 * the presented surface: on a bare Pi the composition buffer is s_tui_pix
	 * while HDMI shows the previous frame. Optional. */
	unsigned (*tui_get_px)(int x, int y);
	void (*tui_set_font)(const unsigned char *font256x16);
	int (*alt_held)(void);
	/* 1 while both Ctrl and Alt are held (USB modifiers). */
	int (*ctrl_alt_held)(void);
	/* Blit RGB888 pixels to HDMI (native depth + SetArea). stride is
	 * pixels per source row. NULL falls back to set_pixel. */
	void (*present_rgb)(int x, int y, int w, int h, const unsigned *rgb888, int stride);
	/* Blit HDMI-native pixels (DEPTH bits each) to the framebuffer.
	 * stride is pixels per source row. Prefer this when pages are native. */
	void (*present_native)(int x, int y, int w, int h, const void *pix, int stride);
	/* Wait for an in-flight async present (SetArea DMA) to finish. Optional. */
	void (*present_wait)(void);
	/* TERM-only: kick async HDMI present; coalesce while DMA is in flight. */
	void (*term_present_async)(int x, int y, int w, int h, const void *pix, int stride);
	/* Wait for in-flight TERM DMA and flush any coalesced pending blit. */
	void (*term_present_drain)(void);
	/* Non-zero when an in-flight TERM present is DMAing straight from the
	 * caller's page; the caller must drain before rewriting that page. */
	int (*term_present_locked)(void);
	/* Next full-frame present may virt-offset flip (PAGE DISPLAY). Optional. */
	void (*present_set_flip)(int on);
	/* RGB888 <-> HDMI-native colour (DEPTH). Used by page storage. */
	unsigned (*rgb_to_native)(unsigned rgb888);
	unsigned (*native_to_rgb)(unsigned native);
	/* Wait for the next HDMI vblank. Returns 1 if the firmware wait ran. */
	int (*wait_vsync)(void);
	/* Read n raw serial bytes (binary, no echo). 0=ok, -1=fail. */
	int (*read_raw)(unsigned char *buf, unsigned n);
	/* Optional DMA memory copy. Returns 1 if DMA performed the copy,
	 * 0 if the caller should memcpy (small transfer, QEMU, or failure). */
	int (*dma_copy)(void *dst, const void *src, unsigned nbytes);
	/* Optional 2D DMA into a pitched destination (source rows packed).
	 * block_stride is bytes skipped after each block_len in dst.
	 * Returns 1 if DMA used, 0 if the caller should memcpy. */
	int (*dma_copy2d)(void *dst, const void *src, unsigned block_len,
			  unsigned block_count, unsigned block_stride);
	/* Virtual consoles: snapshot/restore one console's screen (0-based
	 * slot). console_save captures the currently displayed screen into the
	 * slot; console_restore paints the slot, clearing it when the slot has
	 * never been saved. tui != 0 when the console is hosting a full-screen
	 * character-cell TUI (its pixels live in a separate offscreen buffer).
	 * Return 1 when handled. Optional (NULL = no screen state, e.g. the
	 * headless backend). */
	int (*console_save)(int slot, int tui);
	int (*console_restore)(int slot, int tui);
	/* Optional: stamp the hardware wall clock after an NTP sync so FAT
	 * file timestamps track it. utc_seconds is Unix epoch seconds and
	 * tz_offset_min is minutes east of UTC. NULL when unsupported. */
	void (*set_wall_clock)(long long utc_seconds, int tz_offset_min);
	/* Host OS clipboard bridge (#525). Only the native desktop build has a
	 * host clipboard; both are NULL on the bare-metal Pi, where a copy just
	 * stays in MMBasic's own buffer. clipboard_get returns a malloc'd UTF-8
	 * string the caller frees via free(), or NULL when empty/unavailable.
	 * clipboard_set stores a UTF-8 copy and returns 0 on success. */
	char *(*clipboard_get)(void);
	int (*clipboard_set)(const char *utf8);
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

/* True while the editor's special-character picker is open. */
int mmb_editor_char_picker_active(void);

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

/* True while PAINT owns the screen/keyboard. */
int mmb_in_paint(void);

/* True while AFK screensaver owns the screen/keyboard. */
int mmb_in_afk(void);

/* Feed a keystroke to AFK (Enter or Ctrl+C exits). */
void mmb_afk_key(char c);

/* Background work (audio decode/mix). */
void mmb_poll(void);

/* 1 while RUN is executing a program. */
int mmb_is_running(void);

/* OPTION BREAK key (ASCII). 0 disables the cooked break key; PrtScr still breaks. */
int mmb_break_key(void);

/* True once after CLS: caller should emit the prompt without leading newlines. */
int mmb_take_home_prompt(void);

/* 1 after QUIT: the host app should end. Does not clear the flag. */
int mmb_quit_requested(void);

/* 1 after QUIT, clearing the flag so the app ends once. */
int mmb_take_quit(void);

/* Immediate HDMI+serial write (Wi-Fi diagnostics, etc.). */
void mmb_console_write(const char *s);
/* Serial only — live TERM/CONNECT must not paint debug on HDMI. */
void mmb_serial_write(const char *s);

/* PicoMite/MMBasic copyright, build version, and HELP hint. Call once at boot. */
void mmb_print_startup(void);

/* Apply the boot destination (OPTION BOOT REPL|LAUNCHER|"app") and paint the
 * REPL prompt unless the launcher owns the screen (#515). Call after
 * mmb_print_startup(). */
void mmb_boot_start(void);

/* Stop audio, save settings, unmount disks, then hardware reset. */
void mmb_reboot(void);

/* Apply COLOUR / OPTION DEFAULT COLOURS to HDMI text (ANSI). */
void mmb_console_apply_colour(void);
void mmb_console_reset_prompt(void);
void mmb_hw_cursor(int show);

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

/* Circle net stack: Wi-Fi or Ethernet, one at a time. */
#define MMB_NET_NONE 0
#define MMB_NET_WIFI 1
#define MMB_NET_ETH  2
int mmb_net_kind(void);
int mmb_net_open(int kind);

/* Host input: typeahead / INKEY$ FIFO and KEYDOWN() scan codes. */
void mmb_inkey_push(int c);
void mmb_keydown_set(const int *codes, int n);

#ifdef __cplusplus
}
#endif

#endif
