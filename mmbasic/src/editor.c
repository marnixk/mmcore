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

#define C_MENU_FG   TUI_BLACK
#define C_MENU_BG   TUI_WHITE
#define C_HOT       TUI_BRRED
#define C_SEL_FG    TUI_BLACK
#define C_SEL_BG    TUI_GREEN
#define C_EDIT_FG   TUI_BRWHITE
#define C_EDIT_BG   TUI_BLUE
#define C_STR_FG    TUI_BRYELLOW
#define C_NUM_FG    TUI_BRBLUE
#define C_CMT_FG    TUI_WHITE
#define C_BRD_FG    TUI_WHITE
#define C_BRD_BG    TUI_BLUE
#define C_TAB_FG    TUI_WHITE
#define C_TAB_BG    TUI_BLUE
#define C_TABCUR_FG TUI_BLACK
#define C_TABCUR_BG TUI_WHITE
#define C_DLG_FG    TUI_BLACK
#define C_DLG_BG    TUI_WHITE
#define C_SH_FG     TUI_BLACK
#define C_SH_BG     TUI_BLACK

#define ESC_NONE    0
#define ESC_GOT     1
#define ESC_CSI     2
#define ESC_SS3     3

#define DLG_NONE    0
#define DLG_OPEN    1
#define DLG_SAVEAS  2
#define DLG_HELP    3

#define MENU_FILE   0
#define MENU_EDIT   1
#define MENU_RUN    2
#define MENU_HELP   3
#define MENU_COUNT  4

static char killbuf[512];
static int killlen;
static int alt_pend;
static int esc_state;
static int csi_n;
static int csi_arg;

static const char *menu_name[MENU_COUNT] = { "File", "Edit", "Run", "Help" };
static const char menu_hot[MENU_COUNT] = { 'F', 'E', 'R', 'H' };
static int menu_x[MENU_COUNT];

static const char *file_items[] = {
	"Open...", "Save", "Save As...", "Close tab", "Next tab", "Quit"
};
static const char file_hots[] = { 'o', 's', 'a', 'c', 'n', 'q' };
static const char *edit_items[] = { "Cut line", "Paste" };
static const char edit_hots[] = { 'c', 'p' };
static const char *run_items[] = { "Run" };
static const char run_hots[] = { 'r' };
static const char *help_items[] = { "Keys..." };
static const char help_hots[] = { 'k' };

static void redraw(void);
static void save_tab(void);
static void editor_leave(int run);
static void open_dialog(int which);
static void activate_menu(void);
static int add_or_switch(const char *path);
static void next_tab(void);

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

static int find_tab_path(const char *path)
{
	int i;
	if (!path || !path[0])
		return -1;
	for (i = 0; i < G.ed.ntabs; i++)
		if (G.ed.tab[i].used && mmb_keyword_eq(G.ed.tab[i].path, path))
			return i;
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
		strncpy(t->path, path, sizeof(t->path) - 1);
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
		strncpy(p, path, sizeof(p) - 1);
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
	if (t->cx < t->len)
		memmove(t->buf + t->cx + 1, t->buf + t->cx, (unsigned)(t->len - t->cx));
	t->buf[t->cx++] = c;
	t->len++;
	t->buf[t->len] = 0;
	t->dirty = 1;
}

static void insert_newline_indent(void)
{
	mmb_ed_tab *t = cur_tab();
	char indent[64];
	int n = 0;
	int i;

	if (!t)
		return;
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
	if (!t || t->cx <= 0)
		return;
	memmove(t->buf + t->cx - 1, t->buf + t->cx, (unsigned)(t->len - t->cx + 1));
	t->cx--;
	t->len--;
	t->dirty = 1;
}

