#include "mmb_priv.h"
#include "mmb_tdf.h"
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
#define FU_VIEW     8
#define FU_FTP      9
#define FU_ANSI     10
#define FU_TDF      11

#define AN_MAX_COLS TUI_MAX_COLS
#define AN_MAX_ROWS 512
#define AN_MAX_BYTES (1024u * 1024u)
#define AN_COLS 80

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
	int truncated; /* directory had more entries than FU_MAX_ENT (#621) */
} fu_panel;

typedef struct {
	int active;
	fu_panel pan[2];
	int cur;
	int mode;
	int prompt_kind;
	int esc;
	unsigned esc_at;
	int csi_n;
	int pend_drive;
	int menu_i;
	char filter[32];
	char prompt[FU_PATH];
	char hint[96];
	char info_name[FU_NAME];
	char info_path[FU_PATH];
	int info_size;
	int drop; /* -1 none, 0..4 aligned dropdown */
	int drop_item;
	int menu_x[5];
	int alt;
	char view_path[FU_PATH];
	char view_buf[4096];
	int view_len;
	int view_top;
	int pv_saved;
	int pv_mode;
	int pv_bits;
	char ftp_root[FU_PATH];
	char ftp_addr[64];
	char ftp_last[96];
	int an_top;
	int an_rows;
	int an_mode80;
	int an_saved_mode;
	int an_saved_bits;
	unsigned char *tdf_buf;
	unsigned tdf_size;
	int tdf_variants;
	mmb_tdf tdf;
} fu_state;

static fu_state F_s[MMB_MAX_CONSOLES];
#define F (F_s[g_console])

typedef struct {
	unsigned char ch;
	unsigned char fg;
	unsigned char bg;
} an_cell;

static an_cell an_scr[AN_MAX_ROWS][AN_MAX_COLS];
static int an_p_cols;
static int an_p_row, an_p_col;
static int an_p_fg, an_p_bg, an_p_bold, an_p_inv;
static int an_p_sav_row, an_p_sav_col;
static int an_p_st;
static int an_p_args[8], an_p_narg, an_p_priv;

#define FU_MENU_FG ((int)mmb_editor_theme()->menu_fg)
#define FU_MENU_BG ((int)mmb_editor_theme()->menu_bg)
#define FU_PAN_FG  ((int)mmb_editor_theme()->brd_fg)
#define FU_PAN_BG  ((int)mmb_editor_theme()->brd_bg)
#define FU_SEL_FG  ((int)mmb_editor_theme()->sel_fg)
#define FU_SEL_BG  ((int)mmb_editor_theme()->sel_bg)
#define FU_DIR_FG  ((int)mmb_editor_theme()->str_fg)
#define FU_DIR_BG  ((int)mmb_editor_theme()->edit_bg)
#define FU_ST_FG   ((int)mmb_editor_theme()->cmt_fg)
#define FU_ST_BG   ((int)mmb_editor_theme()->edit_bg)
#define FU_FN_FG   ((int)mmb_editor_theme()->tab_fg)
#define FU_FN_BG   ((int)mmb_editor_theme()->edit_bg)
#define FU_FL_FG   ((int)mmb_editor_theme()->menu_fg)
#define FU_FL_BG   ((int)mmb_editor_theme()->list_bg)
#define FU_HOT     ((int)mmb_editor_theme()->hot)
#define FU_DLG_FG  ((int)mmb_editor_theme()->dlg_fg)
#define FU_DLG_BG  ((int)mmb_editor_theme()->dlg_bg)
#define FU_EDIT_FG ((int)mmb_editor_theme()->edit_fg)
#define FU_EDIT_BG ((int)mmb_editor_theme()->edit_bg)
#define FU_CMT_FG  ((int)mmb_editor_theme()->cmt_fg)
#define FU_STR_FG  ((int)mmb_editor_theme()->str_fg)
#define FU_NUM_FG  ((int)mmb_editor_theme()->num_fg)

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
	       mmb_keyword_eq(e, ".JPEG") || mmb_keyword_eq(e, ".PCX");
}

static int is_aud(const char *n)
{
	const char *e = ext_of(n);
	return mmb_keyword_eq(e, ".MP3") || mmb_keyword_eq(e, ".XM") ||
	       mmb_keyword_eq(e, ".MOD") || mmb_keyword_eq(e, ".WAV");
}

static int is_ansi(const char *n)
{
	const char *e = ext_of(n);
	return mmb_keyword_eq(e, ".ANS") || mmb_keyword_eq(e, ".ASC") ||
	       mmb_keyword_eq(e, ".DIZ");
}

static int is_tdf(const char *n)
{
	return mmb_keyword_eq(ext_of(n), ".TDF");
}

static int is_text(const char *n)
{
	const char *e = ext_of(n);
	return is_bas(n) || mmb_keyword_eq(e, ".TXT") || mmb_keyword_eq(e, ".MD");
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
	static mmb_dirent ents[FU_MAX_ENT];
	char keep[FU_NAME];
	int oldsel = p->sel;
	int n = 0, i, truncated = 0;
	keep[0] = 0;
	if (oldsel >= 0 && oldsel < p->n)
		strncpy(keep, p->ent[oldsel].name, FU_NAME - 1);
	p->n = 0;
	panel_add(p, "..", 1, 0);
	/* One structured scan gives name, type and size, so the panel no longer
	 * stats every file just to fill the size column (#621). */
	n = mmb_vfs_list_entries(p->path, ents, FU_MAX_ENT, &truncated);
	if (n < 0)
		n = 0;
	for (i = 0; i < n; i++)
	{
		int sz = ents[i].size < 0 ? 0 : ents[i].size;
		if (ents[i].name[0] == '.' &&
		    (ents[i].name[1] == 0 ||
		     (ents[i].name[1] == '.' && ents[i].name[2] == 0)))
			continue;
		panel_add(p, ents[i].name, ents[i].is_dir, sz);
	}
	p->truncated = truncated;
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

static void fu_caption(const char *path, char *out, int outsz);

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
	if (curpan()->truncated)
		ser(" MORE=1"); /* listing was cut at FU_MAX_ENT (#621) */
	ser(" COLS=");
	fmt_uint(nbuf, (unsigned)fu_cols());
	ser(nbuf);
	ser(" ROWS=");
	fmt_uint(nbuf, (unsigned)fu_rows());
	ser(nbuf);
	ser(" VOL=");
	{
		char cap[96];
		fu_caption(curpan()->path, cap, sizeof cap);
		ser(cap);
	}
	ser(" HINT=");
	ser(F.hint);
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
	int x = 0;
	const char *names[] = { "Left", "File", "Command", "Options", "Right" };
	const char hots[] = { 'L', 'F', 'C', 'O', 'R' };
	int i;
	tui_put(x++, 0, ' ', FU_MENU_FG, FU_MENU_BG);
	for (i = 0; i < 5; i++)
	{
		int j, sel = (F.drop == i);
		int fg, bg;
		fg = sel ? FU_SEL_FG : FU_MENU_FG;
		bg = sel ? FU_SEL_BG : FU_MENU_BG;
		F.menu_x[i] = x;
		for (j = 0; names[i][j]; j++)
		{
			int c_fg = fg;
			if (names[i][j] == hots[i] || names[i][j] == hots[i] + 32)
				c_fg = FU_HOT;
			tui_put(x++, 0, (unsigned char)names[i][j], c_fg, bg);
		}
		tui_put(x++, 0, ' ', FU_MENU_FG, FU_MENU_BG);
		tui_put(x++, 0, ' ', FU_MENU_FG, FU_MENU_BG);
	}
	if (x < fu_cols())
		tui_pad(x, 0, "", fu_cols() - x, FU_MENU_FG, FU_MENU_BG);
}

