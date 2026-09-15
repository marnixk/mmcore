#include "mmb_priv.h"

static int sprite_ix(int n)
{
	if (n < 0 || n >= MMB_MAX_SPRITE)
		mmb_error("?SPRITE");
	return n;
}

void mmb_sprite_reset(void)
{
	int i;
	for (i = 0; i < MMB_MAX_SPRITE; i++)
	{
		if (G.gfx.sprite[i].pix)
		{
			G.plat->free(G.gfx.sprite[i].pix);
			G.gfx.sprite[i].pix = 0;
		}
		G.gfx.sprite[i].used = 0;
		G.gfx.sprite[i].vis = 0;
		G.gfx.sprite[i].w = 0;
		G.gfx.sprite[i].h = 0;
	}
}

static void sprite_store(int n, uint32_t *pix, int w, int h)
{
	int ix = sprite_ix(n);
	if (G.gfx.sprite[ix].pix)
		G.plat->free(G.gfx.sprite[ix].pix);
	G.gfx.sprite[ix].pix = pix;
	G.gfx.sprite[ix].w = w;
	G.gfx.sprite[ix].h = h;
	G.gfx.sprite[ix].used = pix ? 1 : 0;
	G.gfx.sprite[ix].vis = 0;
	G.gfx.sprite[ix].x = 0;
	G.gfx.sprite[ix].y = 0;
	G.gfx.sprite[ix].layer = 1;
}

static void add_png_ext(char *path, int pathsz)
{
	char *dot = 0, *q = path;
	while (*q)
	{
		if (*q == '.')
			dot = q;
		if (*q == '/' || *q == '\\')
			dot = 0;
		q++;
	}
	if (!dot && (int)strlen(path) + 4 < pathsz)
		strcat(path, ".png");
}

static void sprite_loadpng(void)
{
	mmb_val v;
	char path[128];
	unsigned char *file = 0;
	uint32_t *pix = 0;
	int id, w = 0, h = 0, sz, has_trans = 0;
	unsigned got = 0, trans_rgb = 0;

	id = (int)mmb_as_int(mmb_expr());
	mmb_skip_sp();
	if (*G.p == ',')
		G.p++;
	v = mmb_expr();
	if (v.type != T_STR)
		mmb_syntax();
	strncpy(path, v.s, sizeof(path) - 1);
	path[sizeof(path) - 1] = 0;
	add_png_ext(path, sizeof(path));
	mmb_skip_sp();
	if (*G.p == ',')
	{
		G.p++;
		has_trans = 1;
		trans_rgb = mmb_colour_from_int(mmb_as_int(mmb_expr()));
	}
	sz = mmb_vfs_size(path);
	if (sz < 0)
	{
		uint32_t *pix = G.plat->alloc(8 * 8 * sizeof(uint32_t));
		if (pix)
			memset(pix, 0, 8 * 8 * sizeof(uint32_t));
		sprite_store(id, pix, 8, 8);
		return;
	}
	file = G.plat->alloc((unsigned)sz + 1);
	if (!file)
		mmb_error("?OUT OF MEMORY");
	if (mmb_vfs_read(path, file, (unsigned)sz, &got) != 0)
	{
		G.plat->free(file);
		mmb_error("?PNG");
	}
	if (mmb_png_decode_rgba(file, got, &pix, &w, &h) != 0)
	{
		G.plat->free(file);
		mmb_error("?PNG");
	}
	G.plat->free(file);
	if (has_trans && pix)
	{
		int i, np = w * h;
		for (i = 0; i < np; i++)
		{
			if ((pix[i] & 0xFFFFFFu) == trans_rgb)
				pix[i] = 0;
		}
	}
	sprite_store(id, pix, w, h);
}

static void sprite_read(void)
{
	mmb_val a[6];
	int n = 0, id, x, y, w, h, page, i, j;
	uint32_t *pix;

	id = (int)mmb_as_int(mmb_expr());
	mmb_skip_sp();
	if (*G.p == ',')
		G.p++;
	while (n < 6 && *G.p && *G.p != ':' && *G.p != '\'')
	{
		mmb_skip_sp();
		if (*G.p == ',' || *G.p == 0)
		{
			a[n++] = mmb_int_val(0);
			if (*G.p == ',')
				G.p++;
			continue;
		}
		a[n++] = mmb_expr();
		mmb_skip_sp();
		if (*G.p == ',')
		{
			G.p++;
			continue;
		}
		break;
	}
	if (n < 4)
		mmb_syntax();
	x = (int)mmb_as_int(a[0]);
	y = (int)mmb_as_int(a[1]);
	w = (int)mmb_as_int(a[2]);
	h = (int)mmb_as_int(a[3]);
	page = n >= 5 ? (int)mmb_as_int(a[4]) : G.gfx.write_page;
	if (w <= 0 || h <= 0)
		mmb_error("?SPRITE");
	pix = G.plat->alloc((unsigned)w * (unsigned)h * sizeof(uint32_t));
	if (!pix)
		mmb_error("?OUT OF MEMORY");
	for (j = 0; j < h; j++)
		for (i = 0; i < w; i++)
		{
			unsigned c = mmb_gfx_get_page(x + i, y + j, page);
			if ((c & 0xFFFFFFu) == 0)
				c = 0;
			else
				c |= 0xFF000000u;
			pix[j * w + i] = c;
		}
	sprite_store(id, pix, w, h);
	G.gfx.sprite[sprite_ix(id)].vis = 0;
}

