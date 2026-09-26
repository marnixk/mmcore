"""MOUSE command / software cursor (#792).

Two layers:

* QEMU console tests for the BASIC surface: MOUSE ON/OFF, MOUSE CURSOR and the
  ON MOUSECLICK / ON MOUSEMOVE registration.
* A host C driver for the parts that need a pointer: the non-dirtying overlay
  (a move must leave PAGE pixels byte-identical), the icon set, and the
  click/move event classifier that core.c feeds to the handlers.

The QEMU half reuses the module-scoped ``console`` fixture (see conftest).
The host half compiles mmbasic/src/mouse_cursor.c against a small shim, in the
same spirit as tests/test_paint_cursors.py.
"""
import os
import shutil
import subprocess

import pytest

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(REPO, "mmbasic", "src")
MOUSE_C = os.path.join(SRC, "mouse_cursor.c")


# ---- BASIC surface (QEMU) -------------------------------------------------


def test_mouse_on_off_parse(console):
    assert console.send_line("NEW") == ""
    assert console.send_line("MOUSE ON") == ""
    assert console.send_line("MOUSE OFF") == ""


def test_mouse_cursor_types_parse(console):
    assert console.send_line("NEW") == ""
    for name in ("pointer", "hand", "crosshair", "questionmark", "deny"):
        assert console.send_line("MOUSE CURSOR " + name) == "", name
    for n in ("0", "1", "2", "3", "4"):
        assert console.send_line("MOUSE CURSOR " + n) == "", n
    # No argument is allowed and resets to the pointer.
    assert console.send_line("MOUSE CURSOR") == ""


def test_mouse_cursor_invalid_type(console):
    assert console.send_line("NEW") == ""
    out = console.send_line("MOUSE CURSOR bogus")
    assert "SYNTAX" in out.upper() or "ERROR" in out.upper(), out
    out = console.send_line("MOUSE CURSOR 9")
    assert "SYNTAX" in out.upper() or "ERROR" in out.upper(), out


def test_mouse_unknown_subcommand(console):
    assert console.send_line("NEW") == ""
    out = console.send_line("MOUSE WOBBLE")
    assert "SYNTAX" in out.upper() or "ERROR" in out.upper(), out


def test_on_mouse_handlers_run(console):
    """Handlers register and a program using them runs to completion."""
    assert console.send_line("NEW") == ""
    assert console.send_line("10 MOUSE ON") == ""
    assert console.send_line('20 ON MOUSEMOVE "MoveH"') == ""
    assert console.send_line('30 ON MOUSECLICK "ClickH"') == ""
    assert console.send_line("40 MOUSE CURSOR crosshair") == ""
    assert console.send_line('50 PRINT "DONE"') == ""
    assert console.send_line("60 END") == ""
    assert console.send_line("100 SUB MoveH(x,y)") == ""
    assert console.send_line("110 PRINT x;y") == ""
    assert console.send_line("120 END SUB") == ""
    assert console.send_line("130 SUB ClickH(x,y,b$)") == ""
    assert console.send_line("140 PRINT x;y;b$") == ""
    assert console.send_line("150 END SUB") == ""
    assert console.send_line("RUN") == "DONE"


def test_on_mouse_clears_without_name(console):
    assert console.send_line("NEW") == ""
    assert console.send_line('ON MOUSEMOVE "MoveH"') == ""
    assert console.send_line("ON MOUSEMOVE") == ""
    assert console.send_line('ON MOUSECLICK "ClickH"') == ""
    assert console.send_line("ON MOUSECLICK") == ""


# ---- help docs ------------------------------------------------------------


def test_mouse_help_doc_covers_surface():
    path = os.path.join(REPO, "docs", "help", "mouse.txt")
    text = open(path, encoding="utf-8").read()
    assert "name: MOUSE" in text
    for token in ("MOUSE ON", "MOUSE OFF", "MOUSE CURSOR", "pointer", "hand",
                  "crosshair", "questionmark", "deny"):
        assert token in text, token
    on = open(os.path.join(REPO, "docs", "help", "on.txt"), encoding="utf-8").read()
    assert "ON MOUSECLICK" in on
    assert "ON MOUSEMOVE" in on


# ---- host driver: overlay + event classifier ------------------------------

