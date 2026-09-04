#include "mmb_priv.h"
#include <math.h>

extern void mmb_gfx_copy_page(int src, int dst);
extern void mmb_gfx_blit(int sx, int sy, int w, int h, int dx, int dy);
extern void mmb_gfx_present(void);

static int parse_args(mmb_val *a, int maxn)
{
	int n = 0;
	mmb_skip_sp();
	if (*G.p == 0 || *G.p == ':' || *G.p == '\'')
		return 0;
	while (n < maxn && *G.p && *G.p != ':' && *G.p != '\'')
	{
		a[n++] = mmb_expr();
		mmb_skip_sp();
		if (*G.p == ',')
		{
			G.p++;
			continue;
		}
		break;
	}
	return n;
}

static unsigned colour_from(mmb_val *a, int n, int *used_last, unsigned def)
{
	*used_last = 0;
	if (n <= 0)
		return def;
	if (a[n - 1].type != T_STR && mmb_as_int(a[n - 1]) > 7)
	{
		*used_last = 1;
		return (unsigned)mmb_as_int(a[n - 1]);
	}
	return def;
}

static int is_colour_val(mmb_val v)
{
	return v.type != T_STR && mmb_as_int(v) > 7;
}

void mmb_cmd_cls(void)
{
	mmb_val a[2];
	int n = parse_args(a, 2);
	unsigned c = G.gfx.bg;
	if (n >= 1)
		c = (unsigned)mmb_as_int(a[0]);
	mmb_gfx_cls(c);
}

void mmb_cmd_pixel(void)
{
	mmb_val a[4];
	int n = parse_args(a, 4);
	unsigned c = G.gfx.fg;
	int x, y;
	if (n < 2)
		mmb_syntax();
	x = (int)mmb_as_int(a[0]);
	y = (int)mmb_as_int(a[1]);
	if (n >= 3)
		c = (unsigned)mmb_as_int(a[2]);
	mmb_gfx_plot(x, y, c);
}

void mmb_cmd_line(void)
{
	mmb_val a[8];
	int n = parse_args(a, 8), used = 0, lw = 1;
	unsigned c;
	if (n < 4)
		mmb_syntax();
	c = colour_from(a, n, &used, G.gfx.fg);
	if (used)
		n--;
	if (n >= 5)
		lw = (int)mmb_as_int(a[4]);
	mmb_gfx_line((int)mmb_as_int(a[0]), (int)mmb_as_int(a[1]),
		     (int)mmb_as_int(a[2]), (int)mmb_as_int(a[3]), c, lw);
}

void mmb_cmd_box(void)
{
	mmb_val a[8];
	int n = parse_args(a, 8), lw = 1, fill = -1;
	unsigned c = G.gfx.fg;
	if (n < 4)
		mmb_syntax();
	if (n >= 7)
	{
		lw = (int)mmb_as_int(a[4]);
		c = (unsigned)mmb_as_int(a[5]);
		fill = (int)mmb_as_int(a[6]);
	}
	else if (n == 6)
	{
		lw = (int)mmb_as_int(a[4]);
		c = (unsigned)mmb_as_int(a[5]);
	}
	else if (n == 5)
	{
		if (is_colour_val(a[4]))
			c = (unsigned)mmb_as_int(a[4]);
		else
			lw = (int)mmb_as_int(a[4]);
	}
	mmb_gfx_box((int)mmb_as_int(a[0]), (int)mmb_as_int(a[1]),
		    (int)mmb_as_int(a[2]), (int)mmb_as_int(a[3]), c, lw, fill);
}

void mmb_cmd_circle(void)
{
	mmb_val a[8];
	int n = parse_args(a, 8), lw = 1, fill = -1;
	unsigned c = G.gfx.fg;
	if (n < 3)
		mmb_syntax();
	if (n >= 6)
	{
		lw = (int)mmb_as_int(a[3]);
		c = (unsigned)mmb_as_int(a[4]);
		fill = (int)mmb_as_int(a[5]);
	}
	else if (n == 5)
	{
		lw = (int)mmb_as_int(a[3]);
		c = (unsigned)mmb_as_int(a[4]);
	}
	else if (n == 4)
	{
		if (is_colour_val(a[3]))
			c = (unsigned)mmb_as_int(a[3]);
		else
			lw = (int)mmb_as_int(a[3]);
	}
	mmb_gfx_circle((int)mmb_as_int(a[0]), (int)mmb_as_int(a[1]),
		       (int)mmb_as_int(a[2]), c, lw, fill);
}

