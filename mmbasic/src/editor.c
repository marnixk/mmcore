#include "mmb_priv.h"
#include "tui.h"

/*
 * Turbo-style full-screen editor. Layout tracks the current HDMI mode
 * (8x16 cells). Borders use box-drawing glyphs via the offscreen TUI.
 */

#define ED (G.ed.tab[G.ed.cur])

static int ed_cols(void) { return tui_cols(); }
static int ed_rows(void) { return tui_rows(); }
#define COLS        ed_cols()
#define ROWS        ed_rows()
#define TEXT_ROWS   (ed_rows() - 5)
#define TEXT_COLS   (ed_cols() - 2)
#define ROW_MENU    0
#define ROW_TABS    1
#define ROW_BTOP    2
#define ROW_TEXT    3
#define ROW_BBOT    (ed_rows() - 2)
#define ROW_STAT    (ed_rows() - 1)
#define ED_TAB      4

#define MENU_FILE   0
#define MENU_EDIT   1
#define MENU_RUN    2
#define MENU_THEME  3
#define MENU_HELP   4
#define MENU_COUNT  5

#define ED_THEME_N     10
#define ED_THEME_TURBO 8

typedef struct ed_theme {
	const char *name;
	unsigned char menu_fg, menu_bg, hot;
	unsigned char sel_fg, sel_bg;
	unsigned char edit_fg, edit_bg;
	unsigned char mark_fg, mark_bg;
	unsigned char str_fg, num_fg, cmt_fg;
	unsigned char brd_fg, brd_bg;
	unsigned char tab_fg, tab_bg, tabcur_fg, tabcur_bg;
	unsigned char dlg_fg, dlg_bg;
	unsigned char sh_fg, sh_bg;
	unsigned char list_bg;
	const unsigned *pal;
} ed_theme;

static const unsigned pal_paper[16] = {
	0x2C261Cu, 0xC45C48u, 0x5A8A4Au, 0xC49A4Au,
	0x3D6A9Eu, 0xA05A8Au, 0x4A8A9Eu, 0xF3EBDDu,
	0xE8DCC8u, 0xE07060u, 0x7AB86Au, 0xE0B85Au,
	0x5A8AC8u, 0xC070B0u, 0x6AB8D0u, 0xFFF8F0u
};
static const unsigned pal_cloud[16] = {
	0x1A3040u, 0xC05050u, 0x409060u, 0xC09040u,
	0x2B6CB0u, 0x9050A0u, 0x3090A8u, 0xD0E8F0u,
	0x7AA8B8u, 0xE06060u, 0x50C080u, 0xE0C060u,
	0x4080D0u, 0xC060C0u, 0xC5E8F4u, 0xF5FBFFu
};
static const unsigned pal_snow[16] = {
	0x1A202Cu, 0xC45C5Cu, 0x5A9A6Au, 0xC4A05Au,
	0x4A6AA0u, 0xA05A9Au, 0x4A9AB0u, 0xE8EEF4u,
	0xD0D8E0u, 0xE07070u, 0x70C080u, 0xE8C060u,
	0x6080D0u, 0xC070C0u, 0x90D0E8u, 0xF7FAFCu
};
static const unsigned pal_night[16] = {
	0x0A0A0Cu, 0xC45C5Cu, 0x5A9A6Au, 0xC4A05Au,
	0x4A6AB0u, 0xA05A9Au, 0x6AB4C8u, 0xC8C8D0u,
	0x16161Cu, 0xE07070u, 0x70C080u, 0xE8C060u,
	0x6080D0u, 0xC070C0u, 0x80D0E0u, 0xECECF0u
};
static const unsigned pal_nord[16] = {
	0x2E3440u, 0xBF616Au, 0xA3BE8Cu, 0xEBCB8Bu,
	0x5E81ACu, 0xB48EADu, 0x88C0D0u, 0xD8DEE9u,
	0x3B4252u, 0xD08770u, 0xA3BE8Cu, 0xEBCB8Bu,
	0x81A1C1u, 0xB48EADu, 0x8FBCBBu, 0xECEFF4u
};
static const unsigned pal_slate[16] = {
	0x0C0E12u, 0xC45C5Cu, 0x6A9B72u, 0xC4A06Au,
	0x5A7AB0u, 0xB07AA0u, 0x7EB6C9u, 0xC8CCD4u,
	0x1A1D24u, 0xE07878u, 0x8FBF8Fu, 0xE8C85Au,
	0x7A9AD0u, 0xD090C0u, 0x8FCBD8u, 0xE8EAEEu
};
static const unsigned pal_forest[16] = {
	0x0A140Cu, 0xC45C5Cu, 0x2A5A30u, 0xC4A05Au,
	0x3A6A90u, 0x8A5A8Au, 0x4A9A8Au, 0xC8D8C8u,
	0x122018u, 0xE07070u, 0x6ED06Au, 0xE0C060u,
	0x5A90C0u, 0xC070C0u, 0x70D0B0u, 0xE8F0E8u
};
static const unsigned pal_violet[16] = {
	0x120A18u, 0xC45C5Cu, 0x5A9A6Au, 0xC4A05Au,
	0x5A4AB0u, 0x6A3A88u, 0x6A90B0u, 0xD0C8D8u,
	0x1C1224u, 0xE07070u, 0x70C080u, 0xE8C060u,
	0x8070D0u, 0xC080E0u, 0x90C0E0u, 0xF0E8F8u
};
static const unsigned pal_phosphor[16] = {
	0x000000u, 0xAA0000u, 0x2A8A2Au, 0x8A8A20u,
	0x0000AAu, 0xAA00AAu, 0x2A8A8Au, 0x88AA88u,
	0x0A1A0Au, 0xFF5555u, 0x55FF66u, 0xD4FF4Au,
	0x5555FFu, 0xFF55FFu, 0x55FFCCu, 0xC8FFC8u
};

static const ed_theme k_themes[ED_THEME_N] = {
	/* 3 modern light */
	{ "Paper",
	  TUI_BLACK, TUI_WHITE, TUI_BRRED,
	  TUI_WHITE, TUI_BLUE,
	  TUI_BLACK, TUI_WHITE,
	  TUI_WHITE, TUI_BLUE,
	  TUI_RED, TUI_BLUE, TUI_BRBLACK,
	  TUI_BLACK, TUI_WHITE,
	  TUI_BLACK, TUI_BRWHITE, TUI_WHITE, TUI_BLUE,
	  TUI_BLACK, TUI_WHITE,
	  TUI_BRBLACK, TUI_BRBLACK, TUI_CYAN, pal_paper },
	{ "Cloud",
	  TUI_BLACK, TUI_BRCYAN, TUI_RED,
	  TUI_WHITE, TUI_BLUE,
	  TUI_BLACK, TUI_BRCYAN,
	  TUI_WHITE, TUI_BLUE,
	  TUI_RED, TUI_MAGENTA, TUI_BLUE,
	  TUI_BLUE, TUI_BRCYAN,
	  TUI_BLACK, TUI_CYAN, TUI_WHITE, TUI_BLUE,
	  TUI_BLACK, TUI_WHITE,
	  TUI_BLACK, TUI_BLACK, TUI_CYAN, pal_cloud },
	{ "Snow",
	  TUI_BLACK, TUI_BRWHITE, TUI_BRRED,
	  TUI_WHITE, TUI_CYAN,
	  TUI_BLACK, TUI_BRWHITE,
	  TUI_BLACK, TUI_YELLOW,
	  TUI_MAGENTA, TUI_BLUE, TUI_BRBLACK,
	  TUI_BLACK, TUI_BRWHITE,
	  TUI_BLACK, TUI_WHITE, TUI_WHITE, TUI_CYAN,
	  TUI_BLACK, TUI_WHITE,
	  TUI_BRBLACK, TUI_BRBLACK, TUI_CYAN, pal_snow },
	/* 5 modern dark */
	{ "Night",
	  TUI_BRWHITE, TUI_BRBLACK, TUI_BRCYAN,
	  TUI_BLACK, TUI_CYAN,
	  TUI_BRWHITE, TUI_BLACK,
	  TUI_BLACK, TUI_WHITE,
	  TUI_BRGREEN, TUI_BRCYAN, TUI_BRBLACK,
	  TUI_WHITE, TUI_BLACK,
	  TUI_WHITE, TUI_BRBLACK, TUI_BLACK, TUI_CYAN,
	  TUI_BRWHITE, TUI_BRBLACK,
	  TUI_BLACK, TUI_BLACK, TUI_BLUE, pal_night },
	{ "Nord",
	  TUI_BRWHITE, TUI_BLUE, TUI_BRCYAN,
	  TUI_BLACK, TUI_CYAN,
	  TUI_BRWHITE, TUI_BLACK,
	  TUI_BLACK, TUI_CYAN,
	  TUI_BRCYAN, TUI_BRBLUE, TUI_CYAN,
	  TUI_CYAN, TUI_BLACK,
	  TUI_BRWHITE, TUI_BLUE, TUI_BLACK, TUI_CYAN,
	  TUI_BRWHITE, TUI_BLUE,
	  TUI_BLACK, TUI_BLACK, TUI_BLUE, pal_nord },
	{ "Slate",
	  TUI_WHITE, TUI_BRBLACK, TUI_BRYELLOW,
	  TUI_BLACK, TUI_CYAN,
	  TUI_WHITE, TUI_BRBLACK,
	  TUI_BLACK, TUI_WHITE,
	  TUI_BRMAGENTA, TUI_BRYELLOW, TUI_CYAN,
	  TUI_WHITE, TUI_BRBLACK,
	  TUI_WHITE, TUI_BLACK, TUI_BLACK, TUI_WHITE,
	  TUI_BLACK, TUI_WHITE,
	  TUI_BLACK, TUI_BLACK, TUI_CYAN, pal_slate },
	{ "Forest",
	  TUI_BRGREEN, TUI_BLACK, TUI_BRYELLOW,
	  TUI_BLACK, TUI_GREEN,
	  TUI_BRGREEN, TUI_BLACK,
	  TUI_BLACK, TUI_GREEN,
	  TUI_BRYELLOW, TUI_BRCYAN, TUI_GREEN,
	  TUI_GREEN, TUI_BLACK,
	  TUI_GREEN, TUI_BLACK, TUI_BLACK, TUI_GREEN,
	  TUI_BRGREEN, TUI_BLACK,
	  TUI_BLACK, TUI_BLACK, TUI_GREEN, pal_forest },
	{ "Violet",
	  TUI_BRWHITE, TUI_BLACK, TUI_BRMAGENTA,
	  TUI_BLACK, TUI_MAGENTA,
	  TUI_BRWHITE, TUI_BLACK,
	  TUI_BLACK, TUI_BRMAGENTA,
	  TUI_BRMAGENTA, TUI_BRCYAN, TUI_MAGENTA,
	  TUI_MAGENTA, TUI_BLACK,
	  TUI_BRWHITE, TUI_BLACK, TUI_BLACK, TUI_MAGENTA,
	  TUI_BRWHITE, TUI_BLACK,
	  TUI_BLACK, TUI_BLACK, TUI_MAGENTA, pal_violet },
	/* 2 retro — Turbo is the historical default (VGA palette) */
	{ "Turbo",
	  TUI_BLACK, TUI_WHITE, TUI_BRRED,
	  TUI_BLACK, TUI_GREEN,
	  TUI_BRWHITE, TUI_BLUE,
	  TUI_BLACK, TUI_WHITE,
	  TUI_BRYELLOW, TUI_BRBLUE, TUI_WHITE,
	  TUI_WHITE, TUI_BLUE,
	  TUI_WHITE, TUI_BLUE, TUI_BLACK, TUI_WHITE,
	  TUI_BLACK, TUI_WHITE,
	  TUI_BLACK, TUI_BLACK, TUI_CYAN, 0 },
	{ "Phosphor",
	  TUI_GREEN, TUI_BLACK, TUI_BRYELLOW,
	  TUI_BLACK, TUI_GREEN,
	  TUI_GREEN, TUI_BLACK,
	  TUI_BLACK, TUI_BRGREEN,
	  TUI_BRGREEN, TUI_BRYELLOW, TUI_GREEN,
	  TUI_GREEN, TUI_BLACK,
	  TUI_GREEN, TUI_BLACK, TUI_BLACK, TUI_GREEN,
	  TUI_BRGREEN, TUI_BLACK,
	  TUI_BLACK, TUI_BLACK, TUI_GREEN, pal_phosphor },
};

static const ed_theme *th(void)
{
	int i = G.opt.edit_theme;
	if (i < 0 || i >= ED_THEME_N)
		i = ED_THEME_TURBO;
	return &k_themes[i];
}

#define C_MENU_FG   ((int)th()->menu_fg)
#define C_MENU_BG   ((int)th()->menu_bg)
#define C_HOT       ((int)th()->hot)
#define C_SEL_FG    ((int)th()->sel_fg)
#define C_SEL_BG    ((int)th()->sel_bg)
#define C_EDIT_FG   ((int)th()->edit_fg)
#define C_EDIT_BG   ((int)th()->edit_bg)
#define C_MARK_FG   ((int)th()->mark_fg)
#define C_MARK_BG   ((int)th()->mark_bg)
#define C_STR_FG    ((int)th()->str_fg)
#define C_NUM_FG    ((int)th()->num_fg)
#define C_CMT_FG    ((int)th()->cmt_fg)
#define C_BRD_FG    ((int)th()->brd_fg)
#define C_BRD_BG    ((int)th()->brd_bg)
#define C_TAB_FG    ((int)th()->tab_fg)
#define C_TAB_BG    ((int)th()->tab_bg)
#define C_TABCUR_FG ((int)th()->tabcur_fg)
#define C_TABCUR_BG ((int)th()->tabcur_bg)
#define C_DLG_FG    ((int)th()->dlg_fg)
#define C_DLG_BG    ((int)th()->dlg_bg)
#define C_SH_FG     ((int)th()->sh_fg)
#define C_SH_BG     ((int)th()->sh_bg)
#define C_LIST_BG   ((int)th()->list_bg)

