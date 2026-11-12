"""Host unit tests for the TERM JPEG XL / PPM decoder (#1464).

Compiles ``mmbasic/src/jxl_dec.c`` (which vendors J40) with a small driver.
No QEMU: this locks the size-header probe, capped dimension check, silent
failure on truncated data, and PPM/PBM decoding.  The JXL fixtures were made
with cjxl 0.7 and are tiny (40 B .. 1.4 kB).
"""

import os
import shutil
import subprocess

import pytest

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(REPO, "mmbasic", "src")
THIRD = os.path.join(REPO, "mmbasic", "third_party")
FIX = os.path.join(REPO, "tests", "term", "jxl")

DRIVER = r"""
#include "jxl_dec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
static const char *dir;

static void check(const char *name, int cond)
{
	if (!cond) {
		printf("FAIL %s\n", name);
		fails++;
	}
}

static unsigned char *slurp(const char *name, size_t *len)
{
	char path[1024];
	FILE *f;
	unsigned char *buf;
	long n;

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	buf = (unsigned char *)malloc((size_t)n + 1);
	if (!buf) {
		fclose(f);
		return NULL;
	}
	if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
		free(buf);
		fclose(f);
		return NULL;
	}
	fclose(f);
	*len = (size_t)n;
	return buf;
}

static mmb_img_rgb decode_file(const char *name, int jxl, unsigned maxpx)
{
	mmb_img_rgb img;
	size_t len = 0;
	unsigned char *buf = slurp(name, &len);
	int rc;

	memset(&img, 0, sizeof(img));
	if (!buf)
		return img;
	rc = jxl ? mmb_jxl_decode(buf, len, maxpx, &img)
		 : mmb_ppm_decode(buf, len, maxpx, &img);
	if (rc)
		img.rgb = NULL;
	free(buf);
	return img;
}

int main(int argc, char **argv)
{
	mmb_img_rgb a, b;
	const unsigned char p3[] = "P3\n# c\n2 2\n255\n255 0 0 0 255 0 0 0 255 255 255 255\n";
	const unsigned char p5[] = "P5\n2 1\n255\n\x00\xff";
	const unsigned char p1[] = "P1\n2 1\n1 0\n";
	const unsigned char p4[] = "P4\n2 1\n\x80";
	const unsigned char bad[] = "P6\n0 0\n255\n";
	unsigned char fuzz[96];
	unsigned i, n;
	int w, h;

	if (argc != 2)
		return 2;
	dir = argv[1];

	a = decode_file("lossless.jxl", 1, 4000000);
	b = decode_file("source.ppm", 0, 4000000);
	check("lossless_decodes", a.rgb && a.w == 8 && a.h == 6);
	check("lossless_exact", a.rgb && b.rgb && a.w == b.w && a.h == b.h &&
	      memcmp(a.rgb, b.rgb, (size_t)a.w * a.h * 3) == 0);
	mmb_img_free(&a);
	mmb_img_free(&b);

	a = decode_file("red.jxl", 1, 4000000);
	check("red_decodes", a.rgb && a.w == 8 && a.h == 8);
	if (a.rgb) {
		int ok = 1;
		for (i = 0; i < 8 * 8; i++)
			ok = ok && a.rgb[i * 3] == 255 && a.rgb[i * 3 + 1] == 0 &&
			     a.rgb[i * 3 + 2] == 0;
		check("red_pixels", ok);
	}
	mmb_img_free(&a);

	a = decode_file("frame.jxl", 1, 4000000);
	check("lossy_frame_decodes", a.rgb && a.w == 320 && a.h == 200);
	mmb_img_free(&a);

	{
		size_t len = 0;
		unsigned char *buf = slurp("oversized.jxl", &len);

		check("oversized_probe", buf && mmb_jxl_probe(buf, len, &w, &h) == 0 &&
		      w == 2100 && h == 2100);
		check("oversized_rejected", buf &&
		      mmb_jxl_decode(buf, len, 4000000, &a) != 0 && !a.rgb);
		free(buf);
	}

	check("truncated_rejected", !decode_file("truncated.jxl", 1, 4000000).rgb);

	a.rgb = NULL;
	check("ppm_p3", mmb_ppm_decode(p3, sizeof(p3) - 1, 100, &a) == 0 &&
	      a.w == 2 && a.h == 2 && a.rgb[0] == 255 && a.rgb[4] == 255 &&
	      a.rgb[8] == 255 && a.rgb[9] == 255 && a.rgb[10] == 255);
	mmb_img_free(&a);
	a.rgb = NULL;
	check("ppm_p5", mmb_ppm_decode(p5, sizeof(p5) - 1, 100, &a) == 0 &&
	      a.w == 2 && a.h == 1 && a.rgb[0] == 0 && a.rgb[3] == 255);
	mmb_img_free(&a);
	a.rgb = NULL;
	check("pbm_p1", mmb_ppm_decode(p1, sizeof(p1) - 1, 100, &a) == 0 &&
	      a.rgb[0] == 0 && a.rgb[3] == 255);
	mmb_img_free(&a);
	a.rgb = NULL;
	check("pbm_p4", mmb_ppm_decode(p4, sizeof(p4) - 1, 100, &a) == 0 &&
	      a.rgb[0] == 0 && a.rgb[3] == 255);
	mmb_img_free(&a);
	a.rgb = NULL;
	check("ppm_bad_header", mmb_ppm_decode(bad, sizeof(bad) - 1, 100, &a) != 0);
	check("ppm_too_big", mmb_ppm_decode(p3, sizeof(p3) - 1, 1, &a) != 0);

	{
		size_t len = 0;
		unsigned char *buf = slurp("frame.jxl", &len);

		for (n = 0; n < len && n < 80; n += 7) {
			a.rgb = NULL;
			if (mmb_jxl_decode(buf, n, 4000000, &a) == 0)
				mmb_img_free(&a);
		}
		free(buf);
		for (n = 0; n < sizeof(fuzz); n++)
			fuzz[n] = (unsigned char)((n * 37 + 11) ^ (n << 1));
		for (n = 1; n < sizeof(fuzz); n++) {
			a.rgb = NULL;
			if (mmb_ppm_decode(fuzz, n, 1000, &a) == 0)
				mmb_img_free(&a);
		}
	}

	printf("FAILURES %d\n", fails);
	return fails ? 1 : 0;
}
"""


@pytest.fixture(scope="module")
def jxl_driver(tmp_path_factory):
    if shutil.which("cc") is None:
        pytest.skip("needs a host C toolchain (cc)")
    tmp = tmp_path_factory.mktemp("jxl_decode")
    driver = tmp / "driver.c"
    driver.write_text(DRIVER)
    exe = tmp / "driver"
    subprocess.run(
        [
            "cc", "-std=c11", "-O0", "-Wall", "-Wextra",
            "-Wno-unused-function",
            "-I", SRC, "-I", THIRD,
            "-o", str(exe), str(driver), os.path.join(SRC, "jxl_dec.c"),
            "-lm",
        ],
        check=True,
        cwd=REPO,
    )
    return exe


def test_jxl_and_ppm_decoder_vectors(jxl_driver):
    out = subprocess.run(
        [str(jxl_driver), FIX], capture_output=True, text=True, timeout=120
    )
    assert out.returncode == 0, out.stdout + out.stderr
    assert "FAILURES 0" in out.stdout, out.stdout + out.stderr
