#include "sdl_input.h"

#include "frontend.h"
#include "mmb_priv.h"
#include "session.h"
#include "sdl_video.h"

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Linux virtual-terminal plumbing is only meaningful for the KMS/DRM
 * framebuffer build on Linux; desktop builds never issue VT ioctls. */
#if defined(MMB_SDL_FRAMEBUFFER) && defined(__linux__)
#define MMB_SDL_HAVE_VT 1
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <linux/kd.h>
#include <linux/vt.h>
#endif

static int s_alt, s_ctrl, s_shift;
static int s_swallow_text; /* Alt/Ctrl chords also emit SDL_TEXTINPUT */
static int s_line_input;   /* a blocking line prompt owns the keyboard */
static int s_break;        /* latched BREAK seen while a program runs */

/* CMM2 KEYDOWN() scan codes for the keys currently held. SDL reports a key by
 * keysym in SDL_KEYDOWN/SDL_KEYUP, so the held set is rebuilt on each event and
 * pushed to the interpreter. This mirrors the Circle USB HID path in
 * console/kernel.cpp so the compat games (which poll KEYDOWN) behave the same
 * on the native/Linux-ISO build. */
static int s_keydown[6];
static int s_keydown_n;

static void keydown_publish(void);

/* Pointer state, reported in software-framebuffer pixels so the full-screen
 * apps see the same coordinate space as the framebuffer. */
static int s_mouse_present;
static int s_mouse_x, s_mouse_y;
static int s_mouse_buttons;
static int s_mouse_wheel;

/* Relative pointer motion synthesised from an evdev touchpad/clickpad. SDL's
 * raw evdev backend exposes a touchpad as a touch device and passes
 * window == NULL to SDL_SendTouch(), so SDL's built-in touch-to-mouse
 * synthesis (which requires a window) never runs and mmcore would otherwise
 * get no SDL_MOUSEMOTION for it (#1039). Track one finger and accumulate its
 * normalised deltas into framebuffer pixels. */
static int s_finger_track_valid;
static SDL_FingerID s_finger_track;
static float s_finger_ax, s_finger_ay;

