#include "mmb_priv.h"
#include <math.h>
#include <string.h>

extern void mmb_gfx_copy_page(int src, int dst);

static int parse_args(mmb_val *a, int maxn)
{
	int n = 0;
	mmb_skip_sp();
	if (*G.p == 0 || *G.p == ':' || *G.p == '\'')
		return 0;
	while (n < maxn && *G.p && *G.p != ':' && *G.p != '\'')
	{
		mmb_skip_sp();
		if (*G.p == ',' || *G.p == 0 || *G.p == ':' || *G.p == '\'')
		{
			memset(&a[n], 0, sizeof(a[n]));
			n++;
			if (*G.p == ',')
			{
				G.p++;
				continue;
			}
			break;
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

/* IMAGE/BLIT/PAGE accept the FRAMEBUFFER keyword where a page number goes. */
static int parse_gfx_args(mmb_val *a, int *is_fb, int maxn)
{
	int n = 0;
	mmb_skip_sp();
	if (*G.p == 0 || *G.p == ':' || *G.p == '\'')
		return 0;
	while (n < maxn && *G.p && *G.p != ':' && *G.p != '\'')
	{
		mmb_skip_sp();
		if (mmb_match("FRAMEBUFFER"))
		{
			a[n] = mmb_int_val(0);
			is_fb[n] = 1;
		}
		else
		{
			is_fb[n] = 0;
			a[n] = mmb_expr();
		}
		n++;
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

static int arg_page(mmb_val v, int is_fb)
{
	if (is_fb)
		return MMB_PAGE_FB;
	return (int)mmb_as_int(v);
}

static int parse_page_token(void)
{
	mmb_skip_sp();
	if (mmb_match("FRAMEBUFFER"))
		return MMB_PAGE_FB;
	return (int)mmb_as_int(mmb_expr());
}

static int parse_blit_id(void)
{
	mmb_skip_sp();
	if (*G.p == '#')
		G.p++;
	return (int)mmb_as_int(mmb_expr());
}

void mmb_cmd_cls(void)
{
	mmb_val a[2];
	int n = parse_args(a, 2);
	unsigned c = G.gfx.bg;
	if (n >= 1)
		c = (unsigned)mmb_as_int(a[0]);
	mmb_gfx_cls(c);
	G.home_prompt = 1;
	G.print_x = 0;
	G.print_y = 0;
	G.print_locate = 0;
}

void mmb_cmd_pixel(void)
{
	const char *save = G.p;
	mmb_arrview vx, vy, vc;
	int i, n, colour_is_array;
	unsigned c;

	mmb_skip_sp();
	if (mmb_try_parse_arrview(&vx) && *G.p == ',')
	{
		G.p++;
		mmb_skip_sp();
		if (mmb_try_parse_arrview(&vy))
		{
			c = G.gfx.fg;
			colour_is_array = 0;
			memset(&vc, 0, sizeof(vc));
			vc.moff = -1;
			if (*G.p == ',')
			{
				const char *colp;

				G.p++;
				mmb_skip_sp();
				colp = G.p;
				if (mmb_try_parse_arrview(&vc))
					colour_is_array = 1;
				if (!colour_is_array)
				{
					G.p = colp;
					c = (unsigned)mmb_as_int(mmb_expr());
				}
			}
			if (!vx.v || !vy.v)
				mmb_syntax();
			if ((vx.mtype != T_INT && vx.mtype != T_NUM) ||
			    (vy.mtype != T_INT && vy.mtype != T_NUM))
				mmb_error("?TYPE MISMATCH");
			n = vx.count < vy.count ? vx.count : vy.count;
			if (colour_is_array)
			{
				if (vc.mtype != T_INT && vc.mtype != T_NUM)
					mmb_error("?TYPE MISMATCH");
				if (vc.count < n)
					n = vc.count;
			}
			for (i = 0; i < n; i++)
			{
				int x = mmb_arrview_int(vx, i);
				int y = mmb_arrview_int(vy, i);
				unsigned col = colour_is_array ? (unsigned)mmb_arrview_int(vc, i) : c;
				mmb_gfx_plot(x, y, col);
			}
			return;
		}
	}
	G.p = save;
	{
		mmb_val a[4];
		int nargs = parse_args(a, 4);
		unsigned col = G.gfx.fg;
		int x, y;
		if (nargs < 2)
			mmb_syntax();
		x = (int)mmb_as_int(a[0]);
		y = (int)mmb_as_int(a[1]);
		if (nargs >= 3)
			col = (unsigned)mmb_as_int(a[2]);
		mmb_gfx_plot(x, y, col);
	}
}

void mmb_cmd_line(void)
{
	mmb_val a[8];
	int n, used = 0, lw = 1;
	unsigned c;
	n = parse_args(a, 8);
	if (n < 4)
		mmb_syntax();
	c = colour_from(a, n, &used, G.gfx.fg);
	if (used)
		n--;
	if (n >= 5 && a[4].type)
		lw = (int)mmb_as_int(a[4]);
	if (lw <= 0)
		lw = 1;
	mmb_gfx_line((int)mmb_as_int(a[0]), (int)mmb_as_int(a[1]),
		     (int)mmb_as_int(a[2]), (int)mmb_as_int(a[3]), c, lw);
}

void mmb_cmd_box(void)
{
	mmb_val a[8];
	int n, lw = 1, fill = -1, is_fb[8];
	unsigned c = G.gfx.fg;
	int op = 0;
	if (mmb_match("AND_PIXELS"))
		op = '&';
	else if (mmb_match("OR_PIXELS"))
		op = '|';
	else if (mmb_match("XOR_PIXELS"))
		op = '^';
	if (op)
	{
		n = parse_gfx_args(a, is_fb, 6);
		if (n < 5)
			mmb_syntax();
		mmb_gfx_box_logic(op, (int)mmb_as_int(a[0]), (int)mmb_as_int(a[1]),
				  (int)mmb_as_int(a[2]), (int)mmb_as_int(a[3]),
				  (unsigned)mmb_as_int(a[4]),
				  n >= 6 ? arg_page(a[5], is_fb[5]) : MMB_PAGE_CUR);
		return;
	}
	n = parse_args(a, 8);
	if (n < 4)
		mmb_syntax();
	if (n >= 7)
	{
		if (a[4].type)
			lw = (int)mmb_as_int(a[4]);
		if (a[5].type)
			c = (unsigned)mmb_as_int(a[5]);
		if (a[6].type)
			fill = (int)mmb_as_int(a[6]);
	}
	else if (n == 6)
	{
		if (is_colour_val(a[4]) && is_colour_val(a[5]))
		{
			c = (unsigned)mmb_as_int(a[4]);
			fill = (int)mmb_as_int(a[5]);
		}
		else
		{
			if (a[4].type)
				lw = (int)mmb_as_int(a[4]);
			if (a[5].type)
				c = (unsigned)mmb_as_int(a[5]);
		}
	}
	else if (n == 5)
	{
		if (is_colour_val(a[4]))
			c = (unsigned)mmb_as_int(a[4]);
		else if (a[4].type)
			lw = (int)mmb_as_int(a[4]);
	}
	if (lw < 0)
		lw = 1;
	mmb_gfx_box((int)mmb_as_int(a[0]), (int)mmb_as_int(a[1]),
		    (int)mmb_as_int(a[2]), (int)mmb_as_int(a[3]), c, lw, fill);
}

void mmb_cmd_circle(void)
{
	mmb_val a[8];
	int 	n = parse_args(a, 8), lw = 1, fill = -1;
	unsigned c = G.gfx.fg;
	int ai;
	if (n < 3)
		mmb_syntax();
	ai = 3;
	if (ai < n && !a[ai].type)
		ai++;
	else if (ai < n && a[ai].type && !is_colour_val(a[ai]))
	{
		lw = (int)mmb_as_int(a[ai]);
		ai++;
	}
	if (ai < n && !a[ai].type)
		ai++;
	else if (ai < n && a[ai].type)
	{
		c = (unsigned)mmb_as_int(a[ai]);
		ai++;
	}
	if (ai < n && a[ai].type)
		fill = (int)mmb_as_int(a[ai]);
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
	if (n >= 5 && a[4].type)
		r = (int)mmb_as_int(a[4]);
	if (n >= 7 && is_colour_val(a[5]) && is_colour_val(a[6]))
	{
		/* rbox x,y,w,h,r,colour,fill  (linewidth omitted) */
		c = (unsigned)mmb_as_int(a[5]);
		fill = (int)mmb_as_int(a[6]);
		lw = 1;
	}
	else
	{
		if (n >= 6 && a[5].type)
			lw = (int)mmb_as_int(a[5]);
		if (n >= 7 && a[6].type)
			fill = (int)mmb_as_int(a[6]);
	}
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
	G.gfx.fg = mmb_colour_from_int(mmb_as_int(a[0]));
	if (n >= 2)
		G.gfx.bg = mmb_colour_from_int(mmb_as_int(a[1]));
	mmb_console_apply_colour();
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
	G.home_prompt = 1;
}

void mmb_cmd_page(void)
{
	if (mmb_match("SCROLL"))
	{
		mmb_val a[4];
		int is_fb[4], n = parse_gfx_args(a, is_fb, 4);
		if (n < 3)
			mmb_syntax();
		mmb_gfx_page_scroll(arg_page(a[0], is_fb[0]),
				    (int)mmb_as_int(a[1]), (int)mmb_as_int(a[2]),
				    n >= 4 ? (int)mmb_as_int(a[3]) : 0, n >= 4);
		return;
	}
	{
		int op = 0;
		if (mmb_match("AND_PIXELS"))
			op = '&';
		else if (mmb_match("OR_PIXELS"))
			op = '|';
		else if (mmb_match("XOR_PIXELS"))
			op = '^';
		if (op)
		{
			mmb_val a[3];
			int is_fb[3], n = parse_gfx_args(a, is_fb, 3);
			if (n < 3)
				mmb_syntax();
			mmb_gfx_page_logic(op, arg_page(a[0], is_fb[0]),
					   arg_page(a[1], is_fb[1]),
					   arg_page(a[2], is_fb[2]));
			return;
		}
	}
	if (mmb_match("WRITE"))
	{
		int pg = parse_page_token();
		if (pg == MMB_PAGE_FB)
		{
			mmb_gfx_fb_write();
			return;
		}
		if (pg < 0 || pg >= G.gfx.pages)
			mmb_error("?PAGE");
		G.gfx.write_fb = 0;
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
		mmb_skip_sp();
		if (mmb_match("TO"))
			dst = (int)mmb_as_int(mmb_expr());
		else if (*G.p == ',')
		{
			G.p++;
			dst = (int)mmb_as_int(mmb_expr());
		}
		mmb_skip_sp();
		if (*G.p == ',')
		{
			G.p++;
			mmb_skip_sp();
			/* CMM2: optional I (wait) or B (blit) flag, not an expression. */
			if ((*G.p >= 'A' && *G.p <= 'Z') || (*G.p >= 'a' && *G.p <= 'z'))
			{
				while (mmb_is_ident(*G.p))
					G.p++;
			}
			else if (*G.p && *G.p != ':' && *G.p != '\'')
				(void)mmb_expr();
		}
		mmb_gfx_copy_page(src, dst);
		mmb_gfx_present_if(dst);
		return;
	}
	mmb_syntax();
}

void mmb_cmd_blit(void)
{
	mmb_val a[8];
	int is_fb[8], n;
	if (mmb_match("READ"))
	{
		int id = parse_blit_id();
		mmb_skip_sp();
		if (*G.p == ',')
			G.p++;
		n = parse_gfx_args(a, is_fb, 5);
		if (n < 4)
			mmb_syntax();
		mmb_gfx_blit_read(id, (int)mmb_as_int(a[0]), (int)mmb_as_int(a[1]),
				  (int)mmb_as_int(a[2]), (int)mmb_as_int(a[3]),
				  n >= 5 ? arg_page(a[4], is_fb[4]) : MMB_PAGE_CUR);
		return;
	}
	if (mmb_match("WRITE"))
	{
		int id = parse_blit_id();
		int ori = 4;
		mmb_skip_sp();
		if (*G.p == ',')
			G.p++;
		n = parse_args(a, 3);
		if (n < 2)
			mmb_syntax();
		if (n >= 3)
			ori = (int)mmb_as_int(a[2]);
		mmb_gfx_blit_write(id, (int)mmb_as_int(a[0]), (int)mmb_as_int(a[1]), ori);
		return;
	}
	if (mmb_match("CLOSE"))
	{
		mmb_gfx_blit_close(parse_blit_id());
		return;
	}
	n = parse_gfx_args(a, is_fb, 8);
	if (n < 6)
		mmb_syntax();
	mmb_gfx_blit_copy((int)mmb_as_int(a[0]), (int)mmb_as_int(a[1]),
			  (int)mmb_as_int(a[2]), (int)mmb_as_int(a[3]),
			  (int)mmb_as_int(a[4]), (int)mmb_as_int(a[5]),
			  n >= 7 ? arg_page(a[6], is_fb[6]) : MMB_PAGE_CUR,
			  n >= 8 ? (int)mmb_as_int(a[7]) : 0);
}

void mmb_cmd_image(void)
{
	mmb_val a[12];
	int is_fb[12], n, fast = 0, skip = 0, page = MMB_PAGE_CUR;
	if (mmb_match("RESIZE_FAST"))
		fast = 1;
	else if (!mmb_match("RESIZE"))
	{
		if (mmb_match("ROTATE_FAST"))
			fast = 1;
		else if (mmb_match("ROTATE"))
			fast = 0;
		else if (mmb_match("WARP_H"))
		{
			n = parse_gfx_args(a, is_fb, 12);
			if (n < 10)
				mmb_syntax();
			if (n >= 11)
				page = arg_page(a[10], is_fb[10]);
			if (n >= 12)
				skip = (int)mmb_as_int(a[11]);
			mmb_gfx_image_warp_h((int)mmb_as_int(a[0]), (int)mmb_as_int(a[1]),
					     (int)mmb_as_int(a[2]), (int)mmb_as_int(a[3]),
					     (int)mmb_as_int(a[4]), (int)mmb_as_int(a[5]),
					     (int)mmb_as_int(a[6]), (int)mmb_as_int(a[7]),
					     (int)mmb_as_int(a[8]), (int)mmb_as_int(a[9]),
					     page, skip);
			return;
		}
		else if (mmb_match("WARP_V"))
		{
			n = parse_gfx_args(a, is_fb, 12);
			if (n < 10)
				mmb_syntax();
			if (n >= 11)
				page = arg_page(a[10], is_fb[10]);
			if (n >= 12)
				skip = (int)mmb_as_int(a[11]);
			mmb_gfx_image_warp_v((int)mmb_as_int(a[0]), (int)mmb_as_int(a[1]),
					     (int)mmb_as_int(a[2]), (int)mmb_as_int(a[3]),
					     (int)mmb_as_int(a[4]), (int)mmb_as_int(a[5]),
					     (int)mmb_as_int(a[6]), (int)mmb_as_int(a[7]),
					     (int)mmb_as_int(a[8]), (int)mmb_as_int(a[9]),
					     page, skip);
			return;
		}
		else
			mmb_syntax();
		/* ROTATE / ROTATE_FAST */
		n = parse_gfx_args(a, is_fb, 9);
		if (n < 7)
			mmb_syntax();
		if (n >= 8)
			page = arg_page(a[7], is_fb[7]);
		if (n >= 9)
			skip = (int)mmb_as_int(a[8]);
		mmb_gfx_image_rotate((int)mmb_as_int(a[0]), (int)mmb_as_int(a[1]),
				     (int)mmb_as_int(a[2]), (int)mmb_as_int(a[3]),
				     (int)mmb_as_int(a[4]), (int)mmb_as_int(a[5]),
				     mmb_as_float(a[6]), page, fast, skip);
		return;
	}
	/* RESIZE / RESIZE_FAST */
	n = parse_gfx_args(a, is_fb, 10);
	if (n < 8)
		mmb_syntax();
	if (n >= 9)
		page = arg_page(a[8], is_fb[8]);
	if (n >= 10)
		skip = (int)mmb_as_int(a[9]);
	mmb_gfx_image_resize((int)mmb_as_int(a[0]), (int)mmb_as_int(a[1]),
			     (int)mmb_as_int(a[2]), (int)mmb_as_int(a[3]),
			     (int)mmb_as_int(a[4]), (int)mmb_as_int(a[5]),
			     (int)mmb_as_int(a[6]), (int)mmb_as_int(a[7]),
			     page, fast, skip);
}

void mmb_cmd_framebuffer(void)
{
	if (mmb_match("CREATE"))
	{
		mmb_val a[2];
		int n = parse_args(a, 2);
		if (n < 2)
			mmb_syntax();
		mmb_gfx_fb_create((int)mmb_as_int(a[0]), (int)mmb_as_int(a[1]));
		return;
	}
	if (mmb_match("WRITE"))
	{
		mmb_gfx_fb_write();
		return;
	}
	if (mmb_match("BACKUP"))
	{
		mmb_gfx_fb_backup();
		return;
	}
	if (mmb_match("RESTORE"))
	{
		mmb_val a[4];
		int n = parse_args(a, 4);
		if (n == 0)
			mmb_gfx_fb_restore(0, 0, 0, 0, 1);
		else if (n < 4)
			mmb_syntax();
		else
			mmb_gfx_fb_restore((int)mmb_as_int(a[0]), (int)mmb_as_int(a[1]),
					   (int)mmb_as_int(a[2]), (int)mmb_as_int(a[3]), 0);
		return;
	}
	if (mmb_match("WINDOW"))
	{
		mmb_val a[4];
		int is_fb[4], n = parse_gfx_args(a, is_fb, 4);
		if (n < 3)
			mmb_syntax();
		mmb_gfx_fb_window((int)mmb_as_int(a[0]), (int)mmb_as_int(a[1]),
				  arg_page(a[2], is_fb[2]));
		return;
	}
	if (mmb_match("CLOSE"))
	{
		mmb_gfx_fb_close();
		return;
	}
	mmb_syntax();
}

static double turtle_norm(double d)
{
	while (d < 0)
		d += 360;
	while (d >= 360)
		d -= 360;
	return d;
}

static void turtle_ensure(void)
{
	if (!G.gfx.turtle_on)
		mmb_turtle_init_state(0);
}

static void turtle_record(double x, double y)
{
	if (!G.gfx.turtle_filling)
		return;
	if (G.gfx.turtle_fn >= MMB_TURTLE_MAX)
		return;
	G.gfx.turtle_fx[G.gfx.turtle_fn] = (int)(x + 0.5);
	G.gfx.turtle_fy[G.gfx.turtle_fn] = (int)(y + 0.5);
	G.gfx.turtle_fn++;
}

static void turtle_goto(double nx, double ny)
{
	int x0 = (int)(G.gfx.turtle_x + 0.5);
	int y0 = (int)(G.gfx.turtle_y + 0.5);
	int x1 = (int)(nx + 0.5);
	int y1 = (int)(ny + 0.5);
	if (G.gfx.turtle_pen)
		mmb_gfx_line(x0, y0, x1, y1, G.gfx.turtle_pen_col, 1);
	G.gfx.turtle_x = nx;
	G.gfx.turtle_y = ny;
	turtle_record(nx, ny);
}

void mmb_cmd_turtle(void)
{
	if (mmb_match("RESET"))
	{
		mmb_turtle_init_state(1);
		G.home_prompt = 1;
		return;
	}
	turtle_ensure();
	if (mmb_match("PEN"))
	{
		if (mmb_match("UP"))
		{
			G.gfx.turtle_pen = 0;
			return;
		}
		if (mmb_match("DOWN"))
		{
			G.gfx.turtle_pen = 1;
			return;
		}
		if (mmb_match("COLOUR") || mmb_match("COLOR"))
		{
			G.gfx.turtle_pen_col = (unsigned)mmb_as_int(mmb_expr());
			return;
		}
		mmb_syntax();
	}
	if (mmb_match("FILL"))
	{
		if (mmb_match("COLOUR") || mmb_match("COLOR"))
		{
			G.gfx.turtle_fill_col = (unsigned)mmb_as_int(mmb_expr());
			return;
		}
		if (mmb_match("PIXEL"))
		{
			int x = (int)mmb_as_int(mmb_expr());
			mmb_skip_sp();
			if (*G.p == ',')
				G.p++;
			mmb_gfx_plot(x, (int)mmb_as_int(mmb_expr()), G.gfx.turtle_fill_col);
			return;
		}
		mmb_syntax();
	}
	if (mmb_match("BEGIN") && mmb_match("FILL"))
	{
		G.gfx.turtle_filling = 1;
		G.gfx.turtle_fn = 0;
		turtle_record(G.gfx.turtle_x, G.gfx.turtle_y);
		return;
	}
	if (mmb_match("END") && mmb_match("FILL"))
	{
		G.gfx.turtle_filling = 0;
		if (G.gfx.turtle_fn > 2)
			mmb_gfx_fill_poly(G.gfx.turtle_fx, G.gfx.turtle_fy,
					  G.gfx.turtle_fn, G.gfx.turtle_fill_col);
		G.gfx.turtle_fn = 0;
		return;
	}
	if (mmb_match("FORWARD"))
	{
		double n = mmb_as_float(mmb_expr());
		double rad = G.gfx.turtle_hdg * 3.14159265358979323846 / 180.0;
		turtle_goto(G.gfx.turtle_x + n * sin(rad),
			    G.gfx.turtle_y - n * cos(rad));
		return;
	}
	if (mmb_match("BACKWARD"))
	{
		double n = mmb_as_float(mmb_expr());
		double rad = G.gfx.turtle_hdg * 3.14159265358979323846 / 180.0;
		turtle_goto(G.gfx.turtle_x - n * sin(rad),
			    G.gfx.turtle_y + n * cos(rad));
		return;
	}
	if (mmb_match("TURN"))
	{
		if (mmb_match("LEFT"))
		{
			G.gfx.turtle_hdg = turtle_norm(G.gfx.turtle_hdg - mmb_as_float(mmb_expr()));
			return;
		}
		if (mmb_match("RIGHT"))
		{
			G.gfx.turtle_hdg = turtle_norm(G.gfx.turtle_hdg + mmb_as_float(mmb_expr()));
			return;
		}
		mmb_syntax();
	}
	if (mmb_match("HEADING"))
	{
		G.gfx.turtle_hdg = turtle_norm(mmb_as_float(mmb_expr()));
		return;
	}
	if (mmb_match("MOVE"))
	{
		double x = mmb_as_float(mmb_expr());
		mmb_skip_sp();
		if (*G.p == ',')
			G.p++;
		turtle_goto(x, mmb_as_float(mmb_expr()));
		return;
	}
	if (mmb_match("DOT"))
	{
		mmb_gfx_plot((int)(G.gfx.turtle_x + 0.5), (int)(G.gfx.turtle_y + 0.5),
			     G.gfx.turtle_pen_col);
		return;
	}
	if (mmb_match("DRAW"))
	{
		if (mmb_match("TURTLE"))
		{
			double rad = G.gfx.turtle_hdg * 3.14159265358979323846 / 180.0;
			int x = (int)(G.gfx.turtle_x + 0.5);
			int y = (int)(G.gfx.turtle_y + 0.5);
			int tx = x + (int)(12 * sin(rad));
			int ty = y - (int)(12 * cos(rad));
			int lx = x + (int)(8 * sin(rad + 2.4));
			int ly = y - (int)(8 * cos(rad + 2.4));
			int rx = x + (int)(8 * sin(rad - 2.4));
			int ry = y - (int)(8 * cos(rad - 2.4));
			mmb_gfx_triangle(tx, ty, lx, ly, rx, ry, G.gfx.turtle_pen_col, -1);
			return;
		}
		if (mmb_match("PIXEL"))
		{
			int x = (int)mmb_as_int(mmb_expr());
			mmb_skip_sp();
			if (*G.p == ',')
				G.p++;
			mmb_gfx_plot(x, (int)mmb_as_int(mmb_expr()), G.gfx.turtle_pen_col);
			return;
		}
		if (mmb_match("LINE"))
		{
			mmb_val a[4];
			int n = parse_args(a, 4);
			if (n < 4)
				mmb_syntax();
			mmb_gfx_line((int)mmb_as_int(a[0]), (int)mmb_as_int(a[1]),
				     (int)mmb_as_int(a[2]), (int)mmb_as_int(a[3]),
				     G.gfx.turtle_pen_col, 1);
			return;
		}
		if (mmb_match("CIRCLE"))
		{
			mmb_val a[3];
			int n = parse_args(a, 3);
			if (n < 3)
				mmb_syntax();
			mmb_gfx_circle((int)mmb_as_int(a[0]), (int)mmb_as_int(a[1]),
				       (int)mmb_as_int(a[2]), G.gfx.turtle_pen_col, 1, -1);
			return;
		}
		mmb_syntax();
	}
	mmb_syntax();
}

void mmb_cmd_bitmap(void)
{
	mmb_val a[8];
	int n = parse_args(a, 8);
	int w = 8, h = 8, scale = G.gfx.font_scale, fill_bg = 0;
	unsigned fg = G.gfx.fg, bg = G.gfx.bg;
	unsigned char bytes[32];
	const unsigned char *bits;
	int nbytes, i;
	if (n < 3)
		mmb_syntax();
	if (n >= 4)
		w = (int)mmb_as_int(a[3]);
	if (n >= 5)
		h = (int)mmb_as_int(a[4]);
	if (n >= 6)
		scale = (int)mmb_as_int(a[5]);
	if (n >= 7)
		fg = (unsigned)mmb_as_int(a[6]);
	if (n >= 8)
	{
		bg = (unsigned)mmb_as_int(a[7]);
		fill_bg = 1;
	}
	if (a[2].type == T_STR)
	{
		bits = (const unsigned char *)a[2].s;
		nbytes = (int)strlen(a[2].s);
	}
	else
	{
		int64_t v = mmb_as_int(a[2]);
		for (i = 0; i < 8; i++)
			bytes[i] = (unsigned char)((v >> (8 * i)) & 0xFF);
		bits = bytes;
		nbytes = 8;
	}
	mmb_gfx_bitmap((int)mmb_as_int(a[0]), (int)mmb_as_int(a[1]),
		       bits, nbytes, w, h, scale, fg, bg, fill_bg);
}

void mmb_cmd_graphics(const char *kw)
{
	(void)kw;
}