static const char **drop_items(int menu, int *n, const char **hots)
{
	static const char *left[] = { "Drive A:", "Drive C:", "Drive D:" };
	static const char left_h[] = { 'a', 'c', 'd' };
	static const char *file[] = { "View", "Edit", "Copy", "Move", "Delete" };
	static const char file_h[] = { 'v', 'e', 'c', 'm', 'd' };
	static const char *cmd[] = { "MkDir", "Eject", "FTP server", "Help", "Quit" };
	static const char cmd_h[] = { 'k', 'e', 's', 'h', 'q' };
	static const char *opt[] = { "Help" };
	static const char opt_h[] = { 'h' };
	static const char *right[] = { "Focus right", "Drive A:", "Drive C:", "Drive D:" };
	static const char right_h[] = { 'f', 'a', 'c', 'd' };
	switch (menu)
	{
	case 0: *n = 3; *hots = left_h; return left;
	case 1: *n = 5; *hots = file_h; return file;
	case 2: *n = 5; *hots = cmd_h; return cmd;
	case 3: *n = 1; *hots = opt_h; return opt;
	default: *n = 4; *hots = right_h; return right;
	}
}

static void draw_files_dropdown(void)
{
	int n, i, w, r0, c0;
	const char *hots = 0;
	const char **it;
	if (F.drop < 0)
		return;
	it = drop_items(F.drop, &n, &hots);
	w = 14;
	for (i = 0; i < n; i++)
	{
		int L = (int)strlen(it[i]) + 4;
		if (L > w)
			w = L;
	}
	c0 = F.menu_x[F.drop];
	if (c0 + w + 2 > fu_cols())
		c0 = fu_cols() - w - 2;
	if (c0 < 0)
		c0 = 0;
	r0 = 1;
	tui_hline(c0, r0, w, TUI_TL, TUI_H, TUI_TR, FU_MENU_FG, FU_MENU_BG);
	for (i = 0; i < n; i++)
	{
		int sel = (i == F.drop_item);
		int fg = sel ? FU_SEL_FG : FU_MENU_FG;
		int bg = sel ? FU_SEL_BG : FU_MENU_BG;
		tui_put(c0, r0 + 1 + i, TUI_V, FU_MENU_FG, FU_MENU_BG);
		tui_put(c0 + 1, r0 + 1 + i, ' ', fg, bg);
		tui_pad(c0 + 2, r0 + 1 + i, it[i], w - 4, fg, bg);
		tui_put(c0 + w - 2, r0 + 1 + i, ' ', fg, bg);
		tui_put(c0 + w - 1, r0 + 1 + i, TUI_V, FU_MENU_FG, FU_MENU_BG);
	}
	tui_hline(c0, r0 + 1 + n, w, TUI_BL, TUI_H, TUI_BR, FU_MENU_FG, FU_MENU_BG);
}

static int line_is_comment(const char *s, int n)
{
	int i = 0;
	while (i < n && (s[i] == ' ' || s[i] == '\t'))
		i++;
	while (i < n && s[i] >= '0' && s[i] <= '9')
		i++;
	while (i < n && (s[i] == ' ' || s[i] == '\t'))
		i++;
	if (i < n && s[i] == '\'')
		return 1;
	if (i + 2 < n)
	{
		char a = s[i], b = s[i + 1], c = s[i + 2];
		if (a >= 'a' && a <= 'z') a = (char)(a - 32);
		if (b >= 'a' && b <= 'z') b = (char)(b - 32);
		if (c >= 'a' && c <= 'z') c = (char)(c - 32);
		if (a == 'R' && b == 'E' && c == 'M' &&
		    (i + 3 >= n || s[i + 3] == ' ' || s[i + 3] == '\t'))
			return 1;
	}
	return 0;
}

static void draw_syntax_span(int x, int y, const char *s, int n, int width)
{
	int i, shown = 0, in_str = 0, in_cmt;
	in_cmt = line_is_comment(s, n);
	for (i = 0; i < n && shown < width; i++)
	{
		char ch = s[i];
		int fg;
		if (!in_cmt && !in_str && ch == '\'')
			in_cmt = 1;
		if (!in_cmt && !in_str && ch == '"')
			in_str = 1;
		else if (in_str && ch == '"')
			in_str = 2;
		if (in_cmt)
			fg = FU_CMT_FG;
		else if (in_str)
			fg = FU_STR_FG;
		else if (ch >= '0' && ch <= '9')
			fg = FU_NUM_FG;
		else
			fg = FU_EDIT_FG;
		tui_put(x + shown, y, (unsigned char)((ch == '\t') ? ' ' : ch), fg, FU_EDIT_BG);
		shown++;
		if (in_str == 2)
			in_str = 0;
	}
	if (shown < width)
		tui_pad(x + shown, y, "", width - shown, FU_EDIT_FG, FU_EDIT_BG);
}

static void draw_text_view(void)
{
	int w = fu_cols() - 4, h = fu_rows() - 6, r0 = 2, c0 = 2, y, pos, line;
	if (w < 20)
		w = fu_cols() > 4 ? fu_cols() - 2 : fu_cols();
	if (h < 6)
		h = fu_rows() > 4 ? fu_rows() - 4 : fu_rows();
	tui_fill(c0, r0, w, h, ' ', FU_EDIT_FG, FU_EDIT_BG);
	tui_frame(c0, r0, w, h, FU_MENU_FG, FU_MENU_BG);
	tui_pad(c0 + 1, r0 + 1, F.info_name, w - 2, FU_MENU_FG, FU_MENU_BG);
	pos = 0;
	line = 0;
	y = r0 + 2;
	while (pos < F.view_len && y < r0 + h - 2)
	{
		int start = pos, n = 0;
		while (pos < F.view_len && F.view_buf[pos] != '\n' && F.view_buf[pos] != '\r')
		{
			n++;
			pos++;
		}
		if (line >= F.view_top)
		{
			draw_syntax_span(c0 + 1, y, F.view_buf + start, n, w - 2);
			y++;
		}
		line++;
		if (pos < F.view_len && F.view_buf[pos] == '\r')
			pos++;
		if (pos < F.view_len && F.view_buf[pos] == '\n')
			pos++;
	}
	tui_pad(c0 + 1, r0 + h - 2, "Arrows scroll  any other key closes", w - 2,
		FU_ST_FG, FU_EDIT_BG);
}

static void draw_border_row(int row, int top, const char *left_mid, const char *right_mid)
{
	int lw = fu_left_w(), rw = fu_right_w();
	int L = top ? TUI_TL : TUI_BL;
	int R = top ? TUI_TR : TUI_BR;
	int join = top ? TUI_TT : TUI_BT;
	tui_hline(0, row, lw, L, TUI_H, join, FU_PAN_FG, FU_PAN_BG);
	if (rw > 0)
		tui_hline(lw, row, rw, TUI_H, TUI_H, R, FU_PAN_FG, FU_PAN_BG);
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
	tui_put(0, row, L, FU_PAN_FG, FU_PAN_BG);
	if (fu_cols() > 0)
		tui_put(fu_cols() - 1, row, R, FU_PAN_FG, FU_PAN_BG);
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
	tui_put(x0, row, TUI_V, FU_PAN_FG, FU_PAN_BG);
	tui_pad(x0 + 1, row, shown, name_w, fg, bg);
	tui_pad(x0 + 1 + name_w, row, szs, size_w, fg, bg);
	tui_put(x0 + pw - 1, row, TUI_V, FU_PAN_FG, FU_PAN_BG);
}

static void draw_hint(void)
{
	const char *h = F.hint[0] ? F.hint
				  : "Tab panels  Enter open/run  q quit  v view  e edit  h help";
	tui_status_hint(fu_rows() - 3, h, FU_HOT, FU_ST_FG, FU_ST_BG);
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
	tui_fill(c0, r0, w, h, ' ', FU_DLG_FG, FU_DLG_BG);
	tui_frame(c0, r0, w, h, FU_MENU_FG, FU_MENU_BG);
	tui_pad(c0 + 1, r0 + 1, title, w - 2, FU_MENU_FG, FU_MENU_BG);
	for (i = 0; i < nlines; i++)
		tui_pad(c0 + 1, r0 + 2 + i, lines[i] ? lines[i] : "", w - 2,
			FU_DLG_FG, FU_DLG_BG);
}