int mmb_editor_theme_count(void)
{
	return ED_THEME_N;
}

const char *mmb_editor_theme_name(int i)
{
	if (i < 0 || i >= ED_THEME_N)
		return k_themes[ED_THEME_TURBO].name;
	return k_themes[i].name;
}

int mmb_editor_theme_lookup(const char *s)
{
	char want[32], have[32];
	int i, n;

	if (!s || !s[0])
		return -1;
	n = (int)strlen(s);
	if (n >= (int)sizeof(want))
		n = (int)sizeof(want) - 1;
	memcpy(want, s, (unsigned)n);
	want[n] = 0;
	mmb_upper(want);
	for (i = 0; i < ED_THEME_N; i++)
	{
		strncpy(have, k_themes[i].name, sizeof(have) - 1);
		have[sizeof(have) - 1] = 0;
		mmb_upper(have);
		if (mmb_keyword_eq(want, have))
			return i;
	}
	return -1;
}

#define ESC_NONE    0
#define ESC_GOT     1
#define ESC_CSI     2
#define ESC_SS3     3
#define ESC_IDLE_MS 60

#define DLG_NONE    0
#define DLG_OPEN    1
#define DLG_SAVEAS  2
#define DLG_HELP    3
#define DLG_PICK    4

#define ED_PICK_MAX   80
#define ED_PICK_DEPTH 8

#define FD_MAX        64
#define FD_NAME       40
#define FD_FOCUS_NAME 0
#define FD_FOCUS_FILE 1
#define FD_FOCUS_DIR  2

static char killbuf[8192];
static int killlen;
static char pick_root[128];
static char pick_path[ED_PICK_MAX][128];
static int pick_n;
static int pick_sel;
static int pick_row0;
static int pick_view[ED_PICK_MAX];
static int pick_vn;
static int alt_pend;
static int esc_state;
static unsigned esc_at;
static int csi_n;
static int csi_arg;
static int csi_semi;

static char fd_dir[128];
static char fd_mask[32];
static char fd_files[FD_MAX][FD_NAME];
static char fd_dirs[FD_MAX][FD_NAME];
static int fd_nfile, fd_ndir;
static int fd_fsel, fd_dsel;
static int fd_ftop, fd_dtop;
static int fd_focus;

static const char *menu_name[MENU_COUNT] = { "File", "Edit", "Run", "Theme", "Help" };
static const char menu_hot[MENU_COUNT] = { 'F', 'E', 'R', 'T', 'H' };
static int menu_x[MENU_COUNT];

static const char *file_items[] = {
	"Open...", "Quick open...", "Save", "Save As...", "Close tab", "Next tab", "Quit"
};
static const char file_hots[] = { 'o', 'p', 's', 'a', 'c', 'n', 'q' };
static const char *edit_items[] = { "Copy", "Cut", "Cut line", "Paste" };
static const char edit_hots[] = { 'o', 't', 'c', 'p' };
static const char *run_items[] = { "Run" };
static const char run_hots[] = { 'r' };
static const char *help_items[] = { "Keys...", "Manual" };
static const char help_hots[] = { 'k', 'm' };
static const char theme_hots[] = { 'p', 'c', 's', 'i', 'o', 'l', 'f', 'v', 't', 'h' };

static void redraw(void);
static void save_tab(void);
static void editor_leave(void);
static void editor_run(void);
static void editor_resume(void);
static void open_dialog(int which);
static void open_picker(void);
static void activate_menu(void);
static int add_or_switch(const char *path);
static void next_tab(void);
static void close_ui(void);

static char *put_uint(char *p, int n)
{
	char tmp[12];
	int i = 0;
	if (n <= 0)
	{
		*p++ = '0';
		return p;
	}
	while (n > 0 && i < 11)
	{
		tmp[i++] = (char)('0' + (n % 10));
		n /= 10;
	}
	while (i--)
		*p++ = tmp[i];
	return p;
}

static void ed_copy(char *dst, int dstsz, const char *src)
{
	if (!dst || dstsz <= 0)
		return;
	if (!src)
		src = "";
	strncpy(dst, src, (unsigned)dstsz - 1);
	dst[dstsz - 1] = 0;
}

static void ensure_bas(char *path, int sz)
{
	if (!path[0])
		return;
	if (!strchr(path, '.'))
		strncat(path, ".BAS", (unsigned)sz - strlen(path) - 1);
}

static const char *tab_label(int i)
{
	const char *p = G.ed.tab[i].path;
	const char *s = p;
	if (!p[0])
		return "UNTITLED";
	while (*p)
	{
		if (*p == '/' || *p == ':')
			s = p + 1;
		p++;
	}
	return s[0] ? s : "UNTITLED";
}

static int ch_cols(char ch)
{
	return (ch == '\t') ? ED_TAB : 1;
}

static mmb_ed_tab *cur_tab(void)
{
	if (G.ed.ntabs <= 0 || G.ed.cur < 0 || G.ed.cur >= G.ed.ntabs)
		return 0;
	return &G.ed.tab[G.ed.cur];
}

static void set_status(const char *s)
{
	G.ed.status[0] = 0;
	if (s)
		strncpy(G.ed.status, s, sizeof(G.ed.status) - 1);
}

static void canon_ed_path(const char *path, char *out, int outsz)
{
	char tmp[128];
	out[0] = 0;
	if (!path || !path[0] || outsz < 2)
		return;
	if (mmb_vfs_resolve(path, tmp, sizeof(tmp)) == 0)
		strncpy(out, tmp, (unsigned)outsz - 1);
	else
		strncpy(out, path, (unsigned)outsz - 1);
	out[outsz - 1] = 0;
	ensure_bas(out, outsz);
}

static int find_tab_path(const char *path)
{
	char want[128];
	int i;
	if (!path || !path[0])
		return -1;
	canon_ed_path(path, want, sizeof(want));
	for (i = 0; i < G.ed.ntabs; i++)
	{
		char have[128];
		if (!G.ed.tab[i].used)
			continue;
		canon_ed_path(G.ed.tab[i].path, have, sizeof(have));
		if (mmb_keyword_eq(have, want) || mmb_keyword_eq(G.ed.tab[i].path, path))
			return i;
	}
	return -1;
}

static void load_into(int i, const char *path)
{
	unsigned got = 0;
	mmb_ed_tab *t = &G.ed.tab[i];
	memset(t, 0, sizeof(*t));
	t->used = 1;
	if (path && path[0])
	{
		ed_copy(t->path, sizeof(t->path), path);
		ensure_bas(t->path, sizeof(t->path));
		if (mmb_vfs_read(t->path, t->buf, sizeof(t->buf) - 1, &got) == 0)
			t->len = (int)got;
		t->buf[t->len] = 0;
	}
	t->cx = t->len;
}

static int add_or_switch(const char *path)
{
	char p[128];
	int i;
	p[0] = 0;
	if (path && path[0])
	{
		ed_copy(p, sizeof(p), path);
		ensure_bas(p, sizeof(p));
		i = find_tab_path(p);
		if (i >= 0)
		{
			G.ed.cur = i;
			return i;
		}
	}
	if (G.ed.ntabs >= MMB_ED_TABS)
	{
		set_status("Too many tabs");
		return -1;
	}
	i = G.ed.ntabs++;
	load_into(i, p[0] ? p : path);
	G.ed.cur = i;
	return i;
}

static int ed_ch_eq(char a, char b)
{
	if (a >= 'a' && a <= 'z')
		a = (char)(a - 32);
	if (b >= 'a' && b <= 'z')
		b = (char)(b - 32);
	return a == b;
}

static int ed_str_icmp(const char *a, const char *b)
{
	int i;
	for (i = 0; a[i] || b[i]; i++)
	{
		char ca = a[i], cb = b[i];
		if (ca >= 'a' && ca <= 'z')
			ca = (char)(ca - 32);
		if (cb >= 'a' && cb <= 'z')
			cb = (char)(cb - 32);
		if (ca != cb)
			return (unsigned char)ca - (unsigned char)cb;
	}
	return 0;
}

static int ed_contains(const char *s, const char *sub)
{
	int i, j;
	if (!sub || !sub[0])
		return 1;
	if (!s)
		return 0;
	for (i = 0; s[i]; i++)
	{
		for (j = 0; sub[j] && s[i + j] && ed_ch_eq(s[i + j], sub[j]); j++)
			;
		if (!sub[j])
			return 1;
	}
	return 0;
}

static void ed_join(char *dst, int dstsz, const char *dir, const char *name)
{
	int n;
	strncpy(dst, dir ? dir : "", (unsigned)dstsz - 1);
	dst[dstsz - 1] = 0;
	n = (int)strlen(dst);
	if (n > 0 && dst[n - 1] != '/' && dst[n - 1] != ':' && n + 1 < dstsz)
	{
		dst[n] = '/';
		dst[n + 1] = 0;
	}
	strncat(dst, name ? name : "", (unsigned)dstsz - strlen(dst) - 1);
}

static const char *pick_rel(const char *full)
{
	int i;
	if (!full)
		return "";
	for (i = 0; pick_root[i]; i++)
	{
		if (!full[i] || !ed_ch_eq(full[i], pick_root[i]))
			return full;
	}
	if (full[i] == '/')
		i++;
	return full[i] ? full + i : full;
}

static void pick_sort(void)
{
	int i, j;
	for (i = 0; i < pick_n; i++)
		for (j = i + 1; j < pick_n; j++)
			if (ed_str_icmp(pick_rel(pick_path[j]), pick_rel(pick_path[i])) < 0)
			{
				char tmp[128];
				strncpy(tmp, pick_path[i], sizeof(tmp) - 1);
				tmp[sizeof(tmp) - 1] = 0;
				strncpy(pick_path[i], pick_path[j], sizeof(pick_path[i]) - 1);
				pick_path[i][sizeof(pick_path[i]) - 1] = 0;
				strncpy(pick_path[j], tmp, sizeof(pick_path[j]) - 1);
				pick_path[j][sizeof(pick_path[j]) - 1] = 0;
			}
}

static void pick_rebuild_view(void)
{
	int i, keep = -1, found = 0;
	if (pick_sel >= 0 && pick_sel < pick_vn)
		keep = pick_view[pick_sel];
	pick_vn = 0;
	for (i = 0; i < pick_n; i++)
	{
		if (!ed_contains(pick_rel(pick_path[i]), G.ed.dlg))
			continue;
		if (keep == i)
		{
			pick_sel = pick_vn;
			found = 1;
		}
		pick_view[pick_vn++] = i;
	}
	if (!found)
		pick_sel = 0;
	if (pick_vn <= 0)
		pick_sel = 0;
	else if (pick_sel >= pick_vn)
		pick_sel = pick_vn - 1;
	if (pick_sel < pick_row0)
		pick_row0 = pick_sel;
}

static void pick_walk(const char *dir, int depth)
{
	char list[2048];
	char *s;
	if (!dir || !dir[0] || depth > ED_PICK_DEPTH || pick_n >= ED_PICK_MAX)
		return;
	list[0] = 0;
	if (mmb_vfs_list(dir, list, sizeof(list)) != 0)
		return;
	s = list;
	while (*s && pick_n < ED_PICK_MAX)
	{
		char name[128];
		int n = 0, is_dir = 0;
		while (*s && *s != '\n' && *s != '\r' && n < (int)sizeof(name) - 1)
			name[n++] = *s++;
		name[n] = 0;
		while (*s == '\r' || *s == '\n')
			s++;
		if (n > 0 && name[n - 1] == '/')
		{
			name[n - 1] = 0;
			is_dir = 1;
		}
		if (!name[0] || (name[0] == '.' && (!name[1] || (name[1] == '.' && !name[2]))))
			continue;
		{
			char full[128];
			ed_join(full, sizeof(full), dir, name);
			if (is_dir)
				pick_walk(full, depth + 1);
			else
			{
				strncpy(pick_path[pick_n], full, sizeof(pick_path[0]) - 1);
				pick_path[pick_n][sizeof(pick_path[0]) - 1] = 0;
				pick_n++;
			}
		}
	}
}

static void set_pick_root(const char *path)
{
	char full[128];
	const char *src = (path && path[0]) ? path : mmb_vfs_cwd();
	pick_root[0] = 0;
	if (mmb_vfs_resolve(src, full, sizeof(full)) != 0)
	{
		strncpy(pick_root, mmb_vfs_cwd(), sizeof(pick_root) - 1);
		pick_root[sizeof(pick_root) - 1] = 0;
		return;
	}
	if (!(mmb_vfs_exists(full) && mmb_vfs_size(full) < 0))
	{
		char *slash = 0;
		char *q = full;
		while (*q)
		{
			if (*q == '/')
				slash = q;
			q++;
		}
		if (slash)
		{
			if (slash <= full + 2)
				slash[1] = 0;
			else
				*slash = 0;
		}
	}
	strncpy(pick_root, full, sizeof(pick_root) - 1);
	pick_root[sizeof(pick_root) - 1] = 0;
}

static void pick_geom(int *w, int *h, int *r0, int *c0)
{
	int ww = 58, hh = 16;
	if (ww > COLS - 2)
		ww = COLS - 2;
	if (hh > ROWS - 2)
		hh = ROWS - 2;
	if (w)
		*w = ww;
	if (h)
		*h = hh;
	if (r0)
	{
		*r0 = (ROWS - hh) / 2;
		if (*r0 < 2)
			*r0 = 2;
	}
	if (c0)
	{
		*c0 = (COLS - ww) / 2;
		if (*c0 < 0)
			*c0 = 0;
	}
}

