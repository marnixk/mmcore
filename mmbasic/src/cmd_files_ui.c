#include "mmb_priv.h"
#include "tui.h"

#define FU_MAX_ENT  96
#define FU_NAME     80
#define FU_PATH     128

#define FU_BROWSE   0
#define FU_HELP     1
#define FU_MENU     2
#define FU_PROMPT   3
#define FU_CONFIRM  4
#define FU_INFO     5
#define FU_PREVIEW  6
#define FU_PLAY     7

#define FU_PR_COPY  1
#define FU_PR_MOVE  2
#define FU_PR_MKDIR 3
#define FU_PR_DRIVE 4

typedef struct {
	char name[FU_NAME];
	int is_dir;
	int size;
} fu_ent;

typedef struct {
	char path[FU_PATH];
	fu_ent ent[FU_MAX_ENT];
	int n, sel, top;
} fu_panel;

typedef struct {
	int active;
	fu_panel pan[2];
	int cur;
	int mode;
	int prompt_kind;
	int esc;
	int csi_n;
	int pend_drive;
	int menu_i;
	char filter[32];
	char prompt[FU_PATH];
	char hint[96];
	char info_name[FU_NAME];
	char info_path[FU_PATH];
	int info_size;
} fu_state;

static fu_state F;

#define FU_MENU_FG TUI_BLACK
#define FU_MENU_BG TUI_CYAN
#define FU_PAN_FG  TUI_WHITE
#define FU_PAN_BG  TUI_BLUE
#define FU_SEL_FG  TUI_BLACK
#define FU_SEL_BG  TUI_CYAN
#define FU_DIR_FG  TUI_BRCYAN
#define FU_DIR_BG  TUI_BLUE
#define FU_ST_FG   TUI_CYAN
#define FU_ST_BG   TUI_BLACK
#define FU_FN_FG   TUI_WHITE
#define FU_FN_BG   TUI_BLUE
#define FU_FL_FG   TUI_BLACK
#define FU_FL_BG   TUI_CYAN

static int fu_cols(void) { return tui_cols(); }
static int fu_rows(void) { return tui_rows(); }
static int fu_left_w(void) { return fu_cols() / 2; }
static int fu_right_w(void) { return fu_cols() - fu_left_w(); }
static int fu_list(void)
{
	int n = fu_rows() - 7;
	return n < 3 ? 3 : n;
}

static void ser(const char *s)
{
	unsigned n;
	if (!s || !G.plat || !G.plat->write_serial)
		return;
	n = (unsigned)strlen(s);
	if (n)
		G.plat->write_serial(s, n);
}

static void fmt_uint(char *dst, unsigned v)
{
	char tmp[12];
	int i = 0;
	if (v == 0)
	{
		dst[0] = '0';
		dst[1] = 0;
		return;
	}
	while (v && i < 11)
	{
		tmp[i++] = (char)('0' + (v % 10));
		v /= 10;
	}
	while (i--)
		*dst++ = tmp[i];
	*dst = 0;
}

static void pad(char *dst, int width, const char *s, int right)
{
	int n = (int)strlen(s), i;
	if (n > width)
		n = width;
	if (right)
	{
		for (i = 0; i < width - n; i++)
			dst[i] = ' ';
		memcpy(dst + width - n, s, (unsigned)n);
	}
	else
	{
		memcpy(dst, s, (unsigned)n);
		for (i = n; i < width; i++)
			dst[i] = ' ';
	}
	dst[width] = 0;
}

