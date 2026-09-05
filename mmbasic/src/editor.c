#include "mmb_priv.h"

/*
 * Turbo-style full-screen editor (80x30). Circle Font8x16 is Latin-1, so
 * borders are ASCII (= + |) rather than CP437 box drawing. Alt+letter is
 * not a distinct USB sequence in Circle (keymap returns KeyNone); Esc then
 * letter is the serial/USB fallback, and F10 opens File.
 */

#define ED (G.ed.tab[G.ed.cur])

#define COLS        80
#define ROWS        30
#define TEXT_ROWS   25
#define TEXT_COLS   78
#define ROW_MENU    1
#define ROW_TABS    2
#define ROW_BTOP    3
#define ROW_TEXT    4
#define ROW_BBOT    29
#define ROW_STAT    30

/* Circle SGR takes one parameter per CSI; do not combine 0;30;47. */
#define UI_MENU     "\x1b[0m\x1b[30m\x1b[47m"
#define UI_HOT      "\x1b[91m"
#define UI_BLACK    "\x1b[30m"
#define UI_SEL      "\x1b[0m\x1b[30m\x1b[42m"
#define UI_EDIT     "\x1b[0m\x1b[97m\x1b[44m"
#define UI_STR      "\x1b[93m"
#define UI_BRD      "\x1b[0m\x1b[37m\x1b[44m"
#define UI_TAB      "\x1b[0m\x1b[37m\x1b[44m"
#define UI_TABCUR   "\x1b[0m\x1b[30m\x1b[47m"
#define UI_DLG      "\x1b[0m\x1b[30m\x1b[47m"
#define UI_SHADOW   "\x1b[0m\x1b[40m"
#define UI_RST      "\x1b[0m"

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
static int esc_state;
static int csi_n;
static int csi_arg;

static const char *menu_name[MENU_COUNT] = { "File", "Edit", "Run", "Help" };
static const char menu_hot[MENU_COUNT] = { 'F', 'E', 'R', 'H' };
static const int menu_col[MENU_COUNT] = { 2, 8, 14, 19 };

static const char *file_items[] = {
	"Open...", "Save", "Save As...", "Close tab", "Next tab", "Quit"
};
static const char *edit_items[] = { "Cut line", "Paste" };
static const char *run_items[] = { "Run" };
static const char *help_items[] = { "Keys..." };

static void redraw(void);
static void save_tab(void);
static void editor_leave(int run);
static void open_dialog(int which);
static void activate_menu(void);
static int add_or_switch(const char *path);
static void next_tab(void);

static void ed_flush(const char *s)
{
	unsigned n;
	if (!s || !s[0] || !G.plat)
		return;
	n = (unsigned)strlen(s);
	if (G.plat->write_screen)
		G.plat->write_screen(s, n);
	if (G.plat->write_serial)
		G.plat->write_serial(s, n);
}

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

static void ed_at(int r, int c)
{
	char b[20];
	char *p = b;
	*p++ = '\x1b';
	*p++ = '[';
	p = put_uint(p, r);
	*p++ = ';';
	p = put_uint(p, c);
	*p++ = 'H';
	*p = 0;
	ed_flush(b);
}

static void ed_rep(char ch, int n)
{
	char b[81];
	int i;
	if (n > 80)
		n = 80;
	if (n < 0)
		n = 0;
	for (i = 0; i < n; i++)
		b[i] = ch;
	b[n] = 0;
	if (n)
		ed_flush(b);
}

static void ed_span(const char *s, int width)
{
	int n = 0;
	if (s)
	{
		while (s[n] && n < width)
			n++;
		if (n)
		{
			char b[81];
			int i;
			for (i = 0; i < n; i++)
				b[i] = s[i];
			b[n] = 0;
			ed_flush(b);
		}
	}
	if (n < width)
		ed_rep(' ', width - n);
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
			c++;
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
			r++;
			c = 0;
		}
		else
			c++;
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

