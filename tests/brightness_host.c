/*
 * Host test for mmbasic/src/brightness.c.
 *
 * MM.BRIGHTNESS% reads and writes the panel backlight percentage on native
 * Linux. The device-directory helpers are compiled here and driven against a
 * fake sysfs directory pytest builds, so the percentage maths and clamping are
 * checked on any host without a real backlight.
 *
 * usage: brightness_host read  <devdir> <fallback> <expected>
 *        brightness_host write <devdir> <pct> <expected_raw>
 */
#include "mmbasic.h"
#include "mmb_priv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int read_file_int(const char *path)
{
	FILE *f;
	int v;

	f = fopen(path, "r");
	if (!f)
		return -99999;
	if (fscanf(f, "%d", &v) != 1)
	{
		fclose(f);
		return -99999;
	}
	fclose(f);
	return v;
}

int main(int argc, char **argv)
{
	if (argc == 5 && strcmp(argv[1], "read") == 0)
	{
		int fallback = atoi(argv[3]);
		int want = atoi(argv[4]);
		int got = mmb_brightness_read(argv[2], fallback);

		if (got != want)
		{
			fprintf(stderr, "FAIL read %s: got %d want %d\n",
				argv[2], got, want);
			return 1;
		}
		printf("PASS read %s -> %d\n", argv[2], got);
		return 0;
	}
	if (argc == 5 && strcmp(argv[1], "write") == 0)
	{
		char path[512];
		int pct = atoi(argv[3]);
		int want = atoi(argv[4]);
		int got;

		if (mmb_brightness_write(argv[2], pct) != 0)
		{
			fprintf(stderr, "FAIL write %s: call failed\n", argv[2]);
			return 1;
		}
		snprintf(path, sizeof(path), "%s/brightness", argv[2]);
		got = read_file_int(path);
		if (got != want)
		{
			fprintf(stderr, "FAIL write %s pct %d: raw got %d want %d\n",
				argv[2], pct, got, want);
			return 1;
		}
		printf("PASS write %s pct %d -> %d\n", argv[2], pct, got);
		return 0;
	}
	fprintf(stderr,
		"usage: %s read  <devdir> <fallback> <expected>\n"
		"       %s write <devdir> <pct> <expected_raw>\n",
		argv[0], argv[0]);
	return 2;
}