SHIM_H = r"""
#ifndef MMB_PRIV_H
#define MMB_PRIV_H
#include <stddef.h>
#include <stdint.h>
#define MMB_MAX_CONSOLES 4
#define MMB_MAX_NAME 64
extern int g_console;
typedef struct mmb_mouse_state {
	int present;
	int x, y;
	int buttons;
	int wheel;
} mmb_mouse_state;
typedef struct { int w, h; } mmb_gfx_shim;
typedef struct {
	int running;
	int tick_busy;
	mmb_gfx_shim gfx;
} mmb_global_shim;
extern mmb_global_shim G;
void mmb_upper(char *s);
int mmb_mouse_read(mmb_mouse_state *out);
unsigned mmb_rgb_to_native(unsigned rgb888);
void mmb_gfx_dirty_add(int x, int y, int w, int h);
void mmb_gfx_present(void);
void mmb_gfx_present_native(int x, int y, int w, int h,
			    const uint16_t *pix, int stride);
int mmb_mouse_cursor_type_from_name(const char *name);
const char *mmb_mouse_cursor_type_name(int type);
void mmb_mouse_cursor_set_on(int on);
void mmb_mouse_cursor_set_type(int type);
void mmb_mouse_cursor_refresh(void);
void mmb_mouse_cursor_present(const uint16_t *pg, int w, int h);
void mmb_mouse_cursor_reset_all(void);
int mmb_mouse_take_event(int *x, int *y, int *button);
#endif
"""

DRIVER = r"""
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "mmb_priv.h"

#define W 320
#define H 200

static uint16_t page[W * H];
static uint16_t snap[W * H];
static uint16_t presented[W * H];
static int dirty_calls, present_calls;
static mmb_mouse_state mock;

mmb_global_shim G;
int g_console = 0;

void mmb_upper(char *s)
{
	for (; *s; s++)
		if (*s >= 'a' && *s <= 'z')
			*s = (char)(*s - 32);
}

int mmb_mouse_read(mmb_mouse_state *out)
{
	if (out)
		*out = mock;
	return mock.present;
}

unsigned mmb_rgb_to_native(unsigned rgb888)
{
	if (rgb888 == 0xFFFFFFu)
		return 0xFFFFu;
	if (rgb888 == 0x000000u)
		return 0x0000u;
	return rgb888 & 0xFFFFu;
}

void mmb_gfx_dirty_add(int x, int y, int w, int h)
{
	(void)x; (void)y; (void)w; (void)h;
	dirty_calls++;
}

void mmb_gfx_present(void) { present_calls++; }

void mmb_gfx_present_native(int x, int y, int w, int h,
			    const uint16_t *pix, int stride)
{
	int r, c;
	for (r = 0; r < h; r++)
		for (c = 0; c < w; c++) {
			int dx = x + c, dy = y + r;
			if (dx >= 0 && dx < W && dy >= 0 && dy < H)
				presented[dy * W + dx] = pix[r * stride + c];
		}
}

static uint16_t pat(int x, int y)
{
	unsigned v = (unsigned)(x * 7 + y * 13) % 611u;
	return (uint16_t)(v + 1u); /* never 0x0000 or 0xFFFF */
}

static void fill_page(void)
{
	int x, y;
	for (y = 0; y < H; y++)
		for (x = 0; x < W; x++)
			page[y * W + x] = pat(x, y);
}

static int page_unchanged(void)
{
	return memcmp(page, snap, sizeof(page)) == 0;
}

static int snapshot_restored(int x0, int y0, int w, int h)
{
	int x, y;
	for (y = y0; y < y0 + h; y++)
		for (x = x0; x < x0 + w; x++) {
			if (x < 0 || y < 0 || x >= W || y >= H)
				continue;
			if (presented[y * W + x] != page[y * W + x])
				return 0;
		}
	return 1;
}

static int cursor_drawn(int x0, int y0, int w, int h)
{
	int x, y;
	for (y = y0; y < y0 + h; y++)
		for (x = x0; x < x0 + w; x++) {
			if (x < 0 || y < 0 || x >= W || y >= H)
				continue;
			if (presented[y * W + x] != page[y * W + x])
				return 1;
		}
	return 0;
}

/* Drain take_event into a compact string, e.g. "m10,10 c1 c2". */
static void drain(char *out, size_t cap)
{
	int x, y, b, ev;
	size_t n = 0;
	out[0] = 0;
	while ((ev = mmb_mouse_take_event(&x, &y, &b)) != 0) {
		if (ev == 1)
			n += (size_t)snprintf(out + n, cap - n, " m%d,%d", x, y);
		else
			n += (size_t)snprintf(out + n, cap - n, " c%d", b);
	}
}

static int fails;

static void check(int cond, const char *what)
{
	if (!cond) {
		printf("FAIL %s\n", what);
		fails++;
	}
}

int main(void)
{
	char got[128];
	int t;

	fill_page();
	memcpy(snap, page, sizeof(page));
	memset(presented, 0, sizeof(presented));
	G.running = 1;
	G.tick_busy = 0;
	G.gfx.w = W;
	G.gfx.h = H;

	/* Overlay: drawing the cursor must not touch the PAGE buffer. */
	mock.present = 1;
	mock.x = 100;
	mock.y = 80;
	mock.buttons = 0;
	mmb_mouse_cursor_reset_all();
	mmb_mouse_cursor_set_on(1);
	memset(presented, 0, sizeof(presented));
	mmb_mouse_cursor_present(page, W, H);
	check(page_unchanged(), "page_clean_after_draw");
	check(cursor_drawn(100, 80, 12, 16), "cursor_visible");
	printf("OK page_clean_after_draw\n");

	/* Move: old footprint is restored from the composed frame, the new one
	 * is drawn, and the PAGE buffer still has not changed. */
	mock.x = 200;
	mock.y = 120;
	mmb_mouse_cursor_present(page, W, H);
	check(page_unchanged(), "page_clean_after_move");
	check(snapshot_restored(100, 80, 12, 16), "old_footprint_restored");
	check(cursor_drawn(200, 120, 12, 16), "cursor_moved");
	printf("OK non_dirtying_move\n");

	/* Every icon renders and leaves the page alone. */
	for (t = 0; t < 5; t++) {
		mmb_mouse_cursor_reset_all();
		memset(presented, 0, sizeof(presented));
		mock.x = 150;
		mock.y = 100;
		mmb_mouse_cursor_set_on(1);
		mmb_mouse_cursor_set_type(t);
		mmb_mouse_cursor_present(page, W, H);
		check(cursor_drawn(150, 100, 16, 16), "icon_drawn");
	}
	check(page_unchanged(), "page_clean_all_icons");
	printf("OK all_icons\n");

	/* Icon name lookup. */
	check(mmb_mouse_cursor_type_from_name("pointer") == 0, "name_pointer");
	check(mmb_mouse_cursor_type_from_name("HAND") == 1, "name_hand");
	check(mmb_mouse_cursor_type_from_name("crosshair") == 2, "name_cross");
	check(mmb_mouse_cursor_type_from_name("questionmark") == 3, "name_q");
	check(mmb_mouse_cursor_type_from_name("deny") == 4, "name_deny");
	check(mmb_mouse_cursor_type_from_name("bogus") == -1, "name_bad");
	check(!strcmp(mmb_mouse_cursor_type_name(2), "crosshair"), "tname");
	printf("OK icon_names\n");

	/* Event classification: a move is reported once, then click edges for
	 * each newly pressed button. */
	mmb_mouse_cursor_reset_all();
	mock.present = 1;
	mock.buttons = 0;
	mock.x = 10;
	mock.y = 10;
	drain(got, sizeof(got));
	check(!strcmp(got, " m10,10"), "first_move");
	mock.x = 20;
	mock.y = 20;
	drain(got, sizeof(got));
	check(!strcmp(got, " m20,20"), "second_move");
	mock.buttons = 1;
	drain(got, sizeof(got));
	check(!strcmp(got, " c1"), "left_click");
	mock.buttons = 0;
	drain(got, sizeof(got));
	check(got[0] == 0, "release_is_quiet");
	mock.buttons = 3;
	drain(got, sizeof(got));
	check(!strcmp(got, " c1 c2"), "two_buttons");
	mock.buttons = 0;
	drain(got, sizeof(got));
	check(got[0] == 0, "no_release_event");
	G.tick_busy = 1;
	drain(got, sizeof(got));
	check(got[0] == 0, "tick_busy_suppressed");
	G.tick_busy = 0;
	mock.present = 0;
	drain(got, sizeof(got));
	check(got[0] == 0, "no_mouse_no_event");
	printf("OK event_classify\n");

	printf("%s\n", fails ? "FAILURES" : "ALL OK");
	return fails ? 1 : 0;
}
"""


