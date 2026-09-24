#include "sdl_input.h"

#include "frontend.h"
#include "mmb_priv.h"
#include "session.h"
#include "sdl_video.h"

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int s_alt, s_ctrl, s_shift;
static int s_swallow_text; /* Alt+letter also emits SDL_TEXTINPUT */
static int s_line_input;   /* a blocking line prompt owns the keyboard */

/* Pointer state, reported in software-framebuffer pixels so the full-screen
 * apps see the same coordinate space as the framebuffer. */
static int s_mouse_present;
static int s_mouse_x, s_mouse_y;
static int s_mouse_buttons;
static int s_mouse_wheel;

void sdl_input_init(void)
{
	s_alt = s_ctrl = s_shift = 0;
	s_swallow_text = 0;
	s_line_input = 0;
	s_mouse_present = 1;
	s_mouse_x = s_mouse_y = 0;
	s_mouse_buttons = 0;
	s_mouse_wheel = 0;
}

void sdl_input_mouse_state(int *present, int *x, int *y, int *buttons,
			   int *wheel)
{
	if (present)
		*present = s_mouse_present;
	if (x)
		*x = s_mouse_x;
	if (y)
		*y = s_mouse_y;
	if (buttons)
		*buttons = s_mouse_buttons;
	if (wheel)
		*wheel = s_mouse_wheel;
}

/* SDL button numbers are 1=left, 2=middle, 3=right. */
static int sdl_button_mask(unsigned char button)
{
	switch (button)
	{
	case SDL_BUTTON_LEFT:
		return 1;
	case SDL_BUTTON_RIGHT:
		return 2;
	case SDL_BUTTON_MIDDLE:
		return 4;
	default:
		return 0;
	}
}

static void handle_mouse_motion(const SDL_MouseMotionEvent *me)
{
	int fx, fy;

	if (sdl_video_window_to_fb(me->x, me->y, &fx, &fy))
	{
		s_mouse_x = fx;
		s_mouse_y = fy;
	}
}

static void handle_mouse_button(const SDL_MouseButtonEvent *be)
{
	int mask = sdl_button_mask(be->button);

	if (!mask)
		return;
	if (be->type == SDL_MOUSEBUTTONDOWN)
		s_mouse_buttons |= mask;
	else if (be->type == SDL_MOUSEBUTTONUP)
		s_mouse_buttons &= ~mask;
}

void sdl_input_begin_line(void)
{
	s_line_input = 1;
}

void sdl_input_end_line(void)
{
	s_line_input = 0;
}

int sdl_input_alt_held(void)
{
	return s_alt;
}

int sdl_input_ctrl_alt_held(void)
{
	return s_alt && s_ctrl;
}

/* Feed bytes to the running program or the interactive front end. */
static void deliver(const char *b, unsigned n)
{
	unsigned i;

	if (s_line_input || mmb_is_running())
	{
		for (i = 0; i < n; i++)
			mmb_inkey_push((unsigned char)b[i]);
	}
	else
		mmb_front_feed(b, n);
}

static void deliver_str(const char *s)
{
	deliver(s, (unsigned)strlen(s));
}

static void deliver_ch(char c)
{
	deliver(&c, 1);
}

static void deliver_csi(const char *body)
{
	char buf[16];

	buf[0] = 0x1b;
	buf[1] = '[';
	snprintf(buf + 2, sizeof buf - 2, "%s", body);
	deliver_str(buf);
}

/* Ctrl+Shift+V: feed the host OS clipboard into the active input path (the
 * REPL line editor, a blocking INPUT, or a full-screen app such as EDIT,
 * WORDPAD, TERM, or CONNECT). CR/LF collapse to a single CR and non-ASCII
 * bytes are dropped: MMBasic's input model is CP437/ASCII, so raw UTF-8 would
 * corrupt a line. No host clipboard exists on the bare-metal Pi, and the
 * native headless build keeps an in-process one. */