static int pick_list_h(int h)
{
	int n = h - 6;
	return n > 1 ? n : 1;
}

static void pick_move(int delta)
{
	int vis;
	int w, h, r0, c0;
	if (pick_vn <= 0)
		return;
	pick_sel += delta;
	if (pick_sel < 0)
		pick_sel = 0;
	if (pick_sel >= pick_vn)
		pick_sel = pick_vn - 1;
	pick_geom(&w, &h, &r0, &c0);
	(void)w;
	(void)r0;
	(void)c0;
	vis = pick_list_h(h);
	if (pick_sel < pick_row0)
		pick_row0 = pick_sel;
	if (pick_sel >= pick_row0 + vis)
		pick_row0 = pick_sel - vis + 1;
}

static void draw_picker(void)
{
	int w, h, r0, c0, i, vis, y;
	const char *title = " Quick open ";
	pick_geom(&w, &h, &r0, &c0);
	vis = pick_list_h(h);
	if (pick_sel < pick_row0)
		pick_row0 = pick_sel;
	if (pick_sel >= pick_row0 + vis)
		pick_row0 = pick_sel - vis + 1;
	if (pick_row0 < 0)
		pick_row0 = 0;
	tui_frame(c0, r0, w, h, C_DLG_FG, C_DLG_BG);
	{
		int left = (w - 2 - (int)strlen(title)) / 2;
		if (left < 1)
			left = 1;
		tui_puts(c0 + 1 + left, r0, title, C_DLG_FG, C_DLG_BG);
	}
	tui_pad(c0 + w, r0, "", 2, C_SH_FG, C_SH_BG);
	for (i = 1; i < h - 1; i++)
	{
		tui_pad(c0 + 1, r0 + i, "", w - 2, C_DLG_FG, C_DLG_BG);
		tui_pad(c0 + w, r0 + i, "", 2, C_SH_FG, C_SH_BG);
	}
	tui_pad(c0 + w, r0 + h - 1, "", 2, C_SH_FG, C_SH_BG);
	tui_pad(c0 + 2, r0 + h, "", w, C_SH_FG, C_SH_BG);
	tui_pad(c0 + 2, r0 + 1, pick_root, w - 4, C_DLG_FG, C_DLG_BG);
	tui_pad(c0 + 2, r0 + 2, G.ed.dlg[0] ? G.ed.dlg : "(type to filter)", w - 4,
		G.ed.dlg[0] ? C_SEL_FG : C_DLG_FG,
		G.ed.dlg[0] ? C_SEL_BG : C_DLG_BG);
	for (i = 0; i < vis; i++)
	{
		int fg = C_DLG_FG, bg = C_DLG_BG;
		const char *lab = "";
		y = r0 + 3 + i;
		if (pick_row0 + i < pick_vn)
		{
			int idx = pick_view[pick_row0 + i];
			lab = pick_rel(pick_path[idx]);
			if (pick_row0 + i == pick_sel)
			{
				fg = C_SEL_FG;
				bg = C_SEL_BG;
			}
		}
		tui_pad(c0 + 2, y, lab, w - 4, fg, bg);
	}
	tui_pad(c0 + 2, r0 + h - 2, "Enter=open  Esc=cancel  Up/Down", w - 4,
		C_DLG_FG, C_DLG_BG);
}

static void open_picker(void)
{
	G.ed.menu_open = 0;
	G.ed.dialog = DLG_PICK;
	G.ed.dlg[0] = 0;
	G.ed.dlglen = 0;
	pick_n = 0;
	pick_sel = 0;
	pick_row0 = 0;
	pick_vn = 0;
	if (!pick_root[0])
		set_pick_root(mmb_vfs_cwd());
	pick_walk(pick_root, 0);
	pick_sort();
	pick_rebuild_view();
}

static void pos_to_rowcol(int pos, int *row, int *col)
{
	mmb_ed_tab *t = cur_tab();
	int r = 0, c = 0, i;
	if (!t)
	{
		if (row)
			*row = 0;
		if (col)
			*col = 0;
		return;
	}
	for (i = 0; i < pos && i < t->len; i++)
	{
		if (t->buf[i] == '\n')
		{
			r++;
			c = 0;
		}
		else
			c += ch_cols(t->buf[i]);
	}
	if (row)
		*row = r;
	if (col)
		*col = c;
}

static int rowcol_to_pos(int row, int col)
{
	mmb_ed_tab *t = cur_tab();
	int r = 0, c = 0, i;
	if (!t)
		return 0;
	for (i = 0; i < t->len; i++)
	{
		if (r == row && c == col)
			return i;
		if (t->buf[i] == '\n')
		{
			if (r == row)
				return i;
			r++;
			c = 0;
		}
		else
		{
			int w = ch_cols(t->buf[i]);
			if (r == row && col > c && col < c + w)
				return i;
			c += w;
		}
	}
	if (r == row && c == col)
		return i;
	return t->len;
}

static int line_end(int pos)
{
	mmb_ed_tab *t = cur_tab();
	if (!t)
		return 0;
	while (pos < t->len && t->buf[pos] != '\n')
		pos++;
	return pos;
}

static int line_start(int pos)
{
	mmb_ed_tab *t = cur_tab();
	if (!t)
		return 0;
	while (pos > 0 && t->buf[pos - 1] != '\n')
		pos--;
	return pos;
}

static int is_word_char(char c)
{
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
	       (c >= '0' && c <= '9') || c == '_';
}

static int sel_bounds(int *lo, int *hi)
{
	mmb_ed_tab *t = cur_tab();
	int a, b;
	if (!t || !t->sel)
		return 0;
	a = t->sel_anchor;
	b = t->cx;
	if (a > b)
	{
		int x = a;
		a = b;
		b = x;
	}
	if (a < 0)
		a = 0;
	if (b > t->len)
		b = t->len;
	if (a >= b)
		return 0;
	if (lo)
		*lo = a;
	if (hi)
		*hi = b;
	return 1;
}

static int in_sel(int off)
{
	int lo, hi;
	if (!sel_bounds(&lo, &hi))
		return 0;
	return off >= lo && off < hi;
}

static void sel_clear(void)
{
	mmb_ed_tab *t = cur_tab();
	if (t)
		t->sel = 0;
}

static void sel_prepare(int shift)
{
	mmb_ed_tab *t = cur_tab();
	if (!t)
		return;
	if (!shift)
	{
		t->sel = 0;
		return;
	}
	if (!t->sel)
	{
		t->sel_anchor = t->cx;
		t->sel = 1;
	}
}

static void clip_store(const char *s, int n)
{
	if (n >= (int)sizeof(killbuf))
		n = (int)sizeof(killbuf) - 1;
	if (n < 0)
		n = 0;
	memcpy(killbuf, s, (unsigned)n);
	killlen = n;
	killbuf[killlen] = 0;
}

static int delete_range(int lo, int hi, int to_clip)
{
	mmb_ed_tab *t = cur_tab();
	int n;
	if (!t || hi <= lo)
		return 0;
	n = hi - lo;
	if (to_clip)
		clip_store(t->buf + lo, n);
	memmove(t->buf + lo, t->buf + hi, (unsigned)(t->len - hi + 1));
	t->len -= n;
	t->cx = lo;
	t->sel = 0;
	t->dirty = 1;
	return 1;
}

static int delete_selection(int to_clip)
{
	int lo, hi;
	if (!sel_bounds(&lo, &hi))
	{
		sel_clear();
		return 0;
	}
	return delete_range(lo, hi, to_clip);
}

static void copy_selection(void)
{
	int lo, hi;
	mmb_ed_tab *t = cur_tab();
	if (!sel_bounds(&lo, &hi) || !t)
		return;
	clip_store(t->buf + lo, hi - lo);
	set_status("Copied");
}

static void cut_selection(void)
{
	if (delete_selection(1))
		set_status("Cut");
}

static void move_word_left(void)
{
	mmb_ed_tab *t = cur_tab();
	if (!t || t->cx <= 0)
		return;
	t->cx--;
	while (t->cx > 0 && !is_word_char(t->buf[t->cx]))
		t->cx--;
	while (t->cx > 0 && is_word_char(t->buf[t->cx - 1]))
		t->cx--;
}

static void move_word_right(void)
{
	mmb_ed_tab *t = cur_tab();
	if (!t)
		return;
	while (t->cx < t->len && is_word_char(t->buf[t->cx]))
		t->cx++;
	while (t->cx < t->len && !is_word_char(t->buf[t->cx]))
		t->cx++;
}

static void move_file_home(void)
{
	mmb_ed_tab *t = cur_tab();
	if (t)
		t->cx = 0;
}

static void move_file_end(void)
{
	mmb_ed_tab *t = cur_tab();
	if (t)
		t->cx = t->len;
}

static void ensure_visible(void)
{
	mmb_ed_tab *t = cur_tab();
	int row, col;
	if (!t)
		return;
	pos_to_rowcol(t->cx, &row, &col);
	if (row < t->row0)
		t->row0 = row;
	if (row >= t->row0 + TEXT_ROWS)
		t->row0 = row - TEXT_ROWS + 1;
	if (col < t->col0)
		t->col0 = col;
	if (col >= t->col0 + TEXT_COLS)
		t->col0 = col - TEXT_COLS + 1;
	if (t->row0 < 0)
		t->row0 = 0;
	if (t->col0 < 0)
		t->col0 = 0;
}

static void insert_char(char c)
{
	mmb_ed_tab *t = cur_tab();
	if (!t || t->len >= (int)sizeof(t->buf) - 1)
		return;
	delete_selection(0);
	if (t->len >= (int)sizeof(t->buf) - 1)
		return;
	if (t->cx < t->len)
		memmove(t->buf + t->cx + 1, t->buf + t->cx, (unsigned)(t->len - t->cx));
	t->buf[t->cx++] = c;
	t->len++;
	t->buf[t->len] = 0;
	t->dirty = 1;
}

static int insert_at(int pos, const char *s, int n)
{
	mmb_ed_tab *t = cur_tab();

	if (!t || !s || n <= 0)
		return 0;
	if (pos < 0)
		pos = 0;
	if (pos > t->len)
		pos = t->len;
	if (t->len + n >= (int)sizeof(t->buf) - 1)
		n = (int)sizeof(t->buf) - 1 - t->len;
	if (n <= 0)
		return 0;
	if (pos < t->len)
		memmove(t->buf + pos + n, t->buf + pos, (unsigned)(t->len - pos));
	memcpy(t->buf + pos, s, (unsigned)n);
	t->len += n;
	t->buf[t->len] = 0;
	t->dirty = 1;
	return n;
}

static void shift_off(int *p, int at, int delta)
{
	if (!p)
		return;
	if (delta > 0)
	{
		if (*p >= at)
			*p += delta;
		return;
	}
	if (*p > at)
	{
		int d = -delta;
		if (*p - at < d)
			*p = at;
		else
			*p -= d;
	}
}

static void indent_lines(int outdent)
{
	mmb_ed_tab *t = cur_tab();
	int lo, hi, s, last, n = 0, i;
	int starts[256];
	char pad[ED_TAB];

	if (!t)
		return;
	if (sel_bounds(&lo, &hi))
	{
		if (hi > lo && t->buf[hi - 1] == '\n')
			hi--;
		s = line_start(lo);
		last = line_start(hi > 0 ? hi : lo);
	}
	else
	{
		s = last = line_start(t->cx);
		lo = hi = t->cx;
	}
	while (n < 256 && s <= last)
	{
		starts[n++] = s;
		s = line_end(s);
		if (s < t->len && t->buf[s] == '\n')
			s++;
		else
			break;
	}
	for (i = 0; i < ED_TAB; i++)
		pad[i] = ' ';
	for (i = n - 1; i >= 0; i--)
	{
		int p = starts[i];
		int k = 0;

		if (outdent)
		{
			while (k < ED_TAB && p + k < t->len && t->buf[p + k] == ' ')
				k++;
			if (k == 0 && p < t->len && t->buf[p] == '\t')
				k = 1;
			if (k)
			{
				memmove(t->buf + p, t->buf + p + k,
					(unsigned)(t->len - p - k + 1));
				t->len -= k;
				shift_off(&t->cx, p, -k);
				if (t->sel)
					shift_off(&t->sel_anchor, p, -k);
			}
		}
		else
		{
			k = insert_at(p, pad, ED_TAB);
			shift_off(&t->cx, p, k);
			if (t->sel)
				shift_off(&t->sel_anchor, p, k);
		}
	}
}

static void insert_newline_indent(void)
{
	mmb_ed_tab *t = cur_tab();
	char indent[64];
	int n = 0;
	int i;

	if (!t)
		return;
	delete_selection(0);
	i = t->cx;
	while (i > 0 && t->buf[i - 1] != '\n')
		i--;
	while (i < t->len && t->buf[i] != '\n' && n < (int)sizeof(indent) &&
	       (t->buf[i] == ' ' || t->buf[i] == '\t'))
		indent[n++] = t->buf[i++];
	insert_char('\n');
	for (i = 0; i < n; i++)
		insert_char(indent[i]);
}

static void backspace(void)
{
	mmb_ed_tab *t = cur_tab();
	if (!t)
		return;
	if (delete_selection(0))
		return;
	if (t->cx <= 0)
		return;
	memmove(t->buf + t->cx - 1, t->buf + t->cx, (unsigned)(t->len - t->cx + 1));
	t->cx--;
	t->len--;
	t->dirty = 1;
}

