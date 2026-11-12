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
static int set_pixel_calls;
static int present_x, present_y, present_w, present_h;
static int sync_x = -1, sync_y = -1, sync_w = -1, sync_h = -1;
static uint16_t sync_pix[W * H];

mmb_mouse_state mock;
void mmb_error(const char *m) { (void)m; }
int mmb_mouse_read(mmb_mouse_state *o) { if (o) *o = mock; return mock.present; }
void mmb_upper(char *s) { (void)s; }

/* The frame profiler and sprite recorder are not under test; gfx.c only
 * calls these. */
void mmb_frame_prof_present_begin(void) {}
void mmb_frame_prof_present_end(void) {}
void mmb_frame_prof_wait_begin(void) {}
void mmb_frame_prof_wait_end(void) {}
int mmb_sprite_defer_pending;
void mmb_sprite_defer_flush(void) {}
void mmb_sprite_defer_flush_page(int page) { (void)page; }
void mmb_sprite_defer_invalidate_page(int page) { (void)page; }
void mmb_sprite_defer_discard(void) {}

static void *my_alloc(unsigned n) { return malloc(n); }
static void my_free(void *p) { free(p); }

static void rec_present(int x, int y, int w, int h, const void *p, int s)
{
	const uint16_t *src = p;
	int r, c;
	present_calls++;
	present_x = x; present_y = y; present_w = w; present_h = h;
	for (r = 0; r < h; r++)
		for (c = 0; c < w; c++) {
			int dx = x + c, dy = y + r;
			if (dx >= 0 && dx < W && dy >= 0 && dy < H)
				presented[dy * W + dx] = src[r * s + c];
		}
}
static void rec_sync_full(const void *p, int w, int h, int s)
{ (void)p; (void)w; (void)h; (void)s; sync_full_calls++; }
static void rec_set_pixel(int x, int y, unsigned rgb)
{ (void)x; (void)y; (void)rgb; set_pixel_calls++; }
static int native_calls, native_bad;
static unsigned native_expect;
static void rec_set_pixel_native(int x, int y, unsigned native)
{
	(void)x; (void)y;
	native_calls++;
	if (native != native_expect)
		native_bad++;
}
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
	present_calls = sync_full_calls = sync_rect_calls = set_pixel_calls = 0;
	present_x = present_y = present_w = present_h = -1;
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
	plat.set_pixel = rec_set_pixel;
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

	/* 4. A background console keeps drawing into its own page and dirty AABB
	 * but never writes the shared display: no present, no console-cache sync
	 * and no SetPixel (#1432). The deferred union uploads once the console
	 * becomes visible again. */
	plat.coalesce_presents = 0;
	plat.present_sync_console = rec_sync_full;
	plat.present_sync_console_rect = rec_sync_rect;
	g_cur->gfx.background = 1;
	g_cur->gfx.write_page = 0;
	g_cur->gfx.display_page = 0;
	g_cur->gfx.dirty = 0;
	reset_counts();
	{
		uint16_t before = pg[3 * W + 2];
		mmb_gfx_plot(2, 3, 0x00FF00);
		if (pg[3 * W + 2] == before) {
			printf("FAIL background_page_not_drawn\n");
			return 1;
		}
	}
	mmb_gfx_present_rect(0, 0, 4, 4);
	mmb_gfx_present_native(0, 0, 2, 2, pg, W);
	mmb_gfx_sync_console(0);
	mmb_gfx_present();
	if (present_calls != 0 || sync_rect_calls != 0 || sync_full_calls != 0 ||
	    set_pixel_calls != 0) {
		printf("FAIL background_wrote_display p=%d r=%d f=%d s=%d\n",
		       present_calls, sync_rect_calls, sync_full_calls,
		       set_pixel_calls);
		return 1;
	}
	if (!g_cur->gfx.dirty) {
		printf("FAIL background_lost_dirty\n");
		return 1;
	}
	printf("OK background_isolated\n");

	g_cur->gfx.background = 0;
	reset_counts();
	mmb_gfx_present();
	if (present_calls != 1) {
		printf("FAIL background_resume_present p=%d\n", present_calls);
		return 1;
	}
	printf("OK background_resume\n");

	/* 5. A native-pixel hook removes the colour round trip for live writes
	 * (#1435): plot and the span fast path pass the page pixel unchanged,
	 * and the RGB fallback still runs when the hook is absent. */
	g_cur->gfx.background = 0;
	g_cur->gfx.bits = 16;
	g_cur->gfx.write_page = 0;
	g_cur->gfx.display_page = 0;
	g_cur->gfx.dirty = 0;
	plat.set_pixel_native = rec_set_pixel_native;
	reset_counts();
	native_calls = native_bad = 0;
	native_expect = mmb_pix_store(0x336699);
	mmb_gfx_plot(5, 5, 0x336699);
	if (native_calls != 1 || native_bad != 0 || set_pixel_calls != 0) {
		printf("FAIL native_plot n=%d bad=%d s=%d\n",
		       native_calls, native_bad, set_pixel_calls);
		return 1;
	}
	native_calls = 0;
	mmb_gfx_line(10, 10, 13, 10, 0x336699, 1);
	if (native_calls != 4 || native_bad != 0 || set_pixel_calls != 0) {
		printf("FAIL native_span n=%d bad=%d s=%d\n",
		       native_calls, native_bad, set_pixel_calls);
		return 1;
	}
	plat.set_pixel_native = 0;
	reset_counts();
	native_calls = 0;
	mmb_gfx_plot(6, 6, 0x336699);
	if (set_pixel_calls != 1 || native_calls != 0) {
		printf("FAIL native_fallback s=%d n=%d\n",
		       set_pixel_calls, native_calls);
		return 1;
	}
	printf("OK native_pixel\n");

	/* 6. Delta PAGE COPY (#1436): a first copy is full, later copies upload
	 * only the source's damaged AABB, and an unchanged source skips both the
	 * copy and the present. */
	g_cur->gfx.background = 0;
	g_cur->gfx.bits = 16;
	g_cur->gfx.write_page = 1;
	g_cur->gfx.display_page = 0;
	g_cur->gfx.dirty = 0;
	g_cur->opt.fastdraw = 1;
	mmb_gfx_page_damage_invalidate();
	{
		uint16_t *p0, *p1;
		int pw, ph, j;

		p1 = mmb_gfx_buf_for(1, &pw, &ph);
		p0 = mmb_gfx_buf_for(0, &pw, &ph);
		for (j = 0; j < W * H; j++)
			p1[j] = (uint16_t)(j * 7 + 3);
		memset(p0, 0, sizeof(uint16_t) * W * H);
		reset_counts();
		if (!mmb_gfx_copy_page(1, 0, 0)) {
			printf("FAIL copy_first_skipped\n");
			return 1;
		}
		mmb_gfx_present_if(0);
		if (present_calls != 1 || present_x != 0 || present_y != 0 ||
		    present_w != W || present_h != H) {
			printf("FAIL copy_first %d %d,%d %dx%d\n", present_calls,
			       present_x, present_y, present_w, present_h);
			return 1;
		}
		g_cur->gfx.write_page = 1;
		g_cur->gfx.display_page = 0;
		g_cur->gfx.dirty = 0;
		mmb_gfx_plot(20, 10, 0x336699);
		mmb_gfx_plot(21, 10, 0x336699);
		reset_counts();
		if (!mmb_gfx_copy_page(1, 0, 0)) {
			printf("FAIL copy_delta_skipped\n");
			return 1;
		}
		mmb_gfx_present_if(0);
		if (present_calls != 1 || present_x != 20 || present_y != 10 ||
		    present_w != 2 || present_h != 1) {
			printf("FAIL copy_delta %d %d,%d %dx%d\n", present_calls,
			       present_x, present_y, present_w, present_h);
			return 1;
		}
		reset_counts();
		if (mmb_gfx_copy_page(1, 0, 0)) {
			printf("FAIL copy_static_copied\n");
			return 1;
		}
		if (present_calls != 0) {
			printf("FAIL copy_static_presented %d\n", present_calls);
			return 1;
		}
	}
	printf("OK page_copy_damage\n");

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
    for marker in ("OK rect_present", "OK coalesce_rect", "OK no_console",
                   "OK background_isolated", "OK background_resume",
                   "OK native_pixel", "OK page_copy_damage"):
        assert marker in sync_run.stdout, (marker, sync_run.stdout)