static void paste_host_clipboard(void)
{
	char *t = mmb_clipboard_get();
	size_t i;
	int last_cr = 0;

	if (!t)
		return;
	for (i = 0; t[i]; i++)
	{
		unsigned char c = (unsigned char)t[i];

		if (c == '\r' || c == '\n')
		{
			if (!last_cr)
				deliver_ch('\r');
			last_cr = 1;
			continue;
		}
		last_cr = 0;
		if (c == '\t')
		{
			deliver_ch('\t');
			continue;
		}
		if (c < 0x20 || c >= 0x80)
			continue;
		deliver_ch((char)c);
	}
	free(t);
}

static int ctrl_code(SDL_Keycode k)
{
	if (k >= SDLK_a && k <= SDLK_z)
		return (int)(k - SDLK_a) + 1;
	if (k == SDLK_SPACE)
		return 0;
	return -1;
}

static void handle_keydown(const SDL_KeyboardEvent *ke)
{
	SDL_Keycode k = ke->keysym.sym;
	int ctrl = (ke->keysym.mod & KMOD_CTRL) != 0;
	int alt = (ke->keysym.mod & KMOD_ALT) != 0;
	int shift = (ke->keysym.mod & KMOD_SHIFT) != 0;

	/* Ctrl+Alt+1..4 switch virtual consoles on every platform (#603).
	 * Both the top-row digits and the numeric keypad are accepted. The old
	 * Ctrl+Alt+F1..F4 chord is retired: the host owns those for real TTYs,
	 * so it never reaches the window. Handled here, in-window, before the
	 * desktop environment can steal the chord. */
	if (ctrl && alt)
	{
		int idx = -1;

		if (k >= SDLK_1 && k <= SDLK_4)
			idx = (int)(k - SDLK_1);
		else if (k >= SDLK_KP_1 && k <= SDLK_KP_4)
			idx = (int)(k - SDLK_KP_1);
		if (idx >= 0)
		{
			mmb_console_switch(idx);
			s_swallow_text = 1;
			return;
		}
	}

	/* Ctrl+Shift+V pastes the host clipboard (native desktop only). */
	if (ctrl && shift && k == SDLK_v)
	{
		paste_host_clipboard();
		return;
	}

	/* Ctrl+D at an empty prompt quits, like a Unix shell (#646). Only the
	 * native SDL input path is involved, and only the REPL line editor: a
	 * running program, a blocking INPUT, or a full-screen app keeps the
	 * existing control-byte handling, so the shared Circle path is
	 * unchanged. A non-empty line leaves Ctrl+D as-is. */
	if (ctrl && !alt && k == SDLK_d && !mmb_is_running() &&
	    !s_line_input && !mmb_front_in_app() && mmb_front_line_empty())
	{
		mmb_exec_line("QUIT");
		return;
	}

	/* Alt+Enter toggles fullscreen at the prompt. */
	if ((k == SDLK_RETURN || k == SDLK_KP_ENTER) && alt &&
	    !mmb_is_running() && !mmb_front_in_app())
	{
		sdl_video_toggle_fullscreen();
		return;
	}

	/* Alt+letter is the app Alt menu / picker prefix. */
	if (alt && !ctrl && k >= SDLK_a && k <= SDLK_z)
	{
		char c = (char)('a' + (k - SDLK_a));

		if (!mmb_is_running() && !s_line_input)
		{
			deliver_ch(0x01);
			deliver_ch(c);
			s_swallow_text = 1;
			return;
		}
	}

	if (ctrl)
	{
		if (k == SDLK_RETURN || k == SDLK_KP_ENTER)
		{
			deliver_csi("29~");
			return;
		}
		if (k >= SDLK_a && k <= SDLK_z)
		{
			int code = ctrl_code(k);

			if (code > 0)
				deliver_ch((char)code);
			return;
		}
		/* Ctrl+Space opens the app picker at the prompt (NUL). */
		if (k == SDLK_SPACE)
		{
			deliver_ch(0);
			return;
		}
	}

	switch (k)
	{
	case SDLK_RETURN:
	case SDLK_KP_ENTER:
		deliver_ch('\r');
		return;
	case SDLK_BACKSPACE:
		deliver_ch(127);
		return;
	case SDLK_TAB:
		if (shift)
			deliver_csi("Z");
		else
			deliver_ch('\t');
		return;
	case SDLK_ESCAPE:
		deliver_ch(0x1b);
		return;
	case SDLK_LEFT:
		deliver_csi(shift ? "1;3D" : (ctrl ? "1;5D" : "D"));
		return;
	case SDLK_RIGHT:
		deliver_csi(shift ? "1;3C" : (ctrl ? "1;5C" : "C"));
		return;
	case SDLK_UP:
		deliver_csi(shift ? "1;3A" : (ctrl ? "1;5A" : "A"));
		return;
	case SDLK_DOWN:
		deliver_csi(shift ? "1;3B" : (ctrl ? "1;5B" : "B"));
		return;
	case SDLK_HOME:
		deliver_csi("H");
		return;
	case SDLK_END:
		deliver_csi("F");
		return;
	case SDLK_INSERT:
		deliver_csi("2~");
		return;
	case SDLK_DELETE:
		deliver_csi("3~");
		return;
	case SDLK_PAGEUP:
		deliver_csi("5~");
		return;
	case SDLK_PAGEDOWN:
		deliver_csi("6~");
		return;
	default:
		break;
	}

	if (k >= SDLK_F1 && k <= SDLK_F10)
	{
		static const char *fseq[] = {
			"11~", "12~", "13~", "14~", "15~",
			"17~", "18~", "19~", "20~", "21~"
		};

		deliver_csi(fseq[k - SDLK_F1]);
		return;
	}

	/* Printable keys are delivered via SDL_TEXTINPUT. */
}

