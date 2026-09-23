/*
 * Test-only input injection / framebuffer capture for the native SDL build.
 * See sdl_harness.h for the script format and rationale.
 */
#include "win_compat.h"

#include "sdl_harness.h"

#include "frontend.h"
#include "mmb_priv.h"
#include "paint.h"
#include "sdl_input.h"
#include "sdl_video.h"

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HS_QUEUE_MAX 256
#define HS_ARG_MAX   512
#define HS_LINE_MAX  1024

enum hs_kind {
	HS_MOUSE,
	HS_BUTTON,
	HS_KEY,
	HS_TEXT,
	HS_SHOT,
	HS_MARK,
	HS_FEED,
	HS_QUIT
};

typedef struct {
	enum hs_kind kind;
	int x, y;		/* HS_MOUSE */
	int button;		/* HS_BUTTON: SDL_BUTTON_* */
	int down;		/* HS_BUTTON */
	SDL_Keycode sym;	/* HS_KEY */
	int mods;		/* HS_KEY: SDL_Keymod bits */
	char arg[HS_ARG_MAX];	/* path / text / feed line */
} hs_step;

static FILE *s_fp;
static int s_active;
static hs_step s_q[HS_QUEUE_MAX];
static int s_head, s_tail;
static int s_queued;
static char s_partial[HS_LINE_MAX];
static size_t s_partial_n;

/* ---- queue ------------------------------------------------------------- */

static int hs_push(const hs_step *st)
{
	if (s_queued >= HS_QUEUE_MAX)
		return 0;
	s_q[s_tail] = *st;
	s_tail = (s_tail + 1) % HS_QUEUE_MAX;
	s_queued++;
	return 1;
}

static int hs_pop(hs_step *st)
{
	if (s_queued <= 0)
		return 0;
	*st = s_q[s_head];
	s_head = (s_head + 1) % HS_QUEUE_MAX;
	s_queued--;
	return 1;
}

/* ---- command parsing --------------------------------------------------- */

static char *hs_skip(char *p)
{
	while (*p == ' ' || *p == '\t')
		p++;
	return p;
}

/* The remainder of the line, leading spaces stripped, trailing left intact. */
static void hs_rest(char *p, char *out, size_t outsz)
{
	p = hs_skip(p);
	snprintf(out, outsz, "%s", p);
}

static int hs_button(const char *name)
{
	switch (name[0])
	{
	case 'r':
	case 'R':
		return SDL_BUTTON_RIGHT;
	case 'm':
	case 'M':
		return SDL_BUTTON_MIDDLE;
	default:
		return SDL_BUTTON_LEFT;
	}
}

static SDL_Keycode hs_keycode(const char *name)
{
	static const struct {
		const char *name;
		SDL_Keycode sym;
	} named[] = {
		{ "esc", SDLK_ESCAPE }, { "escape", SDLK_ESCAPE },
		{ "enter", SDLK_RETURN }, { "return", SDLK_RETURN },
		{ "space", SDLK_SPACE }, { "tab", SDLK_TAB },
		{ "backspace", SDLK_BACKSPACE }, { "up", SDLK_UP },
		{ "down", SDLK_DOWN }, { "left", SDLK_LEFT },
		{ "right", SDLK_RIGHT }, { "home", SDLK_HOME },
		{ "end", SDLK_END }, { "insert", SDLK_INSERT },
		{ "delete", SDLK_DELETE }, { "pageup", SDLK_PAGEUP },
		{ "pagedown", SDLK_PAGEDOWN }, { "spacebar", SDLK_SPACE },
	};
	size_t i;

	if (name[0] && !name[1])
	{
		unsigned char c = (unsigned char)name[0];

		if (c >= 'a' && c <= 'z')
			return (SDL_Keycode)SDLK_a + (c - 'a');
		if (c >= 'A' && c <= 'Z')
			return (SDL_Keycode)SDLK_a + (c - 'A');
		if (c >= '0' && c <= '9')
			return (SDL_Keycode)SDLK_0 + (c - '0');
	}
	if (name[0] == 'f' || name[0] == 'F')
	{
		int n = atoi(name + 1);

		if (n >= 1 && n <= 12)
			return (SDL_Keycode)SDLK_F1 + (n - 1);
	}
	for (i = 0; i < sizeof named / sizeof named[0]; i++)
		if (strcmp(name, named[i].name) == 0)
			return named[i].sym;
	return SDLK_UNKNOWN;
}

static void hs_parse_key(char *spec, hs_step *st)
{
	int mods = KMOD_NONE;
	char *key;
	char *tok;

	st->kind = HS_KEY;
	st->mods = 0;
	key = hs_skip(spec);
	while ((tok = strchr(key, '+')) != 0)
	{
		*tok = 0;
		if (strcmp(key, "alt") == 0)
			mods |= KMOD_ALT;
		else if (strcmp(key, "ctrl") == 0 ||
			 strcmp(key, "control") == 0)
			mods |= KMOD_CTRL;
		else if (strcmp(key, "shift") == 0)
			mods |= KMOD_SHIFT;
		else if (strcmp(key, "gui") == 0 || strcmp(key, "super") == 0)
			mods |= KMOD_GUI;
		key = tok + 1;
	}
	st->sym = hs_keycode(key);
	st->mods = mods;
}

