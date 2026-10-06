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
import sys

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
    for name in ("pointer", "hand", "crosshair", "questionmark", "deny",
                 "hidden"):
        assert console.send_line("MOUSE CURSOR " + name) == "", name
    for n in ("0", "1", "2", "3", "4", "5"):
        assert console.send_line("MOUSE CURSOR " + n) == "", n
    # No argument is allowed and resets to the pointer.
    assert console.send_line("MOUSE CURSOR") == ""


def test_mouse_cursor_hidden_aliases(console):
    """#825: hidden accepts the name and its index; no sprite is drawn."""
    assert console.send_line("NEW") == ""
    for name in ("hidden", "none", "off", "5"):
        assert console.send_line("MOUSE CURSOR " + name) == "", name
    # 6 is past the last type now that hidden exists.
    out = console.send_line("MOUSE CURSOR 6")
    assert "SYNTAX" in out.upper() or "ERROR" in out.upper(), out


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


def test_on_mouse_down_up_parse(console):
    """#860: MOUSEDOWN aliases MOUSECLICK; MOUSEUP registers a release handler."""
    assert console.send_line("NEW") == ""
    assert console.send_line('ON MOUSECLICK "C"') == ""
    assert console.send_line('ON MOUSEDOWN "D"') == ""
    assert console.send_line('ON MOUSEUP "U"') == ""
    assert console.send_line("ON MOUSEDOWN DownH") == ""
    assert console.send_line("ON MOUSEUP UpH") == ""
    assert console.send_line("ON MOUSEDOWN") == ""
    assert console.send_line("ON MOUSEUP") == ""


def test_on_mouse_down_up_run(console):
    """#860: a program using all three handler names runs to completion."""
    assert console.send_line("NEW") == ""
    assert console.send_line("10 MOUSE ON") == ""
    assert console.send_line('20 ON MOUSEDOWN "DownH"') == ""
    assert console.send_line('30 ON MOUSEUP "UpH"') == ""
    assert console.send_line('40 ON MOUSECLICK "ClickH"') == ""
    assert console.send_line('50 PRINT "DONE"') == ""
    assert console.send_line("60 END") == ""
    assert console.send_line("100 SUB DownH(x,y,b$)") == ""
    assert console.send_line("110 PRINT x;y;b$") == ""
    assert console.send_line("120 END SUB") == ""
    assert console.send_line("130 SUB UpH(x,y,b$)") == ""
    assert console.send_line("140 PRINT x;y;b$") == ""
    assert console.send_line("150 END SUB") == ""
    assert console.send_line("160 SUB ClickH(x,y,b$)") == ""
    assert console.send_line("170 PRINT x;y;b$") == ""
    assert console.send_line("180 END SUB") == ""
    assert console.send_line("RUN") == "DONE"


# ---- help docs ------------------------------------------------------------


def test_mouse_help_doc_covers_surface():
    path = os.path.join(REPO, "docs", "help", "mouse.txt")
    text = open(path, encoding="utf-8").read()
    assert "name: MOUSE" in text
    for token in ("MOUSE ON", "MOUSE OFF", "MOUSE CURSOR", "pointer", "hand",
                  "crosshair", "questionmark", "deny", "hidden"):
        assert token in text, token
    assert "ON MOUSEUP" in text
    assert "ON MOUSEDOWN" in text
    on = open(os.path.join(REPO, "docs", "help", "on.txt"), encoding="utf-8").read()
    assert "ON MOUSECLICK" in on
    assert "ON MOUSEMOVE" in on
    assert "ON MOUSEUP" in on
    assert "ON MOUSEDOWN" in on


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
int mmb_mouse_cursor_type_count(void);
const char *mmb_mouse_cursor_type_name(int type);
void mmb_mouse_cursor_set_on(int on);
void mmb_mouse_cursor_set_type(int type);
void mmb_mouse_cursor_refresh(void);
void mmb_mouse_cursor_present(const uint16_t *pg, int w, int h);
void mmb_mouse_cursor_reset_all(void);
int mmb_mouse_cursor_art(int type, int *w, int *h, int *hot_x, int *hot_y,
			 const char *const **rows);
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

