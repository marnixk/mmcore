"""PAINT undo/redo history (#637).

The undo module records one history entry per committed edit (a stroke, shape
or fill), keeps at most eight of them, and moves snapshots between the undo and
redo stacks. This test compiles the real ``mmbasic/src/paint_undo.c`` against a
tiny host shim (an ``mmb_priv.h`` that supplies ``G.plat`` and
``MMB_UNDO_DEPTH``) with a C driver that performs edits, undoes and redoes them,
and pixel-compares the canvas against the expected states. It also exercises the
ninth-edit eviction, redo invalidation and the memory ceiling.
"""
import os
import shutil
import subprocess

import pytest

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(REPO, "mmbasic", "src")
UNDO_C = os.path.join(SRC, "paint_undo.c")

SHIM = r"""
#ifndef PT_UNDO_TEST_MMB_PRIV_H
#define PT_UNDO_TEST_MMB_PRIV_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define MMB_UNDO_DEPTH 8

typedef struct mmb_platform {
	void *(*alloc)(unsigned n);
	void (*free)(void *p);
} mmb_platform;

typedef struct mmb_test_globals {
	const mmb_platform *plat;
} mmb_test_globals;

extern mmb_test_globals G;

#endif
"""

DRIVER = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "paint.h"

pt_state PT;

static mmb_platform s_plat;
mmb_test_globals G;

static void *host_alloc(unsigned n) { return malloc(n); }
static void host_free(void *p) { free(p); }

/* Strong stand-in for cmd_paint.c's redraw request. */
void pt_request_redraw(void) { }

/* Introspection hooks implemented by paint_undo.c. */
unsigned pt_undo_mem(void);
unsigned pt_undo_budget(void);

static int fails;

static void fail(const char *msg) { printf("FAIL: %s\n", msg); fails++; }

static void canvas_new(int w, int h)
{
	PT.width = w;
	PT.height = h;
	PT.canvas = malloc((size_t)w * h);
	if (!PT.canvas)
	{
		printf("FAIL: canvas alloc\n");
		exit(2);
	}
	memset(PT.canvas, 0, (size_t)w * h);
	pt_undo_init();
}

/* One committed edit: snapshot first, then paint. */
static void edit(int idx, int val)
{
	pt_undo_push();
	PT.canvas[idx] = (unsigned char)val;
}

static void test_eight_steps(void)
{
	unsigned char S[9][256];
	int i;

	canvas_new(16, 16);
	memcpy(S[0], PT.canvas, 256);
	for (i = 1; i <= 8; i++)
	{
		edit(i, i * 7);
		memcpy(S[i], PT.canvas, 256);
	}
	if (PT.undo_depth != 8)
		fail("undo_depth is not 8 after eight edits");

	for (i = 7; i >= 0; i--)
	{
		pt_undo();
		if (memcmp(PT.canvas, S[i], 256) != 0)
			fail("undo did not restore the exact canvas");
	}
	if (PT.undo_depth != 0 || PT.redo_depth != 8)
		fail("depths wrong after undoing everything");

	for (i = 1; i <= 8; i++)
	{
		pt_redo();
		if (memcmp(PT.canvas, S[i], 256) != 0)
			fail("redo did not restore the exact canvas");
	}
	if (PT.undo_depth != 8 || PT.redo_depth != 0)
		fail("depths wrong after redoing everything");

	free(PT.canvas);
}

static void test_ninth_drops_oldest(void)
{
	unsigned char S[10][256];
	int i;

	canvas_new(16, 16);
	memcpy(S[0], PT.canvas, 256);
	for (i = 1; i <= 9; i++)
	{
		edit(i, i * 3);
		memcpy(S[i], PT.canvas, 256);
	}
	if (PT.undo_depth != 8)
		fail("history is not capped at eight after a ninth edit");

	for (i = 0; i < 8; i++)
		pt_undo();
	if (memcmp(PT.canvas, S[1], 256) != 0)
		fail("ninth edit did not drop only the oldest entry");
	if (PT.undo_depth != 0)
		fail("undo stack not empty after eight undos");

	free(PT.canvas);
}

static void test_redo_cleared_on_edit(void)
{
	canvas_new(16, 16);
	edit(0, 10);
	edit(1, 20);
	edit(2, 30);
	pt_undo();
	if (PT.redo_depth != 1)
		fail("undo did not populate the redo stack");

	edit(3, 40);
	if (PT.redo_depth != 0)
		fail("a new edit did not clear the redo stack");

	pt_redo();
	if (strstr(PT.status, "Nothing to redo") == NULL)
		fail("redo after a new edit did not report an empty future");

	free(PT.canvas);
}

static void test_empty_messages(void)
{
	canvas_new(8, 8);
	pt_undo();
	if (strstr(PT.status, "Nothing to undo") == NULL)
		fail("undo on an empty history did not report it");
	pt_redo();
	if (strstr(PT.status, "Nothing to redo") == NULL)
		fail("redo on an empty future did not report it");
	free(PT.canvas);
}

static void test_memory_budget(void)
{
	int i;
	size_t n = (size_t)PT_CANVAS_W * PT_CANVAS_H;
	unsigned cap = pt_undo_budget();

	canvas_new(PT_CANVAS_W, PT_CANVAS_H);
	for (i = 0; i < 64; i++)
	{
		pt_undo_push();
		memset(PT.canvas, (i & 1) ? 0x11 : 0x22, n);
	}
	if (PT.undo_depth > 8)
		fail("history grew past the eight-step bound");
	if (pt_undo_mem() > cap)
		fail("undo memory exceeded the budget");

	for (i = 0; i < 8; i++)
		pt_undo();
	if (pt_undo_mem() > cap)
		fail("undo memory exceeded the budget while undoing");
	for (i = 0; i < 8; i++)
		pt_redo();
	if (pt_undo_mem() > cap)
		fail("undo memory exceeded the budget while redoing");

	free(PT.canvas);
}

int main(void)
{
	s_plat.alloc = host_alloc;
	s_plat.free = host_free;
	G.plat = &s_plat;

	test_eight_steps();
	test_ninth_drops_oldest();
	test_redo_cleared_on_edit();
	test_empty_messages();
	test_memory_budget();

	if (fails)
	{
		printf("%d FAILURES\n", fails);
		return 1;
	}
	printf("all checks passed\n");
	return 0;
}
"""


@pytest.fixture(scope="module")
def undo_driver(tmp_path_factory):
    if shutil.which("cc") is None:
        pytest.skip("needs a host C toolchain (cc)")
    tmp = tmp_path_factory.mktemp("paint_undo")
    shim = tmp / "shim"
    shim.mkdir()
    (shim / "mmb_priv.h").write_text(SHIM)
    driver = tmp / "driver.c"
    driver.write_text(DRIVER)
    exe = tmp / "driver"
    subprocess.run(
        [
            "cc", "-std=c11", "-O0", "-Wall", "-Wextra", "-Werror",
            "-I", str(shim), "-I", SRC,
            "-o", str(exe), str(driver), UNDO_C,
        ],
        check=True,
        cwd=REPO,
    )
    return exe


def test_undo_redo_native(undo_driver):
    out = subprocess.run(
        [str(undo_driver)], check=True, capture_output=True, text=True
    )
    assert "all checks passed" in out.stdout, out.stdout + out.stderr