void mmb_cmd_rbox(void)
{
	mmb_val a[8];
	int n = parse_args(a, 8), used = 0, r = 8, lw = 1, fill = -1;
	unsigned c;
	if (n < 4)
		mmb_syntax();
	c = colour_from(a, n, &used, G.gfx.fg);
	if (used)
		n--;
	if (n >= 5)
		r = (int)mmb_as_int(a[4]);
	if (n >= 6)
		lw = (int)mmb_as_int(a[5]);
	if (n >= 7)
		fill = (int)mmb_as_int(a[6]);
	mmb_gfx_rbox((int)mmb_as_int(a[0]), (int)mmb_as_int(a[1]),
		     (int)mmb_as_int(a[2]), (int)mmb_as_int(a[3]), r, c, lw, fill);
}

void mmb_cmd_arc(void)
{
	/* ARC x, y, r1, r2, a1, a2 [,c] — approximate with line segments */
	mmb_val a[8];
	int n = parse_args(a, 8), i;
	unsigned c = G.gfx.fg;
	double x, y, r1, r2, a1, a2;
	if (n < 6)
		mmb_syntax();
	if (n >= 7)
		c = (unsigned)mmb_as_int(a[6]);
	x = mmb_as_float(a[0]);
	y = mmb_as_float(a[1]);
	r1 = mmb_as_float(a[2]);
	r2 = mmb_as_float(a[3]);
	a1 = mmb_as_float(a[4]);
	a2 = mmb_as_float(a[5]);
	if (G.opt.angle_degrees)
	{
		a1 *= 3.14159265358979323846 / 180.0;
		a2 *= 3.14159265358979323846 / 180.0;
	}
	{
		int steps = 32;
		int px = 0, py = 0;
		for (i = 0; i <= steps; i++)
		{
			double t = a1 + (a2 - a1) * i / steps;
			int nx = (int)(x + r1 * cos(t));
			int ny = (int)(y + r2 * sin(t));
			if (i)
				mmb_gfx_line(px, py, nx, ny, c, 1);
			px = nx;
			py = ny;
		}
	}
}

void mmb_cmd_triangle(void)
{
	mmb_val a[8];
	int n = parse_args(a, 8), fill = -1;
	unsigned c = G.gfx.fg;
	if (n < 6)
		mmb_syntax();
	if (n >= 7)
		c = (unsigned)mmb_as_int(a[6]);
	if (n >= 8)
		fill = (int)mmb_as_int(a[7]);
	mmb_gfx_triangle((int)mmb_as_int(a[0]), (int)mmb_as_int(a[1]),
			 (int)mmb_as_int(a[2]), (int)mmb_as_int(a[3]),
			 (int)mmb_as_int(a[4]), (int)mmb_as_int(a[5]), c, fill);
}

void mmb_cmd_polygon(void)
{
	mmb_val a[32];
	int n = parse_args(a, 32), i, np, fill = -1;
	unsigned c = G.gfx.fg;
	if (n < 6)
		mmb_syntax();
	if (n >= 8 && (n % 2) == 0)
	{
		fill = (int)mmb_as_int(a[n - 1]);
		c = (unsigned)mmb_as_int(a[n - 2]);
		n -= 2;
	}
	else if (n >= 7 && (n % 2) == 1)
	{
		c = (unsigned)mmb_as_int(a[n - 1]);
		n--;
	}
	if (n < 6 || (n % 2) != 0)
		mmb_syntax();
	np = n / 2;
	if (fill >= 0)
	{
		for (i = 1; i + 1 < np; i++)
			mmb_gfx_triangle((int)mmb_as_int(a[0]), (int)mmb_as_int(a[1]),
					 (int)mmb_as_int(a[i * 2]), (int)mmb_as_int(a[i * 2 + 1]),
					 (int)mmb_as_int(a[i * 2 + 2]), (int)mmb_as_int(a[i * 2 + 3]),
					 c, fill);
	}
	for (i = 0; i + 3 < n; i += 2)
		mmb_gfx_line((int)mmb_as_int(a[i]), (int)mmb_as_int(a[i + 1]),
			     (int)mmb_as_int(a[i + 2]), (int)mmb_as_int(a[i + 3]), c, 1);
	if (n >= 4)
		mmb_gfx_line((int)mmb_as_int(a[n - 2]), (int)mmb_as_int(a[n - 1]),
			     (int)mmb_as_int(a[0]), (int)mmb_as_int(a[1]), c, 1);
}