static void draw_help(void)
{
	const char *lines[] = {
		"Arrows move   Tab switch panel   BS parent",
		"Enter dir=open  .BAS=RUN  image/ANSI=view  audio=play",
		"v/F3 view   e/F4 edit   c/F5 copy   m/F6 move",
		"k/F7 mkdir  d/F8 delete  F9 menu   q/Esc quit",
		"Alt+L/F/C/O/R menus  Alt+L/R panels  F9 command menu",
		"Command menu > FTP server serves this folder over FTP",
		"Type A: or C: to change the active panel drive",
		"Any key closes this help",
	};
	draw_overlay_box(" FILES  (Midnight Commander style) ", lines, 8);
}

static void draw_menu(void)
{
	const char *lines[] = {
		"1  Drive A:     3  Drive C:     4  Drive D:",
		"v  View         e  Edit         c  Copy",
		"m  Move         k  MkDir        d  Delete",
		"s  FTP server   h  Help         q  Quit",
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

static void draw_ftp(void)
{
	char l1[80], l2[80];
	const char *lines[4];
	strncpy(l1, "Root: ", sizeof(l1) - 1);
	strncat(l1, F.ftp_root, sizeof(l1) - strlen(l1) - 1);
	lines[0] = l1;
	strncpy(l2, "Address: ", sizeof(l2) - 1);
	l2[sizeof(l2) - 1] = 0;
	if (F.ftp_addr[0])
		strncat(l2, F.ftp_addr, sizeof(l2) - strlen(l2) - 1);
	else
		strncat(l2, "waiting for network", sizeof(l2) - strlen(l2) - 1);
	lines[1] = l2;
	lines[2] = mmb_ftp_status();
	lines[3] = "Esc stops the server";
	draw_overlay_box(" FTP SERVER ", lines, 4);
}

static void fu_caption(const char *path, char *out, int outsz)
{
	char drv = path[0];
	const char *kind = 0;
	unsigned n = 0;
	if (drv >= 'a' && drv <= 'z')
		drv = (char)(drv - 32);
	if (drv == 'A')
		kind = "RAM";
	else if (drv == 'B')
		kind = "PKG";
	else if (drv == 'C')
		kind = "SD";
	else if (drv == 'H')
		kind = "NVME";
	else if (drv >= 'D' && drv <= 'G')
		kind = "USB";
	if (!kind)
	{
		strncpy(out, path, (unsigned)outsz - 1);
		out[outsz - 1] = 0;
		return;
	}
	out[n++] = drv;
	out[n++] = ':';
	out[n++] = ' ';
	{
		const char *k = kind;
		while (*k && n < (unsigned)outsz - 4)
			out[n++] = *k++;
	}
	if (drv >= 'C')
	{
		char lab[16];
		if (mmb_fat_label(drv, lab, sizeof lab) == 0 && lab[0])
		{
			const char *p = lab;
			out[n++] = ' ';
			out[n++] = '"';
			while (*p && n < (unsigned)outsz - 3)
				out[n++] = *p++;
			out[n++] = '"';
		}
	}
	/* Keep the panel's path tail so the caption still locates the folder. */
	{
		const char *rest = path;
		if (path[0] && path[1] == ':')
			rest = path + 2;
		while (*rest && n < (unsigned)outsz - 1)
			out[n++] = *rest++;
	}
	out[n] = 0;
}

static void files_draw(void)
{
	int i;
	fu_ent *e;
	char footL[80], footR[80];
	char capL[96], capR[96];
	int namew;
	tui_begin();
	mmb_editor_apply_tui_palette();
	tui_clear(FU_PAN_FG, FU_PAN_BG);
	draw_top_menu();
	fu_caption(F.pan[0].path, capL, sizeof capL);
	fu_caption(F.pan[1].path, capR, sizeof capR);
	draw_border_row(1, 1, capL, capR);
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
	draw_border_row(3 + fu_list(), 0, footL, footR);
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
	else if (F.mode == FU_FTP)
		draw_ftp();
	else if (F.mode == FU_VIEW)
		draw_text_view();
	if (F.drop >= 0)
		draw_files_dropdown();
	tui_flush();
	emit_status();
}

static void files_draw_if_idle(void)
{
	if (F.active && F.mode != FU_PREVIEW && F.mode != FU_ANSI &&
	    F.mode != FU_TDF && !mmb_in_editor())
		files_draw();
}

static void set_hint(const char *s)
{
	strncpy(F.hint, s ? s : "", sizeof(F.hint) - 1);
}

/* Storage hotplug notices land in the FILES hint line rather than being
 * printed over the full-screen UI. Returns 1 when the message was claimed. */
int mmb_files_notice(const char *msg)
{
	if (!F.active || F.mode != FU_BROWSE || G.running)
		return 0;
	set_hint(msg);
	files_draw_if_idle();
	return 1;
}

static int s_files_prompted;

static void preview_restore(void);
static void tdf_release(void);

static void files_close_tui(int restore_prompt)
{
	if (F.pv_saved)
		preview_restore();
	tdf_release();
	if (mmb_ftp_running())
		mmb_ftp_stop();
	F.active = 0;
	F.mode = FU_BROWSE;
	F.esc = 0;
	tui_end();
	s_files_prompted = 0;
	if (restore_prompt)
	{
		mmb_console_write("\r\n");
		mmb_console_write(mmb_prompt());
		s_files_prompted = 1;
	}
	else
		ser("\r\n");
}

int mmb_files_take_prompt(void)
{
	int v = s_files_prompted;
	s_files_prompted = 0;
	return v;
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

static void show_info_for(const char *path, const char *name);

static void do_text_view(const char *path, const char *name)
{
	unsigned got = 0;
	int sz;
	strncpy(F.view_path, path, sizeof(F.view_path) - 1);
	strncpy(F.info_name, name, sizeof(F.info_name) - 1);
	F.view_len = 0;
	F.view_top = 0;
	F.view_buf[0] = 0;
	sz = mmb_vfs_size(path);
	if (sz < 0)
	{
		show_info_for(path, name);
		return;
	}
	if (sz > (int)sizeof(F.view_buf) - 1)
		sz = (int)sizeof(F.view_buf) - 1;
	if (mmb_vfs_read(path, F.view_buf, (unsigned)sz, &got) != 0)
	{
		show_info_for(path, name);
		return;
	}
	F.view_len = (int)got;
	F.view_buf[F.view_len] = 0;
	F.mode = FU_VIEW;
	set_hint("Text view  arrows scroll");
}

static void show_info_for(const char *path, const char *name)
{
	strncpy(F.info_path, path, sizeof(F.info_path) - 1);
	strncpy(F.info_name, name, sizeof(F.info_name) - 1);
	F.info_size = mmb_vfs_size(path);
	F.mode = FU_INFO;
	set_hint("File info");
}

static void preview_restore(void)
{
	if (!F.pv_saved)
		return;
	F.pv_saved = 0;
	if (G.gfx.mode != F.pv_mode || G.gfx.bits != F.pv_bits)
		mmb_gfx_set_mode(F.pv_mode, F.pv_bits);
	mmb_gfx_reset_console(1);
}

static void preview_banner(const char *name, int w, int h)
{
	char line[FU_NAME + 24];
	char num[12];
	int scale = G.gfx.font_scale;
	int bar = 16 + 8;

	if (scale < 1)
		scale = 1;
	G.gfx.font_scale = 1; /* captions stay compact whatever FONT set */
	strncpy(line, name, sizeof(line) - 1);
	line[sizeof(line) - 1] = 0;
	strncat(line, "  ", sizeof(line) - strlen(line) - 1);
	fmt_uint(num, (unsigned)w);
	strncat(line, num, sizeof(line) - strlen(line) - 1);
	strncat(line, "x", sizeof(line) - strlen(line) - 1);
	fmt_uint(num, (unsigned)h);
	strncat(line, num, sizeof(line) - strlen(line) - 1);
	mmb_gfx_fill_rect(0, 0, G.gfx.w, bar, 0);
	mmb_gfx_text(4, 4, line, 0xFFFFFFu);
	G.gfx.font_scale = scale;
}

static int files_read_all(const char *path, unsigned char **buf, unsigned *n)
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
		*buf = 0;
		return -1;
	}
	*n = got;
	return 0;
}

static int preview_show(const char *path, const char *name)
{
	int w = 0, h = 0, mode, x, y;
	char line[128];
	int is_pcx = mmb_keyword_eq(ext_of(name), ".PCX");
	unsigned char *pfile = 0;
	unsigned pn = 0;
	uint32_t *pcx = 0;

	if (is_pcx)
	{
		if (files_read_all(path, &pfile, &pn) != 0)
			return -1;
		if (mmb_pcx_decode_rgba(pfile, pn, &pcx, &w, &h) != 0)
		{
			G.plat->free(pfile);
			return -1;
		}
		G.plat->free(pfile);
		pfile = 0;
	}
	else if (!is_img(name) || mmb_img_probe(path, &w, &h) != 0)
		return -1;
	mode = mmb_gfx_mode_for_size(w, h);
	if (G.gfx.mode != mode || G.gfx.bits != 32)
		mmb_gfx_set_mode(mode, 32);
	mmb_gfx_cls(0);
	x = (G.gfx.w - w) / 2;
	y = (G.gfx.h - h) / 2;
	if (x < 0)
		x = 0;
	if (y < 0)
		y = 0;
	if (is_pcx)
	{
		int i, j;
		for (j = 0; j < h; j++)
			for (i = 0; i < w; i++)
				mmb_gfx_plot(x + i, y + j,
					     pcx[(unsigned)j * (unsigned)w + (unsigned)i] &
						     0xFFFFFFu);
		G.plat->free(pcx);
		pcx = 0;
	}
	else if (mmb_keyword_eq(ext_of(name), ".JPG") || mmb_keyword_eq(ext_of(name), ".JPEG"))
	{
		if (mmb_load_jpeg(path, x, y) != 0)
			return -1;
	}
	else if (mmb_load_png(path, x, y, 0, 0) != 0)
		return -1;
	preview_banner(name, w, h);
	strncpy(F.info_name, name, sizeof(F.info_name) - 1);
	strncpy(F.info_path, path, sizeof(F.info_path) - 1);
	F.mode = FU_PREVIEW;
	set_hint("Image preview  arrows browse  Enter/Esc returns");
	strcpy(line, "[FILES] PREVIEW ");
	strncat(line, name, 40);
	strcat(line, " ");
	fmt_uint(line + strlen(line), (unsigned)w);
	strcat(line, "x");
	fmt_uint(line + strlen(line), (unsigned)h);
	strcat(line, " MODE ");
	fmt_uint(line + strlen(line), (unsigned)mode);
	strcat(line, "\r\n");
	ser(line);
	return 0;
}

static int img_step(int dir)
{
	fu_panel *p = curpan();
	int i;

	if (p->n <= 0)
		return -1;
	for (i = 1; i <= p->n; i++)
	{
		int k = p->sel + dir * i;
		while (k < 0)
			k += p->n;
		k %= p->n;
		if (!p->ent[k].is_dir && !is_dotdot(&p->ent[k]) && is_img(p->ent[k].name))
			return k;
	}
	return -1;
}

static void preview_step(int dir)
{
	int k = img_step(dir);
	char path[FU_PATH];

	if (k < 0)
	{
		set_hint("No other images");
		return;
	}
	curpan()->sel = k;
	clamp_sel(curpan());
	sel_path(path, sizeof(path));
	if (preview_show(path, curpan()->ent[k].name) != 0)
	{
		preview_restore();
		show_info_for(path, curpan()->ent[k].name);
	}
}

static void do_preview(const char *path, const char *name)
{
	F.pv_mode = G.gfx.mode;
	F.pv_bits = G.gfx.bits;
	F.pv_saved = 1;
	if (preview_show(path, name) != 0)
	{
		preview_restore();
		show_info_for(path, name);
	}
}

/* ---- ANSI art viewer ----------------------------------------------------
 * Renders a .ANS/.ASC/.DIZ through a small standalone ANSI/VT100 parser into
 * an offscreen cell grid, then paints the visible window with TUI cells on a
 * black background using the standard VGA 16-colour palette.  The grid keeps
 * all rows (no terminal scroll-back loss) so Up/Down can page through art
 * taller than the screen.  F toggles the authentic 80x25 (MODE 2 640x400)
 * view; Esc leaves. */

static void an_touch(int row)
{
	if (row + 1 > F.an_rows)
		F.an_rows = row + 1;
}

static void an_clear_cells(int r0, int c0, int r1, int c1)
{
	int r, c;
	if (r0 < 0)
		r0 = 0;
	if (c0 < 0)
		c0 = 0;
	if (r1 > AN_MAX_ROWS)
		r1 = AN_MAX_ROWS;
	if (c1 > an_p_cols)
		c1 = an_p_cols;
	for (r = r0; r < r1; r++)
		for (c = c0; c < c1; c++)
		{
			an_scr[r][c].ch = ' ';
			an_scr[r][c].fg = (unsigned char)(an_p_inv ? an_p_bg : an_p_fg);
			an_scr[r][c].bg = (unsigned char)(an_p_inv ? an_p_fg : an_p_bg);
		}
}

static int an_nearest16(int r, int g, int b)
{
	static const int vga[16][3] = {
		{ 0, 0, 0 }, { 170, 0, 0 }, { 0, 170, 0 }, { 170, 85, 0 },
		{ 0, 0, 170 }, { 170, 0, 170 }, { 0, 170, 170 }, { 170, 170, 170 },
		{ 85, 85, 85 }, { 255, 85, 85 }, { 85, 255, 85 }, { 255, 255, 85 },
		{ 85, 85, 255 }, { 255, 85, 255 }, { 85, 255, 255 }, { 255, 255, 255 }
	};
	int i, best = 0;
	long best_d = -1;
	for (i = 0; i < 16; i++)
	{
		long dr = r - vga[i][0], dg = g - vga[i][1], db = b - vga[i][2];
		long d = dr * dr + dg * dg + db * db;
		if (best_d < 0 || d < best_d)
		{
			best_d = d;
			best = i;
		}
	}
	return best;
}

static int an_256(int n)
{
	int r, g, b, v;
	if (n < 0)
		n = 0;
	if (n < 16)
		return n;
	if (n < 232)
	{
		n -= 16;
		b = n % 6;
		g = (n / 6) % 6;
		r = n / 36;
		r = r ? r * 40 + 55 : 0;
		g = g ? g * 40 + 55 : 0;
		b = b ? b * 40 + 55 : 0;
		return an_nearest16(r, g, b);
	}
	v = 8 + (n - 232) * 10;
	if (v > 255)
		v = 255;
	return an_nearest16(v, v, v);
}

static void an_cup(int row, int col)
{
	if (row < 1)
		row = 1;
	if (col < 1)
		col = 1;
	an_p_row = row - 1;
	an_p_col = col - 1;
	if (an_p_row >= AN_MAX_ROWS)
		an_p_row = AN_MAX_ROWS - 1;
	if (an_p_col >= an_p_cols)
		an_p_col = an_p_cols - 1;
	if (an_p_row < 0)
		an_p_row = 0;
	if (an_p_col < 0)
		an_p_col = 0;
}

static void an_newline(void)
{
	an_p_col = 0;
	if (an_p_row < AN_MAX_ROWS)
		an_p_row++;
}

static void an_put(unsigned char ch)
{
	int fg, bg;
	if (an_p_row < 0 || an_p_row >= AN_MAX_ROWS)
		return;
	if (an_p_col >= an_p_cols)
		an_newline();
	if (an_p_row < 0 || an_p_row >= AN_MAX_ROWS)
		return;
	if (an_p_col < 0)
		an_p_col = 0;
	if (an_p_col >= an_p_cols)
		return;
	fg = an_p_fg;
	if (an_p_bold && fg < 8)
		fg += 8;
	bg = an_p_bg;
	if (an_p_inv)
	{
		int t = fg;
		fg = bg;
		bg = t;
	}
	an_scr[an_p_row][an_p_col].ch = ch;
	an_scr[an_p_row][an_p_col].fg = (unsigned char)(fg & 15);
	an_scr[an_p_row][an_p_col].bg = (unsigned char)(bg & 15);
	an_p_col++;
	an_touch(an_p_row);
}

static void an_sgr(void)
{
	int i, n = an_p_narg > 0 ? an_p_narg : 1;
	if (an_p_narg == 0)
		an_p_args[0] = 0;
	for (i = 0; i < n; i++)
	{
		int v = an_p_args[i];
		if (v == 0)
		{
			an_p_fg = 7;
			an_p_bg = 0;
			an_p_bold = 0;
			an_p_inv = 0;
		}
		else if (v == 1)
			an_p_bold = 1;
		else if (v == 22)
			an_p_bold = 0;
		else if (v == 7)
			an_p_inv = 1;
		else if (v == 27)
			an_p_inv = 0;
		else if (v >= 30 && v <= 37)
			an_p_fg = v - 30;
		else if (v >= 90 && v <= 97)
			an_p_fg = v - 90 + 8;
		else if (v >= 40 && v <= 47)
			an_p_bg = v - 40;
		else if (v >= 100 && v <= 107)
			an_p_bg = v - 100 + 8;
		else if (v == 39)
			an_p_fg = 7;
		else if (v == 49)
			an_p_bg = 0;
		else if ((v == 38 || v == 48) && i + 2 < n && an_p_args[i + 1] == 5)
		{
			int c = an_256(an_p_args[i + 2]);
			if (v == 38)
				an_p_fg = c;
			else
				an_p_bg = c;
			i += 2;
		}
	}
}

static int an_arg(int i, int dflt)
{
	if (i < an_p_narg && an_p_args[i] > 0)
		return an_p_args[i];
	return dflt;
}

static void an_erase_line(int mode)
{
	int a = 0, b = an_p_cols;
	if (an_p_row < 0 || an_p_row >= AN_MAX_ROWS)
		return;
	if (mode == 0)
		a = an_p_col;
	else if (mode == 1)
		b = an_p_col + 1;
	an_clear_cells(an_p_row, a, an_p_row + 1, b);
}

static void an_erase_disp(int mode)
{
	int r0 = 0, r1 = AN_MAX_ROWS;
	if (mode == 0)
		r0 = an_p_row;
	else if (mode == 1)
		r1 = an_p_row + 1;
	an_clear_cells(r0, 0, r1, an_p_cols);
	if (mode == 2 || mode == 3)
	{
		an_p_row = 0;
		an_p_col = 0;
	}
}

static void an_exec_csi(char cmd)
{
	int n = an_arg(0, 1);
	if (cmd == 'A')
	{
		an_p_row -= n;
		if (an_p_row < 0)
			an_p_row = 0;
	}
	else if (cmd == 'B')
	{
		an_p_row += n;
		if (an_p_row >= AN_MAX_ROWS)
			an_p_row = AN_MAX_ROWS - 1;
	}
	else if (cmd == 'C')
	{
		an_p_col += n;
		if (an_p_col >= an_p_cols)
			an_p_col = an_p_cols - 1;
	}
	else if (cmd == 'D')
	{
		an_p_col -= n;
		if (an_p_col < 0)
			an_p_col = 0;
	}
	else if (cmd == 'G')
		an_cup(an_p_row + 1, an_arg(0, 1));
	else if (cmd == 'd')
		an_cup(an_arg(0, 1), an_p_col + 1);
	else if (cmd == 'H' || cmd == 'f')
		an_cup(an_arg(0, 1), an_arg(1, 1));
	else if (cmd == 'J')
		an_erase_disp(an_p_narg ? an_p_args[0] : 0);
	else if (cmd == 'K')
		an_erase_line(an_p_narg ? an_p_args[0] : 0);
	else if (cmd == 'X')
	{
		int cnt = n < 1 ? 1 : n;
		if (an_p_row >= 0 && an_p_row < AN_MAX_ROWS)
			an_clear_cells(an_p_row, an_p_col, an_p_row + 1, an_p_col + cnt);
	}
	else if (cmd == 'm')
		an_sgr();
	else if (cmd == 's')
	{
		an_p_sav_row = an_p_row;
		an_p_sav_col = an_p_col;
	}
	else if (cmd == 'u')
		an_cup(an_p_sav_row + 1, an_p_sav_col + 1);
}

static void an_feed(unsigned char b)
{
	if (an_p_st == 0)
	{
		if (b == 27)
		{
			an_p_st = 1;
			return;
		}
		if (b == 12)
		{
			an_erase_disp(2);
			return;
		}
		if (b == '\r')
		{
			an_p_col = 0;
			return;
		}
		if (b == '\n' || b == 11)
		{
			an_newline();
			return;
		}
		if (b == 8 || b == 127)
		{
			if (an_p_col > 0)
				an_p_col--;
			return;
		}
		if (b == 9)
		{
			int nx = (an_p_col / 8 + 1) * 8;
			if (nx >= an_p_cols)
				an_newline();
			else
				an_p_col = nx;
			return;
		}
		if (b == 7 || b < 32)
			return;
		an_put(b);
		return;
	}
	if (an_p_st == 1)
	{
		if (b == '[')
		{
			int i;
			an_p_st = 2;
			an_p_narg = 0;
			an_p_priv = 0;
			for (i = 0; i < 8; i++)
				an_p_args[i] = 0;
			return;
		}
		if (b == ']')
		{
			an_p_st = 5;
			return;
		}
		if (b == '7')
		{
			an_p_sav_row = an_p_row;
			an_p_sav_col = an_p_col;
			an_p_st = 0;
			return;
		}
		if (b == '8')
		{
			an_cup(an_p_sav_row + 1, an_p_sav_col + 1);
			an_p_st = 0;
			return;
		}
		if (b == 'c')
		{
			an_erase_disp(2);
			an_p_fg = 7;
			an_p_bg = 0;
			an_p_bold = 0;
			an_p_inv = 0;
			an_p_st = 0;
			return;
		}
		if (b == '(' || b == ')' || b == '*' || b == '+' || b == '-' || b == '#')
		{
			an_p_st = 6;
			return;
		}
		an_p_st = 0;
		return;
	}
	if (an_p_st == 6)
	{
		an_p_st = 0;
		return;
	}
	if (an_p_st == 5)
	{
		if (b == 7)
			an_p_st = 0;
		else if (b == 27)
			an_p_st = 7;
		return;
	}
	if (an_p_st == 7)
	{
		an_p_st = 0;
		return;
	}
	if (b == '?')
	{
		an_p_priv = 1;
		return;
	}
	if (b >= '0' && b <= '9')
	{
		if (an_p_narg == 0)
			an_p_narg = 1;
		an_p_args[an_p_narg - 1] = an_p_args[an_p_narg - 1] * 10 + (b - '0');
		return;
	}
	if (b == ';')
	{
		if (an_p_narg < 8)
			an_p_narg++;
		return;
	}
	if ((b >= 'A' && b <= 'Z') || (b >= 'a' && b <= 'z'))
	{
		if (!an_p_priv)
			an_exec_csi((char)b);
		/* private modes (cursor/alt-screen) have no visual effect */
		an_p_st = 0;
		return;
	}
	an_p_st = 0;
}

static void an_reset(int cols)
{
	int i;
	an_p_cols = cols < 1 ? 1 : (cols > AN_MAX_COLS ? AN_MAX_COLS : cols);
	an_p_row = an_p_col = 0;
	an_p_fg = 7;
	an_p_bg = 0;
	an_p_bold = 0;
	an_p_inv = 0;
	an_p_sav_row = an_p_sav_col = 0;
	an_p_st = 0;
	an_p_narg = 0;
	an_p_priv = 0;
	for (i = 0; i < 8; i++)
		an_p_args[i] = 0;
	memset(an_scr, 0, sizeof(an_scr));
	F.an_rows = 0;
	F.an_top = 0;
}

static void an_parse(int cols)
{
	static unsigned char buf[4096];
	int sz = mmb_vfs_size(F.view_path);
	unsigned pos = 0, got = 0;

	an_reset(cols);
	if (sz <= 0)
		return;
	while (pos < (unsigned)sz && pos < AN_MAX_BYTES)
	{
		unsigned want = (unsigned)sizeof(buf);
		unsigned i;
		if ((unsigned)sz - pos < want)
			want = (unsigned)sz - pos;
		if (mmb_vfs_read_at(F.view_path, pos, buf, want, &got) != 0 || got == 0)
			break;
		for (i = 0; i < got; i++)
			an_feed(buf[i]);
		pos += got;
	}
}

static void an_render(void)
{
	int r, c, rows = tui_rows(), cols = tui_cols();
	int max_top = F.an_rows - rows;

	if (max_top < 0)
		max_top = 0;
	if (F.an_top > max_top)
		F.an_top = max_top;
	if (F.an_top < 0)
		F.an_top = 0;
	tui_set_palette(0);
	tui_clear(7, 0);
	for (r = 0; r < rows; r++)
	{
		int src = F.an_top + r;
		if (src < 0 || src >= F.an_rows || src >= AN_MAX_ROWS)
			continue;
		for (c = 0; c < cols && c < AN_MAX_COLS; c++)
		{
			an_cell *cell = &an_scr[src][c];
			if (!cell->ch)
				continue;
			tui_put(c, r, cell->ch, cell->fg, cell->bg);
		}
	}
	tui_cursor(0, 0, 0);
	tui_flush();
}

static void an_apply_mode(void)
{
	int mode, bits;

	bits = F.an_saved_bits;
	if (bits != 8 && bits != 12 && bits != 16 && bits != 32)
		bits = 16;
	mode = F.an_mode80 ? 2 : F.an_saved_mode;
	if (mode < 1 || mode > 17)
		mode = 14;
	if (G.gfx.mode != mode || G.gfx.bits != bits)
		mmb_gfx_set_mode(mode, bits);
	tui_begin();
	tui_invalidate();
	tui_set_palette(0);
}

static void an_scroll(int dir)
{
	int max_top = F.an_rows - tui_rows();

	if (max_top < 0)
		max_top = 0;
	if (dir < 0)
	{
		if (F.an_top > 0)
			F.an_top--;
	}
	else if (F.an_top < max_top)
		F.an_top++;
	an_render();
}

static void an_page(int dir)
{
	int rows = tui_rows();
	int page = rows > 1 ? rows - 1 : 1;
	int max_top = F.an_rows - rows;

	if (max_top < 0)
		max_top = 0;
	F.an_top += dir < 0 ? -page : page;
	if (F.an_top < 0)
		F.an_top = 0;
	if (F.an_top > max_top)
		F.an_top = max_top;
	an_render();
}

static void an_toggle_80(void)
{
	F.an_mode80 = !F.an_mode80;
	an_apply_mode();
	an_render();
	set_hint(F.an_mode80 ? "ANSI 80x25  up/down/PgUp/PgDn  f restores  Esc exits"
			     : "ANSI preview  up/down/PgUp/PgDn  f 80x25  Esc exits");
	ser(F.an_mode80 ? "[FILES] ANSI 80x25 ON\r\n" : "[FILES] ANSI 80x25 OFF\r\n");
}

static void an_close(void)
{
	if (!F.an_mode80)
		return;
	F.an_mode80 = 0;
	mmb_gfx_set_mode(F.an_saved_mode, F.an_saved_bits);
	mmb_gfx_reset_console(1);
	tui_begin();
	tui_invalidate();
}

static int an_open(const char *path, const char *name)
{
	char line[96];

	if (mmb_vfs_size(path) <= 0)
		return -1;
	strncpy(F.view_path, path, sizeof(F.view_path) - 1);
	F.view_path[sizeof(F.view_path) - 1] = 0;
	strncpy(F.info_name, name, sizeof(F.info_name) - 1);
	F.info_name[sizeof(F.info_name) - 1] = 0;
	F.an_saved_mode = G.gfx.mode;
	F.an_saved_bits = G.gfx.bits;
	F.an_mode80 = 0;
	an_parse(AN_COLS);
	F.mode = FU_ANSI;
	an_render();
	set_hint("ANSI preview  up/down/PgUp/PgDn  f 80x25  Esc exits");
	strcpy(line, "[FILES] ANSI ");
	strncat(line, name, 32);
	strcat(line, " ");
	fmt_uint(line + strlen(line), (unsigned)AN_COLS);
	strcat(line, "x");
	fmt_uint(line + strlen(line), (unsigned)F.an_rows);
	strcat(line, "\r\n");
	ser(line);
	return 0;
}

static void do_ansi_view(const char *path, const char *name)
{
	if (an_open(path, name) != 0)
		show_info_for(path, name);
}

/* ---- TheDraw (.TDF) font preview ---------------------------------------
 * Decodes a .TDF with the shared mmb_tdf decoder and renders a sample of its
 * glyphs into the same CP437 cell grid the ANSI viewer uses, so the preview
 * runs in the current MODE and the console is left as it was. Enter/Esc
 * returns; Up/Down scroll a specimen taller than the screen. A bad or
 * unsupported file falls back to the file-info overlay. */

static void tdf_cell(void *ctx, int x, int y, int ch, int fg, int bg)
{
	(void)ctx;
	if (x < 0 || x >= AN_MAX_COLS || y < 0 || y >= AN_MAX_ROWS)
		return;
	an_scr[y][x].ch = (unsigned char)ch;
	an_scr[y][x].fg = (unsigned char)fg;
	an_scr[y][x].bg = (unsigned char)bg;
}

static void tdf_release(void)
{
	if (F.tdf_buf)
	{
		if (G.plat && G.plat->free)
			G.plat->free(F.tdf_buf);
		F.tdf_buf = 0;
	}
	F.tdf_size = 0;
	F.tdf_variants = 0;
	memset(&F.tdf, 0, sizeof(F.tdf));
}

/* Stamp plain console-font text into the CP437 grid (for variation captions). */
static void tdf_text(int x, int y, const char *s, int fg, int bg)
{
	for (; *s; s++, x++)
	{
		if (x < 1 || x >= AN_MAX_COLS || y < 1 || y >= AN_MAX_ROWS)
			continue;
		an_scr[y][x].ch = (unsigned char)*s;
		an_scr[y][x].fg = (unsigned char)fg;
		an_scr[y][x].bg = (unsigned char)bg;
	}
}

/* Render one font: its name then printable ASCII, wrapped to the 80-column
 * grid. Advances *yp past the block. */
static void tdf_render_font(const mmb_tdf *f, const char *fallback, int *yp)
{
	const char *label = f->name[0] ? f->name : fallback;
	int lh = f->max_height > 0 ? f->max_height : 1;
	int c, x, y = *yp;

	x = 1;
	for (; *label; label++)
		x += mmb_tdf_stamp(f, (unsigned char)*label, x, y, 15, 0,
				   tdf_cell, 0);
	y += lh + 1;

	x = 1;
	for (c = 33; c <= 126 && y < AN_MAX_ROWS - 1; c++)
	{
		x += mmb_tdf_stamp(f, c, x, y, 15, 0, tdf_cell, 0);
		if (x >= AN_COLS - 1)
		{
			x = 1;
			y += lh + 1;
		}
	}
	y += lh;
	*yp = y;
}

static void tdf_render_sample(const char *name)
{
	int nvar = F.tdf_variants > 1 ? F.tdf_variants : 1;
	int v, y;

	an_reset(AN_COLS);
	y = 1;
	if (nvar == 1)
	{
		/* Single-font files look exactly as they always did. */
		tdf_render_font(&F.tdf, name, &y);
	}
	else
	{
		for (v = 0; v < nvar; v++)
		{
			mmb_tdf f;
			char cap[80];
			char num[12];

			if (mmb_tdf_parse(F.tdf_buf, F.tdf_size, v, &f) != 0)
				break;
			strcpy(cap, "#");
			fmt_uint(num, (unsigned)(v + 1));
			strcat(cap, num);
			strcat(cap, " ");
			strncat(cap, f.name[0] ? f.name : name, 20);
			strcat(cap, " (");
			fmt_uint(num, (unsigned)(v + 1));
			strcat(cap, num);
			strcat(cap, "/");
			fmt_uint(num, (unsigned)nvar);
			strcat(cap, num);
			strcat(cap, ")");
			tdf_text(1, y, cap, 14, 0);
			y += 2;
			tdf_render_font(&f, name, &y);
			y += 1;
			if (y >= AN_MAX_ROWS - 1)
				break;
		}
	}
	if (y < 1)
		y = 1;
	if (y > AN_MAX_ROWS)
		y = AN_MAX_ROWS;
	F.an_rows = y;
}

static int tdf_open(const char *path, const char *name)
{
	unsigned char *buf;
	unsigned got = 0;
	int sz;
	char line[96];

	sz = mmb_vfs_size(path);
	if (sz < 233)
		return -1;
	buf = G.plat->alloc((unsigned)sz + 1);
	if (!buf)
		return -1;
	if (mmb_vfs_read(path, buf, (unsigned)sz, &got) != 0)
	{
		G.plat->free(buf);
		return -1;
	}
	tdf_release();
	if (mmb_tdf_parse(buf, got, 0, &F.tdf) != 0)
	{
		G.plat->free(buf);
		return -1;
	}
	F.tdf_buf = buf;
	F.tdf_size = got;
	F.tdf_variants = mmb_tdf_count(buf, got);
	if (F.tdf_variants < 1)
		F.tdf_variants = 1;
	strncpy(F.view_path, path, sizeof(F.view_path) - 1);
	F.view_path[sizeof(F.view_path) - 1] = 0;
	strncpy(F.info_name, name, sizeof(F.info_name) - 1);
	F.info_name[sizeof(F.info_name) - 1] = 0;
	F.an_top = 0;
	tdf_render_sample(name);
	F.mode = FU_TDF;
	an_render();
	set_hint(F.tdf_variants > 1
			 ? "TDF preview  up/down/PgUp/PgDn scroll  Enter/Esc returns"
			 : "TDF preview  up/down/PgUp/PgDn  Enter/Esc returns");
	strcpy(line, "[FILES] TDF ");
	strncat(line, name, 32);
	strcat(line, " ");
	strncat(line, F.tdf.name, 20);
	if (F.tdf_variants > 1)
	{
		strcat(line, " ");
		fmt_uint(line + strlen(line), (unsigned)F.tdf_variants);
		strcat(line, " variants");
	}
	strcat(line, "\r\n");
	ser(line);
	return 0;
}

static void do_tdf_view(const char *path, const char *name)
{
	if (tdf_open(path, name) != 0)
	{
		show_info_for(path, name);
		set_hint("Cannot preview this .TDF");
	}
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
	else if (mmb_keyword_eq(e, ".WAV"))
		rc = mmb_play_wav(path);
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

static void do_ftp_start(void)
{
	char root[FU_PATH];
	char ip[32];
	char num[8];

	strncpy(root, curpan()->path, sizeof(root) - 1);
	root[sizeof(root) - 1] = 0;
	if (mmb_ftp_start(root, 21) != 0)
	{
		set_hint("FTP server: network unavailable");
		return;
	}
	strncpy(F.ftp_root, root, sizeof(F.ftp_root) - 1);
	F.ftp_root[sizeof(F.ftp_root) - 1] = 0;
	F.ftp_addr[0] = 0;
	if (mmb_net_srv_ip(ip, sizeof(ip)) == 0)
	{
		strncpy(F.ftp_addr, ip, sizeof(F.ftp_addr) - 1);
		strncat(F.ftp_addr, ":", sizeof(F.ftp_addr) - strlen(F.ftp_addr) - 1);
		fmt_uint(num, 21);
		strncat(F.ftp_addr, num, sizeof(F.ftp_addr) - strlen(F.ftp_addr) - 1);
	}
	F.ftp_last[0] = 0;
	F.mode = FU_FTP;
	set_hint("FTP server running  Esc stops");
	ser("[FILES] FTP ROOT ");
	ser(root);
	ser(" PORT 21\r\n");
}

static void do_run(const char *path)
{
	char cmd[160];
	files_close_tui(0);
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
		if (letter == 'B')
			set_hint("Drive not available");
		else if (letter >= 'A' && letter <= 'H')
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
	if (is_ansi(e->name))
	{
		do_ansi_view(path, e->name);
		return;
	}
	if (is_tdf(e->name))
	{
		do_tdf_view(path, e->name);
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
	else if (is_ansi(e->name))
		do_ansi_view(path, e->name);
	else if (is_tdf(e->name))
		do_tdf_view(path, e->name);
	else if (is_aud(e->name))
		do_play(path, e->name);
	else if (is_text(e->name))
		do_text_view(path, e->name);
	else
		show_info_for(path, e->name);
}

static void set_drive_letter(int letter)
{
	char spec[8];
	if (letter >= 'a' && letter <= 'z')
		letter -= 32;
	if (letter == 'B')
	{
		set_hint("Drive not available");
		return;
	}
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
	if (F.mode == FU_FTP)
		mmb_ftp_stop();
	if (F.mode == FU_PREVIEW)
		preview_restore();
	if (F.mode == FU_ANSI)
		an_close();
	if (F.mode == FU_TDF)
		tdf_release();
	F.mode = FU_BROWSE;
	F.drop = -1;
	set_hint("");
	tui_invalidate();
}

static void files_open(const char *start)
{
	char cwd[FU_PATH];
	memset(&F, 0, sizeof(F));
	F.active = 1;
	F.mode = FU_BROWSE;
	F.drop = -1;
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
	if (F.mode == FU_PREVIEW || F.mode == FU_INFO || F.mode == FU_HELP ||
	    F.mode == FU_TDF)
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
		files_close_tui(1);
}

static void do_eject(void)
{
	char drv = curpan()->path[0];
	if (drv >= 'a' && drv <= 'z')
		drv = (char)(drv - 32);
	if (mmb_fat_eject(drv) != 0)
	{
		set_hint("No removable drive to eject");
		return;
	}
	curpan()->path[0] = 'A';
	curpan()->path[1] = ':';
	curpan()->path[2] = '/';
	curpan()->path[3] = 0;
	curpan()->sel = 0;
	curpan()->top = 0;
	panel_reload(curpan());
	chdir_panel(curpan());
	set_hint("Ejected - drive removed safely");
}

static void activate_drop(void)
{
	int menu = F.drop;
	int item = F.drop_item;
	F.drop = -1;
	if (menu == 0)
	{
		F.cur = 0;
		if (item == 0)
			set_drive_letter('A');
		else if (item == 1)
			set_drive_letter('C');
		else
			set_drive_letter('D');
	}
	else if (menu == 1)
	{
		if (item == 0)
			do_view();
		else if (item == 1)
		{
			fu_ent *e = cursel();
			char path[FU_PATH];
			if (e && !e->is_dir)
			{
				sel_path(path, sizeof(path));
				do_edit(path);
			}
		}
		else if (item == 2)
			start_copy_or_move(0);
		else if (item == 3)
			start_copy_or_move(1);
		else
			start_delete();
	}
	else if (menu == 2)
	{
		if (item == 0)
			start_mkdir();
		else if (item == 1)
			do_eject();
		else if (item == 2)
			do_ftp_start();
		else if (item == 3)
			F.mode = FU_HELP;
		else
			files_close_tui(1);
	}
	else if (menu == 3)
		F.mode = FU_HELP;
	else
	{
		F.cur = 1;
		if (item == 0)
		{
			chdir_panel(curpan());
			set_hint("Right panel");
		}
		else if (item == 1)
			set_drive_letter('A');
		else if (item == 2)
			set_drive_letter('C');
		else
			set_drive_letter('D');
	}
}

static int files_alt(char c)
{
	if (c >= 'A' && c <= 'Z')
		c = (char)(c - 'A' + 'a');
	if (c == 'l')
	{
		F.drop = 0;
		F.drop_item = 0;
		return 1;
	}
	if (c == 'f')
	{
		F.drop = 1;
		F.drop_item = 0;
		return 1;
	}
	if (c == 'c')
	{
		F.drop = 2;
		F.drop_item = 0;
		return 1;
	}
	if (c == 'o')
	{
		F.drop = 3;
		F.drop_item = 0;
		return 1;
	}
	if (c == 'r')
	{
		F.drop = 4;
		F.drop_item = 0;
		return 1;
	}
	return 0;
}

static void handle_arrow(int which)
{
	fu_panel *p = curpan();
	if (F.mode == FU_ANSI || F.mode == FU_TDF)
	{
		if (which == 0)
			an_scroll(-1);
		else if (which == 1)
			an_scroll(1);
		return;
	}
	if (F.mode == FU_VIEW)
	{
		if (which == 0 && F.view_top > 0)
			F.view_top--;
		else if (which == 1)
			F.view_top++;
		return;
	}
	if (F.drop >= 0)
	{
		int n;
		const char *hots;
		drop_items(F.drop, &n, &hots);
		(void)hots;
		if (which == 0)
			F.drop_item = F.drop_item > 0 ? F.drop_item - 1 : n - 1;
		else if (which == 1)
			F.drop_item = F.drop_item + 1 < n ? F.drop_item + 1 : 0;
		else if (which == 2)
		{
			F.drop = (F.drop + 1) % 5;
			F.drop_item = 0;
		}
		else if (which == 3)
		{
			F.drop = F.drop > 0 ? F.drop - 1 : 4;
			F.drop_item = 0;
		}
		return;
	}
	if (F.mode == FU_PREVIEW)
	{
		if (which == 2)
			preview_step(1);
		else if (which == 3)
			preview_step(-1);
		return;
	}
	if (F.mode == FU_INFO || F.mode == FU_HELP || F.mode == FU_PLAY)
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

static void files_lone_esc(void)
{
	if (F.drop >= 0)
		F.drop = -1;
	else if (F.mode == FU_BROWSE)
		files_close_tui(1);
	else
		close_overlay();
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
		files_lone_esc();
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
			if (F.csi_n >= 11 && F.csi_n <= 15)
				handle_fkey(F.csi_n - 10);
			else if (F.csi_n == 17)
				handle_fkey(6);
			else if (F.csi_n == 18)
				handle_fkey(7);
			else if (F.csi_n == 19)
				handle_fkey(8);
			else if (F.csi_n == 20)
				handle_fkey(9);
			else if (F.csi_n == 21)
				handle_fkey(10);
			else if ((F.mode == FU_ANSI || F.mode == FU_TDF) && F.csi_n == 5)
				an_page(-1);
			else if ((F.mode == FU_ANSI || F.mode == FU_TDF) && F.csi_n == 6)
				an_page(1);
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
	if (F.mode == FU_ANSI)
	{
		if (lc == 'f')
			an_toggle_80();
		return;
	}
	if (F.mode == FU_TDF)
		return;
	if (F.drop >= 0)
	{
		int n, i;
		const char *hots;
		drop_items(F.drop, &n, &hots);
		for (i = 0; i < n; i++)
		{
			if (hots[i] == lc)
			{
				F.drop_item = i;
				activate_drop();
				return;
			}
		}
		return;
	}
	if (F.mode == FU_VIEW)
	{
		close_overlay();
		return;
	}
	if (F.mode == FU_HELP || F.mode == FU_INFO || F.mode == FU_PREVIEW ||
	    F.mode == FU_VIEW)
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
			files_close_tui(1);
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
		else if (lc == 's')
		{
			F.mode = FU_BROWSE;
			do_ftp_start();
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
		files_close_tui(1);
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
		files_close_tui(1);
}

const char *mmb_files_resume(void)
{
	G.outn = 0;
	G.out[0] = 0;
	if (F.active)
		files_draw_if_idle();
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
		files_close_tui(0);
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
	if (F.mode == FU_FTP)
	{
		/* Modal: only a lone Esc stops the server. */
		if (c == 27)
		{
			F.esc = 1;
			F.esc_at = mmb_now_ms();
		}
		return G.out;
	}
	if (F.alt)
	{
		F.alt = 0;
		files_alt(c);
		if (F.active && F.mode != FU_PREVIEW)
			files_draw_if_idle();
		return G.out;
	}
	if ((unsigned char)c == 1)
	{
		F.alt = 1;
		return G.out;
	}
	if (F.esc)
	{
		handle_esc_char(c);
		if (F.active && F.mode != FU_PREVIEW)
			files_draw_if_idle();
		return G.out;
	}
	if (c == 27)
	{
		F.esc = 1;
		F.esc_at = mmb_now_ms();
		return G.out;
	}
	if (F.mode == FU_PREVIEW)
	{
		if (c == '\r' || c == '\n')
		{
			close_overlay();
			files_draw_if_idle();
		}
		return G.out;
	}
	if (F.mode == FU_ANSI || F.mode == FU_TDF)
	{
		if (c == '\r' || c == '\n')
		{
			close_overlay();
			files_draw_if_idle();
			return G.out;
		}
		if (F.mode == FU_ANSI && c >= 32 && c < 127)
			handle_letter(c);
		return G.out;
	}
	if (F.mode == FU_PROMPT)
	{
		if (c == 27)
		{
			F.mode = FU_BROWSE;
			files_draw_if_idle();
			return G.out;
		}
		handle_prompt_char(c);
		if (F.active)
			files_draw_if_idle();
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
			files_draw_if_idle();
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
			 F.mode == FU_INFO || F.mode == FU_PLAY || F.mode == FU_VIEW ||
			 F.mode == FU_TDF)
			close_overlay();
		if (F.active)
			files_draw_if_idle();
		return G.out;
	}
	if (c == '\r' || c == '\n')
	{
		if (F.drop >= 0)
			activate_drop();
		else if (F.mode == FU_PLAY)
			close_overlay();
		else if (F.mode == FU_HELP || F.mode == FU_INFO || F.mode == FU_MENU ||
			 F.mode == FU_VIEW || F.mode == FU_TDF)
			close_overlay();
		else if (F.mode == FU_CONFIRM)
			apply_delete();
		else if (F.mode == FU_BROWSE)
			activate_enter();
		if (F.active)
			files_draw_if_idle();
		return G.out;
	}
	if (c == ':')
	{
		if (F.pend_drive)
		{
			set_drive_letter(F.pend_drive);
			F.pend_drive = 0;
			if (F.active)
				files_draw_if_idle();
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
		files_draw_if_idle();
	return G.out;
}

void mmb_files_poll(void)
{
	if (!F.active)
		return;
	if (F.esc == 1 && mmb_now_ms() - F.esc_at >= 60)
	{
		F.esc = 0;
		files_lone_esc();
		if (F.active && F.mode != FU_PREVIEW)
			files_draw_if_idle();
	}
	if (F.active && F.mode == FU_FTP)
	{
		const char *st;
		mmb_ftp_poll();
		st = mmb_ftp_status();
		if (strcmp(st, F.ftp_last) != 0)
		{
			strncpy(F.ftp_last, st, sizeof(F.ftp_last) - 1);
			F.ftp_last[sizeof(F.ftp_last) - 1] = 0;
			files_draw_if_idle();
		}
	}
}
