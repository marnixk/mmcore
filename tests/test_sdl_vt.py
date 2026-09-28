"""#903: the framebuffer build switches the Linux VT on Ctrl+Alt+F1..F12.

SDL's kmsdrm input mutes the console keyboard, so the kernel never sees
Ctrl+Alt+Fn. The native framebuffer build handles the chord itself and issues
the VT ioctls; desktop builds must be untouched.

A real VT switch cannot run in CI (it needs root on a bare console), so the
host harness composites ``native/sdl_input.c`` twice: once with
``MMB_SDL_FRAMEBUFFER`` (the chord is consumed, no escape leaks to the app)
and once without (the desktop F-key mapping is unchanged). ``MMB_SDL_NO_VT``
keeps any real ``/dev/tty0`` ioctl out of the test process.
"""

import os
import subprocess

import pytest

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Stand-alone host build of sdl_input.c with the minimal interpreter and video
# surface it references. Kept in this file so the bundle owns no extra source.
_HARNESS = r'''
#include "frontend.h"
#include "mmb_priv.h"
#include "sdl_input.h"
#include "sdl_video.h"

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_running;
static int g_front_feeds;
static unsigned char g_feed[64];
static int g_feed_n;
static unsigned char g_inkey[MMB_INKEY];
static int g_inkey_n;
static int g_console_switches;
static int g_console_last;

int mmb_is_running(void) { return g_running; }
int mmb_break_key(void) { return 3; }
int mmb_front_line_empty(void) { return 1; }
int mmb_front_in_app(void) { return 0; }
const char *mmb_exec_line(const char *line) { (void)line; return ""; }
char *mmb_clipboard_get(void) { return 0; }

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

void mmb_front_feed(const char *s, unsigned n)
{
	unsigned i;

	g_front_feeds++;
	for (i = 0; i < n && g_feed_n < (int)sizeof g_feed; i++)
		g_feed[g_feed_n++] = (unsigned char)s[i];
}

void sdl_video_mark_dirty(void) {}
void sdl_video_request_quit(void) {}
void sdl_video_toggle_fullscreen(void) {}

int sdl_video_window_to_fb(int wx, int wy, int *fx, int *fy)
{
	if (fx)
		*fx = wx;
	if (fy)
		*fy = wy;
	return 1;
}

static int fails;

#define CHECK(cond, msg)                                                       \
	do                                                                     \
	{                                                                      \
		if (!(cond))                                                   \
		{                                                              \
			fprintf(stderr, "FAIL %s\n", msg);                     \
			fails++;                                               \
		}                                                              \
	} while (0)

static void push_key(SDL_Keycode sym, Uint16 mod)
{
	SDL_Event e;

	SDL_zero(e);
	e.type = SDL_KEYDOWN;
	e.key.keysym.sym = sym;
	e.key.keysym.mod = mod;
	SDL_PushEvent(&e);
}

static void reset(void)
{
	g_inkey_n = 0;
	g_front_feeds = 0;
	g_feed_n = 0;
	g_console_switches = 0;
	g_console_last = -1;
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

	/* Pure key -> VT mapping: only the top-row F1..F12. */
	CHECK(sdl_input_vt_from_key(SDLK_F1) == 1, "F1 -> VT 1");
	CHECK(sdl_input_vt_from_key(SDLK_F2) == 2, "F2 -> VT 2");
	CHECK(sdl_input_vt_from_key(SDLK_F12) == 12, "F12 -> VT 12");
	CHECK(sdl_input_vt_from_key(SDLK_5) == 0, "top-row digit is not a VT");
	CHECK(sdl_input_vt_from_key(SDLK_KP_2) == 0, "keypad digit is not a VT");
	CHECK(sdl_input_vt_from_key(SDLK_a) == 0, "letter is not a VT");

#ifdef MMB_SDL_FRAMEBUFFER
	/* The chord is consumed: nothing reaches the app and it is not one of
	 * mmcore's own virtual consoles. MMB_SDL_NO_VT stops the handler from
	 * opening /dev/tty0 in the test process. */
	reset();
	g_running = 0;
	push_key(SDLK_F2, KMOD_CTRL | KMOD_ALT);
	sdl_input_pump();
	CHECK(g_front_feeds == 0 && g_feed_n == 0 && g_inkey_n == 0,
	      "fb Ctrl+Alt+F2 is swallowed");
	CHECK(g_console_switches == 0,
	      "fb Ctrl+Alt+F2 is not an mmcore console");

	/* Every top-row F key is swallowed, so none falls through to CSI. */
	reset();
	for (int i = 0; i < 12; i++)
		push_key(SDLK_F1 + i, KMOD_CTRL | KMOD_ALT);
	sdl_input_pump();
	CHECK(g_front_feeds == 0 && g_inkey_n == 0,
	      "fb Ctrl+Alt+F1..F12 are swallowed");
	CHECK(g_console_switches == 0,
	      "fb Ctrl+Alt+F1..F12 do not switch mmcore consoles");

	/* mmcore's own Ctrl+Alt+1..4 still switch its virtual consoles. */
	reset();
	g_running = 0;
	push_key(SDLK_KP_4, KMOD_CTRL | KMOD_ALT);
	sdl_input_pump();
	CHECK(g_console_switches == 1 && g_console_last == 3 &&
	      g_front_feeds == 0,
	      "fb Ctrl+Alt+KP4 switches mmcore console 4");

	/* A bare F2 (no chord) keeps its F-key escape sequence. */
	reset();
	g_running = 1;
	push_key(SDLK_F2, 0);
	sdl_input_pump();
	CHECK(g_inkey_n == 5 && memcmp(g_inkey, "\x1b[12~", 5) == 0,
	      "fb bare F2 -> CSI 12~");
#else
	/* Desktop: Ctrl+Alt+F2 keeps the old F2 mapping and never switches a
	 * VT or an mmcore console. */
	reset();
	g_running = 1;
	push_key(SDLK_F2, KMOD_CTRL | KMOD_ALT);
	sdl_input_pump();
	CHECK(g_console_switches == 0,
	      "desktop Ctrl+Alt+F2 is not an mmcore console");
	CHECK(g_inkey_n == 5 && memcmp(g_inkey, "\x1b[12~", 5) == 0,
	      "desktop Ctrl+Alt+F2 -> CSI 12~");
#endif

	SDL_Quit();
	if (fails)
		return 1;
	printf("all checks passed\n");
	return 0;
}
'''