void mmb_cmd_text(void)
{
	mmb_val a[8];
	int n = parse_args(a, 8);
	unsigned c = G.gfx.fg;
	if (n < 3 || a[2].type != T_STR)
		mmb_syntax();
	if (n >= 7)
		c = (unsigned)mmb_as_int(a[6]);
	else if (n >= 4 && a[3].type != T_STR)
		c = (unsigned)mmb_as_int(a[n - 1]);
	mmb_gfx_text((int)mmb_as_int(a[0]), (int)mmb_as_int(a[1]), a[2].s, c);
}

void mmb_cmd_font(void)
{
	mmb_val a[2];
	int n = parse_args(a, 2);
	if (n < 1)
		mmb_syntax();
	G.gfx.font = (int)mmb_as_int(a[0]);
	if (n >= 2)
		G.gfx.font_scale = (int)mmb_as_int(a[1]);
	if (G.gfx.font_scale < 1)
		G.gfx.font_scale = 1;
}

void mmb_cmd_colour(void)
{
	mmb_val a[2];
	int n = parse_args(a, 2);
	if (n < 1)
		mmb_syntax();
	G.gfx.fg = (unsigned)mmb_as_int(a[0]);
	if (n >= 2)
		G.gfx.bg = (unsigned)mmb_as_int(a[1]);
}

void mmb_cmd_mode(void)
{
	mmb_val a[4];
	int n = parse_args(a, 4);
	int mode, bits = 8;
	if (n < 1)
		mmb_syntax();
	mode = (int)mmb_as_int(a[0]);
	if (n >= 2)
		bits = (int)mmb_as_int(a[1]);
	mmb_gfx_set_mode(mode, bits);
	if (n >= 3)
		mmb_gfx_cls((unsigned)mmb_as_int(a[2]));
}

void mmb_cmd_page(void)
{
	if (mmb_match("WRITE"))
	{
		int pg = (int)mmb_as_int(mmb_expr());
		if (pg < 0 || pg >= G.gfx.pages)
			mmb_error("?PAGE");
		G.gfx.write_page = pg;
		return;
	}
	if (mmb_match("DISPLAY"))
	{
		int pg = (int)mmb_as_int(mmb_expr());
		if (pg < 0 || pg >= G.gfx.pages)
			mmb_error("?PAGE");
		G.gfx.display_page = pg;
		mmb_gfx_present();
		return;
	}
	if (mmb_match("COPY"))
	{
		int src = (int)mmb_as_int(mmb_expr());
		int dst = src;
		if (mmb_match("TO"))
			dst = (int)mmb_as_int(mmb_expr());
		mmb_gfx_copy_page(src, dst);
		return;
	}
	mmb_syntax();
}

void mmb_cmd_blit(void)
{
	mmb_val a[8];
	int n = parse_args(a, 8);
	if (n < 6)
		mmb_syntax();
	mmb_gfx_blit((int)mmb_as_int(a[0]), (int)mmb_as_int(a[1]),
		     (int)mmb_as_int(a[2]), (int)mmb_as_int(a[3]),
		     (int)mmb_as_int(a[4]), (int)mmb_as_int(a[5]));
}

void mmb_cmd_graphics(const char *kw)
{
	(void)kw;
}