static void sprite_show(void)
{
	int id, x, y, layer = 1, ix;
	id = (int)mmb_as_int(mmb_expr());
	mmb_skip_sp();
	if (*G.p == ',')
		G.p++;
	x = (int)mmb_as_int(mmb_expr());
	mmb_skip_sp();
	if (*G.p == ',')
		G.p++;
	y = (int)mmb_as_int(mmb_expr());
	mmb_skip_sp();
	if (*G.p == ',')
	{
		G.p++;
		layer = (int)mmb_as_int(mmb_expr());
	}
	ix = sprite_ix(id);
	G.gfx.sprite[ix].x = x;
	G.gfx.sprite[ix].y = y;
	G.gfx.sprite[ix].layer = layer;
	G.gfx.sprite[ix].vis = 1;
	if (G.gfx.sprite[ix].used)
		mmb_gfx_present();
}

static void sprite_hide(void)
{
	int ix = sprite_ix((int)mmb_as_int(mmb_expr()));
	G.gfx.sprite[ix].vis = 0;
	mmb_gfx_present();
}

static void sprite_move(void)
{
	int id, x, y, ix;
	mmb_skip_sp();
	if (*G.p == 0 || *G.p == ':' || *G.p == '\'')
	{
		/* CMM2: SPRITE MOVE with no args updates all visible sprites. */
		mmb_gfx_present();
		return;
	}
	id = (int)mmb_as_int(mmb_expr());
	mmb_skip_sp();
	if (*G.p == ',')
		G.p++;
	x = (int)mmb_as_int(mmb_expr());
	mmb_skip_sp();
	if (*G.p == ',')
		G.p++;
	y = (int)mmb_as_int(mmb_expr());
	ix = sprite_ix(id);
	G.gfx.sprite[ix].x = x;
	G.gfx.sprite[ix].y = y;
	if (G.gfx.sprite[ix].vis)
		mmb_gfx_present();
}

void mmb_sprite_overlay(void)
{
	int layer, i, x, y, hw, hh;
	if (!G.plat || !G.plat->set_pixel)
		return;
	hw = G.plat->hdmi_width ? G.plat->hdmi_width() : G.gfx.w;
	hh = G.plat->hdmi_height ? G.plat->hdmi_height() : G.gfx.h;
	for (layer = 0; layer <= 8; layer++)
	{
		for (i = 0; i < MMB_MAX_SPRITE; i++)
		{
			int w, h;
			uint32_t *pix;
			if (!G.gfx.sprite[i].used || !G.gfx.sprite[i].vis)
				continue;
			if (G.gfx.sprite[i].layer != layer)
				continue;
			w = G.gfx.sprite[i].w;
			h = G.gfx.sprite[i].h;
			pix = G.gfx.sprite[i].pix;
			if (!pix)
				continue;
			for (y = 0; y < h; y++)
				for (x = 0; x < w; x++)
				{
					int dx = G.gfx.sprite[i].x + x;
					int dy = G.gfx.sprite[i].y + y;
					unsigned c = pix[y * w + x];
					if ((c & 0xFF000000u) == 0 || (c & 0xFFFFFFu) == 0)
						continue;
					if (dx < 0 || dy < 0 || dx >= hw || dy >= hh)
						continue;
					G.plat->set_pixel(dx, dy, c & 0xFFFFFFu);
				}
		}
	}
}

void mmb_cmd_sprite(void)
{
	if (mmb_match("LOADPNG") || mmb_match("LOAD"))
	{
		if (mmb_match("PNG"))
			;
		sprite_loadpng();
		return;
	}
	if (mmb_match("READ"))
	{
		sprite_read();
		return;
	}
	if (mmb_match("SHOW"))
	{
		sprite_show();
		return;
	}
	if (mmb_match("HIDE"))
	{
		sprite_hide();
		return;
	}
	if (mmb_match("MOVE"))
	{
		sprite_move();
		return;
	}
	if (mmb_match("CLOSE") || mmb_match("RESTORE"))
	{
		int ix;
		mmb_skip_sp();
		if (*G.p && *G.p != ':' && *G.p != '\'')
		{
			ix = sprite_ix((int)mmb_as_int(mmb_expr()));
			if (G.gfx.sprite[ix].pix)
			{
				G.plat->free(G.gfx.sprite[ix].pix);
				G.gfx.sprite[ix].pix = 0;
			}
			G.gfx.sprite[ix].used = 0;
			G.gfx.sprite[ix].vis = 0;
		}
		else
			mmb_sprite_reset();
		return;
	}
	mmb_syntax();
}
