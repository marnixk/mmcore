"""Host driver: a partial BLIT present reconciles only the presented rect.

QEMU observes the console-cache restamp behaviour end to end in
``test_mmbasic.py`` (#1068/#1070/#1072), but it cannot tell a full-page
reconcile from a rect reconcile. Compile the real ``gfx.c`` against a
recording platform and assert the BLIT present/flush path calls the rect hook
with the dirty AABB instead of scanning the whole display page (#1403).
"""

import os
import shutil
import subprocess
import sys

import pytest

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(REPO, "mmbasic", "src")
MOUSE_C = os.path.join(SRC, "mouse_cursor.c")

SYNC_DRIVER = r"""
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
static int sync_x = -1, sync_y = -1, sync_w = -1, sync_h = -1;
static uint16_t sync_pix[W * H];

mmb_mouse_state mock;
void mmb_error(const char *m) { (void)m; }
int mmb_mouse_read(mmb_mouse_state *o) { if (o) *o = mock; return mock.present; }
void mmb_upper(char *s) { (void)s; }

/* The frame profiler is not under test; gfx.c only calls these. */
void mmb_frame_prof_present_begin(void) {}
void mmb_frame_prof_present_end(void) {}
void mmb_frame_prof_wait_begin(void) {}
void mmb_frame_prof_wait_end(void) {}

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
	int r, c;
	sync_rect_calls++;
	sync_x = x; sync_y = y; sync_w = w; sync_h = h;
	for (r = 0; r < h; r++)
		for (c = 0; c < w; c++)
			if (x + c >= 0 && x + c < W && y + r >= 0 && y + r < H)
				sync_pix[(y + r) * W + (x + c)] = src[r * s + c];
}

static void reset_counts(void)
{
	present_calls = sync_full_calls = sync_rect_calls = 0;
	sync_x = sync_y = sync_w = sync_h = -1;
}

static int rect_matches(int x, int y, int w, int h)
{
	int r, c;
	if (sync_x != x || sync_y != y || sync_w != w || sync_h != h)
		return 0;
	for (r = 0; r < h; r++)
		for (c = 0; c < w; c++)
			if (sync_pix[(y + r) * W + (x + c)] != page[(y + r) * W + (x + c)])
				return 0;
	return 1;
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
	g_cur->gfx.write_page = 0;
	g_cur->gfx.pages = MMB_MAX_PAGES;
	for (y = 0; y < H; y++)
		for (x = 0; x < W; x++)
			page[y * W + x] = (uint16_t)(x * 3 + y * 5 + 1);
	pg = mmb_gfx_buf_for(0, &x, &y);
	memcpy(pg, page, sizeof page);

	/* 1. Immediate BLIT present: one AABB present, one rect reconcile, no
	 * full-page scan. */
	reset_counts();
	mmb_gfx_dirty_add(4, 6, 12, 10);
	mmb_gfx_dirty_flush();
	if (present_calls != 1 || sync_rect_calls != 1 || sync_full_calls != 0 ||
	    !rect_matches(4, 6, 12, 10)) {
		printf("FAIL rect_present p=%d r=%d f=%d %d,%d %dx%d\n",
		       present_calls, sync_rect_calls, sync_full_calls,
		       sync_x, sync_y, sync_w, sync_h);
		return 1;
	}
	printf("OK rect_present\n");

	/* 2. Coalesced (#1402) BLITs: the deferred flush presents the union AABB
	 * once and reconciles only that union, never the whole page. */
	g_cur->gfx.dirty = 0;
	plat.coalesce_presents = 1;
	reset_counts();
	mmb_gfx_dirty_add(10, 10, 8, 8);
	mmb_gfx_dirty_flush();
	if (present_calls != 0 || sync_rect_calls != 0) {
		printf("FAIL coalesce_defer p=%d r=%d\n",
		       present_calls, sync_rect_calls);
		return 1;
	}
	mmb_gfx_dirty_add(30, 20, 8, 8);
	mmb_gfx_flush_pending();
	if (present_calls != 1 || sync_rect_calls != 1 || sync_full_calls != 0 ||
	    !rect_matches(10, 10, 28, 18)) {
		printf("FAIL coalesce_flush p=%d r=%d f=%d %d,%d %dx%d\n",
		       present_calls, sync_rect_calls, sync_full_calls,
		       sync_x, sync_y, sync_w, sync_h);
		return 1;
	}
	printf("OK coalesce_rect\n");

	/* 3. No console hook: still presents, and never invents a sync. */
	plat.coalesce_presents = 0;
	plat.present_sync_console = 0;
	plat.present_sync_console_rect = 0;
	g_cur->gfx.dirty = 0;
	reset_counts();
	mmb_gfx_dirty_add(0, 0, 4, 4);
	mmb_gfx_dirty_flush();
	if (present_calls != 1 || sync_rect_calls != 0 || sync_full_calls != 0) {
		printf("FAIL no_console p=%d r=%d f=%d\n",
		       present_calls, sync_rect_calls, sync_full_calls);
		return 1;
	}
	printf("OK no_console\n");

	printf("ALL OK\n");
	return 0;
}
"""


@pytest.fixture(scope="module")
def sync_run(tmp_path_factory):
    if shutil.which("cc") is None:
        pytest.skip("needs a host C toolchain (cc)")
    tmp = tmp_path_factory.mktemp("gfx_console_sync")
    (tmp / "driver.c").write_text(SYNC_DRIVER)
    exe = tmp / "driver"
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


def test_partial_blit_syncs_only_presented_rect(sync_run):
    out = sync_run.stdout + sync_run.stderr
    assert sync_run.returncode == 0, out
    assert "ALL OK" in out, out
    assert "FAIL" not in out, out


def test_partial_blit_sync_markers(sync_run):
    for marker in ("OK rect_present", "OK coalesce_rect", "OK no_console"):
        assert marker in sync_run.stdout, (marker, sync_run.stdout)