/* Drain take_event into a compact string, e.g. "m10,10 c1 u1". */
static void drain(char *out, size_t cap)
{
	int x, y, b, ev;
	size_t n = 0;
	out[0] = 0;
	while ((ev = mmb_mouse_take_event(&x, &y, &b)) != 0) {
		if (ev == 1)
			n += (size_t)snprintf(out + n, cap - n, " m%d,%d", x, y);
		else if (ev == 3)
			n += (size_t)snprintf(out + n, cap - n, " u%d", b);
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

	/* Every visible icon renders at its hotspot -- so the hotspot is an
	 * opaque pixel for every s_art entry (#824) -- and leaves the page
	 * alone. Hidden (#825) composites nothing. */
	for (t = 0; t < 5; t++) {
		mmb_mouse_cursor_reset_all();
		memset(presented, 0, sizeof(presented));
		mock.x = 150;
		mock.y = 100;
		mmb_mouse_cursor_set_on(1);
		mmb_mouse_cursor_set_type(t);
		mmb_mouse_cursor_present(page, W, H);
		check(cursor_drawn(150, 100, 16, 16), "icon_drawn");
		check(presented[100 * W + 150] != page[100 * W + 150],
		      "hotspot_opaque");
	}
	check(page_unchanged(), "page_clean_all_icons");
	printf("OK all_icons\n");

	/* Hidden (#825): no sprite, but the pointer still tracks. */
	mmb_mouse_cursor_reset_all();
	mock.present = 1;
	mock.x = 150;
	mock.y = 100;
	mock.buttons = 0;
	mmb_mouse_cursor_set_on(1);
	mmb_mouse_cursor_set_type(5);
	memcpy(presented, page, sizeof(presented));
	mmb_mouse_cursor_present(page, W, H);
	check(!cursor_drawn(120, 70, 60, 60), "hidden_not_drawn");
	check(page_unchanged(), "hidden_page_clean");
	mock.x = 180;
	mock.y = 130;
	drain(got, sizeof(got));
	check(!strcmp(got, " m180,130"), "hidden_tracks_move");
	printf("OK hidden\n");

	/* Icon name lookup. */
	check(mmb_mouse_cursor_type_count() == 6, "type_count");
	check(mmb_mouse_cursor_type_from_name("pointer") == 0, "name_pointer");
	check(mmb_mouse_cursor_type_from_name("HAND") == 1, "name_hand");
	check(mmb_mouse_cursor_type_from_name("crosshair") == 2, "name_cross");
	check(mmb_mouse_cursor_type_from_name("questionmark") == 3, "name_q");
	check(mmb_mouse_cursor_type_from_name("deny") == 4, "name_deny");
	check(mmb_mouse_cursor_type_from_name("hidden") == 5, "name_hidden");
	check(mmb_mouse_cursor_type_from_name("none") == 5, "name_none");
	check(mmb_mouse_cursor_type_from_name("bogus") == -1, "name_bad");
	check(!strcmp(mmb_mouse_cursor_type_name(2), "crosshair"), "tname");
	check(!strcmp(mmb_mouse_cursor_type_name(5), "hidden"), "tname_hidden");
	printf("OK icon_names\n");

	/* #845: every authored row is at least `w` characters and the geometry
	 * fits MMB_CURSOR_MAX_W/H, so present() can never read past a row's NUL.
	 * The hidden entry (type 5) carries no baked art. */
	{
		int art_ok = 1;
		for (t = 0; t < mmb_mouse_cursor_type_count(); t++) {
			int w = 0, h = 0, hx = 0, hy = 0, y;
			const char *const *rows = 0;
			if (!mmb_mouse_cursor_art(t, &w, &h, &hx, &hy, &rows)) {
				printf("FAIL art_lookup t=%d\n", t);
				art_ok = 0;
				continue;
			}
			/* Mirrors MMB_CURSOR_MAX_W/H in mouse_cursor.c. */
			if (w < 0 || w > 24 || h < 0 || h > 24) {
				printf("FAIL art_cap t=%d w=%d h=%d\n", t, w, h);
				art_ok = 0;
			}
			if (w == 0 || h == 0)
				continue;
			if (hx < 0 || hx >= w || hy < 0 || hy >= h) {
				printf("FAIL art_hotspot t=%d\n", t);
				art_ok = 0;
			}
			for (y = 0; y < h; y++)
				if (rows[y] == 0 ||
				    (int)strlen(rows[y]) < w) {
					printf("FAIL art_row_width t=%d y=%d\n",
					       t, y);
					art_ok = 0;
				}
		}
		if (art_ok)
			printf("OK art_bounds\n");
		else
			fails++;
	}

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
	check(!strcmp(got, " u1"), "left_release");
	mock.buttons = 3;
	drain(got, sizeof(got));
	check(!strcmp(got, " c1 c2"), "two_buttons");
	mock.buttons = 0;
	drain(got, sizeof(got));
	check(!strcmp(got, " u1 u2"), "two_releases");
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
                   "OK all_icons", "OK hidden", "OK icon_names",
                   "OK art_bounds", "OK event_classify"):
        assert marker in host_run.stdout, (marker, host_run.stdout)


# ---- host driver: console-cache reconciliation (#1088) --------------------

# QEMU cannot inject a pointer, so the restamp cannot be driven end to end.
# Compile the real gfx.c + mouse_cursor.c with a recording platform instead:
# an overlay present must reconcile the console cache with the pixels it put
# on the framebuffer, and a page sync must re-apply the cursor so a later
# console row flush cannot re-stamp the page over a stationary pointer.
RECONCILE_DRIVER = r"""
#include "mmb_priv.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

mmb s_mmb;
mmb *g_cur = &s_mmb;
int g_console;
mmb_audio g_audio;

#define W 64
#define H 48
static uint16_t page[W * H];
static uint16_t presented[W * H];
static int present_calls, sync_full_calls, sync_rect_calls;
static int sync_rect_page, sync_rect_cursor;
static int sync_x, sync_y, sync_w, sync_h;
static uint16_t sync_pix[W * H];

mmb_mouse_state mock;
void mmb_error(const char *m) { (void)m; }
int mmb_mouse_read(mmb_mouse_state *o) { if (o) *o = mock; return mock.present; }
void mmb_upper(char *s) { (void)s; }

static void *my_alloc(unsigned n) { return malloc(n); }
static void my_free(void *p) { free(p); }

static void rec_present(int x, int y, int w, int h, const void *p, int s)
{
	const uint16_t *src = p;
	int r, c;
	present_calls++;
	for (r = 0; r < h; r++)
		for (c = 0; c < w; c++) {
			int dx = x + c, dy = y + r;
			if (dx >= 0 && dx < W && dy >= 0 && dy < H)
				presented[dy * W + dx] = src[r * s + c];
		}
}
static void rec_sync_full(const void *p, int w, int h, int s)
{ (void)p; (void)w; (void)h; (void)s; sync_full_calls++; }
static void rec_sync_rect(int x, int y, int w, int h, const void *p, int s)
{
	const uint16_t *src = p;
	int r, c, diff = 0;
	sync_rect_calls++;
	if (w > 0 && h > 0 && w * h <= (int)(sizeof sync_pix / sizeof sync_pix[0]))
		memcpy(sync_pix, src, (size_t)w * (size_t)h * sizeof(uint16_t));
	sync_x = x; sync_y = y; sync_w = w; sync_h = h;
	for (r = 0; r < h; r++)
		for (c = 0; c < w; c++)
			if (src[r * s + c] != page[(y + r) * W + (x + c)])
				diff = 1;
	if (diff)
		sync_rect_cursor = 1;
	else
		sync_rect_page = 1;
}

int main(void)
{
	static mmb_platform plat;
	uint16_t *pg;
	int x, y;
	g_cur->plat = &plat;
	plat.present_native = rec_present;
	plat.present_sync_console = rec_sync_full;
	plat.present_sync_console_rect = rec_sync_rect;
	plat.alloc = my_alloc;
	plat.free = my_free;
	g_cur->running = 1;
	g_cur->gfx.w = W;
	g_cur->gfx.h = H;
	g_cur->gfx.display_page = 0;
	g_cur->gfx.pages = MMB_MAX_PAGES;
	for (y = 0; y < H; y++)
		for (x = 0; x < W; x++)
			page[y * W + x] = (uint16_t)(x * 3 + y * 5 + 1);
	pg = mmb_gfx_buf_for(0, &x, &y);
	memcpy(pg, page, sizeof page);

	/* 1. An overlay present reconciles the console cache with the exact
	 * pixels it wrote to the framebuffer, same as present-rect (#1079). */
	{
		static uint16_t sprite[4 * 4];
		int i;
		for (i = 0; i < 16; i++)
			sprite[i] = 0xABCD;
		present_calls = sync_rect_calls = 0;
		mmb_gfx_present_native(4, 5, 4, 4, sprite, 4);
		if (present_calls != 1 || sync_rect_calls != 1 ||
		    sync_x != 4 || sync_y != 5 || sync_w != 4 || sync_h != 4 ||
		    memcmp(sync_pix, sprite, sizeof sprite) != 0) {
			printf("FAIL reconcile_present p=%d s=%d\n",
			       present_calls, sync_rect_calls);
			return 1;
		}
		printf("OK reconcile_present\n");
	}

	/* 2. After a page sync the cursor is re-applied: the erase syncs the
	 * page pixels, the draw syncs the composited sprite, so a subsequent
	 * console row flush reproduces the stationary cursor (#1088). */
	mock.present = 1;
	mock.x = 10;
	mock.y = 10;
	mock.buttons = 0;
	mmb_mouse_cursor_reset_all();
	mmb_mouse_cursor_set_on(1);
	present_calls = sync_full_calls = sync_rect_calls = 0;
	sync_rect_page = sync_rect_cursor = 0;
	g_cur->gfx.dirty = 0;
	mmb_gfx_sync_console(0);
	if (sync_full_calls < 1 || present_calls < 2 ||
	    !sync_rect_page || !sync_rect_cursor) {
		printf("FAIL reconcile_sync full=%d p=%d page=%d cursor=%d\n",
		       sync_full_calls, present_calls, sync_rect_page,
		       sync_rect_cursor);
		return 1;
	}
	printf("OK reconcile_sync\n");

	printf("ALL OK\n");
	return 0;
}
"""


@pytest.fixture(scope="module")
def reconcile_run(tmp_path_factory):
    if shutil.which("cc") is None:
        pytest.skip("needs a host C toolchain (cc)")
    tmp = tmp_path_factory.mktemp("gfx_reconcile")
    (tmp / "driver.c").write_text(RECONCILE_DRIVER)
    exe = tmp / "driver"
    # -ffunction-sections/-fdata-sections + gc-sections let the driver link
    # only mmb_gfx_present_native()/mmb_gfx_sync_console() and their callees;
    # the rest of gfx.c is dropped instead of needing stubs.
    gc = ["-Wl,-dead_strip"] if sys.platform == "darwin" else ["-Wl,--gc-sections"]
    subprocess.run(
        [
            "cc", "-std=c11", "-O0", "-Wall", "-Wextra", "-Werror",
            "-DMMB_PLATFORM_POSIX", "-ffunction-sections", "-fdata-sections",
            "-I", os.path.join(REPO, "mmbasic", "include"),
            "-I", SRC,
            *gc,
            "-o", str(exe),
            str(tmp / "driver.c"),
            os.path.join(SRC, "gfx.c"),
            MOUSE_C,
        ],
        check=True,
        cwd=REPO,
    )
    return subprocess.run([str(exe)], check=False, capture_output=True, text=True)


def test_host_console_sync_reconciles_overlay(reconcile_run):
    out = reconcile_run.stdout + reconcile_run.stderr
    assert reconcile_run.returncode == 0, out
    assert "ALL OK" in out, out
    assert "FAIL" not in out, out


def test_host_console_sync_markers(reconcile_run):
    for marker in ("OK reconcile_present", "OK reconcile_sync"):
        assert marker in reconcile_run.stdout, (marker, reconcile_run.stdout)