static void delete_char(void)
{
	mmb_ed_tab *t = cur_tab();
	if (!t)
		return;
	if (delete_selection(0))
		return;
	if (t->cx >= t->len)
		return;
	memmove(t->buf + t->cx, t->buf + t->cx + 1, (unsigned)(t->len - t->cx));
	t->len--;
	t->dirty = 1;
}

static void move_left(void)
{
	mmb_ed_tab *t = cur_tab();
	if (t && t->cx > 0)
		t->cx--;
}

static void move_right(void)
{
	mmb_ed_tab *t = cur_tab();
	if (t && t->cx < t->len)
		t->cx++;
}

static void move_up(void)
{
	mmb_ed_tab *t = cur_tab();
	int row, col;
	if (!t)
		return;
	pos_to_rowcol(t->cx, &row, &col);
	if (row > 0)
		t->cx = rowcol_to_pos(row - 1, col);
}

static void move_down(void)
{
	mmb_ed_tab *t = cur_tab();
	int row, col;
	if (!t)
		return;
	pos_to_rowcol(t->cx, &row, &col);
	t->cx = rowcol_to_pos(row + 1, col);
}

static void move_home(void)
{
	mmb_ed_tab *t = cur_tab();
	int row, col;
	if (!t)
		return;
	pos_to_rowcol(t->cx, &row, &col);
	t->cx = rowcol_to_pos(row, 0);
}

static void move_end(void)
{
	mmb_ed_tab *t = cur_tab();
	if (t)
		t->cx = line_end(t->cx);
}

static void page_up(void)
{
	mmb_ed_tab *t = cur_tab();
	int row, col;
	if (!t)
		return;
	pos_to_rowcol(t->cx, &row, &col);
	row -= TEXT_ROWS;
	if (row < 0)
		row = 0;
	t->cx = rowcol_to_pos(row, col);
}

static void page_down(void)
{
	mmb_ed_tab *t = cur_tab();
	int row, col;
	if (!t)
		return;
	pos_to_rowcol(t->cx, &row, &col);
	t->cx = rowcol_to_pos(row + TEXT_ROWS, col);
}

static void cut_line(void)
{
	mmb_ed_tab *t = cur_tab();
	int end;
	if (!t)
		return;
	sel_clear();
	end = line_end(t->cx);
	if (end < t->len && t->buf[end] == '\n')
		end++;
	killlen = 0;
	if (end > t->cx)
	{
		clip_store(t->buf + t->cx, end - t->cx);
		memmove(t->buf + t->cx, t->buf + end, (unsigned)(t->len - end + 1));
		t->len -= (end - t->cx);
		t->dirty = 1;
	}
}

static void paste_kill(void)
{
	mmb_ed_tab *t = cur_tab();
	int n;
	if (!t || killlen <= 0)
		return;
	delete_selection(0);
	n = killlen;
	if (t->len + n >= (int)sizeof(t->buf) - 1)
		n = (int)sizeof(t->buf) - 1 - t->len;
	if (n <= 0)
		return;
	if (t->cx < t->len)
		memmove(t->buf + t->cx + n, t->buf + t->cx, (unsigned)(t->len - t->cx + 1));
	memcpy(t->buf + t->cx, killbuf, (unsigned)n);
	t->cx += n;
	t->len += n;
	t->buf[t->len] = 0;
	t->dirty = 1;
}

static const char **menu_items(int menu, int *n)
{
	switch (menu)
	{
	case MENU_FILE:
		*n = (int)(sizeof(file_items) / sizeof(file_items[0]));
		return file_items;
	case MENU_EDIT:
		*n = (int)(sizeof(edit_items) / sizeof(edit_items[0]));
		return edit_items;
	case MENU_RUN:
		*n = (int)(sizeof(run_items) / sizeof(run_items[0]));
		return run_items;
	case MENU_THEME:
	{
		static const char *names[ED_THEME_N];
		static int ready;
		int i;
		if (!ready)
		{
			for (i = 0; i < ED_THEME_N; i++)
				names[i] = k_themes[i].name;
			ready = 1;
		}
		*n = ED_THEME_N;
		return names;
	}
	default:
		*n = (int)(sizeof(help_items) / sizeof(help_items[0]));
		return help_items;
	}
}

static const char *menu_hots(int menu)
{
	switch (menu)
	{
	case MENU_FILE:
		return file_hots;
	case MENU_EDIT:
		return edit_hots;
	case MENU_RUN:
		return run_hots;
	case MENU_THEME:
		return theme_hots;
	default:
		return help_hots;
	}
}

static int menu_width(int menu)
{
	int n, i, w = 10;
	const char **it = menu_items(menu, &n);
	for (i = 0; i < n; i++)
	{
		int L = (int)strlen(it[i]);
		if (L + 2 > w)
			w = L + 2;
	}
	return w + 2;
}

static void draw_hot_word(int *x, int y, const char *word, char hot, int selected)
{
	int i;
	int fg = selected ? C_SEL_FG : C_MENU_FG;
	int bg = selected ? C_SEL_BG : C_MENU_BG;
	for (i = 0; word[i]; i++)
	{
		int c_fg = fg;
		if ((word[i] == hot || word[i] == hot + 32 || word[i] == hot - 32) &&
		    (i == 0 || hot == word[i]))
			c_fg = C_HOT;
		tui_put(*x, y, (unsigned char)word[i], c_fg, bg);
		(*x)++;
	}
}

static void draw_menu_bar(void)
{
	int i, x = 0;
	tui_put(x++, ROW_MENU, ' ', C_MENU_FG, C_MENU_BG);
	for (i = 0; i < MENU_COUNT; i++)
	{
		int sel = G.ed.menu_open && G.ed.menu == i;
		menu_x[i] = x;
		draw_hot_word(&x, ROW_MENU, menu_name[i], menu_hot[i], sel);
		tui_put(x++, ROW_MENU, ' ', C_MENU_FG, C_MENU_BG);
	}
	if (x < COLS)
		tui_pad(x, ROW_MENU, "", COLS - x, C_MENU_FG, C_MENU_BG);
}

static void draw_tabs(void)
{
	int i, x = 0;
	for (i = 0; i < G.ed.ntabs && x < COLS; i++)
	{
		const char *name = tab_label(i);
		int n = (int)strlen(name);
		int fg = (i == G.ed.cur) ? C_TABCUR_FG : C_TAB_FG;
		int bg = (i == G.ed.cur) ? C_TABCUR_BG : C_TAB_BG;
		char lab[16];
		int k;
		if (n > 12)
			n = 12;
		if (x + n + 3 > COLS)
			break;
		tui_put(x++, ROW_TABS, ' ', fg, bg);
		for (k = 0; k < n; k++)
			lab[k] = name[k];
		lab[n] = 0;
		tui_puts(x, ROW_TABS, lab, fg, bg);
		x += n;
		tui_put(x++, ROW_TABS, G.ed.tab[i].dirty ? '*' : ' ', fg, bg);
		tui_put(x++, ROW_TABS, ' ', fg, bg);
	}
	if (x < COLS)
		tui_pad(x, ROW_TABS, "", COLS - x, C_TAB_FG, C_TAB_BG);
}