static void draw_hot_word(const char *word, char hot, int selected)
{
	int i;
	ed_flush(selected ? UI_SEL : UI_MENU);
	for (i = 0; word[i]; i++)
	{
		char b[2];
		if ((word[i] == hot || word[i] == hot + 32 || word[i] == hot - 32) &&
		    (i == 0 || hot == word[i]))
		{
			ed_flush(UI_HOT);
			b[0] = word[i];
			b[1] = 0;
			ed_flush(b);
			ed_flush(selected ? "\x1b[30m\x1b[42m" : "\x1b[30m\x1b[47m");
		}
		else
		{
			b[0] = word[i];
			b[1] = 0;
			ed_flush(b);
		}
	}
}

static void draw_menu_bar(void)
{
	int i, used = 1;
	ed_at(ROW_MENU, 1);
	ed_flush(UI_MENU);
	ed_flush(" ");
	for (i = 0; i < MENU_COUNT; i++)
	{
		int sel = G.ed.menu_open && G.ed.menu == i;
		draw_hot_word(menu_name[i], menu_hot[i], sel);
		ed_flush(UI_MENU);
		ed_flush(" ");
		used += (int)strlen(menu_name[i]) + 1;
	}
	if (used < COLS)
		ed_rep(' ', COLS - used);
}

static void draw_tabs(void)
{
	int i, used = 0;
	ed_at(ROW_TABS, 1);
	ed_flush(UI_TAB);
	for (i = 0; i < G.ed.ntabs && used < COLS; i++)
	{
		const char *name = tab_label(i);
		int n = (int)strlen(name);
		if (n > 12)
			n = 12;
		if (used + n + 3 > COLS)
			break;
		ed_flush(i == G.ed.cur ? UI_TABCUR : UI_TAB);
		ed_flush(" ");
		{
			char b[16];
			int k;
			for (k = 0; k < n; k++)
				b[k] = name[k];
			b[n] = 0;
			ed_flush(b);
		}
		if (G.ed.tab[i].dirty)
			ed_flush("*");
		else
			ed_flush(" ");
		ed_flush(" ");
		used += n + 3;
	}
	ed_flush(UI_TAB);
	if (used < COLS)
		ed_rep(' ', COLS - used);
}

static void draw_border_row(int row, int top)
{
	mmb_ed_tab *t = cur_tab();
	const char *title;
	char titled[40];
	int n, left, i;
	ed_at(row, 1);
	ed_flush(UI_BRD);
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
		ed_flush("+");
		ed_flush(loc);
		ed_rep('=', COLS - 2 - (int)strlen(loc));
		ed_flush("+");
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
	ed_flush("+");
	ed_rep('=', left);
	ed_flush(titled);
	ed_rep('=', COLS - 2 - left - n);
	ed_flush("+");
}

static void draw_text_line(const char *s, int n, int col0)
{
	int i, shown = 0, in_str = 0;
	char q = 0;
	char run[81];
	int rn = 0;
	int colour = 0; /* 0 white 1 yellow */

	ed_flush(UI_EDIT);
	for (i = 0; i < n && shown < TEXT_COLS; i++)
	{
		char ch = s[i];
		int want;
		if (!in_str && (ch == '"' || ch == '\''))
		{
			in_str = 1;
			q = ch;
			want = 1;
		}
		else if (in_str && ch == q)
			want = 1;
		else
			want = in_str ? 1 : 0;
		if (i >= col0)
		{
			if (want != colour && rn)
			{
				run[rn] = 0;
				ed_flush(run);
				rn = 0;
			}
			if (want != colour)
			{
				ed_flush(want ? UI_STR : "\x1b[97m");
				colour = want;
			}
			run[rn++] = ch;
			shown++;
		}
		if (in_str && ch == q && i > 0)
			in_str = 0;
		if (!in_str && i == 0 && (ch == '"' || ch == '\''))
			; /* opened above */
	}
	if (rn)
	{
		run[rn] = 0;
		ed_flush(run);
	}
	if (shown < TEXT_COLS)
	{
		if (colour)
			ed_flush("\x1b[97m");
		ed_rep(' ', TEXT_COLS - shown);
	}
}

