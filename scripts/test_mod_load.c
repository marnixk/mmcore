#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hxcmod.h"

static unsigned char *read_file(const char *path, int *size)
{
	FILE *f = fopen(path, "rb");
	unsigned char *buf;
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
	*size = (int)n;
	return buf;
}

int main(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : "mmbasic/assets/test.mod";
	modcontext ctx;
	unsigned char *data;
	int n, ok;

	data = read_file(path, &n);
	if (!data)
	{
		fprintf(stderr, "cannot read %s\n", path);
		return 1;
	}
	hxcmod_init(&ctx);
	hxcmod_setcfg(&ctx, 44100, 1, 1);
	ok = hxcmod_load(&ctx, data, n);
	printf("hxcmod_load(%s, %d bytes) = %d\n", path, n, ok);
	free(data);
	return ok ? 0 : 1;
}
