#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#define JAR_XM_IMPLEMENTATION
#include "jar_xm.h"

static char *read_file(const char *path, size_t *size)
{
	FILE *f = fopen(path, "rb");
	char *buf;
	long n;
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	buf = malloc((size_t)n);
	if (!buf)
	{
		fclose(f);
		return NULL;
	}
	if (fread(buf, 1, (size_t)n, f) != (size_t)n)
	{
		free(buf);
		fclose(f);
		return NULL;
	}
	fclose(f);
	*size = (size_t)n;
	return buf;
}

int main(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : "ramdisk/tests/TEST.XM";
	jar_xm_context_t *ctx = NULL;
	char *data;
	size_t n;
	int rc;

	data = read_file(path, &n);
	if (!data)
	{
		fprintf(stderr, "cannot read %s\n", path);
		return 1;
	}
	rc = jar_xm_create_context_safe(&ctx, data, n, 44100);
	printf("jar_xm_create_context_safe(%s, %zu bytes) = %d\n", path, n, rc);
	if (ctx)
		jar_xm_free_context(ctx);
	free(data);
	return rc == 0 ? 0 : 1;
}