static void draw_editor_body(void)
{
	mmb_ed_tab *t = cur_tab();
	int vis, i, pos, row;
	if (!t)
	{
		for (vis = 0; vis < TEXT_ROWS; vis++)
		{
			ed_at(ROW_TEXT + vis, 1);
			ed_flush(UI_BRD);
			ed_flush("|");
			ed_flush(UI_EDIT);
			ed_rep(' ', TEXT_COLS);
			ed_flush(UI_BRD);
			ed_flush("|");
		}
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
		ed_at(ROW_TEXT + vis, 1);
		ed_flush(UI_BRD);
		ed_flush("|");
		if (pos > t->len)
			pos = t->len;
		while (pos < t->len && t->buf[pos] != '\n')
		{
			n++;
			pos++;
		}
		draw_text_line(t->buf + start, n, t->col0);
		ed_flush(UI_BRD);
		ed_flush("|");
		if (pos < t->len && t->buf[pos] == '\n')
			pos++;
		else if (pos >= t->len && vis + 1 < TEXT_ROWS)
		{
			/* remaining empty lines */
			for (i = vis + 1; i < TEXT_ROWS; i++)
			{
				ed_at(ROW_TEXT + i, 1);
				ed_flush(UI_BRD);
				ed_flush("|");
				ed_flush(UI_EDIT);
				ed_rep(' ', TEXT_COLS);
				ed_flush(UI_BRD);
				ed_flush("|");
			}
			break;
		}
	}
}

static void draw_dropdown(void)
{
	int n, i, w, r0, c0;
	const char **it;
	if (!G.ed.menu_open)
		return;
	it = menu_items(G.ed.menu, &n);
	w = menu_width(G.ed.menu);
	c0 = menu_col[G.ed.menu];
	if (c0 + w + 2 > COLS)
		c0 = COLS - w - 2;
	r0 = ROW_TABS;
	ed_at(r0, c0);
	ed_flush(UI_DLG);
	ed_flush("+");
	ed_rep('-', w - 2);
	ed_flush("+");
	ed_flush(UI_SHADOW);
	ed_flush("  ");
	for (i = 0; i < n; i++)
	{
		ed_at(r0 + 1 + i, c0);
		ed_flush(i == G.ed.menu_item ? UI_SEL : UI_DLG);
		ed_flush("| ");
		ed_span(it[i], w - 4);
		ed_flush(" |");
		ed_flush(UI_SHADOW);
		ed_flush("  ");
	}
	ed_at(r0 + 1 + n, c0);
	ed_flush(UI_DLG);
	ed_flush("+");
	ed_rep('-', w - 2);
	ed_flush("+");
	ed_flush(UI_SHADOW);
	ed_flush("  ");
	ed_at(r0 + 2 + n, c0 + 2);
	ed_flush(UI_SHADOW);
	ed_rep(' ', w);
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
	r0 = (ROWS - h) / 2;
	c0 = (COLS - w) / 2;
	if (r0 < 3)
		r0 = 3;
	ed_at(r0, c0);
	ed_flush(UI_DLG);
	ed_flush("+");
	{
		int left = (w - 2 - (int)strlen(title)) / 2;
		if (left < 1)
			left = 1;
		ed_rep('-', left);
		ed_flush(title);
		ed_rep('-', w - 2 - left - (int)strlen(title));
	}
	ed_flush("+");
	ed_flush(UI_SHADOW);
	ed_flush("  ");
	for (i = 1; i < h - 1; i++)
	{
		ed_at(r0 + i, c0);
		ed_flush(UI_DLG);
		ed_flush("|");
		ed_rep(' ', w - 2);
		ed_flush("|");
		ed_flush(UI_SHADOW);
		ed_flush("  ");
	}
	ed_at(r0 + h - 1, c0);
	ed_flush(UI_DLG);
	ed_flush("+");
	ed_rep('-', w - 2);
	ed_flush("+");
	ed_flush(UI_SHADOW);
	ed_flush("  ");
	ed_at(r0 + h, c0 + 2);
	ed_flush(UI_SHADOW);
	ed_rep(' ', w);

	if (G.ed.dialog == DLG_HELP)
	{
		static const char *lines[] = {
			"Esc+F  File menu     Esc+E  Edit",
			"Esc+R  Run menu      Esc+H  Help",
			"F10    File menu     Esc    close",
			"F1     This help     F2     Save",
			"F3     Open          F9     Run",
			"^O     Save          ^X     Quit",
			"^R     Save and Run  ^K/^U  Cut/Paste",
			"Tab    Next tab      Esc+1..9 tab",
			"Arrows move          Enter  activate",
			"",
			"     Enter or Esc closes this box",
		};
		int L = (int)(sizeof(lines) / sizeof(lines[0]));
		for (i = 0; i < L; i++)
		{
			ed_at(r0 + 2 + i, c0 + 2);
			ed_flush(UI_DLG);
			ed_span(lines[i], w - 4);
		}
	}
	else
	{
		ed_at(r0 + 2, c0 + 2);
		ed_flush(UI_DLG);
		ed_span("Path:", w - 4);
		ed_at(r0 + 3, c0 + 2);
		ed_flush(UI_SEL);
		ed_span(G.ed.dlg, w - 4);
		ed_at(r0 + 5, c0 + 2);
		ed_flush(UI_DLG);
		ed_span("Enter=OK   Esc=Cancel", w - 4);
	}
}

