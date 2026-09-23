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

int mmb_is_running(void) { return g_running; }

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

int mmb_front_in_app(void) { return 0; }

int mmb_console_switch(int idx) { (void)idx; return 0; }

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

static void reset(void)
{
	g_inkey_n = 0;
	g_front_feeds = 0;
	g_feed_n = 0;
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
