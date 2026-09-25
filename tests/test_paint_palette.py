"""PAINT palette strip and FG/BG selection (#635).

``mmbasic/src/paint_palette.c`` implements the palette half of the frozen
``paint.h`` API: the fixed default VGA 256 palette, the 4x64 swatch strip, the
FG/BG indicator and mouse hit-testing/selection.

These tests compile the module with a host toolchain against a tiny driver
that records ``pt_fill_rect`` into a software framebuffer and plays back mouse
clicks. That keeps the checks fast and exact: click two swatches, read the
framebuffer, assert FG/BG changed.
"""
import os
import shutil
import subprocess

import pytest

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(REPO, "mmbasic", "src")
INCLUDE = os.path.join(REPO, "mmbasic", "include")
THIRD_PARTY = os.path.join(REPO, "mmbasic", "third_party")
MODULE = os.path.join(SRC, "paint_palette.c")

DRIVER = r"""
#include "paint.h"	/* first: it declares sprintf before stdio's fortify macro */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

pt_state pt_console_state[MMB_MAX_CONSOLES];
int g_console;

static unsigned char fb[PT_W * PT_H * 3];

void pt_fill_rect(int x, int y, int w, int h, unsigned rgb)
{
	int xx, yy;

	if (w < 1 || h < 1)
		return;
	for (yy = y; yy < y + h; yy++)
	{
		if (yy < 0 || yy >= PT_H)
			continue;
		for (xx = x; xx < x + w; xx++)
		{
			unsigned char *p;
			if (xx < 0 || xx >= PT_W)
				continue;
			p = fb + ((size_t)yy * PT_W + xx) * 3;
			p[0] = (rgb >> 16) & 255;
			p[1] = (rgb >> 8) & 255;
			p[2] = rgb & 255;
		}
	}
}

void pt_request_redraw(void) { }

static int fails;
static int checks;

static void check(int cond, const char *name)
{
	checks++;
	if (!cond)
	{
		printf("FAIL %s\n", name);
		fails++;
	}
}

static unsigned px(int x, int y)
{
	const unsigned char *p = fb + ((size_t)y * PT_W + x) * 3;
	return ((unsigned)p[0] << 16) | ((unsigned)p[1] << 8) | p[2];
}

static void swatch_center(int i, int *sx, int *sy)
{
	int c = i % PT_PAL_COLS;
	int r = i / PT_PAL_COLS;

	*sx = PT_PAL_X + c * PT_PAL_SW + PT_PAL_SW / 2;
	*sy = PT_PAL_Y + r * PT_PAL_SW + PT_PAL_SW / 2;
}

int main(void)
{
	int i, idx, sx, sy;

	/* ---- fixed default VGA 256 anchors ---- */
	pt_palette_init();
	pt_palette_init();		/* idempotent */
	check(pt_palette_rgb(0) == 0x000000u, "ega 0 black");
	check(pt_palette_rgb(1) == 0x0000AAu, "ega 1 blue");
	check(pt_palette_rgb(4) == 0xAA0000u, "ega 4 red");
	check(pt_palette_rgb(15) == 0xFFFFFFu, "ega 15 white");
	check(pt_palette_rgb(16) == 0x000000u, "grey 16 black");
	check(pt_palette_rgb(31) == 0xFFFFFFu, "grey 31 white");
	for (i = 17; i <= 31; i++)
		check(pt_palette_rgb(i) >= pt_palette_rgb(i - 1), "grey ramp");
	check(pt_palette_rgb(17) == 0x141414u, "grey ramp 6-bit");
	check(pt_palette_rgb(32) == 0x000000u, "cube 32 black");
	check(pt_palette_rgb(247) == 0xFFFFFFu, "cube 247 white");
	check(pt_palette_rgb(32 + 5 * 36) == 0xFF0000u, "cube red corner");
	check(pt_palette_rgb(32 + 5 * 36 + 5 * 6) == 0xFFFF00u, "cube yellow corner");
	/* Tail is black, unlike the old build's trailing primaries. */
	for (i = 248; i < 256; i++)
		check(pt_palette_rgb(i) == 0x000000u, "tail black");
	check(pt_palette_rgb(252) == 0x000000u, "not the old build");
	check(pt_palette_rgb(-1) == pt_palette_rgb(0), "clamp low");
	check(pt_palette_rgb(999) == pt_palette_rgb(255), "clamp high");

	/* ---- strip draws all 256 swatches as a solid 4x64 grid ---- */
	PT.fg = 15;
	PT.bg = 0;
	memset(fb, 0xAB, sizeof(fb));
	pt_palette_draw();
	for (i = 0; i < 256; i++)
	{
		int c = i % PT_PAL_COLS;
		int r = i / PT_PAL_COLS;

		swatch_center(i, &sx, &sy);
		check(px(sx, sy) == pt_palette_rgb(i), "swatch centre");
		check(px(PT_PAL_X + c * PT_PAL_SW, PT_PAL_Y + r * PT_PAL_SW) ==
		      pt_palette_rgb(i), "swatch corner");
		check(px(PT_PAL_X + c * PT_PAL_SW + PT_PAL_SW - 1,
			 PT_PAL_Y + r * PT_PAL_SW + PT_PAL_SW - 1) ==
		      pt_palette_rgb(i), "swatch far corner");
	}

	/* ---- hit-testing maps every mouse cell to its index ---- */
	for (i = 0; i < 256; i++)
	{
		swatch_center(i, &sx, &sy);
		check(pt_palette_hit(sx, sy, &idx) && idx == i, "hit swatch");
	}
	check(!pt_palette_hit(PT_PAL_X - 1, PT_PAL_Y, &idx), "hit left out");
	check(!pt_palette_hit(PT_PAL_X, PT_PAL_Y - 1, &idx), "hit top out");
	check(!pt_palette_hit(PT_PAL_X + PT_PAL_W, PT_PAL_Y, &idx), "hit right out");
	check(!pt_palette_hit(PT_PAL_X, PT_PAL_Y + PT_PAL_H, &idx), "hit bottom out");
	check(!pt_palette_hit(PT_IND_X + 4, PT_IND_Y + 4, &idx), "strip excludes ind");
	check(pt_palette_indicator_hit(PT_IND_X + PT_IND_W / 2,
				       PT_IND_Y + PT_IND_H / 2), "indicator hit");
	check(!pt_palette_indicator_hit(PT_IND_X - 1, PT_IND_Y), "indicator left out");
	check(!pt_palette_indicator_hit(PT_IND_X + PT_IND_W, PT_IND_Y),
	      "indicator right out");

	/* ---- click two swatches: left sets FG, right sets BG ---- */
	PT.fg = 15;
	PT.bg = 0;
	swatch_center(2, &sx, &sy);
	check(pt_palette_hit(sx, sy, &idx) && idx == 2, "click fg map");
	pt_palette_select(idx, PT_BTN_LEFT);
	check(PT.fg == 2, "left click sets fg");
	swatch_center(3 * PT_PAL_COLS + 60, &sx, &sy);
	check(pt_palette_hit(sx, sy, &idx) && idx == 3 * PT_PAL_COLS + 60,
	      "click bg map");
	pt_palette_select(idx, PT_BTN_RIGHT);
	check(PT.bg == 3 * PT_PAL_COLS + 60, "right click sets bg");

	/* selection clamps out-of-range indices */
	pt_palette_select(-5, PT_BTN_LEFT);
	check(PT.fg == 0, "select clamp low");
	pt_palette_select(300, PT_BTN_RIGHT);
	check(PT.bg == 255, "select clamp high");

	/* ---- clicking the indicator swaps FG/BG ---- */
	PT.fg = 2;
	PT.bg = 5;
	check(pt_palette_indicator_hit(PT_IND_X + 16, PT_IND_Y + 16),
	      "indicator click");
	pt_palette_swap();
	check(PT.fg == 5 && PT.bg == 2, "indicator swaps fg/bg");

	/* ---- indicator paints the current FG/BG ---- */
	PT.fg = 4;			/* red */
	PT.bg = 1;			/* blue */
	memset(fb, 0, sizeof(fb));
	pt_palette_draw();
	check(px(PT_IND_X + 2 + 9, PT_IND_Y + 2 + 9) == pt_palette_rgb(1),
	      "indicator paints bg");
	check(px(PT_IND_X + 13 + 9, PT_IND_Y + 13 + 9) == pt_palette_rgb(4),
	      "indicator paints fg");

	if (fails)
	{
		printf("FAILURES %d\n", fails);
		return 1;
	}
	printf("checks=%d swatches=256\n", checks);
	printf("all checks passed\n");
	return 0;
}
"""


