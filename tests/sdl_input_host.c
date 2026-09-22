/*
 * Host test for native/sdl_input.c keyboard routing.
 *
 * Regression: at a blocking INPUT prompt the window must keep receiving
 * keystrokes (they go to the raw inkey queue), instead of the front end
 * REPL editor swallowing them or the loop blocking on fgetc(stdin).
 */
#include "frontend.h"
#include "mmb_priv.h"
#include "sdl_input.h"
#include "sdl_video.h"

#include <SDL.h>
#include <stdio.h>
#include <string.h>

/* ---- minimal interpreter/backend surface used by sdl_input.c ---- */
static int g_running;
static int g_front_feeds;
static unsigned char g_inkey[MMB_INKEY];
static int g_inkey_n;

int mmb_is_running(void) { return g_running; }

void mmb_front_feed(const char *s, unsigned n)
{
	(void)s;
	(void)n;
	g_front_feeds++;
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

static int fails;

static void push_text(const char *s)
{
	SDL_Event e;

	SDL_zero(e);
	e.type = SDL_TEXTINPUT;
	snprintf(e.text.text, sizeof e.text.text, "%s", s);
	SDL_PushEvent(&e);
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

	SDL_Quit();
	if (fails)
		return 1;
	printf("all checks passed\n");
	return 0;
}