void sdl_input_init(void)
{
	s_alt = s_ctrl = s_shift = 0;
	s_swallow_text = 0;
	s_line_input = 0;
	s_break = 0;
	s_keydown_n = 0;
	keydown_publish();
	s_mouse_present = 1;
	s_mouse_x = s_mouse_y = 0;
	s_mouse_buttons = 0;
	s_mouse_wheel = 0;
	s_finger_track_valid = 0;
	s_finger_ax = s_finger_ay = 0.0f;
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

void sdl_input_mouse_move(int x, int y)
{
	s_mouse_x = x;
	s_mouse_y = y;
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

/* A touchpad/clickpad reaches SDL as a touch device: SDL_evdev sends
 * SDL_SendTouch(fingerId, window=NULL, ...), and SDL's touch-to-mouse
 * synthesis is gated on a non-NULL window, so no SDL_MOUSEMOTION is emitted
 * (#1039). Turn the normalised finger motion into the same relative pointer
 * state a mouse would produce. Clicks already arrive as SDL_MOUSEBUTTON from
 * the device's BTN_LEFT, so only motion is synthesised here. */
static void handle_finger_down(const SDL_TouchFingerEvent *fe)
{
	if (!s_finger_track_valid)
	{
		s_finger_track_valid = 1;
		s_finger_track = fe->fingerId;
		s_finger_ax = s_finger_ay = 0.0f;
	}
}

static void handle_finger_up(const SDL_TouchFingerEvent *fe)
{
	if (s_finger_track_valid && fe->fingerId == s_finger_track)
		s_finger_track_valid = 0;
}

static void handle_finger_motion(const SDL_TouchFingerEvent *fe)
{
	int w, h, dx, dy;

	if (s_finger_track_valid && fe->fingerId != s_finger_track)
		return;
	w = sdl_video_width();
	h = sdl_video_height();
	if (w <= 0 || h <= 0)
		return;
	/* fe->dx/dy are normalised across the touchpad; scaling by the panel
	 * lets a full-width swipe cross the screen. Keep the sub-pixel
	 * remainder so slow drags still move. */
	s_finger_ax += fe->dx * (float)w;
	s_finger_ay += fe->dy * (float)h;
	dx = (int)s_finger_ax;
	dy = (int)s_finger_ay;
	if (!dx && !dy)
		return;
	s_finger_ax -= (float)dx;
	s_finger_ay -= (float)dy;
	s_mouse_x += dx;
	s_mouse_y += dy;
	if (s_mouse_x < 0)
		s_mouse_x = 0;
	else if (s_mouse_x > w - 1)
		s_mouse_x = w - 1;
	if (s_mouse_y < 0)
		s_mouse_y = 0;
	else if (s_mouse_y > h - 1)
		s_mouse_y = h - 1;
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

/* Feed bytes to the running program or the interactive front end. While a
 * program runs, a key that matches the configured BREAK key is latched as a
 * BREAK (returned by sdl_input_take_break) instead of reaching INKEY$; this
 * mirrors Circle's PollInputChars/TakeBreak and lets Ctrl+C stop RUN. A
 * blocking line prompt still receives its bytes raw so sdl_read_line can
 * break out of INPUT itself. */
static void deliver(const char *b, unsigned n)
{
	unsigned i;

	if (s_line_input)
	{
		for (i = 0; i < n; i++)
			mmb_inkey_push((unsigned char)b[i]);
	}
	else if (mmb_is_running())
	{
		int bk = mmb_break_key();

		for (i = 0; i < n; i++)
		{
			unsigned char c = (unsigned char)b[i];

			/* Only a single cooked key can be a BREAK; do not
			 * mistake a byte inside a CSI navigation sequence for
			 * the break key. */
			if (n == 1 && bk && c == (unsigned char)bk)
				s_break = 1;
			else
				mmb_inkey_push(c);
		}
	}
	else
		mmb_front_feed(b, n);
}

int sdl_input_take_break(void)
{
	int v = s_break;

	s_break = 0;
	return v;
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

/* xterm CSI modifier parameter: 1 + Shift + 2*Alt + 4*Ctrl. A value of 1
 * means "no modifiers" and is omitted from the sequence. */
static int csi_mod(int shift, int alt, int ctrl)
{
	return 1 + shift + 2 * alt + 4 * ctrl;
}

/* Arrow/Home/End style key: bare final when unmodified, else CSI
 * 1;<mod><final>. */
static void deliver_nav_final(char final, int shift, int alt, int ctrl)
{
	char body[8];
	int mod = csi_mod(shift, alt, ctrl);

	if (mod == 1)
		snprintf(body, sizeof body, "%c", final);
	else
		snprintf(body, sizeof body, "1;%d%c", mod, final);
	deliver_csi(body);
}

/* Ins/Del/PgUp/PgDn: bare CSI <num>~ when unmodified, else CSI <num>;<mod>~. */
static void deliver_nav_tilde(int num, int shift, int alt, int ctrl)
{
	char body[16];
	int mod = csi_mod(shift, alt, ctrl);

	if (mod == 1)
		snprintf(body, sizeof body, "%d~", num);
	else
		snprintf(body, sizeof body, "%d;%d~", num, mod);
	deliver_csi(body);
}

int sdl_input_vt_from_key(int sym)
{
	if (sym >= SDLK_F1 && sym <= SDLK_F12)
		return (int)(sym - SDLK_F1) + 1;
	return 0;
}

#ifdef MMB_SDL_FRAMEBUFFER
/* Another Linux VT is foreground; wait for mmcore's VT before delivering more
 * input. SDL's evdev driver keeps enqueuing events across a VT switch (#922),
 * so the key/text/mouse handlers gate on this latch. It is set by vt_switch()
 * and cleared by vt_sync(); non-Linux framebuffer builds never set it. */
static int s_vt_left;
#endif

#ifdef MMB_SDL_HAVE_VT
/* SDL's kmsdrm input reads keys from evdev and mutes the kernel console
 * keyboard (KDSKBMODE K_OFF), so keystrokes never reach tty1's line
 * discipline. The kernel therefore never sees Ctrl+Alt+F<n>, and SDL does not
 * translate it either, so the framebuffer build drives the switch itself:
 * unmute the console keyboard, ask the VT layer to activate the target, then
 * re-mute once SDL reacquires mmcore's VT. SDL's KMSDRM_AcquireVT recreates
 * the surfaces and sends SDL_WINDOWEVENT_RESIZED, which vt_sync() hooks;
 * vt_sync() is also polled from the input pump. Set MMB_SDL_NO_VT=1 to leave
 * the ioctls alone (test/automation aid) while still consuming the chord. */
static int s_vt_fd = -1; /* /dev/tty0, opened on first switch */
static int s_vt_ours;    /* mmcore's VT, learned when we switch away */

static int vt_ioctl_enabled(void)
{
	return getenv("MMB_SDL_NO_VT") == 0;
}

static int vt_open(void)
{
	if (s_vt_fd >= 0)
		return 1;
	s_vt_fd = open("/dev/tty0", O_RDWR | O_CLOEXEC);
	return s_vt_fd >= 0;
}

static int vt_set_kbd_mode(int mode)
{
	return vt_open() && ioctl(s_vt_fd, KDSKBMODE, (unsigned long)mode) == 0;
}

static int vt_active(void)
{
	struct vt_stat st;

	if (!vt_open() || ioctl(s_vt_fd, VT_GETSTATE, &st) != 0)
		return 0;
	return st.v_active;
}

/* Ctrl+Alt+F<n>: leave mmcore for Linux VT n. The target getty can only read
 * input once the console keyboard is unmuted; SDL's VT_PROCESS release
 * callback then drops DRM master and pauses. */
static void vt_switch(int n)
{
	if (n < 1 || n > 12 || !vt_ioctl_enabled())
		return;
	if (s_vt_ours > 0 && n == s_vt_ours)
		return; /* already here: keep the keyboard muted */
	if (!vt_open())
		return;
	if (s_vt_ours <= 0)
		s_vt_ours = vt_active();
	vt_set_kbd_mode(K_UNICODE);
	if (ioctl(s_vt_fd, VT_ACTIVATE, (unsigned long)n) == 0)
		s_vt_left = 1;
}

/* Once mmcore's VT is foreground again, re-apply the mute and force a repaint
 * (kmsdrm destroyed the surfaces across the switch). Cheap and idempotent, so
 * it is safe to call on every window event and frame while waiting. */
static void vt_sync(void)
{
	if (!s_vt_left || !vt_ioctl_enabled())
		return;
	if (vt_active() == s_vt_ours)
	{
		s_vt_left = 0;
		vt_set_kbd_mode(K_OFF);
		sdl_video_mark_dirty();
	}
}
#else
#ifdef MMB_SDL_FRAMEBUFFER
static void vt_switch(int n)
{
	(void)n;
}
#endif
static void vt_sync(void)
{
}
#endif

#ifdef MMB_SDL_TEST
/* Test-only: read/set the "another VT is foreground" latch so a host harness
 * can drive the input gate without real DRM or /dev/tty0 (#922). */
int sdl_input_test_vt_left(void)
{
#ifdef MMB_SDL_FRAMEBUFFER
	return s_vt_left;
#else
	return 0;
#endif
}

void sdl_input_test_set_vt_left(int v)
{
#ifdef MMB_SDL_FRAMEBUFFER
	s_vt_left = v;
#else
	(void)v;
#endif
}
#endif

/* Map an SDL keysym to the CMM2 KEYDOWN() code the compat games expect.
 * Letters, digits and punctuation are their ASCII codes; the arrows use the
 * Colour Maximite codes Up=128, Down=129, Left=130, Right=131. This is the
 * same set console/kernel.cpp emits from USB HID usage codes. */
static int sdl_key_to_cmm2(SDL_Keycode k)
{
	if (k >= SDLK_a && k <= SDLK_z)
		return (int)('a' + (k - SDLK_a));
	if (k >= SDLK_1 && k <= SDLK_9)
		return (int)('1' + (k - SDLK_1));
	if (k == SDLK_0)
		return '0';
	switch (k)
	{
	case SDLK_RETURN:
		return 10;
	case SDLK_ESCAPE:
		return 27;
	case SDLK_SPACE:
		return 32;
	case SDLK_MINUS:
		return '-';
	case SDLK_EQUALS:
		return '=';
	case SDLK_LEFTBRACKET:
		return '[';
	case SDLK_RIGHTBRACKET:
		return ']';
	case SDLK_BACKSLASH:
		return '\\';
	case SDLK_SEMICOLON:
		return ';';
	case SDLK_QUOTE:
		return '\'';
	case SDLK_COMMA:
		return ',';
	case SDLK_PERIOD:
		return '.';
	case SDLK_SLASH:
		return '/';
	case SDLK_RIGHT:
		return 131;
	case SDLK_LEFT:
		return 130;
	case SDLK_DOWN:
		return 129;
	case SDLK_UP:
		return 128;
	default:
		return 0;
	}
}

/* g_cur is NULL during early platform init, before the interpreter session
 * exists; nothing can be held then, so skip the push rather than dereference
 * it. */
static void keydown_publish(void)
{
	if (g_cur)
		mmb_keydown_set(s_keydown, s_keydown_n);
}

static void keydown_clear(void)
{
	s_keydown_n = 0;
	keydown_publish();
}

/* Add a held key (ignoring auto-repeat) and publish the set. */
static void keydown_press(SDL_Keycode k)
{
	int code = sdl_key_to_cmm2(k);
	int i;

	if (!code)
		return;
	for (i = 0; i < s_keydown_n; i++)
		if (s_keydown[i] == code)
			return;
	if (s_keydown_n < (int)(sizeof s_keydown / sizeof s_keydown[0]))
		s_keydown[s_keydown_n++] = code;
	keydown_publish();
}

/* Remove a released key and publish the set. */
static void keydown_release(SDL_Keycode k)
{
	int code = sdl_key_to_cmm2(k);
	int i, j;

	if (!code)
		return;
	for (i = 0; i < s_keydown_n; i++)
	{
		if (s_keydown[i] != code)
			continue;
		for (j = i; j + 1 < s_keydown_n; j++)
			s_keydown[j] = s_keydown[j + 1];
		s_keydown_n--;
		keydown_publish();
		return;
	}
}

static void handle_keydown(const SDL_KeyboardEvent *ke)
{
	SDL_Keycode k = ke->keysym.sym;
	int ctrl = (ke->keysym.mod & KMOD_CTRL) != 0;
	int alt = (ke->keysym.mod & KMOD_ALT) != 0;
	int shift = (ke->keysym.mod & KMOD_SHIFT) != 0;
	int gui = (ke->keysym.mod & KMOD_GUI) != 0;

	/* Any keydown ends the previous chord's text window. The flag only
	 * exists to drop the SDL_TEXTINPUT that can mirror an Alt+letter,
	 * Ctrl+letter, or Ctrl+Alt+<digit> chord, and that event always follows
	 * immediately in the queue. Ctrl+Alt+F<n> produces no text at all, so
	 * without this a stale flag would silently eat the next ordinary
	 * character (#927). */
	s_swallow_text = 0;

	/* Ctrl+Alt+1..4 switch virtual consoles on every platform (#603).
	 * Both the top-row digits and the numeric keypad are accepted. The old
	 * Ctrl+Alt+F1..F4 chord is retired: the host owns those for real TTYs,
	 * so it never reaches the window. Handled here, in-window, before the
	 * desktop environment can steal the chord. */
	if (ctrl && alt)
	{
		int idx = -1;

#ifdef MMB_SDL_FRAMEBUFFER
		/* Framebuffer build: on the bare Linux console Ctrl+Alt+F1..F12
		 * switches the real VT (tty1 is mmcore, tty2 a root shell). A
		 * desktop host owns this chord, so other builds leave it for
		 * the app/F-key mapping. mmcore's own consoles stay on
		 * Ctrl+Alt+1..4 below. */
		{
			int vt = sdl_input_vt_from_key((int)k);

			if (vt)
			{
				vt_switch(vt);
				s_swallow_text = 1;
				return;
			}
		}
#endif

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
		s_swallow_text = 1;
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
		s_swallow_text = 1;
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
			s_swallow_text = 1;
			return;
		}
		if (k >= SDLK_a && k <= SDLK_z)
		{
			int code = ctrl_code(k);

			if (code > 0)
				deliver_ch((char)code);
			s_swallow_text = 1;
			return;
		}
		/* Ctrl+Space opens the app picker at the prompt (NUL). */
		if (k == SDLK_SPACE)
		{
			deliver_ch(0);
			s_swallow_text = 1;
			return;
		}
	}

	/* Chromebooks have no dedicated Home/End/PgUp/PgDn keys; the Search
	 * key is Super (Left Meta). Map Super+arrows onto those keys so they
	 * emit the real keys' bytes, leaving the arrow keys alone and Super by
	 * itself inert (#1054). Shift/Alt/Ctrl still combine through the same
	 * xterm modifier encoding as the real keys. */
	if (gui)
	{
		switch (k)
		{
		case SDLK_UP:
			k = SDLK_PAGEUP;
			break;
		case SDLK_DOWN:
			k = SDLK_PAGEDOWN;
			break;
		case SDLK_LEFT:
			k = SDLK_HOME;
			break;
		case SDLK_RIGHT:
			k = SDLK_END;
			break;
		default:
			break;
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
		deliver_nav_final('D', shift, alt, ctrl);
		return;
	case SDLK_RIGHT:
		deliver_nav_final('C', shift, alt, ctrl);
		return;
	case SDLK_UP:
		deliver_nav_final('A', shift, alt, ctrl);
		return;
	case SDLK_DOWN:
		deliver_nav_final('B', shift, alt, ctrl);
		return;
	case SDLK_HOME:
		deliver_nav_final('H', shift, alt, ctrl);
		return;
	case SDLK_END:
		deliver_nav_final('F', shift, alt, ctrl);
		return;
	case SDLK_INSERT:
		deliver_nav_tilde(2, shift, alt, ctrl);
		return;
	case SDLK_DELETE:
		deliver_nav_tilde(3, shift, alt, ctrl);
		return;
	case SDLK_PAGEUP:
		deliver_nav_tilde(5, shift, alt, ctrl);
		return;
	case SDLK_PAGEDOWN:
		deliver_nav_tilde(6, shift, alt, ctrl);
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

	/* #1074: the KMS/DRM evdev backend emits SDL_TEXTINPUT for the base
	 * letter of a Ctrl chord (desktop backends suppress it), which would
	 * type the letter after the shortcut already ran. handle_keydown()
	 * marks its chord returns with s_swallow_text; this guard is the
	 * backstop for any Ctrl chord lacking a paired mark. Leave AltGr
	 * (Ctrl+Alt) alone: some layouts deliver real characters that way. */
	if ((s_ctrl && !s_alt) || s_swallow_text)
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

/* True while another VT owns the console: SDL's evdev driver keeps enqueuing
 * input regardless of the active VT, so it must be dropped here. Always false
 * on builds without the framebuffer VT latch. */
static int vt_holds_input(void)
{
#ifdef MMB_SDL_FRAMEBUFFER
	return s_vt_left;
#else
	return 0;
#endif
}

static int is_input_event(Uint32 type)
{
	switch (type)
	{
	case SDL_KEYDOWN:
	case SDL_KEYUP:
	case SDL_TEXTINPUT:
	case SDL_FINGERDOWN:
	case SDL_FINGERUP:
	case SDL_FINGERMOTION:
	case SDL_MOUSEMOTION:
	case SDL_MOUSEBUTTONDOWN:
	case SDL_MOUSEBUTTONUP:
	case SDL_MOUSEWHEEL:
		return 1;
	default:
		return 0;
	}
}

static void handle_event(const SDL_Event *e)
{
	if (vt_holds_input() && is_input_event(e->type))
	{
		/* #922: another VT is foreground, but SDL still delivers its
		 * keys/text/mouse to us. Drop them, and clear any modifier the
		 * switch chord latched so it cannot stick across the switch.
		 * Window events still flow through so vt_sync() can re-mute the
		 * keyboard once mmcore's VT returns. */
		s_alt = s_ctrl = s_shift = 0;
		s_swallow_text = 0;
		s_finger_track_valid = 0;
		return;
	}

	switch (e->type)
	{
	case SDL_QUIT:
		sdl_video_request_quit();
		break;
	case SDL_WINDOWEVENT:
		if (e->window.event == SDL_WINDOWEVENT_CLOSE)
			sdl_video_request_quit();
#ifndef MMB_SDL_FRAMEBUFFER
		else if (e->window.event == SDL_WINDOWEVENT_ENTER)
			/* The pointer is over our framebuffer: hide the system
			 * cursor so the app's own sprite is the only pointer
			 * (PAINT paints its tool cursor; two arrows otherwise). */
			SDL_ShowCursor(SDL_DISABLE);
		else if (e->window.event == SDL_WINDOWEVENT_LEAVE)
			SDL_ShowCursor(SDL_ENABLE);
#endif
		else if (e->window.event == SDL_WINDOWEVENT_FOCUS_LOST)
			/* SDL does not deliver the KEYUPs for keys released while
			 * unfocused, so drop the held-key set or they stick. */
			keydown_clear();
		else if (e->window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
			 e->window.event == SDL_WINDOWEVENT_RESIZED ||
			 e->window.event == SDL_WINDOWEVENT_MAXIMIZED ||
			 e->window.event == SDL_WINDOWEVENT_RESTORED ||
			 e->window.event == SDL_WINDOWEVENT_EXPOSED)
		{
			sdl_video_mark_dirty();
			/* SDL's KMSDRM_AcquireVT recreates the surfaces and
			 * sends RESIZED when mmcore's VT comes back, so this is
			 * where the framebuffer build re-mutes the console
			 * keyboard. A no-op on desktop builds and until a VT
			 * switch has been requested. */
			vt_sync();
		}
		break;
	case SDL_KEYDOWN:
		if (e->key.repeat && e->key.keysym.sym >= SDLK_F1 &&
		    e->key.keysym.sym <= SDLK_F10)
			break;
		s_alt = (e->key.keysym.mod & KMOD_ALT) != 0;
		s_ctrl = (e->key.keysym.mod & KMOD_CTRL) != 0;
		s_shift = (e->key.keysym.mod & KMOD_SHIFT) != 0;
		keydown_press(e->key.keysym.sym);
		handle_keydown(&e->key);
		break;
	case SDL_KEYUP:
		s_alt = (e->key.keysym.mod & KMOD_ALT) != 0;
		s_ctrl = (e->key.keysym.mod & KMOD_CTRL) != 0;
		s_shift = (e->key.keysym.mod & KMOD_SHIFT) != 0;
		keydown_release(e->key.keysym.sym);
		/* A chord's mirror SDL_TEXTINPUT (if the backend emits one) is
		 * queued with the keydown, so it has already been processed by
		 * this keyup. The one-shot swallow is therefore spent; drop it
		 * here so a chord that emits no text cannot eat an unrelated
		 * later text event (#1104). */
		s_swallow_text = 0;
		break;
	case SDL_TEXTINPUT:
		handle_text(&e->text);
		break;
	case SDL_FINGERDOWN:
		handle_finger_down(&e->tfinger);
		break;
	case SDL_FINGERUP:
		handle_finger_up(&e->tfinger);
		break;
	case SDL_FINGERMOTION:
		handle_finger_motion(&e->tfinger);
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
	/* Fallback for a VT switch: if SDL did not send a window event (or the
	 * acquire callback arrived between frames), notice it here. No-op unless
	 * a switch is in flight. */
	vt_sync();
}