def _read(rel):
    with open(os.path.join(REPO, rel), encoding="utf-8") as fh:
        return fh.read()


def _sdl_flags():
    r = subprocess.run(
        ["pkg-config", "--cflags", "--libs", "sdl2"],
        capture_output=True,
        text=True,
    )
    if r.returncode != 0:
        pytest.skip("SDL2 not found (pkg-config sdl2 missing)")
    return r.stdout.split()


def _build_harness(tmp_path, name, extra):
    src = tmp_path / (name + ".c")
    src.write_text(_HARNESS, encoding="utf-8")
    exe = tmp_path / name
    subprocess.run(
        [
            "cc",
            "-O0",
            "-Wall",
            "-Werror",
            "-DMMB_PLATFORM_POSIX",
            "-I",
            os.path.join(REPO, "native"),
            "-I",
            os.path.join(REPO, "mmbasic", "include"),
            "-I",
            os.path.join(REPO, "mmbasic", "third_party"),
            "-I",
            os.path.join(REPO, "console"),
            *extra,
            *_sdl_flags(),
            "-o",
            str(exe),
            str(src),
            os.path.join(REPO, "native", "sdl_input.c"),
        ],
        check=True,
        cwd=REPO,
    )
    return exe


def _run(exe):
    env = dict(os.environ, SDL_VIDEODRIVER="dummy", MMB_SDL_NO_VT="1")
    proc = subprocess.run(
        [str(exe)], check=True, capture_output=True, text=True, env=env
    )
    assert "all checks passed" in proc.stdout, proc.stdout + proc.stderr


def test_framebuffer_build_consumes_vt_chord(tmp_path):
    """Ctrl+Alt+F1..F12 are handled by the framebuffer build, not delivered."""
    _run(_build_harness(tmp_path, "sdl_vt_fb", ["-DMMB_SDL_FRAMEBUFFER=1"]))


def test_desktop_build_leaves_the_chord_alone(tmp_path):
    """Without MMB_SDL_FRAMEBUFFER the Ctrl+Alt+F2 F-key mapping is unchanged."""
    _run(_build_harness(tmp_path, "sdl_vt_desktop", []))


def test_vt_code_is_guarded_and_uses_the_right_ioctls():
    """The VT ioctls live behind MMB_SDL_FRAMEBUFFER + Linux headers."""
    text = _read(os.path.join("native", "sdl_input.c"))
    assert "MMB_SDL_FRAMEBUFFER" in text
    assert "linux/vt.h" in text and "linux/kd.h" in text
    assert "VT_ACTIVATE" in text and "VT_GETSTATE" in text
    assert "KDSKBMODE" in text and "K_UNICODE" in text and "K_OFF" in text
    # SDL reacquires the VT through SDL_WINDOWEVENT_RESIZED, which must
    # re-mute the keyboard.
    assert "vt_sync" in text
    # A test/automation escape hatch exists so a host test never opens
    # /dev/tty0.
    assert "MMB_SDL_NO_VT" in text


def test_iso_build_installs_linux_headers():
    """<linux/vt.h>/<linux/kd.h> need Alpine's linux-headers package."""
    text = _read(os.path.join("scripts", "iso", "build-mmcore.sh"))
    assert "linux-headers" in text
