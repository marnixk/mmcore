/*
 * Host test for native/sdl_input.c keyboard routing.
 *
 * Regression: at a blocking INPUT prompt the window must keep receiving
 * keystrokes (they go to the raw inkey queue), instead of the front end
 * REPL editor swallowing them or the loop blocking on fgetc(stdin).
 *
 * Also covers the #525 host clipboard paste hotkey (Ctrl+Shift+V): the host
 * clipboard bytes are delivered, CR/LF collapse to one CR, and an empty
 * clipboard delivers nothing.
 */
#include "frontend.h"
#include "mmb_priv.h"
#include "sdl_input.h"
#include "sdl_video.h"

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- minimal interpreter/backend surface used by sdl_input.c ---- */
static int g_running;
static int g_front_feeds;
static unsigned char g_feed[256];
static int g_feed_n;
static unsigned char g_inkey[MMB_INKEY];
static int g_inkey_n;
static char g_clip[256];
static int g_clip_set;
static int g_console_switches;
static int g_console_last;
static int g_line_empty = 1;
static int g_in_app;
static char g_exec[64];
static int g_exec_n;
static int g_break_key = 3;

int mmb_is_running(void) { return g_running; }

int mmb_break_key(void) { return g_break_key; }

int mmb_front_line_empty(void) { return g_line_empty; }

const char *mmb_exec_line(const char *line)
{
	g_exec_n++;
	snprintf(g_exec, sizeof g_exec, "%s", line);
	return "";
}

char *mmb_clipboard_get(void)
{
	if (!g_clip_set)
		return 0;
	return strdup(g_clip);
}

void mmb_front_feed(const char *s, unsigned n)
{
	unsigned i;

	g_front_feeds++;
	for (i = 0; i < n && g_feed_n < (int)sizeof g_feed; i++)
		g_feed[g_feed_n++] = (unsigned char)s[i];
}

int mmb_front_in_app(void) { return g_in_app; }

int mmb_console_switch(int idx)
{
	g_console_switches++;
	g_console_last = idx;
	return 1;
}

void mmb_inkey_push(int c)
{
	if (g_inkey_n < (int)sizeof g_inkey)
		g_inkey[g_inkey_n++] = (unsigned char)c;
}

void sdl_video_mark_dirty(void) {}
void sdl_video_request_quit(void) {}
void sdl_video_toggle_fullscreen(void) {}

/* Identity viewport for the host test: window pixels == framebuffer pixels. */
int sdl_video_window_to_fb(int wx, int wy, int *fx, int *fy)
{
	if (fx)
		*fx = wx;
	if (fy)
		*fy = wy;
	return 1;
}

static int fails;

static void push_text(const char *s)
{
	SDL_Event e;

	SDL_zero(e);
	e.type = SDL_TEXTINPUT;
	snprintf(e.text.text, sizeof e.text.text, "%s", s);
	SDL_PushEvent(&e);
}

static void push_key(SDL_Keycode sym, Uint16 mod)
{
	SDL_Event e;

	SDL_zero(e);
	e.type = SDL_KEYDOWN;
	e.key.keysym.sym = sym;
	e.key.keysym.mod = mod;
	SDL_PushEvent(&e);
}

static void push_motion(int x, int y)
{
	SDL_Event e;

	SDL_zero(e);
	e.type = SDL_MOUSEMOTION;
	e.motion.x = x;
	e.motion.y = y;
	SDL_PushEvent(&e);
}

static void push_button(int button, int down)
{
	SDL_Event e;

	SDL_zero(e);
	e.type = down ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
	e.button.button = (Uint8)button;
	SDL_PushEvent(&e);
}

