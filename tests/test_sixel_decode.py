"""Host unit tests for the streaming sixel decoder (#1444).

Compiles ``mmbasic/src/sixel.c`` with a small driver that owns a ``uint16_t``
buffer and an HDMI-style pack hook, then decodes raw sixel bodies.  No QEMU
and no screen integration: this locks the grammar, palette, repeat, raster
clip and P2 background rules from docs/analysis/term-sixel.md.
"""

import os
import shutil
import subprocess

import pytest

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(REPO, "mmbasic", "src")
SIXEL_C = os.path.join(SRC, "sixel.c")

DRIVER = r"""
#include "sixel.h"

#include <stdio.h>
#include <string.h>

#define W 32
#define H 32

static uint16_t buf[W * H];
static int fails;
static int last_adv, last_pw, last_ph;

static uint16_t pack(unsigned rgb)
{
	unsigned r = (rgb >> 16) & 255, g = (rgb >> 8) & 255, b = rgb & 255;
	return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static uint16_t px(int x, int y) { return buf[y * W + x]; }

static void check(const char *name, int cond)
{
	if (!cond) {
		printf("FAIL %s\n", name);
		fails++;
	}
}

static void dec_at(const char *body, int p2, unsigned bg_rgb, int oy)
{
	sixel_config c;
	sixel_image s;
	const unsigned char *p;

	memset(buf, 0, sizeof(buf));
	memset(&c, 0, sizeof(c));
	c.pix = buf;
	c.stride = W;
	c.width = W;
	c.height = H;
	c.ox = 0;
	c.oy = oy;
	c.p2 = p2;
	c.bg = pack(bg_rgb);
	c.pack = pack;
	sixel_begin(&s, &c);
	for (p = (const unsigned char *)body; *p; p++)
		sixel_feed(&s, *p);
	sixel_finish(&s);
	last_adv = s.advance_h;
	last_pw = s.painted_w;
	last_ph = s.painted_h;
}

static void dec(const char *body, int p2, unsigned bg_rgb)
{
	dec_at(body, p2, bg_rgb, 0);
}

int main(void)
{
	const uint16_t red = pack(0xFF0000);
	const uint16_t green = pack(0x00FF00);
	const uint16_t blue = pack(0x0000FF);
	int x, y, ok;

	/* 1. One red column; bit 0 is the top pixel. */
	dec("#0;2;100;0;0~", 0, 0);
	ok = 1;
	for (y = 0; y < 6; y++)
		ok = ok && px(0, y) == red;
	check("red_column", ok && px(1, 0) == 0 && last_adv == 6);

	/* 2. Repeat: !6? / !6~ paint six columns; !0 and a truncated repeat
	 *    paint nothing. */
	dec("#1;2;100;0;0!6~", 0, 0);
	ok = 1;
	for (x = 0; x < 6; x++)
		for (y = 0; y < 6; y++)
			ok = ok && px(x, y) == red;
	check("repeat_six", ok && px(6, 0) == 0);

	dec("#1;2;100;0;0!0~", 0, 0);
	check("repeat_zero", px(0, 0) == 0 && last_adv == 0);

	dec("#1;2;100;0;0!6", 0, 0);
	check("repeat_truncated", px(0, 0) == 0 && last_adv == 0);

	/* 3. $ returns to the image's left edge and overwrites the band. */
	dec("#1;2;100;0;0~$#2;2;0;0;100~", 0, 0);
	check("dollar_rewind", px(0, 0) == blue && px(0, 5) == blue &&
	      px(1, 0) == 0 && last_adv == 6);

	/* - advances six pixels and also returns to the left edge. */
	dec("#1;2;100;0;0~-~", 0, 0);
	check("newline_band", px(0, 0) == red && px(0, 6) == red &&
	      px(1, 6) == 0 && last_adv == 12);

	/* 4. P2 modes: 0/2 paint a 0 bit with the captured background; 1 leaves
	 *    the pixel untouched. */
	dec("#1;2;0;100;0~$#2;2;0;0;0?", 0, 0x0000FF);
	check("p2_zero_paints_background", px(0, 0) == blue);

	dec("#1;2;0;100;0~$#2;2;0;0;0?", 1, 0x0000FF);
	check("p2_one_transparent", px(0, 0) == green);

	/* 5. Raster attributes clip a longer band to 4x6 and fix the advance. */
	dec("\"1;1;4;6#1;2;100;0;0!10~", 1, 0);
	check("raster_clip", px(3, 5) == red && px(4, 0) == 0 &&
	      px(0, 6) == 0 && last_adv == 6);

	/* A raster with P2 0 also fills never-addressed pixels with the
	 * background; P2 1 leaves them alone. */
	dec("\"1;1;4;6#1;2;100;0;0~", 0, 0x0000FF);
	check("raster_bg_fill", px(2, 2) == blue);
	dec("\"1;1;4;6#1;2;100;0;0~", 1, 0x0000FF);
	check("raster_no_fill_when_p2_one", px(2, 2) == 0);

	/* A raster that starts below the pane top still draws its full height:
	 * the clip box is relative to the image origin, not the pane top. */
	dec_at("\"1;1;4;6#1;2;100;0;0!10~", 1, 0, 9);
	check("raster_at_offset",
	      px(0, 9) == red && px(0, 14) == red && px(0, 15) == 0 &&
	      px(3, 14) == red && px(4, 9) == 0 && last_adv == 6);

	/* 6. DEC HLS vectors; hue 0 is blue and saturation 0 is grey. */
	dec("#0;1;0;50;100~", 0, 0);
	check("hls_blue", px(0, 0) == blue);
	dec("#0;1;120;50;100~", 0, 0);
	check("hls_red", px(0, 0) == red);
	dec("#0;1;240;50;100~", 0, 0);
	check("hls_green", px(0, 0) == green);
	dec("#0;1;0;50;0~", 0, 0);
	check("hls_grey", px(0, 0) == pack(0x808080));

	/* 7. VT340 default palette, reloaded per image. */
	dec("#2~", 0, 0);
	check("default_register_2", px(0, 0) == pack(0xCC2121));
	dec("#0;2;0;0;100#1~", 0, 0);
	check("default_register_1_untouched", px(0, 0) == pack(0x3333CC));

	/* 8. CRLF inside the payload is ignored. */
	dec("#1;2;100;0;0\r\n~", 0, 0);
	check("crlf_ignored", px(0, 0) == red && last_adv == 6);

	/* 9. Register 300 clips to 255; a huge repeat saturates at the width. */
	dec("#300;2;100;0;0~", 0, 0);
	check("register_clip", px(0, 0) == red);

	dec("#1;2;100;0;0!999999999~", 0, 0);
	ok = 1;
	for (x = 0; x < W; x++)
		ok = ok && px(x, 0) == red;
	check("huge_repeat_saturates", ok && px(W - 1, 0) == red);

	/* 10. A command with no sixel data after it is harmless. */
	dec("\"1;1;4;6#1;2;100;0;0", 1, 0);
	check("raster_without_data", last_adv == 6 && last_pw == 0);

	/* 11. A failed target allocation discards the image silently: the
	 *     decoder still consumes the grammar but writes no pixels and
	 *     marks no coverage. */
	{
		unsigned char cov[8 * 4];
		sixel_config c;
		sixel_image s;
		const unsigned char *p;
		int clean = 1;

		memset(cov, 0, sizeof(cov));
		memset(&c, 0, sizeof(c));
		c.pix = NULL;
		c.stride = W;
		c.width = W;
		c.height = H;
		c.cover = cov;
		c.cover_cols = 8;
		c.cell_w = 8;
		c.cell_h = 16;
		c.pack = pack;
		sixel_begin(&s, &c);
		for (p = (const unsigned char *)"#0;2;100;0;0~"; *p; p++)
			sixel_feed(&s, *p);
		sixel_finish(&s);
		for (x = 0; x < (int)sizeof(cov); x++)
			clean = clean && cov[x] == 0;
		check("alloc_failure_discards",
		      clean && s.painted_w == 0 && s.painted_h == 0);
	}

	printf("FAILURES %d\n", fails);
	return fails ? 1 : 0;
}
"""


@pytest.fixture(scope="module")
def sixel_driver(tmp_path_factory):
    if shutil.which("cc") is None:
        pytest.skip("needs a host C toolchain (cc)")
    tmp = tmp_path_factory.mktemp("sixel_decode")
    driver = tmp / "driver.c"
    driver.write_text(DRIVER)
    exe = tmp / "driver"
    subprocess.run(
        [
            "cc", "-std=c11", "-O0", "-Wall", "-Wextra", "-Werror",
            "-I", SRC,
            "-o", str(exe), str(driver), SIXEL_C,
        ],
        check=True,
        cwd=REPO,
    )
    return exe


def test_sixel_decoder_vectors(sixel_driver):
    out = subprocess.run(
        [str(sixel_driver)], capture_output=True, text=True, timeout=30
    )
    assert out.returncode == 0, out.stdout + out.stderr
    assert "FAILURES 0" in out.stdout, out.stdout + out.stderr
