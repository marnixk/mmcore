#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
	void *(*alloc)(unsigned n);
	void (*free)(void *p);
} mmb_plat_t;

typedef struct {
	mmb_plat_t *plat;
} mmb_globals_t;

mmb_globals_t G;

static void *stub_alloc(unsigned n) { return malloc(n); }
static void stub_free(void *p) { free(p); }
static mmb_plat_t s_plat = { stub_alloc, stub_free };

unsigned mmb_rgb_pack(int r, int g, int b)
{
	return (unsigned)((r << 16) | (g << 8) | b);
}

static unsigned s_fb[8 * 8];

void mmb_gfx_plot(int x, int y, unsigned rgb)
{
	if (x >= 0 && x < 8 && y >= 0 && y < 8)
		s_fb[y * 8 + x] = rgb;
}

int mmb_png_decode(const unsigned char *file, unsigned n, int x, int y);

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
	if (!buf) { fclose(f); return NULL; }
	if (fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return NULL; }
	fclose(f);
	*size = (int)n;
	return buf;
}

int main(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : "mmbasic/assets/test.png";
	unsigned char *data;
	int n, rc, r;

	G.plat = &s_plat;
	data = read_file(path, &n);
	if (!data) { fprintf(stderr, "cannot read %s\n", path); return 1; }
	rc = mmb_png_decode(data, (unsigned)n, 0, 0);
	r = (s_fb[0] >> 16) & 0xFF;
	printf("mmb_png_decode(%s, %d bytes) = %d, pixel(0,0) R=%d\n", path, n, rc, r);
	free(data);
	return (rc == 0 && r > 150) ? 0 : 1;
}