@pytest.fixture(scope="module")
def host_run(tmp_path_factory):
    if shutil.which("cc") is None:
        pytest.skip("needs a host C toolchain (cc)")
    tmp = tmp_path_factory.mktemp("mouse_cursor")
    (tmp / "mmb_priv.h").write_text(SHIM_H)
    (tmp / "driver.c").write_text(DRIVER)
    exe = tmp / "driver"
    subprocess.run(
        [
            "cc", "-std=c11", "-O0", "-Wall", "-Wextra", "-Werror",
            "-I", str(tmp), "-I", SRC,
            "-I", os.path.join(REPO, "mmbasic", "include"),
            "-o", str(exe),
            str(tmp / "driver.c"), MOUSE_C,
        ],
        check=True,
        cwd=REPO,
    )
    return subprocess.run([str(exe)], check=False, capture_output=True, text=True)


def test_host_overlay_all_checks_pass(host_run):
    out = host_run.stdout + host_run.stderr
    assert host_run.returncode == 0, out
    assert "ALL OK" in out, out
    assert "FAIL" not in out, out


def test_host_markers(host_run):
    for marker in ("OK page_clean_after_draw", "OK non_dirtying_move",
                   "OK all_icons", "OK icon_names", "OK event_classify"):
        assert marker in host_run.stdout, (marker, host_run.stdout)