static void draw_border_row(int row, int top)
{
	mmb_ed_tab *t = cur_tab();
	const char *title;
	char titled[40];
	int n, left, i;
	tui_hline(0, row, COLS, TUI_TL, TUI_H, TUI_TR, C_BRD_FG, C_BRD_BG);
	if (!top)
	{
		int r = 1, c = 1;
		char loc[24];
		char *p;
		if (t)
			pos_to_rowcol(t->cx, &r, &c);
		p = loc;
		p = put_uint(p, r + 1);
		*p++ = ':';
		p = put_uint(p, c + 1);
		*p = 0;
		tui_put(0, row, TUI_BL, C_BRD_FG, C_BRD_BG);
		tui_puts(1, row, loc, C_BRD_FG, C_BRD_BG);
		tui_put(COLS - 1, row, TUI_BR, C_BRD_FG, C_BRD_BG);
		return;
	}
	title = t ? tab_label(G.ed.cur) : "UNTITLED";
	titled[0] = ' ';
	n = (int)strlen(title);
	if (n > 24)
		n = 24;
	for (i = 0; i < n; i++)
		titled[i + 1] = title[i];
	titled[n + 1] = ' ';
	titled[n + 2] = 0;
	n += 2;
	left = (COLS - 2 - n) / 2;
	if (left < 1)
		left = 1;
	tui_puts(1 + left, row, titled, C_BRD_FG, C_BRD_BG);
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

static void draw_text_line(int x, int y, const char *s, int n, int col0, int buf_off)
{
	int i, vis = 0, shown = 0, in_str = 0, in_cmt;
	int pad_mark;
	in_cmt = line_is_comment(s, n);
	for (i = 0; i < n && shown < TEXT_COLS; i++)
	{
		char ch = s[i];
		int fg, bg, k, w, marked;
		if (!in_cmt && !in_str && ch == '\'')
			in_cmt = 1;
		if (!in_cmt && !in_str && ch == '"')
			in_str = 1;
		else if (in_str && ch == '"')
			in_str = 2;
		marked = in_sel(buf_off + i);
		if (marked)
		{
			fg = C_MARK_FG;
			bg = C_MARK_BG;
		}
		else if (in_cmt)
		{
			fg = C_CMT_FG;
			bg = C_EDIT_BG;
		}
		else if (in_str)
		{
			fg = C_STR_FG;
			bg = C_EDIT_BG;
		}
		else if (ch >= '0' && ch <= '9')
		{
			fg = C_NUM_FG;
			bg = C_EDIT_BG;
		}
		else
		{
			fg = C_EDIT_FG;
			bg = C_EDIT_BG;
		}
		w = ch_cols(ch);
		for (k = 0; k < w && shown < TEXT_COLS; k++)
		{
			if (vis >= col0)
			{
				char out = (ch == '\t') ? ' ' : ch;
				tui_put(x + shown, y, (unsigned char)out, fg, bg);
				shown++;
			}
			vis++;
		}
		if (in_str == 2)
			in_str = 0;
	}
	pad_mark = in_sel(buf_off + n);
	if (shown < TEXT_COLS)
		tui_pad(x + shown, y, "", TEXT_COLS - shown,
			pad_mark ? C_MARK_FG : C_EDIT_FG,
			pad_mark ? C_MARK_BG : C_EDIT_BG);
}

static void draw_empty_text_row(int y)
{
	tui_put(0, y, TUI_V, C_BRD_FG, C_BRD_BG);
	tui_pad(1, y, "", TEXT_COLS, C_EDIT_FG, C_EDIT_BG);
	tui_put(COLS - 1, y, TUI_V, C_BRD_FG, C_BRD_BG);
}

static void draw_editor_body(void)
{
	mmb_ed_tab *t = cur_tab();
	int vis, i, pos, row;
	if (!t)
	{
		for (vis = 0; vis < TEXT_ROWS; vis++)
			draw_empty_text_row(ROW_TEXT + vis);
		return;
	}
	pos = 0;
	row = 0;
	while (row < t->row0 && pos < t->len)
	{
		if (t->buf[pos] == '\n')
			row++;
		pos++;
	}
	for (vis = 0; vis < TEXT_ROWS; vis++)
	{
		int start = pos, n = 0;
		int y = ROW_TEXT + vis;
		tui_put(0, y, TUI_V, C_BRD_FG, C_BRD_BG);
		if (pos > t->len)
			pos = t->len;
		while (pos < t->len && t->buf[pos] != '\n')
		{
			n++;
			pos++;
		}
		draw_text_line(1, y, t->buf + start, n, t->col0, start);
		tui_put(COLS - 1, y, TUI_V, C_BRD_FG, C_BRD_BG);
		if (pos < t->len && t->buf[pos] == '\n')
			pos++;
		else if (pos >= t->len && vis + 1 < TEXT_ROWS)
		{
			for (i = vis + 1; i < TEXT_ROWS; i++)
				draw_empty_text_row(ROW_TEXT + i);
			break;
		}
	}
}

static void draw_dropdown(void)
{
	int n, i, w, r0, c0;
	const char **it;
	int fg, bg;
	if (!G.ed.menu_open)
		return;
	it = menu_items(G.ed.menu, &n);
	w = menu_width(G.ed.menu);
	c0 = menu_x[G.ed.menu];
	if (c0 + w + 2 > COLS)
		c0 = COLS - w - 2;
	if (c0 < 0)
		c0 = 0;
	r0 = ROW_TABS;
	tui_hline(c0, r0, w, TUI_TL, TUI_H, TUI_TR, C_DLG_FG, C_DLG_BG);
	tui_pad(c0 + w, r0, "", 2, C_SH_FG, C_SH_BG);
	for (i = 0; i < n; i++)
	{
		fg = (i == G.ed.menu_item) ? C_SEL_FG : C_DLG_FG;
		bg = (i == G.ed.menu_item) ? C_SEL_BG : C_DLG_BG;
		tui_put(c0, r0 + 1 + i, TUI_V, C_DLG_FG, C_DLG_BG);
		tui_put(c0 + 1, r0 + 1 + i, ' ', fg, bg);
		tui_pad(c0 + 2, r0 + 1 + i, it[i], w - 4, fg, bg);
		tui_put(c0 + w - 2, r0 + 1 + i, ' ', fg, bg);
		tui_put(c0 + w - 1, r0 + 1 + i, TUI_V, C_DLG_FG, C_DLG_BG);
		tui_pad(c0 + w, r0 + 1 + i, "", 2, C_SH_FG, C_SH_BG);
	}
	tui_hline(c0, r0 + 1 + n, w, TUI_BL, TUI_H, TUI_BR, C_DLG_FG, C_DLG_BG);
	tui_pad(c0 + w, r0 + 1 + n, "", 2, C_SH_FG, C_SH_BG);
	tui_pad(c0 + 2, r0 + 2 + n, "", w, C_SH_FG, C_SH_BG);
}

static int fd_on(void)
{
	return G.ed.dialog == DLG_OPEN || G.ed.dialog == DLG_SAVEAS;
}

static void fd_copy(char *dst, int n, const char *s)
{
	if (!dst || n <= 0)
		return;
	if (!s)
		s = "";
	strncpy(dst, s, (unsigned)n - 1);
	dst[n - 1] = 0;
}

static int fd_icmp(const char *a, const char *b)
{
	for (;;)
	{
		unsigned char ca = (unsigned char)*a++;
		unsigned char cb = (unsigned char)*b++;
		if (ca >= 'a' && ca <= 'z')
			ca = (unsigned char)(ca - 32);
		if (cb >= 'a' && cb <= 'z')
			cb = (unsigned char)(cb - 32);
		if (ca != cb)
			return (int)ca - (int)cb;
		if (!ca)
			return 0;
	}
}

static int fd_match(const char *name, const char *pat)
{
	const char *n, *p, *star, *match;
	if (!pat || !pat[0] || (pat[0] == '*' && pat[1] == 0))
		return 1;
	n = name;
	p = pat;
	star = 0;
	match = 0;
	while (*n)
	{
		char cn = *n, cp = *p;
		if (cn >= 'a' && cn <= 'z')
			cn = (char)(cn - 32);
		if (cp >= 'a' && cp <= 'z')
			cp = (char)(cp - 32);
		if (cp == '*')
		{
			star = p++;
			match = n;
			continue;
		}
		if (cp == '?' || cn == cp)
		{
			n++;
			p++;
			continue;
		}
		if (star)
		{
			p = star + 1;
			match++;
			n = match;
			continue;
		}
		return 0;
	}
	while (*p == '*')
		p++;
	return *p == 0;
}

static int fd_has_glob(const char *s)
{
	return s && (strchr(s, '*') || strchr(s, '?'));
}

static const char *fd_rchr(const char *s, char ch)
{
	const char *last = 0;
	if (!s)
		return 0;
	for (; *s; s++)
		if (*s == ch)
			last = s;
	return last;
}

static int fd_is_dir(const char *path)
{
	if (!path || !path[0])
		return 0;
	if (!mmb_vfs_exists(path))
		return 0;
	return mmb_vfs_size(path) < 0;
}

static void fd_parent(char *path)
{
	char *slash = 0, *p;
	if (!path[0])
		return;
	if (path[0] && path[1] == ':' && path[2] == 0)
	{
		path[2] = '/';
		path[3] = 0;
		return;
	}
	for (p = path + 2; *p; p++)
		if (*p == '/')
			slash = p;
	if (!slash || slash <= path + 2)
	{
		if (path[0] && path[1] == ':')
		{
			path[2] = '/';
			path[3] = 0;
		}
		return;
	}
	*slash = 0;
}

static void fd_join(char *dst, int n, const char *dir, const char *name)
{
	int last;
	fd_copy(dst, n, dir);
	if (!name || !name[0])
		return;
	last = dst[0] ? (int)(unsigned char)dst[strlen(dst) - 1] : 0;
	if (last && last != '/' && last != ':')
		strncat(dst, "/", (unsigned)n - strlen(dst) - 1);
	else if (last == ':')
		strncat(dst, "/", (unsigned)n - strlen(dst) - 1);
	strncat(dst, name, (unsigned)n - strlen(dst) - 1);
}

static void fd_make_full(char *dst, int n, const char *name)
{
	if (!name || !name[0])
	{
		fd_copy(dst, n, fd_dir);
		return;
	}
	if (name[1] == ':')
	{
		fd_copy(dst, n, name);
		return;
	}
	fd_join(dst, n, fd_dir, name);
}

static void fd_basename(char *out, int n, const char *path)
{
	const char *s = path, *p;
	if (!path)
		path = "";
	for (p = path; *p; p++)
		if (*p == '/' || *p == ':')
			s = p + 1;
	fd_copy(out, n, s);
}

static void fd_dirname(char *out, int n, const char *path)
{
	char tmp[128];
	char *slash;
	fd_copy(tmp, sizeof(tmp), path);
	slash = (char *)fd_rchr(tmp, '/');
	if (!slash)
	{
		out[0] = 0;
		return;
	}
	if (slash <= tmp + 2 && tmp[1] == ':')
	{
		tmp[2] = '/';
		tmp[3] = 0;
		fd_copy(out, n, tmp);
		return;
	}
	*slash = 0;
	if (tmp[0] && tmp[1] == ':' && tmp[2] == 0)
	{
		tmp[2] = '/';
		tmp[3] = 0;
	}
	fd_copy(out, n, tmp);
}

static void fd_add(char arr[][FD_NAME], int *n, const char *s)
{
	int i;
	if (*n >= FD_MAX || !s || !s[0])
		return;
	for (i = 0; i < *n; i++)
		if (mmb_keyword_eq(arr[i], s))
			return;
	strncpy(arr[*n], s, FD_NAME - 1);
	arr[*n][FD_NAME - 1] = 0;
	(*n)++;
}

static void fd_swap(char arr[][FD_NAME], int i, int j)
{
	char t[FD_NAME];
	memcpy(t, arr[i], FD_NAME);
	memcpy(arr[i], arr[j], FD_NAME);
	memcpy(arr[j], t, FD_NAME);
}

static int fd_dir_rank(const char *s)
{
	if (s[0] == '.' && s[1] == '.' && s[2] == 0)
		return 0;
	if (s[0] == '[' && s[1] == '-')
		return 2;
	return 1;
}

static void fd_sort_files(void)
{
	int i, j;
	for (i = 0; i < fd_nfile; i++)
		for (j = i + 1; j < fd_nfile; j++)
			if (fd_icmp(fd_files[j], fd_files[i]) < 0)
				fd_swap(fd_files, i, j);
}

static void fd_sort_dirs(void)
{
	int i, j;
	for (i = 0; i < fd_ndir; i++)
		for (j = i + 1; j < fd_ndir; j++)
		{
			int ra = fd_dir_rank(fd_dirs[i]);
			int rb = fd_dir_rank(fd_dirs[j]);
			if (rb < ra || (rb == ra && fd_icmp(fd_dirs[j], fd_dirs[i]) < 0))
				fd_swap(fd_dirs, i, j);
		}
}

static void fd_scan(void)
{
	char list[2048], drives[256];
	char *s, *nl;
	fd_nfile = 0;
	fd_ndir = 0;
	fd_add(fd_dirs, &fd_ndir, "..");
	list[0] = 0;
	if (mmb_vfs_list(fd_dir, list, sizeof(list)) != 0)
		list[0] = 0;
	s = list;
	while (*s)
	{
		char name[FD_NAME];
		int n, is_dir = 0;
		nl = s;
		while (*nl && *nl != '\n' && *nl != '\r')
			nl++;
		n = (int)(nl - s);
		if (n >= FD_NAME)
			n = FD_NAME - 1;
		memcpy(name, s, (unsigned)n);
		name[n] = 0;
		if (n > 0 && name[n - 1] == '/')
			is_dir = 1;
		if (name[0] && !(name[0] == '.' && name[1] == 0) &&
		    !(name[0] == '.' && name[1] == '.' &&
		      (name[2] == 0 || name[2] == '/')))
		{
			if (is_dir)
				fd_add(fd_dirs, &fd_ndir, name);
			else if (fd_match(name, fd_mask))
				fd_add(fd_files, &fd_nfile, name);
		}
		s = nl;
		if (*s == '\r')
			s++;
		if (*s == '\n')
			s++;
	}
	drives[0] = 0;
	mmb_vfs_drives(drives, sizeof(drives));
	s = drives;
	while (*s)
	{
		nl = s;
		while (*nl && *nl != '\n' && *nl != '\r')
			nl++;
		if (s[0] && s[1] == ':')
		{
			char spec[8];
			char L = s[0];
			if (L >= 'a' && L <= 'z')
				L = (char)(L - 32);
			spec[0] = '[';
			spec[1] = '-';
			spec[2] = L;
			spec[3] = '-';
			spec[4] = ']';
			spec[5] = 0;
			fd_add(fd_dirs, &fd_ndir, spec);
		}
		s = nl;
		if (*s == '\r')
			s++;
		if (*s == '\n')
			s++;
	}
	fd_sort_files();
	fd_sort_dirs();
	fd_fsel = 0;
	fd_dsel = 0;
	fd_ftop = 0;
	fd_dtop = 0;
}

static void fd_geom(int *c0, int *r0, int *w, int *h)
{
	*w = 62;
	*h = 18;
	if (*w > COLS - 2)
		*w = COLS - 2;
	if (*h > ROWS - 2)
		*h = ROWS - 2;
	*r0 = (ROWS - *h) / 2;
	*c0 = (COLS - *w) / 2;
	if (*r0 < 2)
		*r0 = 2;
	if (*c0 < 0)
		*c0 = 0;
}

static int fd_list_h(void)
{
	int c0, r0, w, h, lh;
	fd_geom(&c0, &r0, &w, &h);
	lh = h - 8;
	return lh < 4 ? 4 : lh;
}

static void fd_clamp(void)
{
	int lh = fd_list_h();
	if (fd_nfile <= 0)
	{
		fd_fsel = 0;
		fd_ftop = 0;
	}
	else
	{
		if (fd_fsel < 0)
			fd_fsel = 0;
		if (fd_fsel >= fd_nfile)
			fd_fsel = fd_nfile - 1;
		if (fd_fsel < fd_ftop)
			fd_ftop = fd_fsel;
		if (fd_fsel >= fd_ftop + lh)
			fd_ftop = fd_fsel - lh + 1;
		if (fd_ftop < 0)
			fd_ftop = 0;
	}
	if (fd_ndir <= 0)
	{
		fd_dsel = 0;
		fd_dtop = 0;
	}
	else
	{
		if (fd_dsel < 0)
			fd_dsel = 0;
		if (fd_dsel >= fd_ndir)
			fd_dsel = fd_ndir - 1;
		if (fd_dsel < fd_dtop)
			fd_dtop = fd_dsel;
		if (fd_dsel >= fd_dtop + lh)
			fd_dtop = fd_dsel - lh + 1;
		if (fd_dtop < 0)
			fd_dtop = 0;
	}
}

static void fd_copy_file_to_name(void)
{
	if (fd_fsel < 0 || fd_fsel >= fd_nfile)
		return;
	fd_copy(G.ed.dlg, sizeof(G.ed.dlg), fd_files[fd_fsel]);
	G.ed.dlglen = (int)strlen(G.ed.dlg);
}

static void fd_clear_name(void)
{
	G.ed.dlg[0] = 0;
	G.ed.dlglen = 0;
}

static void fd_set_dir(const char *path)
{
	char resolved[128];
	if (mmb_vfs_resolve(path, resolved, sizeof(resolved)) == 0)
		fd_copy(fd_dir, sizeof(fd_dir), resolved);
	else
		fd_copy(fd_dir, sizeof(fd_dir), path);
	fd_scan();
	fd_clamp();
}

static void fd_after_dir_nav(int from_lists)
{
	if (from_lists && fd_nfile > 0)
	{
		fd_focus = FD_FOCUS_FILE;
		fd_copy_file_to_name();
	}
	else
	{
		if (from_lists)
			fd_focus = FD_FOCUS_DIR;
		fd_clear_name();
	}
}

static void fd_enter_listed(const char *name, int from_lists)
{
	char full[128], tmp[FD_NAME];
	int n;
	if (!name || !name[0])
		return;
	if (name[0] == '[' && name[1] == '-' && name[3] == '-' && name[4] == ']')
	{
		tmp[0] = name[2];
		tmp[1] = ':';
		tmp[2] = '/';
		tmp[3] = 0;
		fd_set_dir(tmp);
		fd_after_dir_nav(from_lists);
		return;
	}
	if (name[0] == '.' && name[1] == '.' && name[2] == 0)
	{
		fd_copy(full, sizeof(full), fd_dir);
		fd_parent(full);
		fd_set_dir(full);
		fd_after_dir_nav(from_lists);
		return;
	}
	fd_copy(tmp, sizeof(tmp), name);
	n = (int)strlen(tmp);
	if (n > 0 && tmp[n - 1] == '/')
		tmp[n - 1] = 0;
	fd_join(full, sizeof(full), fd_dir, tmp);
	fd_set_dir(full);
	fd_after_dir_nav(from_lists);
}

static void fd_submit_path(const char *path)
{
	char full[128];
	fd_copy(full, sizeof(full), path);
	if (G.ed.dialog == DLG_OPEN)
	{
		if (full[0] && add_or_switch(full) < 0)
			set_status("Open failed");
		close_ui();
		tui_invalidate();
		return;
	}
	if (G.ed.dialog == DLG_SAVEAS)
	{
		mmb_ed_tab *t = cur_tab();
		if (t && full[0])
		{
			strncpy(t->path, full, sizeof(t->path) - 1);
			ensure_bas(t->path, sizeof(t->path));
			save_tab();
		}
		close_ui();
	}
}

static void fd_apply_glob(const char *spec)
{
	const char *slash = fd_rchr(spec, '/');
	char dirpart[128];
	if (slash)
	{
		int n = (int)(slash - spec);
		if (n >= (int)sizeof(dirpart))
			n = (int)sizeof(dirpart) - 1;
		memcpy(dirpart, spec, (unsigned)n);
		dirpart[n] = 0;
		if (dirpart[0] && dirpart[1] == ':' && dirpart[2] == 0)
		{
			dirpart[2] = '/';
			dirpart[3] = 0;
		}
		if (dirpart[0])
			fd_set_dir(dirpart);
		else
			fd_scan();
		fd_copy(fd_mask, sizeof(fd_mask), slash + 1);
		fd_scan();
		fd_clamp();
		return;
	}
	if (spec[0] && spec[1] == ':')
	{
		dirpart[0] = spec[0];
		dirpart[1] = ':';
		dirpart[2] = '/';
		dirpart[3] = 0;
		fd_set_dir(dirpart);
		fd_copy(fd_mask, sizeof(fd_mask), spec + 2);
		fd_scan();
		fd_clamp();
		return;
	}
	fd_copy(fd_mask, sizeof(fd_mask), spec);
	fd_scan();
	fd_clamp();
}

static void fd_enter_key(void)
{
	char full[128];
	if (fd_focus == FD_FOCUS_DIR)
	{
		if (fd_dsel >= 0 && fd_dsel < fd_ndir)
			fd_enter_listed(fd_dirs[fd_dsel], 1);
		return;
	}
	if (fd_focus == FD_FOCUS_FILE)
	{
		if (fd_fsel >= 0 && fd_fsel < fd_nfile)
		{
			fd_make_full(full, sizeof(full), fd_files[fd_fsel]);
			fd_submit_path(full);
		}
		return;
	}
	if (!G.ed.dlg[0])
	{
		if (fd_nfile > 0 && fd_fsel >= 0 && fd_fsel < fd_nfile)
		{
			fd_make_full(full, sizeof(full), fd_files[fd_fsel]);
			fd_submit_path(full);
		}
		return;
	}
	if (fd_has_glob(G.ed.dlg))
	{
		fd_apply_glob(G.ed.dlg);
		fd_clear_name();
		return;
	}
	fd_make_full(full, sizeof(full), G.ed.dlg);
	if (fd_is_dir(full))
	{
		fd_set_dir(full);
		fd_clear_name();
		fd_focus = FD_FOCUS_NAME;
		return;
	}
	fd_submit_path(full);
}

static void fd_tab(void)
{
	if (fd_focus == FD_FOCUS_NAME)
		fd_focus = fd_nfile ? FD_FOCUS_FILE : FD_FOCUS_DIR;
	else if (fd_focus == FD_FOCUS_FILE)
		fd_focus = fd_ndir ? FD_FOCUS_DIR : FD_FOCUS_NAME;
	else
		fd_focus = FD_FOCUS_NAME;
	if (fd_focus == FD_FOCUS_FILE)
		fd_copy_file_to_name();
}

static void fd_move_sel(int *sel, int n, int delta)
{
	if (n <= 0)
		return;
	*sel += delta;
	if (*sel < 0)
		*sel = 0;
	if (*sel >= n)
		*sel = n - 1;
}

static int fd_arrow(int kind)
{
	int lh = fd_list_h();
	if (kind == 0)
		return 0;
	if (fd_focus == FD_FOCUS_NAME)
	{
		if (kind == 2)
			fd_tab();
		return 1;
	}
	if (kind == 3 && fd_focus == FD_FOCUS_FILE)
	{
		fd_focus = FD_FOCUS_DIR;
		return 1;
	}
	if (kind == 4 && fd_focus == FD_FOCUS_DIR)
	{
		fd_focus = fd_nfile ? FD_FOCUS_FILE : FD_FOCUS_NAME;
		if (fd_focus == FD_FOCUS_FILE)
			fd_copy_file_to_name();
		return 1;
	}
	if (fd_focus == FD_FOCUS_FILE)
	{
		if (kind == 1)
			fd_move_sel(&fd_fsel, fd_nfile, -1);
		else if (kind == 2)
			fd_move_sel(&fd_fsel, fd_nfile, 1);
		else if (kind == 5)
			fd_fsel = 0;
		else if (kind == 6)
			fd_fsel = fd_nfile ? fd_nfile - 1 : 0;
		else if (kind == 9)
			fd_move_sel(&fd_fsel, fd_nfile, -lh);
		else if (kind == 10)
			fd_move_sel(&fd_fsel, fd_nfile, lh);
		fd_clamp();
		fd_copy_file_to_name();
		return 1;
	}
	if (kind == 1)
		fd_move_sel(&fd_dsel, fd_ndir, -1);
	else if (kind == 2)
		fd_move_sel(&fd_dsel, fd_ndir, 1);
	else if (kind == 5)
		fd_dsel = 0;
	else if (kind == 6)
		fd_dsel = fd_ndir ? fd_ndir - 1 : 0;
	else if (kind == 9)
		fd_move_sel(&fd_dsel, fd_ndir, -lh);
	else if (kind == 10)
		fd_move_sel(&fd_dsel, fd_ndir, lh);
	fd_clamp();
	return 1;
}

static void fd_status_line(char *out, int n)
{
	int last;
	fd_copy(out, n, fd_dir);
	last = out[0] ? (int)(unsigned char)out[strlen(out) - 1] : 0;
	if (last && last != '/')
		strncat(out, "/", (unsigned)n - strlen(out) - 1);
	strncat(out, fd_mask, (unsigned)n - strlen(out) - 1);
}

static void draw_dialog(void)
{
	int w = 50, h = 8, r0, c0, i;
	const char *title;
	if (G.ed.dialog == DLG_NONE)
		return;
	if (G.ed.dialog == DLG_PICK)
	{
		draw_picker();
		return;
	}
	if (G.ed.dialog == DLG_HELP)
	{
		w = 48;
		h = 20;
		title = " Help ";
	}
	else if (G.ed.dialog == DLG_OPEN)
	{
		fd_geom(&c0, &r0, &w, &h);
		title = " Open ";
	}
	else
	{
		fd_geom(&c0, &r0, &w, &h);
		title = " Save As ";
	}
	if (G.ed.dialog == DLG_HELP)
	{
		if (w > COLS - 2)
			w = COLS - 2;
		if (h > ROWS - 2)
			h = ROWS - 2;
		r0 = (ROWS - h) / 2;
		c0 = (COLS - w) / 2;
		if (r0 < 2)
			r0 = 2;
		if (c0 < 0)
			c0 = 0;
	}
	tui_frame(c0, r0, w, h, C_DLG_FG, C_DLG_BG);
	{
		int left = (w - 2 - (int)strlen(title)) / 2;
		if (left < 1)
			left = 1;
		tui_puts(c0 + 1 + left, r0, title, C_DLG_FG, C_DLG_BG);
	}
	tui_pad(c0 + w, r0, "", 2, C_SH_FG, C_SH_BG);
	for (i = 1; i < h - 1; i++)
	{
		tui_pad(c0 + 1, r0 + i, "", w - 2, C_DLG_FG, C_DLG_BG);
		tui_pad(c0 + w, r0 + i, "", 2, C_SH_FG, C_SH_BG);
	}
	tui_pad(c0 + w, r0 + h - 1, "", 2, C_SH_FG, C_SH_BG);
	tui_pad(c0 + 2, r0 + h, "", w, C_SH_FG, C_SH_BG);

	if (G.ed.dialog == DLG_HELP)
	{
		static const char *lines[] = {
			"Alt+F  File menu     Alt+E  Edit",
			"Alt+R  Run menu      Alt+T  Theme",
			"Alt+H  Help          Esc    close",
			"F1     This help     F2     Save",
			"F3     Open          F4     #include",
			"F9     Run           Alt+X  Quit",
			"^C/^X/^V copy/cut/paste  ^W close tab",
			"Alt+Left/Right tabs (no wrap)",
			"^O     Save            ^K/^U  Cut line/Paste",
			"^R/F9  Run; press a key to return",
			"Shift+Arrows select  Del    erase sel",
			"^Ins copy  Shift+Del cut  Shift+Ins paste",
			"Tab    4 spaces      Alt+1..9 file tab",
			"Arrows move          Enter  activate",
			"Open/Save: Name, Files, Directories",
			"        Tab cycles  Enter file/folder",
			"",
			"     Enter or Esc closes this box",
		};
		int L = (int)(sizeof(lines) / sizeof(lines[0]));
		for (i = 0; i < L && r0 + 2 + i < r0 + h - 1; i++)
			tui_pad(c0 + 2, r0 + 2 + i, lines[i], w - 4, C_DLG_FG, C_DLG_BG);
	}
	else
	{
		int inner = w - 4;
		int fw = inner * 3 / 5;
		int dw;
		int fx = c0 + 2;
		int dx, ly, lh, nfg, nbg, ffg, fbg, dfg, dbg;
		char st[160];
		if (fw < 18)
			fw = 18;
		dw = inner - fw - 1;
		if (dw < 12)
		{
			dw = 12;
			fw = inner - dw - 1;
		}
		dx = fx + fw + 1;
		ly = r0 + 4;
		lh = fd_list_h();
		nfg = (fd_focus == FD_FOCUS_NAME) ? C_SEL_FG : C_DLG_FG;
		nbg = (fd_focus == FD_FOCUS_NAME) ? C_SEL_BG : C_LIST_BG;
		ffg = (fd_focus == FD_FOCUS_FILE) ? C_SEL_FG : C_DLG_FG;
		fbg = (fd_focus == FD_FOCUS_FILE) ? C_SEL_BG : C_DLG_BG;
		dfg = (fd_focus == FD_FOCUS_DIR) ? C_SEL_FG : C_DLG_FG;
		dbg = (fd_focus == FD_FOCUS_DIR) ? C_SEL_BG : C_DLG_BG;
		tui_pad(c0 + 2, r0 + 1, "Name", w - 4, C_DLG_FG, C_DLG_BG);
		tui_pad(c0 + 2, r0 + 2, G.ed.dlg, w - 4, nfg, nbg);
		tui_pad(fx, r0 + 3, "Files", fw, ffg, fbg);
		tui_pad(dx, r0 + 3, "Directories", dw, dfg, dbg);
		for (i = 0; i < lh && ly + i < r0 + h - 3; i++)
		{
			int fi = fd_ftop + i;
			int di = fd_dtop + i;
			int sfg, sbg;
			const char *fn = (fi >= 0 && fi < fd_nfile) ? fd_files[fi] : "";
			sfg = C_DLG_FG;
			sbg = C_DLG_BG;
			if (fi == fd_fsel && fd_nfile > 0)
			{
				sfg = (fd_focus == FD_FOCUS_FILE) ? C_SEL_FG : C_DLG_FG;
				sbg = (fd_focus == FD_FOCUS_FILE) ? C_SEL_BG : C_LIST_BG;
			}
			tui_pad(fx, ly + i, fn, fw, sfg, sbg);
			fn = (di >= 0 && di < fd_ndir) ? fd_dirs[di] : "";
			sfg = C_DLG_FG;
			sbg = C_DLG_BG;
			if (di == fd_dsel && fd_ndir > 0)
			{
				sfg = (fd_focus == FD_FOCUS_DIR) ? C_SEL_FG : C_DLG_FG;
				sbg = (fd_focus == FD_FOCUS_DIR) ? C_SEL_BG : C_LIST_BG;
			}
			tui_pad(dx, ly + i, fn, dw, sfg, sbg);
		}
		fd_status_line(st, sizeof(st));
		tui_pad(c0 + 2, r0 + h - 3, st, w - 4, C_DLG_FG, C_DLG_BG);
		tui_pad(c0 + 2, r0 + h - 2, "Tab  Enter=OK  Esc=Cancel", w - 4, C_DLG_FG, C_DLG_BG);
	}
}

static void draw_fkey(int *x, int y, const char *key, const char *lab)
{
	tui_puts(*x, y, key, C_HOT, C_MENU_BG);
	*x += (int)strlen(key);
	tui_puts(*x, y, lab, C_MENU_FG, C_MENU_BG);
	*x += (int)strlen(lab);
}

static void draw_status(void)
{
	int row, col, x = 0;
	char right[48];
	char *p;
	int rightn;
	mmb_ed_tab *t = cur_tab();
	draw_fkey(&x, ROW_STAT, "F1", " Help ");
	draw_fkey(&x, ROW_STAT, "F2", " Save ");
	draw_fkey(&x, ROW_STAT, "F3", " Open ");
	draw_fkey(&x, ROW_STAT, "F9", " Run ");
	draw_fkey(&x, ROW_STAT, "Alt+X", " Quit");
	if (G.ed.status[0])
	{
		tui_puts(x, ROW_STAT, " + ", C_MENU_FG, C_MENU_BG);
		x += 3;
		tui_pad(x, ROW_STAT, G.ed.status, 18, C_MENU_FG, C_MENU_BG);
		x += 18;
	}
	row = 1;
	col = 1;
	if (t)
		pos_to_rowcol(t->cx, &row, &col);
	p = right;
	*p++ = (t && t->dirty) ? '*' : ' ';
	*p++ = ' ';
	{
		const char *name = t ? tab_label(G.ed.cur) : "";
		int k, n = (int)strlen(name);
		if (n > 12)
			n = 12;
		for (k = 0; k < n; k++)
			*p++ = name[k];
	}
	*p++ = ' ';
	p = put_uint(p, row + 1);
	*p++ = ':';
	p = put_uint(p, col + 1);
	*p = 0;
	rightn = (int)strlen(right);
	if (x + rightn < COLS)
	{
		tui_pad(x, ROW_STAT, "", COLS - x - rightn, C_MENU_FG, C_MENU_BG);
		x = COLS - rightn;
	}
	tui_puts(x, ROW_STAT, right, C_MENU_FG, C_MENU_BG);
}

static void place_cursor(void)
{
	if (G.ed.dialog == DLG_PICK)
	{
		int w, h, r0, c0;
		int col = G.ed.dlglen;
		pick_geom(&w, &h, &r0, &c0);
		if (col > w - 4)
			col = w - 4;
		tui_cursor(c0 + 2 + col, r0 + 2, 1);
		return;
	}
	if (fd_on())
	{
		int w, h, r0, c0, col;
		if (fd_focus != FD_FOCUS_NAME)
		{
			tui_cursor(-1, -1, 0);
			return;
		}
		fd_geom(&c0, &r0, &w, &h);
		col = G.ed.dlglen;
		if (col > w - 4)
			col = w - 4;
		tui_cursor(c0 + 2 + col, r0 + 2, 1);
		return;
	}
	if (G.ed.dialog || G.ed.menu_open)
	{
		tui_cursor(-1, -1, 0);
		return;
	}
	{
		mmb_ed_tab *t = cur_tab();
		int row = 0, col = 0, sr, sc;
		if (t)
			pos_to_rowcol(t->cx, &row, &col);
		sr = ROW_TEXT + (t ? row - t->row0 : 0);
		sc = 1 + (t ? col - t->col0 : 0);
		if (sr < ROW_TEXT)
			sr = ROW_TEXT;
		if (sr > ROW_BBOT - 1)
			sr = ROW_BBOT - 1;
		if (sc < 1)
			sc = 1;
		if (sc > COLS - 2)
			sc = COLS - 2;
		tui_cursor(sc, sr, 1);
	}
}

static void redraw(void)
{
	G.outn = 0;
	G.out[0] = 0;
	ensure_visible();
	tui_begin();
	tui_set_palette(th()->pal);
	tui_clear(C_EDIT_FG, C_EDIT_BG);
	draw_menu_bar();
	draw_tabs();
	draw_border_row(ROW_BTOP, 1);
	draw_editor_body();
	draw_border_row(ROW_BBOT, 0);
	draw_status();
	if (G.ed.menu_open)
		draw_dropdown();
	if (G.ed.dialog)
		draw_dialog();
	place_cursor();
	tui_flush();
}


static void save_tab(void)
{
	mmb_ed_tab *t = cur_tab();
	if (!t)
		return;
	if (!t->path[0])
		strcpy(t->path, "UNTITLED.BAS");
	mmb_vfs_write(t->path, t->buf, (unsigned)t->len, 0);
	t->dirty = 0;
	strncpy(G.current_prog, t->path, sizeof(G.current_prog) - 1);
	set_status("Saved");
}

static void editor_leave(void)
{
	mmb_ed_tab *t = cur_tab();
	if (t && t->dirty)
		save_tab();
	tui_end();
	G.ed.active = 0;
	G.ed.wait_continue = 0;
	G.ed.menu_open = 0;
	G.ed.dialog = 0;
}

static void editor_restore_gfx(void)
{
	if (G.ed.saved_mode != G.gfx.mode || G.ed.saved_bits != G.gfx.bits)
		mmb_gfx_set_mode(G.ed.saved_mode, G.ed.saved_bits);
	G.gfx.write_fb = 0;
	if (G.ed.saved_write_fb && G.gfx.fb)
		G.gfx.write_fb = 1;
	else
	{
		int pg = G.ed.saved_write_page;
		if (pg < 0 || pg >= G.gfx.pages)
			pg = 0;
		G.gfx.write_page = pg;
	}
	if (G.ed.saved_display_page >= 0 && G.ed.saved_display_page < G.gfx.pages)
		G.gfx.display_page = G.ed.saved_display_page;
	else
		G.gfx.display_page = 0;
	mmb_gfx_present();
}

static void editor_run(void)
{
	mmb_ed_tab *t = cur_tab();
	char cmd[160];

	if (t)
		save_tab();
	G.ed.saved_mode = G.gfx.mode;
	G.ed.saved_bits = G.gfx.bits;
	G.ed.saved_write_page = G.gfx.write_page;
	G.ed.saved_display_page = G.gfx.display_page;
	G.ed.saved_write_fb = G.gfx.write_fb;
	G.ed.menu_open = 0;
	G.ed.dialog = 0;
	tui_end();
	G.ed.active = 0;
	if (!t)
		return;
	strcpy(cmd, "RUN \"");
	strncat(cmd, t->path, sizeof(cmd) - 8);
	strcat(cmd, "\"");
	mmb_exec_line(cmd);
	if (G.err[0])
	{
		set_status(G.err);
		editor_resume();
		return;
	}
	if (G.outn && G.out[G.outn - 1] != '\n' && G.out[G.outn - 1] != '\r')
		mmb_out("\n");
	mmb_out("Press any key to continue");
	G.ed.wait_continue = 1;
}

static void editor_resume(void)
{
	G.ed.wait_continue = 0;
	G.ed.active = 1;
	esc_state = 0;
	alt_pend = 0;
	editor_restore_gfx();
	tui_begin();
	tui_invalidate();
	redraw();
}

static void open_menu(int which)
{
	int n;
	G.ed.dialog = DLG_NONE;
	G.ed.menu_open = 1;
	G.ed.menu = which;
	G.ed.menu_item = 0;
	if (which == MENU_THEME)
	{
		int cur = G.opt.edit_theme;
		if (cur >= 0 && cur < ED_THEME_N)
			G.ed.menu_item = cur;
	}
	menu_items(which, &n);
	(void)n;
}

static void close_ui(void)
{
	G.ed.menu_open = 0;
	G.ed.dialog = 0;
	G.ed.dlglen = 0;
	G.ed.dlg[0] = 0;
}

static void open_dialog(int which)
{
	G.ed.menu_open = 0;
	G.ed.dialog = which;
	G.ed.dlg[0] = 0;
	G.ed.dlglen = 0;
	if (which == DLG_OPEN || which == DLG_SAVEAS)
	{
		fd_focus = FD_FOCUS_NAME;
		fd_copy(fd_mask, sizeof(fd_mask), "*.BAS");
		fd_copy(fd_dir, sizeof(fd_dir), mmb_vfs_cwd());
		if (which == DLG_SAVEAS && cur_tab() && cur_tab()->path[0])
		{
			char base[128], dir[128];
			fd_dirname(dir, sizeof(dir), cur_tab()->path);
			fd_basename(base, sizeof(base), cur_tab()->path);
			if (dir[0])
				fd_copy(fd_dir, sizeof(fd_dir), dir);
			fd_copy(G.ed.dlg, sizeof(G.ed.dlg), base);
			G.ed.dlglen = (int)strlen(G.ed.dlg);
		}
		fd_scan();
		fd_clamp();
	}
}

static void next_tab(void)
{
	if (G.ed.ntabs <= 1)
		return;
	G.ed.cur = (G.ed.cur + 1) % G.ed.ntabs;
	set_status(0);
}

static void tab_right(void)
{
	if (G.ed.cur + 1 < G.ed.ntabs)
	{
		G.ed.cur++;
		set_status(0);
	}
}

static void tab_left(void)
{
	if (G.ed.cur > 0)
	{
		G.ed.cur--;
		set_status(0);
	}
}

static void close_tab(void)
{
	int i;
	mmb_ed_tab *t = cur_tab();
	if (t && t->dirty)
		save_tab();
	if (G.ed.ntabs <= 1)
	{
		editor_leave();
		return;
	}
	for (i = G.ed.cur; i < G.ed.ntabs - 1; i++)
		G.ed.tab[i] = G.ed.tab[i + 1];
	G.ed.ntabs--;
	memset(&G.ed.tab[G.ed.ntabs], 0, sizeof(G.ed.tab[0]));
	if (G.ed.cur >= G.ed.ntabs)
		G.ed.cur = G.ed.ntabs - 1;
}

static void submit_dialog(void)
{
	if (G.ed.dialog == DLG_HELP)
	{
		G.ed.dialog = 0;
		return;
	}
	if (G.ed.dialog == DLG_PICK)
	{
		if (pick_vn > 0 && pick_sel >= 0 && pick_sel < pick_vn)
		{
			int idx = pick_view[pick_sel];
			if (idx >= 0 && idx < pick_n)
			{
				if (add_or_switch(pick_path[idx]) < 0)
					set_status("Open failed");
			}
		}
		close_ui();
		tui_invalidate();
		return;
	}
	if (G.ed.dialog == DLG_OPEN || G.ed.dialog == DLG_SAVEAS)
	{
		fd_enter_key();
		return;
	}
}

static void activate_menu(void)
{
	int menu = G.ed.menu;
	int item = G.ed.menu_item;
	G.ed.menu_open = 0;
	if (menu == MENU_FILE)
	{
		if (item == 0)
			open_dialog(DLG_OPEN);
		else if (item == 1)
			open_picker();
		else if (item == 2)
			save_tab();
		else if (item == 3)
			open_dialog(DLG_SAVEAS);
		else if (item == 4)
			close_tab();
		else if (item == 5)
			next_tab();
		else if (item == 6)
			editor_leave();
	}
	else if (menu == MENU_EDIT)
	{
		if (item == 0)
			copy_selection();
		else if (item == 1)
			cut_selection();
		else if (item == 2)
			cut_line();
		else
			paste_kill();
	}
	else if (menu == MENU_RUN)
		editor_run();
	else if (menu == MENU_THEME)
	{
		if (item >= 0 && item < ED_THEME_N)
		{
			G.opt.edit_theme = item;
			mmb_settings_save();
			set_status(k_themes[item].name);
		}
	}
	else if (menu == MENU_HELP)
	{
		if (item == 1)
			mmb_ihelp_open("");
		else
			open_dialog(DLG_HELP);
	}
}

static int handle_alt(char c)
{
	if (c >= 'A' && c <= 'Z')
		c = (char)(c - 'A' + 'a');
	if (c == 'f')
	{
		open_menu(MENU_FILE);
		return 1;
	}
	if (c == 'e')
	{
		open_menu(MENU_EDIT);
		return 1;
	}
	if (c == 'r')
	{
		open_menu(MENU_RUN);
		return 1;
	}
	if (c == 't')
	{
		open_menu(MENU_THEME);
		return 1;
	}
	if (c == 'h')
	{
		open_menu(MENU_HELP);
		return 1;
	}
	if (c == 'x')
	{
		editor_leave();
		return 1;
	}
	if (c >= '1' && c <= '9')
	{
		int i = c - '1';
		if (i < G.ed.ntabs)
			G.ed.cur = i;
		return 1;
	}
	return 0;
}

static int parse_include_line(const char *line, char *inc, int incsz)
{
	const char *p = line;
	int n = 0;
	while (*p == ' ' || *p == '\t')
		p++;
	if (*p != '#')
		return 0;
	p++;
	while (*p == ' ' || *p == '\t')
		p++;
	if (!((p[0] == 'I' || p[0] == 'i') && (p[1] == 'N' || p[1] == 'n') &&
	      (p[2] == 'C' || p[2] == 'c') && (p[3] == 'L' || p[3] == 'l') &&
	      (p[4] == 'U' || p[4] == 'u') && (p[5] == 'D' || p[5] == 'd') &&
	      (p[6] == 'E' || p[6] == 'e')))
		return 0;
	p += 7;
	while (*p == ' ' || *p == '\t')
		p++;
	if (*p == '"')
	{
		p++;
		while (*p && *p != '"' && n < incsz - 1)
			inc[n++] = *p++;
	}
	else
	{
		while (*p && *p != ' ' && *p != '\t' && *p != '\'' && n < incsz - 1)
			inc[n++] = *p++;
	}
	inc[n] = 0;
	return n ? 1 : 0;
}

static int copy_line_at(mmb_ed_tab *t, int pos, char *line, int linesz)
{
	int s, e, n;
	s = line_start(pos);
	e = line_end(pos);
	n = e - s;
	if (n < 0)
		n = 0;
	if (n >= linesz)
		n = linesz - 1;
	memcpy(line, t->buf + s, (unsigned)n);
	line[n] = 0;
	if (n > 0 && line[n - 1] == '\r')
		line[--n] = 0;
	return n;
}

static void goto_include(void)
{
	mmb_ed_tab *t = cur_tab();
	int n;
	char line[256], inc[128], full[128], dir[128];
	if (!t || G.ed.dialog)
		return;
	n = copy_line_at(t, t->cx, line, (int)sizeof(line));
	if (!parse_include_line(line, inc, sizeof(inc)))
	{
		if (n == 0 && t->cx > 0)
			copy_line_at(t, t->cx - 1, line, (int)sizeof(line));
		if (!parse_include_line(line, inc, sizeof(inc)))
		{
			set_status("No #include on this line");
			return;
		}
	}
	if ((inc[0] && inc[1] == ':') || inc[0] == '/')
		ed_copy(full, sizeof(full), inc);
	else
	{
		fd_dirname(dir, sizeof(dir), t->path[0] ? t->path : mmb_vfs_cwd());
		if (!dir[0])
			ed_copy(dir, sizeof(dir), mmb_vfs_cwd());
		ed_join(full, sizeof(full), dir, inc);
	}
	if (!strchr(inc, '.'))
	{
		char with_inc[128], with_bas[128];
		ed_copy(with_inc, sizeof(with_inc), full);
		strncat(with_inc, ".INC", sizeof(with_inc) - strlen(with_inc) - 1);
		ed_copy(with_bas, sizeof(with_bas), full);
		strncat(with_bas, ".BAS", sizeof(with_bas) - strlen(with_bas) - 1);
		if (mmb_vfs_exists(with_inc))
			ed_copy(full, sizeof(full), with_inc);
		else if (mmb_vfs_exists(with_bas))
			ed_copy(full, sizeof(full), with_bas);
		else
			ed_copy(full, sizeof(full), with_inc);
	}
	if (add_or_switch(full) < 0)
		set_status("Open failed");
	else
	{
		set_status(0);
		tui_invalidate();
	}
}

static void do_fkey(int n)
{
	if (n == 1)
		open_dialog(DLG_HELP);
	else if (n == 2)
		save_tab();
	else if (n == 3)
		open_dialog(DLG_OPEN);
	else if (n == 4)
		goto_include();
	else if (n == 9)
		editor_run();
	else if (n == 10)
		open_menu(MENU_FILE);
}

static int handle_arrow_or_special(int kind, int mod)
{
	/* kind: 1 up 2 down 3 right 4 left 5 home 6 end 7 del 8 ins 9 pgup 10 pgdn */
	int shift = 0, ctrl = 0, alt = 0;
	if (mod > 1)
	{
		shift = ((mod - 1) & 1) != 0;
		alt = ((mod - 1) & 2) != 0;
		ctrl = ((mod - 1) & 4) != 0;
	}
	if (G.ed.dialog == DLG_HELP)
	{
		if (kind == 0)
			return 0;
		return 1;
	}
	if (G.ed.dialog == DLG_PICK)
	{
		int vis, h;
		pick_geom(0, &h, 0, 0);
		vis = pick_list_h(h);
		if (kind == 1)
			pick_move(-1);
		else if (kind == 2)
			pick_move(1);
		else if (kind == 5)
			pick_move(-pick_vn);
		else if (kind == 6)
			pick_move(pick_vn);
		else if (kind == 9)
			pick_move(-vis);
		else if (kind == 10)
			pick_move(vis);
		return 1;
	}
	if (fd_on())
	{
		fd_arrow(kind);
		return 1;
	}
	if (G.ed.dialog)
		return 1;
	if (G.ed.menu_open)
	{
		int n;
		menu_items(G.ed.menu, &n);
		if (kind == 1)
		{
			if (G.ed.menu_item > 0)
				G.ed.menu_item--;
			else
				G.ed.menu_item = n - 1;
		}
		else if (kind == 2)
			G.ed.menu_item = (G.ed.menu_item + 1) % n;
		else if (kind == 4)
		{
			G.ed.menu = (G.ed.menu + MENU_COUNT - 1) % MENU_COUNT;
			G.ed.menu_item = 0;
		}
		else if (kind == 3)
		{
			G.ed.menu = (G.ed.menu + 1) % MENU_COUNT;
			G.ed.menu_item = 0;
		}
		return 1;
	}
	if (alt && !ctrl && !shift)
	{
		if (kind == 3)
			tab_right();
		else if (kind == 4)
			tab_left();
		return 1;
	}
	if (kind == 7)
	{
		if (shift)
			cut_selection();
		else
			delete_char();
		return 1;
	}
	if (kind == 8)
	{
		if (ctrl)
			copy_selection();
		else if (shift)
			paste_kill();
		return 1;
	}
	if (shift && !ctrl && (kind == 1 || kind == 2))
	{
		mmb_ed_tab *t = cur_tab();
		if (t && !t->sel)
		{
			t->sel_anchor = line_start(t->cx);
			t->sel = 1;
		}
		else
			sel_prepare(shift);
		if (t && kind == 2)
		{
			int e = line_end(t->cx);
			if (e < t->len && t->buf[e] == '\n')
				t->cx = e + 1;
			else
				t->cx = e;
		}
		else if (t)
		{
			int s = line_start(t->cx);
			if (s > 0)
				t->cx = line_start(s - 1);
			else
				t->cx = 0;
		}
		return 1;
	}
	sel_prepare(shift);
	if (ctrl && kind == 4)
		move_word_left();
	else if (ctrl && kind == 3)
		move_word_right();
	else if (ctrl && kind == 5)
		move_file_home();
	else if (ctrl && kind == 6)
		move_file_end();
	else if (kind == 1)
		move_up();
	else if (kind == 2)
		move_down();
	else if (kind == 3)
		move_right();
	else if (kind == 4)
		move_left();
	else if (kind == 5)
		move_home();
	else if (kind == 6)
		move_end();
	else if (kind == 9)
		page_up();
	else if (kind == 10)
		page_down();
	return 1;
}

static int handle_escape(char c)
{
	if (esc_state == ESC_GOT)
	{
		if (c == '[')
		{
			esc_state = ESC_CSI;
			csi_n = 0;
			csi_arg = 0;
			csi_semi = 0;
			return 1;
		}
		if (c == 'O')
		{
			esc_state = ESC_SS3;
			return 1;
		}
		esc_state = ESC_NONE;
		if (G.ed.menu_open || G.ed.dialog)
		{
			close_ui();
			if (G.ed.active)
				redraw();
		}
		return 1;
	}
	if (esc_state == ESC_SS3)
	{
		esc_state = ESC_NONE;
		if (c >= 'A' && c <= 'E')
			do_fkey(c - 'A' + 1);
		if (G.ed.active)
			redraw();
		return 1;
	}
	if (esc_state == ESC_CSI)
	{
		if (c >= '0' && c <= '9')
		{
			csi_arg = csi_arg * 10 + (c - '0');
			return 1;
		}
		if (c == ';')
		{
			if (!csi_n)
				csi_n = csi_arg;
			csi_arg = 0;
			csi_semi = 1;
			return 1;
		}
		if (c == '[')
		{
			esc_state = ESC_SS3;
			return 1;
		}
		esc_state = ESC_NONE;
		{
			int mod = csi_semi ? csi_arg : 0;
			if (c == 'A')
				handle_arrow_or_special(1, mod);
			else if (c == 'B')
				handle_arrow_or_special(2, mod);
			else if (c == 'C')
				handle_arrow_or_special(3, mod);
			else if (c == 'D')
				handle_arrow_or_special(4, mod);
			else if (c == 'H')
				handle_arrow_or_special(5, mod);
			else if (c == 'F')
				handle_arrow_or_special(6, mod);
			else if (c == '~')
			{
				int n = csi_semi ? csi_n : csi_arg;
				if (n == 1 || n == 7)
					handle_arrow_or_special(5, mod);
				else if (n == 4 || n == 8)
					handle_arrow_or_special(6, mod);
				else if (n == 3)
					handle_arrow_or_special(7, mod);
				else if (n == 2)
					handle_arrow_or_special(8, mod);
				else if (n == 5)
					handle_arrow_or_special(9, mod);
				else if (n == 6)
					handle_arrow_or_special(10, mod);
				else if (n >= 11 && n <= 15)
					do_fkey(n - 10);
				else if (n >= 17 && n <= 21)
					do_fkey(n - 11);
			}
			else if (c == 'Z')
				indent_lines(1);
		}
		if (G.ed.active)
			redraw();
		return 1;
	}
	return 0;
}

static int dialog_key(char c)
{
	if (G.ed.dialog == DLG_NONE)
		return 0;
	if (c == 27)
	{
		esc_state = ESC_GOT;
		return 1;
	}
	if (G.ed.dialog == DLG_HELP)
	{
		if (c == '\r' || c == '\n' || c == ' ')
		{
			G.ed.dialog = 0;
			redraw();
		}
		return 1;
	}
	if (G.ed.dialog == DLG_PICK)
	{
		if (c == '\r' || c == '\n')
		{
			submit_dialog();
			if (G.ed.active)
				redraw();
			return 1;
		}
		if (c == 8 || c == 127)
		{
			if (G.ed.dlglen > 0)
			{
				G.ed.dlg[--G.ed.dlglen] = 0;
				pick_rebuild_view();
				redraw();
			}
			return 1;
		}
		if (c >= 32 && c < 127 && G.ed.dlglen < (int)sizeof(G.ed.dlg) - 1)
		{
			G.ed.dlg[G.ed.dlglen++] = c;
			G.ed.dlg[G.ed.dlglen] = 0;
			pick_rebuild_view();
			redraw();
			return 1;
		}
		return 1;
	}
	if (c == '\r' || c == '\n')
	{
		submit_dialog();
		if (G.ed.active)
			redraw();
		return 1;
	}
	if (fd_on() && c == 9)
	{
		fd_tab();
		redraw();
		return 1;
	}
	if (c == 8 || c == 127)
	{
		if (fd_on() && fd_focus != FD_FOCUS_NAME)
			fd_focus = FD_FOCUS_NAME;
		if (G.ed.dlglen > 0)
		{
			G.ed.dlg[--G.ed.dlglen] = 0;
			redraw();
		}
		return 1;
	}
	if (c >= 32 && c < 127 && G.ed.dlglen < (int)sizeof(G.ed.dlg) - 1)
	{
		if (fd_on() && fd_focus != FD_FOCUS_NAME)
		{
			fd_focus = FD_FOCUS_NAME;
			fd_clear_name();
		}
		G.ed.dlg[G.ed.dlglen++] = c;
		G.ed.dlg[G.ed.dlglen] = 0;
		redraw();
		return 1;
	}
	return 1;
}

void mmb_editor_open(const char *path)
{
	memset(&G.ed, 0, sizeof(G.ed));
	esc_state = 0;
	G.ed.active = 1;
	set_pick_root(path && path[0] ? path : mmb_vfs_cwd());
	add_or_switch(path && path[0] ? path : "");
	if (G.ed.ntabs <= 0)
		add_or_switch("");
	tui_begin();
	tui_invalidate();
	redraw();
}

const char *mmb_editor_feed(char c)
{
	G.outn = 0;
	G.out[0] = 0;
	if (G.ed.wait_continue)
	{
		(void)c;
		editor_resume();
		return G.out;
	}
	if (alt_pend)
	{
		alt_pend = 0;
		if (handle_alt(c))
		{
			if (G.ed.active)
				redraw();
			return G.out;
		}
	}
	if ((unsigned char)c == 1)
	{
		alt_pend = 1;
		return G.out;
	}
	if (esc_state)
	{
		if (c == 27 && esc_state == ESC_GOT)
		{
			esc_state = ESC_NONE;
			close_ui();
			if (G.ed.active)
				redraw();
			return G.out;
		}
		if (handle_escape(c))
			return G.out;
	}
	if (c == 27)
	{
		esc_state = ESC_GOT;
		esc_at = mmb_now_ms();
		return G.out;
	}
	if (c == 16) /* Ctrl+P quick open */
	{
		open_picker();
		if (G.ed.active)
			redraw();
		return G.out;
	}
	if (G.ed.dialog)
	{
		dialog_key(c);
		return G.out;
	}
	if (G.ed.menu_open)
	{
		int n;
		const char **it = menu_items(G.ed.menu, &n);
		if (c == '\r' || c == '\n')
		{
			activate_menu();
			if (G.ed.active && !mmb_in_ihelp())
				redraw();
			return G.out;
		}
		if (c >= 32 && c < 127)
		{
			int i;
			char u = c;
			if (u >= 'A' && u <= 'Z')
				u = (char)(u - 'A' + 'a');
			const char *hots = menu_hots(G.ed.menu);
			(void)it;
			for (i = 0; i < n; i++)
			{
				char h = hots[i];
				if (h >= 'A' && h <= 'Z')
					h = (char)(h - 'A' + 'a');
				if (h == u)
				{
					G.ed.menu_item = i;
					activate_menu();
					if (G.ed.active && !mmb_in_ihelp())
						redraw();
					return G.out;
				}
			}
		}
		return G.out;
	}
	if (c == 15) /* Ctrl+O save */
	{
		save_tab();
		redraw();
		return G.out;
	}
	if (c == 23) /* Ctrl+W close tab */
	{
		close_tab();
		if (G.ed.active)
			redraw();
		return G.out;
	}
	if (c == 3) /* Ctrl+C copy */
	{
		copy_selection();
		if (G.ed.active)
			redraw();
		return G.out;
	}
	if (c == 24) /* Ctrl+X cut */
	{
		cut_selection();
		if (G.ed.active)
			redraw();
		return G.out;
	}
	if (c == 22) /* Ctrl+V paste */
	{
		paste_kill();
		if (G.ed.active)
			redraw();
		return G.out;
	}
	if (c == 18) /* Ctrl+R run */
	{
		editor_run();
		return G.out;
	}
	if (c == 11) /* Ctrl+K cut */
	{
		cut_line();
		redraw();
		return G.out;
	}
	if (c == 25) /* Ctrl+Y cut line (QBasic) */
	{
		cut_line();
		redraw();
		return G.out;
	}
	if (c == 21) /* Ctrl+U paste */
	{
		paste_kill();
		redraw();
		return G.out;
	}
	if (c == '\t')
	{
		if (cur_tab() && cur_tab()->sel)
			indent_lines(0);
		else
		{
			int i;
			for (i = 0; i < ED_TAB; i++)
				insert_char(' ');
		}
		redraw();
		return G.out;
	}
	if (c == 8 || c == 127)
	{
		backspace();
		redraw();
		return G.out;
	}
	if (c == '\r' || c == '\n')
	{
		insert_newline_indent();
		redraw();
		return G.out;
	}
	if (c >= 32 && c < 127)
	{
		insert_char(c);
		redraw();
		return G.out;
	}
	return G.out;
}

void mmb_cmd_edit(void)
{
	char path[128];
	path[0] = 0;
	mmb_skip_sp();
	if (*G.p && *G.p != ':' && *G.p != '\'')
	{
		mmb_val v = mmb_expr();
		if (v.type == T_STR)
			strncpy(path, v.s, sizeof(path) - 1);
	}
	else if (G.current_prog[0])
		strncpy(path, G.current_prog, sizeof(path) - 1);
	mmb_editor_open(path);
}

void mmb_editor_poll(void)
{
	if (!G.ed.active || esc_state != ESC_GOT)
		return;
	if (!G.ed.menu_open && !G.ed.dialog)
		return;
	if (mmb_now_ms() - esc_at < ESC_IDLE_MS)
		return;
	esc_state = ESC_NONE;
	close_ui();
	redraw();
}

void mmb_editor_on_ihelp_exit(void)
{
	if (!G.ed.active)
		return;
	tui_begin();
	tui_invalidate();
	redraw();
}