static void delete_char(void)
{
	mmb_ed_tab *t = cur_tab();
	if (!t || t->cx >= t->len)
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
	end = line_end(t->cx);
	if (end < t->len && t->buf[end] == '\n')
		end++;
	killlen = 0;
	if (end > t->cx)
	{
		int n = end - t->cx;
		if (n >= (int)sizeof(killbuf))
			n = (int)sizeof(killbuf) - 1;
		memcpy(killbuf, t->buf + t->cx, (unsigned)n);
		killlen = n;
		killbuf[killlen] = 0;
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

static void draw_text_line(int x, int y, const char *s, int n, int col0)
{
	int i, vis = 0, shown = 0, in_str = 0, in_cmt;
	in_cmt = line_is_comment(s, n);
	for (i = 0; i < n && shown < TEXT_COLS; i++)
	{
		char ch = s[i];
		int fg, k, w;
		if (!in_cmt && !in_str && ch == '\'')
			in_cmt = 1;
		if (!in_cmt && !in_str && ch == '"')
			in_str = 1;
		else if (in_str && ch == '"')
			in_str = 2;
		if (in_cmt)
			fg = C_CMT_FG;
		else if (in_str)
			fg = C_STR_FG;
		else if (ch >= '0' && ch <= '9')
			fg = C_NUM_FG;
		else
			fg = C_EDIT_FG;
		w = ch_cols(ch);
		for (k = 0; k < w && shown < TEXT_COLS; k++)
		{
			if (vis >= col0)
			{
				char out = (ch == '\t') ? ' ' : ch;
				tui_put(x + shown, y, (unsigned char)out, fg, C_EDIT_BG);
				shown++;
			}
			vis++;
		}
		if (in_str == 2)
			in_str = 0;
	}
	if (shown < TEXT_COLS)
		tui_pad(x + shown, y, "", TEXT_COLS - shown, C_EDIT_FG, C_EDIT_BG);
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
		draw_text_line(1, y, t->buf + start, n, t->col0);
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

static void draw_dialog(void)
{
	int w = 50, h = 8, r0, c0, i;
	const char *title;
	if (G.ed.dialog == DLG_NONE)
		return;
	if (G.ed.dialog == DLG_HELP)
	{
		w = 48;
		h = 16;
		title = " Help ";
	}
	else if (G.ed.dialog == DLG_OPEN)
		title = " Open ";
	else
		title = " Save As ";
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
			"Alt+R  Run menu      Alt+H  Help",
			"F10    File menu     Esc    close",
			"F1     This help     F2     Save",
			"F3     Open          F9     Run",
			"^O     Save          ^X     Quit",
			"^R     Save and Run  ^K/^U  Cut/Paste",
			"Tab    4 spaces      Alt+1..9 file tab",
			"Arrows move          Enter  activate",
			"",
			"     Enter or Esc closes this box",
		};
		int L = (int)(sizeof(lines) / sizeof(lines[0]));
		for (i = 0; i < L && r0 + 2 + i < r0 + h - 1; i++)
			tui_pad(c0 + 2, r0 + 2 + i, lines[i], w - 4, C_DLG_FG, C_DLG_BG);
	}
	else
	{
		tui_pad(c0 + 2, r0 + 2, "Path:", w - 4, C_DLG_FG, C_DLG_BG);
		tui_pad(c0 + 2, r0 + 3, G.ed.dlg, w - 4, C_SEL_FG, C_SEL_BG);
		tui_pad(c0 + 2, r0 + 5, "Enter=OK   Esc=Cancel", w - 4, C_DLG_FG, C_DLG_BG);
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
	if (G.ed.dialog == DLG_OPEN || G.ed.dialog == DLG_SAVEAS)
	{
		int w = 50, h = 8;
		int r0 = (ROWS - h) / 2;
		int c0 = (COLS - w) / 2;
		int col = G.ed.dlglen;
		if (col > w - 4)
			col = w - 4;
		tui_cursor(c0 + 2 + col, r0 + 3, 1);
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

static void editor_leave(int run)
{
	mmb_ed_tab *t = cur_tab();
	if (run)
		save_tab();
	else if (t && t->dirty)
		save_tab();
	tui_end();
	G.ed.active = 0;
	G.ed.menu_open = 0;
	G.ed.dialog = 0;
	if (run && t)
	{
		char cmd[160];
		G.ed.run_on_exit = 1;
		strcpy(cmd, "RUN \"");
		strncat(cmd, t->path, sizeof(cmd) - 8);
		strcat(cmd, "\"");
		mmb_exec_line(cmd);
	}
}

static void open_menu(int which)
{
	int n;
	G.ed.dialog = DLG_NONE;
	G.ed.menu_open = 1;
	G.ed.menu = which;
	G.ed.menu_item = 0;
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
	if (which == DLG_SAVEAS && cur_tab() && cur_tab()->path[0])
	{
		strncpy(G.ed.dlg, cur_tab()->path, sizeof(G.ed.dlg) - 1);
		G.ed.dlglen = (int)strlen(G.ed.dlg);
	}
}

static void next_tab(void)
{
	if (G.ed.ntabs <= 1)
		return;
	G.ed.cur = (G.ed.cur + 1) % G.ed.ntabs;
	set_status(0);
}

static void close_tab(void)
{
	int i;
	mmb_ed_tab *t = cur_tab();
	if (t && t->dirty)
		save_tab();
	if (G.ed.ntabs <= 1)
	{
		editor_leave(0);
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
	if (G.ed.dialog == DLG_OPEN)
	{
		if (G.ed.dlg[0])
		{
			if (add_or_switch(G.ed.dlg) < 0)
				set_status("Open failed");
		}
		close_ui();
		return;
	}
	if (G.ed.dialog == DLG_SAVEAS)
	{
		mmb_ed_tab *t = cur_tab();
		if (t && G.ed.dlg[0])
		{
			strncpy(t->path, G.ed.dlg, sizeof(t->path) - 1);
			ensure_bas(t->path, sizeof(t->path));
			save_tab();
		}
		close_ui();
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
			save_tab();
		else if (item == 2)
			open_dialog(DLG_SAVEAS);
		else if (item == 3)
			close_tab();
		else if (item == 4)
			next_tab();
		else if (item == 5)
			editor_leave(0);
	}
	else if (menu == MENU_EDIT)
	{
		if (item == 0)
			cut_line();
		else
			paste_kill();
	}
	else if (menu == MENU_RUN)
		editor_leave(1);
	else
		open_dialog(DLG_HELP);
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
	if (c == 'h')
	{
		open_menu(MENU_HELP);
		return 1;
	}
	if (c == 'x')
	{
		editor_leave(0);
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

static void do_fkey(int n)
{
	if (n == 1)
		open_dialog(DLG_HELP);
	else if (n == 2)
		save_tab();
	else if (n == 3)
		open_dialog(DLG_OPEN);
	else if (n == 9)
		editor_leave(1);
	else if (n == 10)
		open_menu(MENU_FILE);
}

static int handle_arrow_or_special(int kind)
{
	/* kind: 1 up 2 down 3 right 4 left 5 home 6 end 7 del 8 ins 9 pgup 10 pgdn */
	if (G.ed.dialog == DLG_HELP)
	{
		if (kind == 0)
			return 0;
		return 1;
	}
	if (G.ed.dialog)
	{
		if (kind == 4 && G.ed.dlglen > 0)
			; /* no cursor in field besides end */
		return 1;
	}
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
	if (kind == 1)
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
	else if (kind == 7)
		delete_char();
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
			return 1;
		}
		esc_state = ESC_NONE;
		if (handle_alt(c))
		{
			if (G.ed.active)
				redraw();
			return 1;
		}
		if (G.ed.menu_open || G.ed.dialog)
		{
			close_ui();
			if (G.ed.active)
				redraw();
			return 1;
		}
		return 0;
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
			return 1;
		}
		if (c == '[')
		{
			esc_state = ESC_SS3;
			return 1;
		}
		esc_state = ESC_NONE;
		if (c == 'A')
			handle_arrow_or_special(1);
		else if (c == 'B')
			handle_arrow_or_special(2);
		else if (c == 'C')
			handle_arrow_or_special(3);
		else if (c == 'D')
			handle_arrow_or_special(4);
		else if (c == 'H')
			handle_arrow_or_special(5);
		else if (c == 'F')
			handle_arrow_or_special(6);
		else if (c == '~')
		{
			int n = csi_arg;
			if (n == 1 || n == 7)
				handle_arrow_or_special(5);
			else if (n == 4 || n == 8)
				handle_arrow_or_special(6);
			else if (n == 3)
				handle_arrow_or_special(7);
			else if (n == 2)
				;
			else if (n == 5)
				handle_arrow_or_special(9);
			else if (n == 6)
				handle_arrow_or_special(10);
			else if (n >= 11 && n <= 15)
				do_fkey(n - 10);
			else if (n >= 17 && n <= 21)
				do_fkey(n - 11);
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
			redraw();
		}
		return 1;
	}
	if (c >= 32 && c < 127 && G.ed.dlglen < (int)sizeof(G.ed.dlg) - 1)
	{
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
			if (G.ed.active)
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
					if (G.ed.active)
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
	if (c == 24) /* Ctrl+X quit */
	{
		editor_leave(0);
		return G.out;
	}
	if (c == 18) /* Ctrl+R run */
	{
		editor_leave(1);
		return G.out;
	}
	if (c == 11) /* Ctrl+K cut */
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
		int i;
		for (i = 0; i < ED_TAB; i++)
			insert_char(' ');
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
