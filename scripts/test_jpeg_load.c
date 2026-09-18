#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "picojpeg.h"

int16_t *gCoeffBuf;
uint8_t *gMCUBufR;
uint8_t *gMCUBufG;
uint8_t *gMCUBufB;
int16_t *gQuant0;
int16_t *gQuant1;
uint8_t *gHuffVal2;
uint8_t *gHuffVal3;
uint8_t *gInBuf;

static int16_t s_coeff[64];
static uint8_t s_mcu_r[256], s_mcu_g[256], s_mcu_b[256];
static int16_t s_q0[64], s_q1[64];
static uint8_t s_h2[256], s_h3[256], s_in[256];

typedef struct {
	const unsigned char *p;
	unsigned n, off;
} jctx;

static unsigned char jpeg_need_bytes(unsigned char *buf, unsigned char max,
				     unsigned char *got, void *priv)
{
	jctx *c = (jctx *)priv;
	unsigned remain = c->n > c->off ? c->n - c->off : 0;
	if (remain > max)
		remain = max;
	if (remain)
		memcpy(buf, c->p + c->off, remain);
	c->off += remain;
	*got = (unsigned char)remain;
	return 0;
}

int main(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : "ramdisk/tests/TEST.JPG";
	FILE *f = fopen(path, "rb");
	unsigned char *data;
	long n;
	pjpeg_image_info_t info;
	jctx ctx;
	unsigned char status;
	if (!f) { fprintf(stderr, "cannot open %s\n", path); return 1; }
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	data = malloc((size_t)n);
	if (fread(data, 1, (size_t)n, f) != (size_t)n) return 1;
	fclose(f);
	gCoeffBuf = s_coeff; gMCUBufR = s_mcu_r; gMCUBufG = s_mcu_g; gMCUBufB = s_mcu_b;
	gQuant0 = s_q0; gQuant1 = s_q1; gHuffVal2 = s_h2; gHuffVal3 = s_h3; gInBuf = s_in;
	ctx.p = data; ctx.n = (unsigned)n; ctx.off = 0;
	status = pjpeg_decode_init(&info, jpeg_need_bytes, &ctx, 0);
	printf("init status=%u w=%u h=%u comps=%u\n", status, info.m_width, info.m_height, info.m_comps);
	free(data);
	return status ? 1 : 0;
}