static int icmp(const char *a, const char *b)
{
	while (*a && *b)
	{
		char ca = *a, cb = *b;
		if (ca >= 'a' && ca <= 'z')
			ca = (char)(ca - 32);
		if (cb >= 'a' && cb <= 'z')
			cb = (char)(cb - 32);
		if (ca != cb)
			return (int)(unsigned char)ca - (int)(unsigned char)cb;
		a++;
		b++;
	}
	return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static const char *ext_of(const char *name)
{
	const char *d = 0, *p = name;
	while (*p)
	{
		if (*p == '.')
			d = p;
		p++;
	}
	return d ? d : "";
}

static int is_bas(const char *n)
{
	const char *e = ext_of(n);
	return mmb_keyword_eq(e, ".BAS") || mmb_keyword_eq(e, ".INC");
}

static int is_img(const char *n)
{
	const char *e = ext_of(n);
	return mmb_keyword_eq(e, ".PNG") || mmb_keyword_eq(e, ".JPG") ||
	       mmb_keyword_eq(e, ".JPEG");
}

static int is_aud(const char *n)
{
	const char *e = ext_of(n);
	return mmb_keyword_eq(e, ".MP3") || mmb_keyword_eq(e, ".XM") ||
	       mmb_keyword_eq(e, ".MOD");
}

static int is_dotdot(const fu_ent *e)
{
	return e->name[0] == '.' && e->name[1] == '.' && e->name[2] == 0;
}

static void parent_of(char *path)
{
	char *slash = 0, *p;
	for (p = path + 2; *p; p++)
		if (*p == '/')
			slash = p;
	if (!slash || slash == path + 2)
	{
		path[2] = '/';
		path[3] = 0;
		return;
	}
	*slash = 0;
}

static void join_path(char *dst, int dstsz, const char *dir, const char *name)
{
	if (name[0] == '.' && name[1] == '.' && name[2] == 0)
	{
		strncpy(dst, dir, (unsigned)dstsz - 1);
		dst[dstsz - 1] = 0;
		parent_of(dst);
		return;
	}
	strncpy(dst, dir, (unsigned)dstsz - 1);
	dst[dstsz - 1] = 0;
	if (dst[0] && dst[strlen(dst) - 1] != '/')
		strncat(dst, "/", (unsigned)dstsz - strlen(dst) - 1);
	strncat(dst, name, (unsigned)dstsz - strlen(dst) - 1);
}

static fu_panel *curpan(void)
{
	return &F.pan[F.cur];
}

static fu_panel *otherpan(void)
{
	return &F.pan[F.cur ^ 1];
}

static fu_ent *cursel(void)
{
	fu_panel *p = curpan();
	if (p->sel < 0 || p->sel >= p->n)
		return 0;
	return &p->ent[p->sel];
}

static void sel_path(char *dst, int dstsz)
{
	fu_ent *e = cursel();
	if (!e)
	{
		strncpy(dst, curpan()->path, (unsigned)dstsz - 1);
		dst[dstsz - 1] = 0;
		return;
	}
	join_path(dst, dstsz, curpan()->path, e->name);
}

static int prefix_ok(const char *name)
{
	int i;
	if (!F.filter[0])
		return 1;
	if (name[0] == '.' && name[1] == '.' && name[2] == 0)
		return 1;
	for (i = 0; F.filter[i]; i++)
	{
		char a = name[i], b = F.filter[i];
		if (a >= 'a' && a <= 'z')
			a = (char)(a - 32);
		if (b >= 'a' && b <= 'z')
			b = (char)(b - 32);
		if (!a || a != b)
			return 0;
	}
	return 1;
}

static int ent_less(const fu_ent *a, const fu_ent *b)
{
	if (is_dotdot(a))
		return 1;
	if (is_dotdot(b))
		return 0;
	if (a->is_dir != b->is_dir)
		return a->is_dir > b->is_dir;
	return icmp(a->name, b->name) < 0;
}

static void panel_sort(fu_panel *p)
{
	int i, j;
	for (i = 0; i < p->n; i++)
		for (j = i + 1; j < p->n; j++)
			if (ent_less(&p->ent[j], &p->ent[i]))
			{
				fu_ent t = p->ent[i];
				p->ent[i] = p->ent[j];
				p->ent[j] = t;
			}
}

static void panel_add(fu_panel *p, const char *name, int is_dir, int size)
{
	fu_ent *e;
	if (p->n >= FU_MAX_ENT)
		return;
	if (!prefix_ok(name))
		return;
	e = &p->ent[p->n++];
	memset(e, 0, sizeof(*e));
	strncpy(e->name, name, FU_NAME - 1);
	e->is_dir = is_dir;
	e->size = size;
}

static void panel_reload(fu_panel *p)
{
	char list[2048];
	char keep[FU_NAME];
	char *s, *nl;
	int oldsel = p->sel;
	keep[0] = 0;
	if (oldsel >= 0 && oldsel < p->n)
		strncpy(keep, p->ent[oldsel].name, FU_NAME - 1);
	p->n = 0;
	panel_add(p, "..", 1, 0);
	list[0] = 0;
	if (mmb_vfs_list(p->path, list, sizeof(list)) != 0)
		list[0] = 0;
	s = list;
	while (*s)
	{
		char name[FU_NAME];
		int is_dir = 0, sz = 0, n = 0;
		nl = s;
		while (*nl && *nl != '\n' && *nl != '\r')
			nl++;
		n = (int)(nl - s);
		if (n >= FU_NAME)
			n = FU_NAME - 1;
		memcpy(name, s, (unsigned)n);
		name[n] = 0;
		if (n > 0 && name[n - 1] == '/')
		{
			name[n - 1] = 0;
			is_dir = 1;
		}
		if (name[0] && !(name[0] == '.' && name[1] == 0) &&
		    !(name[0] == '.' && name[1] == '.' && name[2] == 0))
		{
			if (!is_dir)
			{
				char full[FU_PATH];
				join_path(full, sizeof(full), p->path, name);
				sz = mmb_vfs_size(full);
				if (sz < 0)
					sz = 0;
			}
			panel_add(p, name, is_dir, sz);
		}
		s = nl;
		if (*s == '\r')
			s++;
		if (*s == '\n')
			s++;
	}
	panel_sort(p);
	p->sel = 0;
	if (keep[0])
	{
		int i;
		for (i = 0; i < p->n; i++)
			if (mmb_keyword_eq(p->ent[i].name, keep))
			{
				p->sel = i;
				break;
			}
	}
	if (p->sel >= p->n)
		p->sel = p->n ? p->n - 1 : 0;
	if (p->sel < p->top)
		p->top = p->sel;
	if (p->sel >= p->top + fu_list())
		p->top = p->sel - fu_list() + 1;
	if (p->top < 0)
		p->top = 0;
}

static void reload_all(void)
{
	panel_reload(&F.pan[0]);
	panel_reload(&F.pan[1]);
}

static void clamp_sel(fu_panel *p)
{
	if (p->n <= 0)
	{
		p->sel = 0;
		p->top = 0;
		return;
	}
	if (p->sel < 0)
		p->sel = 0;
	if (p->sel >= p->n)
		p->sel = p->n - 1;
	if (p->sel < p->top)
		p->top = p->sel;
	if (p->sel >= p->top + fu_list())
		p->top = p->sel - fu_list() + 1;
	if (p->top < 0)
		p->top = 0;
}

static void emit_status(void)
{
	char nbuf[16];
	fu_ent *e = cursel();
	ser("[FILES] L=");
	ser(F.pan[0].path);
	ser(" R=");
	ser(F.pan[1].path);
	ser(" P=");
	ser(F.cur ? "R" : "L");
	ser(" SEL=");
	ser(e ? e->name : "");
	ser(e && e->is_dir ? "/" : "");
	ser(" DIR=");
	ser(e && e->is_dir ? "1" : "0");
	ser(" PATH=");
	ser(curpan()->path);
	ser(" N=");
	fmt_uint(nbuf, (unsigned)curpan()->n);
	ser(nbuf);
	ser(" COLS=");
	fmt_uint(nbuf, (unsigned)fu_cols());
	ser(nbuf);
	ser(" ROWS=");
	fmt_uint(nbuf, (unsigned)fu_rows());
	ser(nbuf);
	ser("\r\n[FILES-LIST]");
	{
		int i;
		fu_panel *p = curpan();
		for (i = 0; i < p->n && i < 24; i++)
		{
			ser(" ");
			ser(p->ent[i].name);
			if (p->ent[i].is_dir)
				ser("/");
		}
	}
	ser("\r\n");
}

static void draw_top_menu(void)
{
	tui_pad(0, 0, " Left   File   Command  Options   Right", fu_cols(),
		FU_MENU_FG, FU_MENU_BG);
}

static void draw_border_row(int row, const char *left_mid, const char *right_mid)
{
	int lw = fu_left_w(), rw = fu_right_w();
	tui_hline(0, row, lw, TUI_TL, TUI_H, TUI_TR, FU_PAN_FG, FU_PAN_BG);
	tui_hline(lw, row, rw, TUI_TL, TUI_H, TUI_TR, FU_PAN_FG, FU_PAN_BG);
	if (left_mid && left_mid[0] && lw > 8)
	{
		int n = (int)strlen(left_mid);
		if (n > lw - 8)
			n = lw - 8;
		tui_put(2, row, ' ', FU_PAN_FG, FU_PAN_BG);
		{
			int i;
			for (i = 0; i < n; i++)
				tui_put(3 + i, row, (unsigned char)left_mid[i], FU_PAN_FG, FU_PAN_BG);
		}
		tui_put(3 + n, row, ' ', FU_PAN_FG, FU_PAN_BG);
	}
	if (right_mid && right_mid[0] && rw > 8)
	{
		int n = (int)strlen(right_mid);
		if (n > rw - 8)
			n = rw - 8;
		tui_put(lw + 2, row, ' ', FU_PAN_FG, FU_PAN_BG);
		{
			int i;
			for (i = 0; i < n; i++)
				tui_put(lw + 3 + i, row, (unsigned char)right_mid[i], FU_PAN_FG, FU_PAN_BG);
		}
		tui_put(lw + 3 + n, row, ' ', FU_PAN_FG, FU_PAN_BG);
	}
}

static void draw_header_cols(void)
{
	int lw = fu_left_w(), rw = fu_right_w();
	int size_w = 11;
	int name_w;
	if (lw < 28)
		size_w = 4;
	name_w = lw - 2 - size_w;
	if (name_w < 4)
		name_w = 4;
	tui_put(0, 2, TUI_V, FU_PAN_FG, FU_PAN_BG);
	tui_pad(1, 2, "Name", name_w, FU_PAN_FG, FU_PAN_BG);
	tui_pad(1 + name_w, 2, "Size", size_w, FU_PAN_FG, FU_PAN_BG);
	tui_put(lw - 1, 2, TUI_V, FU_PAN_FG, FU_PAN_BG);
	tui_put(lw, 2, TUI_V, FU_PAN_FG, FU_PAN_BG);
	tui_pad(lw + 1, 2, "Name", name_w, FU_PAN_FG, FU_PAN_BG);
	tui_pad(lw + 1 + name_w, 2, "Size", size_w, FU_PAN_FG, FU_PAN_BG);
	tui_put(lw + rw - 1, 2, TUI_V, FU_PAN_FG, FU_PAN_BG);
	(void)rw;
}

static void draw_file_row(int side, int vis)
{
	fu_panel *p = &F.pan[side];
	int idx = p->top + vis;
	int row = 3 + vis;
	int active = (side == F.cur);
	int lw = fu_left_w();
	int pw = side ? fu_right_w() : lw;
	int x0 = side ? lw : 0;
	int size_w = (pw < 28) ? 4 : 11;
	int name_w = pw - 2 - size_w;
	int fg, bg;
	char shown[80], szs[16];
	const fu_ent *e = (idx >= 0 && idx < p->n) ? &p->ent[idx] : 0;
	int selected = e && active && idx == p->sel;
	if (name_w < 4)
		name_w = 4;
	shown[0] = 0;
	szs[0] = 0;
	if (e)
	{
		if (e->is_dir)
		{
			shown[0] = '/';
			strncpy(shown + 1, e->name, sizeof(shown) - 2);
			shown[sizeof(shown) - 1] = 0;
			strcpy(szs, "<DIR>");
		}
		else
		{
			strncpy(shown, e->name, sizeof(shown) - 1);
			shown[sizeof(shown) - 1] = 0;
			fmt_uint(szs, (unsigned)e->size);
		}
	}
	if (selected)
	{
		fg = FU_SEL_FG;
		bg = FU_SEL_BG;
	}
	else if (e && e->is_dir)
	{
		fg = FU_DIR_FG;
		bg = FU_DIR_BG;
	}
	else
	{
		fg = FU_PAN_FG;
		bg = FU_PAN_BG;
	}
	tui_put(x0, row, TUI_V, fg, bg);
	tui_pad(x0 + 1, row, shown, name_w, fg, bg);
	tui_pad(x0 + 1 + name_w, row, szs, size_w, fg, bg);
	tui_put(x0 + pw - 1, row, TUI_V, fg, bg);
}

static void draw_hint(void)
{
	const char *h = F.hint[0] ? F.hint
				  : "Tab panels  Enter open/run  q quit  v view  e edit  h help";
	tui_pad(0, fu_rows() - 3, h, fu_cols(), FU_ST_FG, FU_ST_BG);
}

static void draw_prompt_row(void)
{
	char tmp[256];
	tmp[0] = 0;
	if (F.mode == FU_PROMPT)
	{
		const char *lab = "Name: ";
		if (F.prompt_kind == FU_PR_COPY)
			lab = "Copy to: ";
		else if (F.prompt_kind == FU_PR_MOVE)
			lab = "Move to: ";
		else if (F.prompt_kind == FU_PR_MKDIR)
			lab = "MkDir: ";
		else if (F.prompt_kind == FU_PR_DRIVE)
			lab = "Drive: ";
		strncpy(tmp, lab, sizeof(tmp) - 1);
		strncat(tmp, F.prompt, sizeof(tmp) - strlen(tmp) - 1);
	}
	else if (F.mode == FU_CONFIRM)
	{
		strncpy(tmp, "Delete ", sizeof(tmp) - 1);
		strncat(tmp, F.prompt, sizeof(tmp) - strlen(tmp) - 1);
		strncat(tmp, " ? (Y/N)", sizeof(tmp) - strlen(tmp) - 1);
	}
	else if (F.filter[0])
	{
		strncpy(tmp, "Filter: ", sizeof(tmp) - 1);
		strncat(tmp, F.filter, sizeof(tmp) - strlen(tmp) - 1);
	}
	else
		strncpy(tmp, "$  (type A: C: to change drive)", sizeof(tmp) - 1);
	tui_pad(0, fu_rows() - 2, tmp, fu_cols(), FU_ST_FG, FU_ST_BG);
}

static void fkey_slot(int *x, int y, const char *num, const char *lab)
{
	int nlen = (int)strlen(num);
	int llen = (int)strlen(lab);
	tui_puts(*x, y, num, FU_FN_FG, FU_FN_BG);
	*x += nlen;
	tui_puts(*x, y, lab, FU_FL_FG, FU_FL_BG);
	*x += llen;
}

static void draw_fkeys(void)
{
	int y = fu_rows() - 1;
	int x = 0;
	fkey_slot(&x, y, " 1", "Help ");
	fkey_slot(&x, y, " 3", "View ");
	fkey_slot(&x, y, " 4", "Edit ");
	fkey_slot(&x, y, " 5", "Copy ");
	fkey_slot(&x, y, " 6", "Move ");
	fkey_slot(&x, y, " 7", "MkDir");
	fkey_slot(&x, y, " 8", "Del  ");
	fkey_slot(&x, y, " 9", "Menu ");
	fkey_slot(&x, y, " Q", "Quit ");
	if (x < fu_cols())
		tui_pad(x, y, "", fu_cols() - x, FU_FN_FG, FU_FN_BG);
}

static void draw_overlay_box(const char *title, const char **lines, int nlines)
{
	int w = fu_cols() - 16;
	int h, r0, c0, i;
	if (w > 56)
		w = 56;
	if (w < 28)
		w = fu_cols() > 28 ? 28 : fu_cols() - 2;
	h = nlines + 4;
	r0 = (fu_rows() - h) / 2;
	c0 = (fu_cols() - w) / 2;
	if (r0 < 1)
		r0 = 1;
	if (c0 < 1)
		c0 = 1;
	tui_fill(c0, r0, w, h, ' ', TUI_WHITE, TUI_BLACK);
	tui_frame(c0, r0, w, h, FU_MENU_FG, FU_MENU_BG);
	tui_pad(c0 + 1, r0 + 1, title, w - 2, FU_MENU_FG, FU_MENU_BG);
	for (i = 0; i < nlines; i++)
		tui_pad(c0 + 1, r0 + 2 + i, lines[i] ? lines[i] : "", w - 2,
			TUI_WHITE, TUI_BLACK);
}

static void draw_help(void)
{
	const char *lines[] = {
		"Arrows move   Tab switch panel   BS parent",
		"Enter dir=open  .BAS=RUN  image=view  audio=play",
		"v/F3 view   e/F4 edit   c/F5 copy   m/F6 move",
		"k/F7 mkdir  d/F8 delete  F9 menu   q/Esc quit",
		"Type A: or C: to change the active panel drive",
		"Any key closes this help",
	};
	draw_overlay_box(" FILES  (Midnight Commander style) ", lines, 6);
}

static void draw_menu(void)
{
	const char *lines[] = {
		"1  Drive A:     3  Drive C:     4  Drive D:",
		"v  View         e  Edit         c  Copy",
		"m  Move         k  MkDir        d  Delete",
		"h  Help         q  Quit         Esc close",
	};
	draw_overlay_box(" Menu ", lines, 4);
}

static void draw_info(void)
{
	char l1[48], l2[48], l3[48], nbuf[16];
	const char *lines[4];
	strncpy(l1, "Name: ", sizeof(l1) - 1);
	strncat(l1, F.info_name, sizeof(l1) - strlen(l1) - 1);
	strncpy(l2, "Size: ", sizeof(l2) - 1);
	fmt_uint(nbuf, (unsigned)(F.info_size < 0 ? 0 : F.info_size));
	strncat(l2, nbuf, sizeof(l2) - strlen(l2) - 1);
	strncpy(l3, "Path: ", sizeof(l3) - 1);
	strncat(l3, F.info_path, sizeof(l3) - strlen(l3) - 1);
	lines[0] = l1;
	lines[1] = l2;
	lines[2] = l3;
	lines[3] = "Any key returns to FILES";
	draw_overlay_box(" File info ", lines, 4);
}

static void draw_play(void)
{
	char l1[48];
	const char *lines[3];
	strncpy(l1, "Playing ", sizeof(l1) - 1);
	strncat(l1, F.info_name, sizeof(l1) - strlen(l1) - 1);
	lines[0] = l1;
	lines[1] = "Enter or Esc stops playback";
	lines[2] = "";
	draw_overlay_box(" PLAY ", lines, 3);
}

static void files_draw(void)
{
	int i;
	fu_ent *e;
	char footL[80], footR[80];
	int namew;
	tui_begin();
	tui_clear(FU_PAN_FG, FU_PAN_BG);
	draw_top_menu();
	draw_border_row(1, F.pan[0].path, F.pan[1].path);
	draw_header_cols();
	for (i = 0; i < fu_list(); i++)
	{
		draw_file_row(0, i);
		draw_file_row(1, i);
	}
	namew = fu_left_w() - 6;
	if (namew > 70)
		namew = 70;
	if (namew < 4)
		namew = 4;
	e = F.pan[0].n ? &F.pan[0].ent[F.pan[0].sel] : 0;
	pad(footL, namew, e ? e->name : "", 0);
	e = F.pan[1].n ? &F.pan[1].ent[F.pan[1].sel] : 0;
	pad(footR, namew, e ? e->name : "", 0);
	draw_border_row(3 + fu_list(), footL, footR);
	draw_hint();
	draw_prompt_row();
	draw_fkeys();
	if (F.mode == FU_HELP)
		draw_help();
	else if (F.mode == FU_MENU)
		draw_menu();
	else if (F.mode == FU_INFO)
		draw_info();
	else if (F.mode == FU_PLAY)
		draw_play();
	tui_flush();
	emit_status();
}

static void set_hint(const char *s)
{
	strncpy(F.hint, s ? s : "", sizeof(F.hint) - 1);
}

static void files_close_tui(void)
{
	F.active = 0;
	F.mode = FU_BROWSE;
	F.esc = 0;
	tui_end();
	ser("\r\n");
}

static void chdir_panel(fu_panel *p)
{
	if (p->path[0])
		mmb_vfs_chdir(p->path);
}

static void enter_dir(void)
{
	fu_ent *e = cursel();
	char next[FU_PATH];
	if (!e || !e->is_dir)
		return;
	join_path(next, sizeof(next), curpan()->path, e->name);
	strncpy(curpan()->path, next, FU_PATH - 1);
	curpan()->sel = 0;
	curpan()->top = 0;
	panel_reload(curpan());
	chdir_panel(curpan());
	set_hint("Opened directory");
}

static void show_info_for(const char *path, const char *name)
{
	strncpy(F.info_path, path, sizeof(F.info_path) - 1);
	strncpy(F.info_name, name, sizeof(F.info_name) - 1);
	F.info_size = mmb_vfs_size(path);
	F.mode = FU_INFO;
	set_hint("File info");
}

static void do_preview(const char *path, const char *name)
{
	(void)name;
	mmb_gfx_cls(0);
	if (is_img(name))
	{
		if (mmb_keyword_eq(ext_of(name), ".JPG") || mmb_keyword_eq(ext_of(name), ".JPEG"))
		{
			if (mmb_load_jpeg(path, 0, 0) != 0)
			{
				show_info_for(path, name);
				return;
			}
		}
		else if (mmb_load_png(path, 0, 0) != 0)
		{
			show_info_for(path, name);
			return;
		}
		F.mode = FU_PREVIEW;
		set_hint("Image preview  any key returns");
		ser("[FILES] PREVIEW ");
		ser(name);
		ser("\r\n");
		return;
	}
	show_info_for(path, name);
}

static void do_play(const char *path, const char *name)
{
	int rc = -1;
	const char *e = ext_of(name);
	if (mmb_keyword_eq(e, ".MP3"))
		rc = mmb_play_mp3(path);
	else if (mmb_keyword_eq(e, ".MOD"))
		rc = mmb_play_mod(path);
	else if (mmb_keyword_eq(e, ".XM"))
		rc = mmb_play_xm(path);
	if (rc != 0)
	{
		show_info_for(path, name);
		set_hint("Cannot play this file");
		return;
	}
	strncpy(F.info_name, name, sizeof(F.info_name) - 1);
	strncpy(F.info_path, path, sizeof(F.info_path) - 1);
	F.mode = FU_PLAY;
	set_hint("Playing  Enter/Esc stop");
	ser("[FILES] PLAY ");
	ser(name);
	ser("\r\n");
}

static void do_run(const char *path)
{
	char cmd[160];
	files_close_tui();
	strcpy(cmd, "RUN \"");
	strncat(cmd, path, sizeof(cmd) - 8);
	strcat(cmd, "\"");
	mmb_exec_line(cmd);
}

static void do_edit(const char *path)
{
	F.mode = FU_BROWSE;
	mmb_editor_open(path);
}

static void start_prompt(int kind, const char *seed)
{
	F.mode = FU_PROMPT;
	F.prompt_kind = kind;
	strncpy(F.prompt, seed ? seed : "", sizeof(F.prompt) - 1);
}

static void start_copy_or_move(int move)
{
	fu_ent *e = cursel();
	char dst[FU_PATH];
	if (!e || is_dotdot(e))
	{
		set_hint("Nothing to copy");
		return;
	}
	if (e->is_dir)
	{
		set_hint("Directory copy is not supported");
		return;
	}
	join_path(dst, sizeof(dst), otherpan()->path, e->name);
	start_prompt(move ? FU_PR_MOVE : FU_PR_COPY, dst);
}

static void start_mkdir(void)
{
	start_prompt(FU_PR_MKDIR, "");
}

static void start_delete(void)
{
	fu_ent *e = cursel();
	char path[FU_PATH];
	if (!e || is_dotdot(e))
	{
		set_hint("Nothing to delete");
		return;
	}
	sel_path(path, sizeof(path));
	F.mode = FU_CONFIRM;
	strncpy(F.prompt, path, sizeof(F.prompt) - 1);
}

static void apply_prompt(void)
{
	int kind = F.prompt_kind;
	char src[FU_PATH];
	F.mode = FU_BROWSE;
	if (kind == FU_PR_COPY || kind == FU_PR_MOVE)
	{
		sel_path(src, sizeof(src));
		if (kind == FU_PR_COPY)
		{
			if (mmb_vfs_copy(src, F.prompt) != 0)
				set_hint("Copy failed");
			else
				set_hint("Copied");
		}
		else
		{
			if (mmb_vfs_rename(src, F.prompt) != 0)
				set_hint("Move failed");
			else
				set_hint("Moved");
		}
		reload_all();
	}
	else if (kind == FU_PR_MKDIR)
	{
		char full[FU_PATH];
		if (!F.prompt[0])
			return;
		join_path(full, sizeof(full), curpan()->path, F.prompt);
		if (mmb_vfs_mkdir(full) != 0)
			set_hint("MkDir failed");
		else
			set_hint("Directory created");
		reload_all();
	}
	else if (kind == FU_PR_DRIVE)
	{
		int letter = F.prompt[0];
		if (letter >= 'a' && letter <= 'z')
			letter = (char)(letter - 32);
		if (letter >= 'A' && letter <= 'H')
		{
			char spec[8];
			spec[0] = (char)letter;
			spec[1] = ':';
			spec[2] = 0;
			if (letter != 'A' && !mmb_fat_ready(letter))
				set_hint("Drive not ready");
			else
			{
				strncpy(curpan()->path, spec, 4);
				curpan()->path[2] = '/';
				curpan()->path[3] = 0;
				curpan()->sel = 0;
				panel_reload(curpan());
				chdir_panel(curpan());
				set_hint("Drive changed");
			}
		}
	}
}

static void apply_delete(void)
{
	fu_ent *e = cursel();
	F.mode = FU_BROWSE;
	if (!e)
		return;
	if (e->is_dir)
	{
		if (mmb_vfs_rmdir(F.prompt) != 0)
			set_hint("Delete failed");
		else
			set_hint("Directory removed");
	}
	else
	{
		if (mmb_vfs_kill(F.prompt) != 0)
			set_hint("Delete failed");
		else
			set_hint("Deleted");
	}
	reload_all();
}

static void activate_enter(void)
{
	fu_ent *e = cursel();
	char path[FU_PATH];
	if (!e)
		return;
	sel_path(path, sizeof(path));
	if (e->is_dir)
	{
		enter_dir();
		return;
	}
	if (is_bas(e->name))
	{
		do_run(path);
		return;
	}
	if (is_img(e->name))
	{
		do_preview(path, e->name);
		return;
	}
	if (is_aud(e->name))
	{
		do_play(path, e->name);
		return;
	}
	show_info_for(path, e->name);
}

static void do_view(void)
{
	fu_ent *e = cursel();
	char path[FU_PATH];
	if (!e)
		return;
	sel_path(path, sizeof(path));
	if (e->is_dir)
	{
		show_info_for(path, e->name);
		return;
	}
	if (is_img(e->name))
		do_preview(path, e->name);
	else if (is_aud(e->name))
		do_play(path, e->name);
	else
		show_info_for(path, e->name);
}

static void set_drive_letter(int letter)
{
	char spec[8];
	if (letter >= 'a' && letter <= 'z')
		letter -= 32;
	if (letter < 'A' || letter > 'H')
		return;
	if (letter != 'A' && !mmb_fat_ready(letter))
	{
		set_hint("Drive not ready");
		return;
	}
	spec[0] = (char)letter;
	spec[1] = ':';
	spec[2] = '/';
	spec[3] = 0;
	strncpy(curpan()->path, spec, FU_PATH - 1);
	curpan()->sel = 0;
	curpan()->top = 0;
	panel_reload(curpan());
	chdir_panel(curpan());
	set_hint("Drive changed");
}

static void close_overlay(void)
{
	if (F.mode == FU_PLAY)
		mmb_play_stop();
	if (F.mode == FU_PREVIEW)
		mmb_gfx_cls(0x000028);
	F.mode = FU_BROWSE;
	set_hint("");
	tui_invalidate();
}

static void files_open(const char *start)
{
	char cwd[FU_PATH];
	memset(&F, 0, sizeof(F));
	F.active = 1;
	F.mode = FU_BROWSE;
	strncpy(cwd, start && start[0] ? start : mmb_vfs_cwd(), sizeof(cwd) - 1);
	if (!strchr(cwd, ':'))
	{
		char tmp[FU_PATH];
		tmp[0] = (char)(G.drive ? G.drive : 'A');
		tmp[1] = ':';
		tmp[2] = 0;
		if (cwd[0] != '/')
			strcat(tmp, "/");
		strncat(tmp, cwd, sizeof(tmp) - strlen(tmp) - 1);
		strncpy(cwd, tmp, sizeof(cwd) - 1);
	}
	if (cwd[2] == 0)
		strcat(cwd, "/");
	strncpy(F.pan[0].path, cwd, FU_PATH - 1);
	strncpy(F.pan[1].path, cwd, FU_PATH - 1);
	panel_reload(&F.pan[0]);
	panel_reload(&F.pan[1]);
	set_hint("Tab panels  Enter open/run  q quit  v view  e edit");
	files_draw();
}

static void handle_fkey(int n)
{
	if (F.mode == FU_PREVIEW || F.mode == FU_INFO || F.mode == FU_HELP)
	{
		close_overlay();
		return;
	}
	if (F.mode == FU_PLAY)
	{
		close_overlay();
		return;
	}
	if (n == 1)
		F.mode = FU_HELP;
	else if (n == 3)
		do_view();
	else if (n == 4)
	{
		fu_ent *e = cursel();
		char path[FU_PATH];
		if (e && !e->is_dir)
		{
			sel_path(path, sizeof(path));
			do_edit(path);
		}
	}
	else if (n == 5)
		start_copy_or_move(0);
	else if (n == 6)
		start_copy_or_move(1);
	else if (n == 7)
		start_mkdir();
	else if (n == 8)
		start_delete();
	else if (n == 9)
		F.mode = FU_MENU;
	else if (n == 10)
		files_close_tui();
}

static void handle_arrow(int which)
{
	fu_panel *p = curpan();
	if (F.mode == FU_PREVIEW || F.mode == FU_INFO || F.mode == FU_HELP ||
	    F.mode == FU_PLAY)
	{
		if (F.mode != FU_PLAY || which == 0)
			close_overlay();
		return;
	}
	if (F.mode != FU_BROWSE && F.mode != FU_MENU)
		return;
	if (which == 0) /* up */
	{
		if (p->sel > 0)
			p->sel--;
		clamp_sel(p);
	}
	else if (which == 1) /* down */
	{
		if (p->sel + 1 < p->n)
			p->sel++;
		clamp_sel(p);
	}
	else if (which == 2) /* right: switch? keep as no-op or tab */
		;
	else if (which == 3) /* left */
		;
}

static int handle_esc_char(char c)
{
	if (F.esc == 1)
	{
		if (c == '[' || c == 'O')
		{
			F.esc = (c == '[') ? 2 : 5;
			F.csi_n = 0;
			return 1;
		}
		F.esc = 0;
		if (F.mode == FU_BROWSE)
			files_close_tui();
		else
			close_overlay();
		return 1;
	}
	if (F.esc == 2)
	{
		if (c == '[')
		{
			F.esc = 3;
			return 1;
		}
		if (c == 'A')
			handle_arrow(0);
		else if (c == 'B')
			handle_arrow(1);
		else if (c == 'C')
			handle_arrow(2);
		else if (c == 'D')
			handle_arrow(3);
		else if (c >= '0' && c <= '9')
		{
			F.csi_n = c - '0';
			F.esc = 4;
			return 1;
		}
		F.esc = 0;
		return 1;
	}
	if (F.esc == 3)
	{
		F.esc = 0;
		if (c == 'A')
			handle_fkey(1);
		else if (c == 'B')
			handle_fkey(2);
		else if (c == 'C')
			handle_fkey(3);
		else if (c == 'D')
			handle_fkey(4);
		else if (c == 'E')
			handle_fkey(5);
		return 1;
	}
	if (F.esc == 4)
	{
		if (c >= '0' && c <= '9')
		{
			F.csi_n = F.csi_n * 10 + (c - '0');
			return 1;
		}
		F.esc = 0;
		if (c == '~')
		{
			if (F.csi_n == 17)
				handle_fkey(6);
			else if (F.csi_n == 18)
				handle_fkey(7);
			else if (F.csi_n == 19)
				handle_fkey(8);
			else if (F.csi_n == 20)
				handle_fkey(9);
			else if (F.csi_n == 21)
				handle_fkey(10);
		}
		return 1;
	}
	if (F.esc == 5)
	{
		F.esc = 0;
		if (c == 'A')
			handle_arrow(0);
		else if (c == 'B')
			handle_arrow(1);
		return 1;
	}
	return 0;
}

static void handle_letter(char c)
{
	char lc = c;
	if (lc >= 'A' && lc <= 'Z')
		lc = (char)(lc + 32);
	if (F.mode == FU_HELP || F.mode == FU_INFO || F.mode == FU_PREVIEW)
	{
		close_overlay();
		return;
	}
	if (F.mode == FU_PLAY)
	{
		if (lc == 'q' || c == 27)
			close_overlay();
		return;
	}
	if (F.mode == FU_MENU)
	{
		if (lc == 'q')
			files_close_tui();
		else if (lc == 'h')
			F.mode = FU_HELP;
		else if (lc == 'v')
		{
			F.mode = FU_BROWSE;
			do_view();
		}
		else if (lc == 'e')
		{
			fu_ent *e = cursel();
			char path[FU_PATH];
			F.mode = FU_BROWSE;
			if (e && !e->is_dir)
			{
				sel_path(path, sizeof(path));
				do_edit(path);
			}
		}
		else if (lc == 'c')
		{
			F.mode = FU_BROWSE;
			start_copy_or_move(0);
		}
		else if (lc == 'm')
		{
			F.mode = FU_BROWSE;
			start_copy_or_move(1);
		}
		else if (lc == 'k')
		{
			F.mode = FU_BROWSE;
			start_mkdir();
		}
		else if (lc == 'd')
		{
			F.mode = FU_BROWSE;
			start_delete();
		}
		else if (c == '1')
		{
			F.mode = FU_BROWSE;
			set_drive_letter('A');
		}
		else if (c == '3')
		{
			F.mode = FU_BROWSE;
			set_drive_letter('C');
		}
		else if (c == '4')
		{
			F.mode = FU_BROWSE;
			set_drive_letter('D');
		}
		return;
	}
	if (F.mode == FU_CONFIRM)
	{
		if (lc == 'y')
			apply_delete();
		else if (lc == 'n' || lc == 'q')
			F.mode = FU_BROWSE;
		return;
	}
	if (F.mode == FU_PROMPT)
		return;
	/* Shortcuts are lowercase so A: / C: can change drive. */
	if (c >= 'A' && c <= 'Z')
		return;
	if (lc == 'q')
	{
		files_close_tui();
		return;
	}
	if (lc == 'h')
	{
		F.mode = FU_HELP;
		return;
	}
	if (lc == 'v')
	{
		do_view();
		return;
	}
	if (lc == 'e')
	{
		fu_ent *e = cursel();
		char path[FU_PATH];
		if (e && !e->is_dir)
		{
			sel_path(path, sizeof(path));
			do_edit(path);
		}
		return;
	}
	if (lc == 'c')
	{
		start_copy_or_move(0);
		return;
	}
	if (lc == 'm')
	{
		start_copy_or_move(1);
		return;
	}
	if (lc == 'k')
	{
		start_mkdir();
		return;
	}
	if (lc == 'd')
	{
		start_delete();
		return;
	}
}

static void handle_prompt_char(char c)
{
	int n = (int)strlen(F.prompt);
	if (c == 8 || c == 127)
	{
		if (n > 0)
			F.prompt[n - 1] = 0;
		return;
	}
	if (c == '\r' || c == '\n')
	{
		apply_prompt();
		return;
	}
	if (c >= 32 && c < 127 && n < (int)sizeof(F.prompt) - 1)
	{
		F.prompt[n] = c;
		F.prompt[n + 1] = 0;
	}
}

void mmb_cmd_files_ui(void)
{
	char spec[128];
	spec[0] = 0;
	mmb_skip_sp();
	if (*G.p && *G.p != ':' && *G.p != '\'')
	{
		mmb_val v = mmb_expr();
		if (v.type == T_STR)
			strncpy(spec, v.s, sizeof(spec) - 1);
	}
	files_open(spec[0] ? spec : mmb_vfs_cwd());
}

int mmb_in_files(void)
{
	return F.active;
}

void mmb_files_close(void)
{
	if (F.active)
		files_close_tui();
}

const char *mmb_files_resume(void)
{
	G.outn = 0;
	G.out[0] = 0;
	if (F.active)
		files_draw();
	return G.out;
}

const char *mmb_files_on_editor_exit(void)
{
	G.outn = 0;
	G.out[0] = 0;
	if (!F.active)
		return G.out;
	if (G.ed.run_on_exit)
	{
		files_close_tui();
		return G.out;
	}
	files_draw();
	return G.out;
}

const char *mmb_files_key(char c)
{
	int was_active;
	G.outn = 0;
	G.out[0] = 0;
	if (!F.active)
		return G.out;
	was_active = 1;
	if (F.esc)
	{
		handle_esc_char(c);
		if (F.active && F.mode != FU_PREVIEW)
			files_draw();
		return G.out;
	}
	if (c == 27)
	{
		F.esc = 1;
		return G.out;
	}
	if (F.mode == FU_PREVIEW)
	{
		close_overlay();
		files_draw();
		return G.out;
	}
	if (F.mode == FU_PROMPT)
	{
		if (c == 27)
		{
			F.mode = FU_BROWSE;
			files_draw();
			return G.out;
		}
		handle_prompt_char(c);
		if (F.active)
			files_draw();
		return G.out;
	}
	if (c == '\t')
	{
		if (F.mode == FU_BROWSE)
		{
			F.cur ^= 1;
			chdir_panel(curpan());
			set_hint(F.cur ? "Right panel" : "Left panel");
		}
		if (F.active)
			files_draw();
		return G.out;
	}
	if (c == 8 || c == 127)
	{
		if (F.mode == FU_BROWSE)
		{
			fu_ent *e;
			curpan()->sel = 0;
			/* parent via .. */
			if (curpan()->n > 0)
			{
				e = &curpan()->ent[0];
				(void)e;
			}
			{
				char next[FU_PATH];
				join_path(next, sizeof(next), curpan()->path, "..");
				strncpy(curpan()->path, next, FU_PATH - 1);
				curpan()->sel = 0;
				curpan()->top = 0;
				panel_reload(curpan());
				chdir_panel(curpan());
				set_hint("Parent directory");
			}
		}
		else if (F.mode == FU_CONFIRM || F.mode == FU_MENU || F.mode == FU_HELP ||
			 F.mode == FU_INFO || F.mode == FU_PLAY)
			close_overlay();
		if (F.active)
			files_draw();
		return G.out;
	}
	if (c == '\r' || c == '\n')
	{
		if (F.mode == FU_PLAY)
			close_overlay();
		else if (F.mode == FU_HELP || F.mode == FU_INFO || F.mode == FU_MENU)
			close_overlay();
		else if (F.mode == FU_CONFIRM)
			apply_delete();
		else if (F.mode == FU_BROWSE)
			activate_enter();
		if (F.active)
			files_draw();
		return G.out;
	}
	if (c == ':')
	{
		if (F.pend_drive)
		{
			set_drive_letter(F.pend_drive);
			F.pend_drive = 0;
			if (F.active)
				files_draw();
			return G.out;
		}
	}
	if ((c >= 'A' && c <= 'H') || (c >= 'a' && c <= 'h'))
		F.pend_drive = (c >= 'a') ? (c - 32) : c;
	else
		F.pend_drive = 0;
	if (c >= 32 && c < 127)
		handle_letter(c);
	if (was_active && F.active && F.mode != FU_PREVIEW)
		files_draw();
	return G.out;
}
