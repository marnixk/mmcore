/*
 * Host test for mmbasic/src/battery.c (#857).
 *
 * MM.BATTERY% reads /sys/class/power_supply/BAT0/capacity on native Linux and
 * falls back to 100 otherwise. The path-taking helper is compiled here and
 * driven against files pytest writes, so the parsing and clamping are checked
 * on any host without a real battery.
 *
 * usage: battery_host <path> <fallback> <expected>
 */
#include "mmbasic.h"
#include "mmb_priv.h"

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
	int got, want, fallback;

	if (argc != 4)
	{
		fprintf(stderr, "usage: %s path fallback expected\n", argv[0]);
		return 2;
	}
	fallback = atoi(argv[2]);
	want = atoi(argv[3]);
	got = mmb_battery_capacity_read(argv[1], fallback);
	if (got != want)
	{
		fprintf(stderr, "FAIL %s: got %d want %d\n", argv[1], got, want);
		return 1;
	}
	printf("PASS %s -> %d\n", argv[1], got);
	return 0;
}