static void expect_mouse(const char *name, int x, int y, int buttons)
{
	int present = 0, mx = 0, my = 0, mb = 0, wheel = 0;

	sdl_input_mouse_state(&present, &mx, &my, &mb, &wheel);
	if (!present || mx != x || my != y || mb != buttons)
	{
		fprintf(stderr,
			"FAIL %s: present=%d pos=(%d,%d) want (%d,%d) "
			"buttons=%d want %d\n",
			name, present, mx, my, x, y, mb, buttons);
		fails++;
	}
}
static void expect_queue(const char *name, const char *want)
{
	int want_n = (int)strlen(want);

	if (g_inkey_n != want_n || memcmp(g_inkey, want, (size_t)want_n) != 0)
	{
		fprintf(stderr, "FAIL %s: queue len %d want %d\n", name,
			g_inkey_n, want_n);
		fails++;
	}
}

/* The front end receives one deliver_str call per key; compare the whole
 * escape sequence it was fed. */
static void expect_feed(const char *name, const char *want)
{
	int want_n = (int)strlen(want);

	if (g_feed_n != want_n || memcmp(g_feed, want, (size_t)want_n) != 0)
	{
		fprintf(stderr, "FAIL %s: feed len %d want %d\n", name,
			g_feed_n, want_n);
		fails++;
	}
}

static void reset(void)
{
	g_inkey_n = 0;
	g_front_feeds = 0;
	g_feed_n = 0;
	g_line_empty = 1;
	g_in_app = 0;
	g_exec_n = 0;
	g_exec[0] = '\0';
	g_break_key = 3;
	(void)sdl_input_take_break();
	/* Ctrl+Alt+digit sets the Alt+letter text swallow; clear all per-key
	 * modifier state so each case starts clean. */
	sdl_input_init();
	if (SDL_InitSubSystem(SDL_INIT_VIDEO) == 0)
		SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
}