static void draw_status(void)
{
	int row, col;
	char right[40];
	char *p;
	int leftn, rightn;
	mmb_ed_tab *t = cur_tab();
	ed_at(ROW_STAT, 1);
	ed_flush(UI_MENU);
	ed_flush(UI_HOT);
	ed_flush("F1");
	ed_flush(UI_BLACK);
	ed_flush(" Help ");
	ed_flush(UI_HOT);
	ed_flush("F2");
	ed_flush(UI_BLACK);
	ed_flush(" Save ");
	ed_flush(UI_HOT);
	ed_flush("F3");
	ed_flush(UI_BLACK);
	ed_flush(" Open ");
	ed_flush(UI_HOT);
	ed_flush("F9");
	ed_flush(UI_BLACK);
	ed_flush(" Run ");
	ed_flush(UI_HOT);
	ed_flush("Alt+X");
	ed_flush(UI_BLACK);
	ed_flush(" Quit");
	if (G.ed.status[0])
	{
		ed_flush(" | ");
		ed_span(G.ed.status, 18);
		leftn = 48 + 3 + (int)strlen(G.ed.status);
		if (leftn > 60)
			leftn = 60;
	}
	else
		leftn = 48;
	row = 1;
	col = 1;
	if (t)
		pos_to_rowcol(t->cx, &row, &col);
	p = right;
	if (t && t->dirty)
		*p++ = '*';
	else
		*p++ = ' ';
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
	if (leftn + rightn < COLS)
		ed_rep(' ', COLS - leftn - rightn);
	ed_flush(right);
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
		ed_at(r0 + 3, c0 + 2 + col);
		ed_flush("\x1b[?25h");
		return;
	}
	if (G.ed.dialog || G.ed.menu_open)
	{
		ed_flush("\x1b[?25l");
		return;
	}
	{
		mmb_ed_tab *t = cur_tab();
		int row = 0, col = 0, sr, sc;
		if (t)
			pos_to_rowcol(t->cx, &row, &col);
		sr = ROW_TEXT + (t ? row - t->row0 : 0);
		sc = 2 + (t ? col - t->col0 : 0);
		if (sr < ROW_TEXT)
			sr = ROW_TEXT;
		if (sr > ROW_BBOT - 1)
			sr = ROW_BBOT - 1;
		if (sc < 2)
			sc = 2;
		if (sc > COLS - 1)
			sc = COLS - 1;
		ed_at(sr, sc);
		ed_flush("\x1b[?25h");
	}
}

static void redraw(void)
{
	G.outn = 0;
	G.out[0] = 0;
	ensure_visible();
	ed_flush("\x1b[?25l\x1b[H\x1b[J");
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
	ed_flush("\x1b[0m\x1b[?25h\x1b[H\x1b[J");
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

static void prev_tab(void)
{
	if (G.ed.ntabs <= 1)
		return;
	G.ed.cur = (G.ed.cur + G.ed.ntabs - 1) % G.ed.ntabs;
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
	redraw();
}

const char *mmb_editor_feed(char c)
{
	G.outn = 0;
	G.out[0] = 0;
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
			for (i = 0; i < n; i++)
			{
				char h = it[i][0];
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
		next_tab();
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
		insert_char('\n');
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
