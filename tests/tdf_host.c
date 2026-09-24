/* Host test for mmb_tdf_count(): single- and multi-record .TDF files.
 *
 * Usage: tdf_host <single.tdf> <single_count> <multi.tdf> <multi_count>
 */

#include "mmb_tdf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;

static void expect_eq(const char *name, int got, int want)
{
	if (got != want)
	{
		fprintf(stderr, "FAIL %s: got %d want %d\n", name, got, want);
		fails++;
	}
}

static unsigned char *slurp(const char *path, unsigned *n)
{
	FILE *f = fopen(path, "rb");
	unsigned char *b;
	long sz;

	if (!f)
	{
		fprintf(stderr, "cannot open %s\n", path);
		exit(2);
	}
	fseek(f, 0, SEEK_END);
	sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	b = malloc((size_t)sz + 1);
	if (fread(b, 1, (size_t)sz, f) != (size_t)sz)
	{
		fprintf(stderr, "cannot read %s\n", path);
		exit(2);
	}
	fclose(f);
	*n = (unsigned)sz;
	return b;
}

int main(int argc, char **argv)
{
	unsigned char *single, *multi;
	unsigned ns, nm;
	int single_n, multi_n, t;

	if (argc < 5)
	{
		fprintf(stderr, "usage: %s single.tdf single_count multi.tdf multi_count\n",
			argv[0]);
		return 2;
	}
	single = slurp(argv[1], &ns);
	multi = slurp(argv[3], &nm);
	single_n = atoi(argv[2]);
	multi_n = atoi(argv[4]);

	expect_eq("single count", mmb_tdf_count(single, ns), single_n);
	expect_eq("multi count", mmb_tdf_count(multi, nm), multi_n);

	/* A truncated/garbage tail must not invent records. */
	t = mmb_tdf_count(multi, nm / 2);
	if (t < 1 || t > multi_n)
	{
		fprintf(stderr, "FAIL multi truncated: got %d\n", t);
		fails++;
	}

	/* Non-TDF input reports zero records. */
	expect_eq("garbage", mmb_tdf_count((const unsigned char *)"not a tdf", 9), 0);
	expect_eq("too short", mmb_tdf_count(single, 10), 0);

	free(single);
	free(multi);
	if (fails)
		return 1;
	printf("all checks passed\n");
	return 0;
}