/* Parse one complete line into a step (possibly two, for `click`). */
static void hs_parse_line(char *line)
{
	char *p = hs_skip(line);
	char *word;
	hs_step st;

	if (!*p || *p == '#')
		return;
	memset(&st, 0, sizeof st);

	word = p;
	while (*p && *p != ' ' && *p != '\t')
		p++;
	if (*p)
		*p++ = 0;

	if (strcmp(word, "feed") == 0)
	{
		st.kind = HS_FEED;
		hs_rest(p, st.arg, sizeof st.arg);
		hs_push(&st);
	}
	else if (strcmp(word, "text") == 0)
	{
		st.kind = HS_TEXT;
		hs_rest(p, st.arg, sizeof st.arg);
		hs_push(&st);
	}
	else if (strcmp(word, "mouse") == 0)
	{
		char *end;
		st.kind = HS_MOUSE;
		p = hs_skip(p);
		st.x = (int)strtol(p, &end, 10);
		p = hs_skip(end);
		st.y = (int)strtol(p, &end, 10);
		hs_push(&st);
	}
	else if (strcmp(word, "down") == 0 || strcmp(word, "up") == 0)
	{
		st.kind = HS_BUTTON;
		st.down = word[0] == 'd';
		st.button = hs_button(hs_skip(p));
		hs_push(&st);
	}
	else if (strcmp(word, "click") == 0)
	{
		int button = hs_button(hs_skip(p));

		st.kind = HS_BUTTON;
		st.down = 1;
		st.button = button;
		hs_push(&st);
		st.down = 0;
		hs_push(&st);
	}
	else if (strcmp(word, "key") == 0)
	{
		hs_parse_key(p, &st);
		hs_push(&st);
	}
	else if (strcmp(word, "shot") == 0 || strcmp(word, "mark") == 0)
	{
		st.kind = word[0] == 's' ? HS_SHOT : HS_MARK;
		hs_rest(p, st.arg, sizeof st.arg);
		hs_push(&st);
	}
	else if (strcmp(word, "quit") == 0)
	{
		st.kind = HS_QUIT;
		hs_push(&st);
	}
}

/* Read any newly appended bytes and enqueue their complete lines. */
static void hs_read_new(void)
{
	int c;

	if (!s_fp)
		return;
	while ((c = fgetc(s_fp)) != EOF)
	{
		if (c == '\n')
		{
			s_partial[s_partial_n] = 0;
			hs_parse_line(s_partial);
			s_partial_n = 0;
		}
		else if (c != '\r')
		{
			if (s_partial_n + 1 < sizeof s_partial)
				s_partial[s_partial_n++] = (char)c;
		}
	}
	clearerr(s_fp);
}

/* ---- execution --------------------------------------------------------- */

static void hs_push_event(SDL_Event *e)
{
	SDL_PushEvent(e);
}

static void hs_exec(const hs_step *st)
{
	SDL_Event e;

	switch (st->kind)
	{
	case HS_MOUSE:
		memset(&e, 0, sizeof e);
		e.type = SDL_MOUSEMOTION;
		e.motion.x = st->x;
		e.motion.y = st->y;
		e.motion.xrel = st->x;
		e.motion.yrel = st->y;
		hs_push_event(&e);
		break;
	case HS_BUTTON:
		memset(&e, 0, sizeof e);
		e.type = st->down ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
		e.button.button = (Uint8)st->button;
		e.button.state = st->down ? SDL_PRESSED : SDL_RELEASED;
		e.button.clicks = 1;
		hs_push_event(&e);
		break;
	case HS_KEY:
		memset(&e, 0, sizeof e);
		e.type = SDL_KEYDOWN;
		e.key.state = SDL_PRESSED;
		e.key.repeat = 0;
		e.key.keysym.sym = st->sym;
		e.key.keysym.mod = (SDL_Keymod)st->mods;
		hs_push_event(&e);
		memset(&e, 0, sizeof e);
		e.type = SDL_KEYUP;
		e.key.state = SDL_RELEASED;
		e.key.keysym.sym = st->sym;
		e.key.keysym.mod = (SDL_Keymod)st->mods;
		hs_push_event(&e);
		break;
	case HS_TEXT:
		memset(&e, 0, sizeof e);
		e.type = SDL_TEXTINPUT;
		snprintf(e.text.text, sizeof e.text.text, "%s", st->arg);
		hs_push_event(&e);
		break;
	case HS_SHOT:
		if (st->arg[0])
			sdl_video_dump_ppm(st->arg);
		break;
	case HS_MARK:
		if (st->arg[0])
		{
			FILE *f = fopen(st->arg, "wb");

			if (f)
			{
				fputs("ok\n", f);
				fclose(f);
			}
		}
		break;
	case HS_FEED:
		mmb_front_feed(st->arg, (unsigned)strlen(st->arg));
		mmb_front_feed_byte('\n');
		break;
	case HS_QUIT:
		sdl_video_request_quit();
		break;
	}
}

/* ---- API --------------------------------------------------------------- */

void sdl_harness_init(void)
{
	const char *path = getenv("MMB_SDL_HARNESS");

	s_fp = 0;
	s_active = 0;
	s_head = s_tail = s_queued = 0;
	s_partial_n = 0;
	if (!path || !path[0])
		return;
	s_fp = fopen(path, "rb");
	if (!s_fp)
		return;
	s_active = 1;
	/* The harness simulates a pointer: report one as present on both the
	 * platform and PAINT's test override so the no-mouse gate is bypassed. */
	sdl_input_init();
	pt_force_mouse(1);
}

void sdl_harness_poll(void)
{
	hs_step st;

	if (!s_active)
		return;
	if (s_queued == 0)
		hs_read_new();
	if (hs_pop(&st))
		hs_exec(&st);
}
