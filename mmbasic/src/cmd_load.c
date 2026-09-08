#include "mmb_priv.h"
#include "picojpeg.h"

int mmb_png_decode(const unsigned char *file, unsigned n, int x, int y);

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

static void jpeg_bind_bufs(void)
{
	gCoeffBuf = s_coeff;
	gMCUBufR = s_mcu_r;
	gMCUBufG = s_mcu_g;
	gMCUBufB = s_mcu_b;
	gQuant0 = s_q0;
	gQuant1 = s_q1;
	gHuffVal2 = s_h2;
	gHuffVal3 = s_h3;
	gInBuf = s_in;
}

static int read_file(const char *path, unsigned char **buf, unsigned *n)
{
	unsigned got = 0;
	int sz = mmb_vfs_size(path);
	if (sz < 0)
		return -1;
	*buf = G.plat->alloc((unsigned)sz + 1);
	if (!*buf)
		return -1;
	if (mmb_vfs_read(path, *buf, (unsigned)sz, &got) != 0)
	{
		G.plat->free(*buf);
		return -1;
	}
	*n = got;
	return 0;
}

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

int mmb_load_jpeg(const char *path, int x, int y)
{
	unsigned char *file = 0;
	unsigned n = 0;
	pjpeg_image_info_t info;
	jctx ctx;
	unsigned char status;
	int mx, my;
	jpeg_bind_bufs();
	if (read_file(path, &file, &n) != 0)
		return -1;
	ctx.p = file;
	ctx.n = n;
	ctx.off = 0;
	status = pjpeg_decode_init(&info, jpeg_need_bytes, &ctx, 0);
	if (status)
	{
		G.plat->free(file);
		return -1;
	}
	for (my = 0; my < info.m_MCUSPerCol; my++)
	{
		for (mx = 0; mx < info.m_MCUSPerRow; mx++)
		{
			int i, j, bw, origin_x, origin_y;
			status = pjpeg_decode_mcu();
			if (status)
			{
				G.plat->free(file);
				return status == PJPG_NO_MORE_BLOCKS ? 0 : -1;
			}
			bw = info.m_MCUWidth / 8;
			if (bw < 1)
				bw = 1;
			origin_x = mx * info.m_MCUWidth;
			origin_y = my * info.m_MCUHeight;
			for (j = 0; j < info.m_MCUHeight; j++)
				for (i = 0; i < info.m_MCUWidth; i++)
				{
					int px = origin_x + i, py = origin_y + j;
					int block = (j / 8) * bw + (i / 8);
					int idx = block * 64 + (j % 8) * 8 + (i % 8);
					unsigned char r = info.m_pMCUBufR[idx];
					unsigned char g = info.m_comps > 1 ? info.m_pMCUBufG[idx] : r;
					unsigned char b = info.m_comps > 1 ? info.m_pMCUBufB[idx] : r;
					if (px < info.m_width && py < info.m_height)
						mmb_gfx_plot(x + px, y + py, mmb_rgb_pack(r, g, b));
				}
		}
	}
	G.plat->free(file);
	return 0;
}

int mmb_load_png(const char *path, int x, int y)
{
	unsigned char *file = 0;
	unsigned n = 0;
	int rc;
	if (read_file(path, &file, &n) != 0)
		return -1;
	rc = mmb_png_decode(file, n, x, y);
	G.plat->free(file);
	return rc;
}

void mmb_cmd_load(void)
{
	char path[128];
	int x = 0, y = 0, page = -1, saved_page = 0, saved_fb = 0;
	mmb_val v;
	int is_jpg = 0, is_png = 0;
	if (mmb_match("JPG") || mmb_match("JPEG"))
		is_jpg = 1;
	else if (mmb_match("PNG"))
		is_png = 1;
	else if (mmb_match("BMP") || mmb_match("GIF") || mmb_match("IMAGE"))
		is_png = 1;
	v = mmb_expr();
	if (v.type != T_STR)
		mmb_syntax();
	strncpy(path, v.s, sizeof(path) - 1);
	path[sizeof(path) - 1] = 0;
	mmb_skip_sp();
	if (*G.p == ',')
	{
		G.p++;
		x = (int)mmb_as_int(mmb_expr());
		mmb_skip_sp();
		if (*G.p == ',')
		{
			G.p++;
			y = (int)mmb_as_int(mmb_expr());
			mmb_skip_sp();
			if (*G.p == ',')
			{
				G.p++;
				page = (int)mmb_as_int(mmb_expr());
			}
		}
	}
	if (!is_jpg && !is_png)
	{
		char *dot = 0;
		char *q = path;
		while (*q)
		{
			if (*q == '.')
				dot = q;
			q++;
		}
		if (dot && mmb_keyword_eq(dot, ".APP"))
			mmb_error("?FILE");
		if (dot && (mmb_keyword_eq(dot, ".JPG") || mmb_keyword_eq(dot, ".JPEG")))
			is_jpg = 1;
		else
			is_png = 1;
	}
	if (page >= 0)
	{
		saved_page = G.gfx.write_page;
		saved_fb = G.gfx.write_fb;
		if (page < G.gfx.pages)
		{
			G.gfx.write_fb = 0;
			G.gfx.write_page = page;
		}
	}
	if (is_jpg)
	{
		if (mmb_gfx_writing_fb())
			mmb_error("?FRAMEBUFFER");
		if (mmb_load_jpeg(path, x, y) != 0)
		{
			if (mmb_vfs_size(path) >= 0)
				mmb_error("?JPEG");
		}
	}
	else if (mmb_load_png(path, x, y) != 0)
	{
		if (mmb_vfs_size(path) >= 0)
			mmb_error("?PNG");
	}
	if (page >= 0)
	{
		G.gfx.write_page = saved_page;
		G.gfx.write_fb = saved_fb;
	}
}