static void handle_text(const SDL_TextInputEvent *te)
{
	const char *t = te->text;
	size_t i, n;

	if (s_swallow_text)
	{
		s_swallow_text = 0;
		return;
	}
	n = strlen(t);
	for (i = 0; i < n; i++)
	{
		unsigned char c = (unsigned char)t[i];

		if (c < 0x80)
		{
			char ch = (char)c;

			deliver(&ch, 1);
		}
	}
}

static void handle_event(const SDL_Event *e)
{
	switch (e->type)
	{
	case SDL_QUIT:
		sdl_video_request_quit();
		break;
	case SDL_WINDOWEVENT:
		if (e->window.event == SDL_WINDOWEVENT_CLOSE)
			sdl_video_request_quit();
		else if (e->window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
			 e->window.event == SDL_WINDOWEVENT_RESIZED ||
			 e->window.event == SDL_WINDOWEVENT_MAXIMIZED ||
			 e->window.event == SDL_WINDOWEVENT_RESTORED ||
			 e->window.event == SDL_WINDOWEVENT_EXPOSED)
			sdl_video_mark_dirty();
		break;
	case SDL_KEYDOWN:
		if (e->key.repeat && e->key.keysym.sym >= SDLK_F1 &&
		    e->key.keysym.sym <= SDLK_F10)
			break;
		s_alt = (e->key.keysym.mod & KMOD_ALT) != 0;
		s_ctrl = (e->key.keysym.mod & KMOD_CTRL) != 0;
		s_shift = (e->key.keysym.mod & KMOD_SHIFT) != 0;
		handle_keydown(&e->key);
		break;
	case SDL_KEYUP:
		s_alt = (e->key.keysym.mod & KMOD_ALT) != 0;
		s_ctrl = (e->key.keysym.mod & KMOD_CTRL) != 0;
		s_shift = (e->key.keysym.mod & KMOD_SHIFT) != 0;
		break;
	case SDL_TEXTINPUT:
		handle_text(&e->text);
		break;
	case SDL_MOUSEMOTION:
		handle_mouse_motion(&e->motion);
		break;
	case SDL_MOUSEBUTTONDOWN:
	case SDL_MOUSEBUTTONUP:
		handle_mouse_button(&e->button);
		break;
	case SDL_MOUSEWHEEL:
		s_mouse_wheel += e->wheel.y;
		break;
	default:
		break;
	}
}

void sdl_input_pump(void)
{
	SDL_Event e;

	while (SDL_PollEvent(&e))
		handle_event(&e);
}