@pytest.fixture(scope="module")
def palette_driver(tmp_path_factory):
    if shutil.which("cc") is None:
        pytest.skip("needs a host C toolchain (cc)")
    tmp = tmp_path_factory.mktemp("paint_palette")
    driver = tmp / "driver.c"
    driver.write_text(DRIVER)
    exe = tmp / "driver"
    subprocess.run(
        [
            "cc", "-std=c11", "-O0", "-Wall", "-Wextra", "-Werror",
            "-DMMB_PLATFORM_POSIX",
            "-I", SRC, "-I", INCLUDE, "-I", THIRD_PARTY,
            "-o", str(exe), str(driver), MODULE,
        ],
        check=True,
        cwd=REPO,
    )
    return exe


def test_palette_module_compiles_and_passes(palette_driver):
    out = subprocess.run(
        [str(palette_driver)], check=True, capture_output=True, text=True
    )
    assert "all checks passed" in out.stdout, out.stdout + out.stderr
    assert "FAILURES" not in out.stdout


def test_palette_driver_ran_enough_checks(palette_driver):
    out = subprocess.run(
        [str(palette_driver)], check=True, capture_output=True, text=True
    )
    checks = int(out.stdout.split("checks=")[1].split()[0])
    assert checks > 1000, out.stdout


def test_palette_module_defines_strong_symbols():
    """Strong defs, not the weak stubs copied out of cmd_paint.c."""
    src = open(MODULE, encoding="utf-8").read()
    assert "PT_WEAK" not in src
    for fn in (
        "pt_palette_init",
        "pt_palette_rgb",
        "pt_palette_draw",
        "pt_palette_hit",
        "pt_palette_indicator_hit",
        "pt_palette_select",
        "pt_palette_swap",
    ):
        assert fn in src, fn