int main(void)
{
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0)
	{
		fprintf(stderr, "SDL init failed: %s\n", SDL_GetError());
		return 1;
	}
	sdl_input_init();

	/* A running program: keys land in the raw inkey queue. */
	reset();
	g_running = 1;
	push_text("A");
	sdl_input_pump();
	expect_queue("running", "A");

	/* Idle REPL: the front end owns the line editor. */
	reset();
	g_running = 0;
	push_text("B");
	sdl_input_pump();
	if (g_front_feeds == 0 || g_inkey_n != 0)
	{
		fprintf(stderr, "FAIL repl: feeds=%d queue=%d\n", g_front_feeds,
			g_inkey_n);
		fails++;
	}

	/* Not running, but a blocking line prompt is active: it must still get
	 * the raw bytes rather than feeding the REPL. */
	reset();
	g_running = 0;
	sdl_input_begin_line();
	push_text("C");
	sdl_input_pump();
	expect_queue("line prompt", "C");
	if (g_front_feeds != 0)
	{
		fprintf(stderr, "FAIL line prompt: REPL swallowed %d events\n",
			g_front_feeds);
		fails++;
	}
	sdl_input_end_line();

	/* After the prompt the editor is back in charge. */
	reset();
	push_text("D");
	sdl_input_pump();
	if (g_front_feeds == 0 || g_inkey_n != 0)
	{
		fprintf(stderr, "FAIL restored: feeds=%d queue=%d\n",
			g_front_feeds, g_inkey_n);
		fails++;
	}

	/* xterm CSI modifier encoding (#737/#738): mod = 1 + Shift + 2*Alt +
	 * 4*Ctrl. Shift+arrows must not be mistaken for Alt, and plain
	 * Alt+Left/Right stay 1;3 for the editor tab switch. */
	reset();
	g_running = 0;
	push_key(SDLK_LEFT, KMOD_SHIFT);
	sdl_input_pump();
	expect_feed("shift-left", "\x1b[1;2D");

	reset();
	g_running = 0;
	push_key(SDLK_LEFT, KMOD_CTRL);
	sdl_input_pump();
	expect_feed("ctrl-left", "\x1b[1;5D");

	reset();
	g_running = 0;
	push_key(SDLK_LEFT, KMOD_CTRL | KMOD_SHIFT);
	sdl_input_pump();
	expect_feed("ctrl-shift-left", "\x1b[1;6D");

	reset();
	g_running = 0;
	push_key(SDLK_RIGHT, KMOD_ALT);
	sdl_input_pump();
	expect_feed("alt-right", "\x1b[1;3C");

	reset();
	g_running = 0;
	push_key(SDLK_UP, KMOD_SHIFT);
	sdl_input_pump();
	expect_feed("shift-up", "\x1b[1;2A");

	reset();
	g_running = 0;
	push_key(SDLK_DOWN, 0);
	sdl_input_pump();
	expect_feed("plain-down", "\x1b[B");

	/* Ins/Del/PgUp/PgDn carry the same modifiers (Circle PollUsbEditorNav):
	 * Shift+Del cut, Ctrl+Ins copy, Shift+Ins paste. */
	reset();
	g_running = 0;
	push_key(SDLK_DELETE, KMOD_SHIFT);
	sdl_input_pump();
	expect_feed("shift-del", "\x1b[3;2~");

	reset();
	g_running = 0;
	push_key(SDLK_INSERT, KMOD_CTRL);
	sdl_input_pump();
	expect_feed("ctrl-ins", "\x1b[2;5~");

	reset();
	g_running = 0;
	push_key(SDLK_INSERT, KMOD_SHIFT);
	sdl_input_pump();
	expect_feed("shift-ins", "\x1b[2;2~");

	reset();
	g_running = 0;
	push_key(SDLK_PAGEDOWN, KMOD_CTRL);
	sdl_input_pump();
	expect_feed("ctrl-pgdn", "\x1b[6;5~");

	reset();
	g_running = 0;
	push_key(SDLK_HOME, 0);
	sdl_input_pump();
	expect_feed("plain-home", "\x1b[H");

	reset();
	g_running = 0;
	push_key(SDLK_END, KMOD_SHIFT);
	sdl_input_pump();
	expect_feed("shift-end", "\x1b[1;2F");

	/* Ctrl+Space at the idle REPL opens the app picker: the front end sees a
	 * single NUL byte (#589). */
	reset();
	g_running = 0;
	push_key(SDLK_SPACE, KMOD_CTRL);
	sdl_input_pump();
	if (g_front_feeds != 1 || g_feed_n != 1 || g_feed[0] != 0 || g_inkey_n != 0)
	{
		fprintf(stderr,
			"FAIL ctrl-space: feeds=%d n=%d first=%d queue=%d\n",
			g_front_feeds, g_feed_n,
			g_feed_n ? g_feed[0] : -1, g_inkey_n);
		fails++;
	}

	/* Ctrl+D at an empty prompt runs QUIT (#646). */
	reset();
	g_running = 0;
	g_line_empty = 1;
	push_key(SDLK_d, KMOD_CTRL);
	sdl_input_pump();
	if (g_exec_n != 1 || strcmp(g_exec, "QUIT") != 0 ||
	    g_front_feeds != 0 || g_inkey_n != 0)
	{
		fprintf(stderr,
			"FAIL ctrl-d empty: exec=%d '%s' feeds=%d queue=%d\n",
			g_exec_n, g_exec, g_front_feeds, g_inkey_n);
		fails++;
	}

	/* Ctrl+D on a non-empty line keeps its old meaning: the 0x04 control
	 * byte is delivered and QUIT is not run. */
	reset();
	g_running = 0;
	g_line_empty = 0;
	push_key(SDLK_d, KMOD_CTRL);
	sdl_input_pump();
	if (g_exec_n != 0 || g_front_feeds != 1 || g_feed_n != 1 ||
	    g_feed[0] != 4)
	{
		fprintf(stderr,
			"FAIL ctrl-d non-empty: exec=%d feeds=%d first=%d\n",
			g_exec_n, g_front_feeds, g_feed_n ? g_feed[0] : -1);
		fails++;
	}

	/* Ctrl+D while a full-screen app owns the keyboard is not a quit. */
	reset();
	g_running = 0;
	g_line_empty = 1;
	g_in_app = 1;
	push_key(SDLK_d, KMOD_CTRL);
	sdl_input_pump();
	if (g_exec_n != 0 || g_front_feeds != 1 || g_feed_n != 1 ||
	    g_feed[0] != 4)
	{
		fprintf(stderr,
			"FAIL ctrl-d in-app: exec=%d feeds=%d first=%d\n",
			g_exec_n, g_front_feeds, g_feed_n ? g_feed[0] : -1);
		fails++;
	}

	/* Ctrl+D while a blocking INPUT owns the keyboard goes to the program. */
	reset();
	g_running = 0;
	sdl_input_begin_line();
	push_key(SDLK_d, KMOD_CTRL);
	sdl_input_pump();
	expect_queue("ctrl-d line prompt", "\x04");
	if (g_exec_n != 0 || g_front_feeds != 0)
	{
		fprintf(stderr, "FAIL ctrl-d line prompt: exec=%d feeds=%d\n",
			g_exec_n, g_front_feeds);
		fails++;
	}
	sdl_input_end_line();

	/* Ctrl+D while a program runs is still its usual control byte. */
	reset();
	g_running = 1;
	push_key(SDLK_d, KMOD_CTRL);
	sdl_input_pump();
	expect_queue("ctrl-d running", "\x04");
	if (g_exec_n != 0)
	{
		fprintf(stderr, "FAIL ctrl-d running: exec=%d\n", g_exec_n);
		fails++;
	}

	/* Ctrl+Alt+1..4 switch virtual consoles on every platform (#603); the
	 * numeric keypad works too. */
	reset();
	g_running = 0;
	g_console_switches = 0;
	g_console_last = -1;
	push_key(SDLK_1, KMOD_CTRL | KMOD_ALT);
	push_key(SDLK_KP_4, KMOD_CTRL | KMOD_ALT);
	sdl_input_pump();
	if (g_console_switches != 2 || g_console_last != 3)
	{
		fprintf(stderr,
			"FAIL ctrl-alt-digit: switches=%d last=%d\n",
			g_console_switches, g_console_last);
		fails++;
	}

	/* A digit chord without both modifiers is not a console switch. */
	reset();
	g_running = 0;
	g_console_switches = 0;
	push_key(SDLK_2, KMOD_ALT);
	push_key(SDLK_3, KMOD_CTRL);
	sdl_input_pump();
	if (g_console_switches != 0)
	{
		fprintf(stderr, "FAIL partial chord: switches=%d\n",
			g_console_switches);
		fails++;
	}

	/* Ctrl+Alt+F1..F4 is retired: it must not switch consoles. */
	reset();
	g_running = 1;
	g_console_switches = 0;
	push_key(SDLK_F2, KMOD_CTRL | KMOD_ALT);
	sdl_input_pump();
	if (g_console_switches != 0)
	{
		fprintf(stderr, "FAIL retired fkey: switches=%d\n",
			g_console_switches);
		fails++;
	}

	/* Ctrl+Shift+V pastes the host clipboard into the raw inkey queue while a
	 * program runs: CR/LF collapse to a single CR and are not swallowed. */
	reset();
	g_running = 1;
	snprintf(g_clip, sizeof g_clip, "P1\r\nP2\nP3");
	g_clip_set = 1;
	push_key(SDLK_v, KMOD_CTRL | KMOD_SHIFT);
	sdl_input_pump();
	expect_queue("clipboard paste", "P1\rP2\rP3");

	/* An empty host clipboard pastes nothing (and no raw Ctrl+V control). */
	reset();
	g_running = 1;
	g_clip_set = 0;
	push_key(SDLK_v, KMOD_CTRL | KMOD_SHIFT);
	sdl_input_pump();
	expect_queue("empty clipboard", "");

	/* Ctrl+V without Shift keeps its old meaning: the 0x16 control byte. */
	reset();
	g_running = 1;
	push_key(SDLK_v, KMOD_CTRL);
	sdl_input_pump();
	expect_queue("ctrl-v control", "\x16");

	/* Ctrl+C while a program runs latches BREAK instead of INKEY$ 3 (#740),
	 * so mmb_check_break() can stop the RUN. */
	reset();
	g_running = 1;
	push_key(SDLK_c, KMOD_CTRL);
	sdl_input_pump();
	if (g_inkey_n != 0 || !sdl_input_take_break())
	{
		fprintf(stderr, "FAIL break ctrl-c: queue=%d\n", g_inkey_n);
		fails++;
	}
	/* Latched BREAK is consumed once. */
	if (sdl_input_take_break())
	{
		fprintf(stderr, "FAIL break latch not cleared\n");
		fails++;
	}

	/* OPTION BREAK 0 disables the break key: Ctrl+C is no longer a BREAK. */
	reset();
	g_running = 1;
	g_break_key = 0;
	push_key(SDLK_c, KMOD_CTRL);
	sdl_input_pump();
	{
		int brk = sdl_input_take_break();

		if (g_inkey_n != 1 || g_inkey[0] != 3 || brk)
		{
			fprintf(stderr,
				"FAIL break disabled: queue=%d first=%d brk=%d\n",
				g_inkey_n, g_inkey_n ? g_inkey[0] : -1, brk);
			fails++;
		}
	}

	/* OPTION BREAK n follows the configured key, not a hard-coded 3. */
	reset();
	g_running = 1;
	g_break_key = 'A';
	push_text("A");
	sdl_input_pump();
	if (g_inkey_n != 0 || !sdl_input_take_break())
	{
		fprintf(stderr, "FAIL break custom: queue=%d\n", g_inkey_n);
		fails++;
	}

	/* A byte inside a CSI navigation sequence is not a BREAK even when it
	 * matches the configured break key: Shift+Left ends in 'D'. */
	reset();
	g_running = 1;
	g_break_key = 'D';
	push_key(SDLK_LEFT, KMOD_SHIFT);
	sdl_input_pump();
	expect_queue("csi not break", "\x1b[1;2D");
	if (sdl_input_take_break())
	{
		fprintf(stderr, "FAIL csi latched break\n");
		fails++;
	}

	/* A blocking INPUT inside a running program keeps its raw bytes: the
	 * line reader breaks on the 0x03 itself. */
	reset();
	g_running = 1;
	sdl_input_begin_line();
	push_key(SDLK_c, KMOD_CTRL);
	sdl_input_pump();
	expect_queue("break during input", "\x03");
	if (sdl_input_take_break())
	{
		fprintf(stderr, "FAIL break during input latched\n");
		fails++;
	}
	sdl_input_end_line();

	/* At the idle REPL Ctrl+C still cancels the line via the front end and
	 * does not latch a BREAK. */
	reset();
	g_running = 0;
	push_key(SDLK_c, KMOD_CTRL);
	sdl_input_pump();
	expect_feed("ctrl-c repl", "\x03");
	if (sdl_input_take_break())
	{
		fprintf(stderr, "FAIL break latched at REPL\n");
		fails++;
	}

	/* A pointer is reported in framebuffer pixels with a button bitmask. */
	reset();
	push_motion(120, 64);
	push_button(SDL_BUTTON_LEFT, 1);
	sdl_input_pump();
	expect_mouse("mouse move+left", 120, 64, 1);
	push_button(SDL_BUTTON_LEFT, 0);
	push_button(SDL_BUTTON_RIGHT, 1);
	sdl_input_pump();
	expect_mouse("mouse right", 120, 64, 2);
	push_button(SDL_BUTTON_RIGHT, 0);
	push_motion(200, 96);
	sdl_input_pump();
	expect_mouse("mouse release+move", 200, 96, 0);

	SDL_Quit();
	if (fails)
		return 1;
	printf("all checks passed\n");
	return 0;
}
