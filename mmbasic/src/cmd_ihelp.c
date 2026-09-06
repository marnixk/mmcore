#include "mmb_priv.h"
#include "tui.h"

#define IH_MAX_LINES 220
#define IH_COLS      96
#define IH_MAX_LINKS 384
#define IH_STACK     16
#define IH_NAME      40
#define IH_MATCH     220

#define ATTR_TEXT  0
#define ATTR_BRACK 1
#define ATTR_LINK  2
#define ATTR_HEAD  3
#define ATTR_DIM   4

#define TGT_CONTENTS (-1)
#define TGT_INDEX    (-2)
#define TGT_BACK     (-3)

#define PAGE_INDEX    0
#define PAGE_CONTENTS 1
#define PAGE_TOPIC    2

#define NAV_N 3

typedef struct {
	int line;
	int col;
	int len;
	int target;
	char label[IH_NAME];
} ih_link;

typedef struct {
	int page;
	int topic_i;
	int scroll;
	int sel;
} ih_frame;

typedef struct {
	const char *pat;
	int plen;
	int target;
} ih_match;

static struct {
	int active;
	int page;
	int topic_i;
	int scroll;
	int sel;
	int wrap_w;
	int nlines;
	int nlinks;
	int cx;
	int cy;
	int esc;
	int csi_n;
	int stack_n;
	char title[64];
	char status[96];
	char lines[IH_MAX_LINES][IH_COLS + 1];
	unsigned char attr[IH_MAX_LINES][IH_COLS];
	ih_link links[IH_MAX_LINKS];
	ih_frame stack[IH_STACK];
	ih_match matches[IH_MATCH];
	int nmatches;
	int matches_ready;
} H;

static void ih_draw(void);
static void load_index(void);
static void load_contents(void);
static void load_topic(int ti);
static void ensure_visible(void);

static int to_upper(char c)
{
	if (c >= 'a' && c <= 'z')
		return c - 32;
	return (unsigned char)c;
}

static int is_id(char c)
{
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
	       (c >= '0' && c <= '9') || c == '_' || c == '$' || c == '.';
}

static int ieq_n(const char *a, const char *b, int n)
{
	int i;
	for (i = 0; i < n; i++)
	{
		if (!a[i] || !b[i])
			return 0;
		if (to_upper(a[i]) != to_upper(b[i]))
			return 0;
	}
	return 1;
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

static void set_status(const char *s)
{
	int i;
	for (i = 0; i < (int)sizeof(H.status) - 1 && s && s[i]; i++)
		H.status[i] = s[i];
	H.status[i] = 0;
}

static void set_title(const char *s)
{
	int i;
	H.title[0] = 0;
	if (!s)
		return;
	strncpy(H.title, "HELP: ", sizeof(H.title) - 1);
	i = (int)strlen(H.title);
	strncpy(H.title + i, s, sizeof(H.title) - 1 - (unsigned)i);
	H.title[sizeof(H.title) - 1] = 0;
}

static void add_match(const char *pat, int target)
{
	int i, n;
	if (!pat || !pat[0] || H.nmatches >= IH_MATCH)
		return;
	n = (int)strlen(pat);
	for (i = 0; i < H.nmatches; i++)
	{
		if (H.matches[i].plen == n && mmb_keyword_eq(H.matches[i].pat, pat))
			return;
	}
	H.matches[H.nmatches].pat = pat;
	H.matches[H.nmatches].plen = n;
	H.matches[H.nmatches].target = target;
	H.nmatches++;
}

static void build_matches(void)
{
	int i, j, n, a;
	ih_match tmp;
	if (H.matches_ready)
		return;
	H.nmatches = 0;
	n = mmb_help_topic_count();
	for (i = 0; i < n; i++)
		add_match(mmb_help_topic_name(i), i);
	for (i = 0; i < H.nmatches; i++)
	{
		for (j = i + 1; j < H.nmatches; j++)
		{
			if (H.matches[j].plen > H.matches[i].plen)
			{
				tmp = H.matches[i];
				H.matches[i] = H.matches[j];
				H.matches[j] = tmp;
			}
		}
	}
	H.matches_ready = 1;
}

static int match_at(const char *s, int skip_tgt)
{
	int i;
	for (i = 0; i < H.nmatches; i++)
	{
		if (H.matches[i].target == skip_tgt)
			continue;
		if (!ieq_n(s, H.matches[i].pat, H.matches[i].plen))
			continue;
		if (is_id(s[H.matches[i].plen]))
			continue;
		return i;
	}
	return -1;
}

static void line_clear(int y)
{
	int i;
	if (y < 0 || y >= IH_MAX_LINES)
		return;
	for (i = 0; i < H.wrap_w && i < IH_COLS; i++)
	{
		H.lines[y][i] = ' ';
		H.attr[y][i] = ATTR_TEXT;
	}
	H.lines[y][i] = 0;
}

static void page_reset(void)
{
	H.nlines = 0;
	H.nlinks = 0;
	H.cx = 0;
	H.cy = -1;
	H.scroll = 0;
}

static void newline(void)
{
	if (H.nlines >= IH_MAX_LINES)
		return;
	H.cy = H.nlines;
	H.cx = 0;
	line_clear(H.cy);
	H.nlines++;
}

static void putc_attr(char ch, unsigned char a)
{
	if (H.cy < 0 || H.cx >= H.wrap_w)
		newline();
	if (H.cy < 0 || H.cy >= IH_MAX_LINES)
		return;
	if (H.cx >= H.wrap_w)
		return;
	H.lines[H.cy][H.cx] = ch;
	H.attr[H.cy][H.cx] = a;
	H.cx++;
}

static void puts_attr(const char *s, unsigned char a)
{
	if (!s)
		return;
	while (*s)
		putc_attr(*s++, a);
}

static void add_link(const char *label, int target)
{
	int n, start;
	if (!label || !label[0])
		return;
	n = (int)strlen(label);
	if (n >= IH_NAME)
		n = IH_NAME - 1;
	if (H.cx + n + 2 > H.wrap_w && H.cx > 0)
		newline();
	if (H.cy < 0)
		newline();
	start = H.cx;
	putc_attr('<', ATTR_BRACK);
	{
		int i;
		for (i = 0; i < n; i++)
			putc_attr(label[i], ATTR_LINK);
	}
	putc_attr('>', ATTR_BRACK);
	if (H.nlinks < IH_MAX_LINKS)
	{
		H.links[H.nlinks].line = H.cy;
		H.links[H.nlinks].col = start;
		H.links[H.nlinks].len = n + 2;
		H.links[H.nlinks].target = target;
		memcpy(H.links[H.nlinks].label, label, (unsigned)n);
		H.links[H.nlinks].label[n] = 0;
		H.nlinks++;
	}
}

static void add_nav(void)
{
	H.links[0].line = -1;
	H.links[0].col = 1;
	H.links[0].len = 10;
	H.links[0].target = TGT_CONTENTS;
	strncpy(H.links[0].label, "Contents", IH_NAME - 1);
	H.links[1].line = -1;
	H.links[1].col = 13;
	H.links[1].len = 7;
	H.links[1].target = TGT_INDEX;
	strncpy(H.links[1].label, "Index", IH_NAME - 1);
	H.links[2].line = -1;
	H.links[2].col = 22;
	H.links[2].len = 6;
	H.links[2].target = TGT_BACK;
	strncpy(H.links[2].label, "Back", IH_NAME - 1);
	H.nlinks = NAV_N;
}

static void emit_source(const char *src, int skip_tgt)
{
	const char *p;
	if (!src)
		return;
	p = src;
	while (*p)
	{
		const char *e = p;
		int n, i;
		while (*e && *e != '\n')
			e++;
		n = (int)(e - p);
		i = 0;
		while (i < n)
		{
			if (i == 0 || !is_id(p[i - 1]))
			{
				int m = match_at(p + i, skip_tgt);
				if (m >= 0)
				{
					add_link(H.matches[m].pat, H.matches[m].target);
					i += H.matches[m].plen;
					continue;
				}
			}
			putc_attr(p[i], ATTR_TEXT);
			i++;
		}
		if (*e == '\n')
		{
			newline();
			p = e + 1;
		}
		else
			break;
	}
}

static void add_letter_header(char letter)
{
	int i;
	char box[8];
	newline();
	for (i = 0; i < H.wrap_w - 5; i++)
		putc_attr('=', ATTR_HEAD);
	box[0] = ' ';
	box[1] = '[';
	box[2] = letter;
	box[3] = ']';
	box[4] = 0;
	puts_attr(box, ATTR_HEAD);
}

static int name_cmp(int a, int b)
{
	const char *na = mmb_help_topic_name(a);
	const char *nb = mmb_help_topic_name(b);
	while (*na && *nb)
	{
		int ca = to_upper(*na), cb = to_upper(*nb);
		if (ca != cb)
			return ca - cb;
		na++;
		nb++;
	}
	return to_upper(*na) - to_upper(*nb);
}

static int body_h(void)
{
	int h = tui_rows() - 3;
	return h < 3 ? 3 : h;
}

static int body_y0(void)
{
	return 2;
}

static void load_index(void)
{
	int ord[128];
	int n, i, j, colw;
	char letter;
	page_reset();
	add_nav();
	H.page = PAGE_INDEX;
	H.topic_i = -1;
	set_title("Index");
	n = mmb_help_topic_count();
	if (n > 128)
		n = 128;
	for (i = 0; i < n; i++)
		ord[i] = i;
	for (i = 0; i < n; i++)
	{
		for (j = i + 1; j < n; j++)
		{
			if (name_cmp(ord[j], ord[i]) < 0)
			{
				int t = ord[i];
				ord[i] = ord[j];
				ord[j] = t;
			}
		}
	}
	colw = H.wrap_w / 2;
	if (colw < 18)
		colw = H.wrap_w;
	newline();
	puts_attr("Use the arrow keys to move the cursor to a keyword.", ATTR_DIM);
	newline();
	puts_attr("Press Enter to open a topic. Escape goes back.", ATTR_DIM);
	for (letter = 'A'; letter <= 'Z'; letter++)
	{
		int grp[128];
		int ng = 0;
		for (i = 0; i < n; i++)
		{
			const char *nm = mmb_help_topic_name(ord[i]);
			if (nm[0] && to_upper(nm[0]) == letter)
				grp[ng++] = ord[i];
		}
		if (!ng)
			continue;
		add_letter_header(letter);
		for (i = 0; i < ng; i += 2)
		{
			newline();
			add_link(mmb_help_topic_name(grp[i]), grp[i]);
			if (i + 1 < ng)
			{
				H.cx = colw;
				add_link(mmb_help_topic_name(grp[i + 1]), grp[i + 1]);
			}
		}
	}
	newline();
	newline();
	puts_attr("Type IHELP topic to open a page. See also ", ATTR_DIM);
	add_link("Contents", TGT_CONTENTS);
	puts_attr(" (HELP BASIC).", ATTR_DIM);
	H.sel = 0;
	H.scroll = 0;
}

static void load_contents(void)
{
	int i, n;
	page_reset();
	add_nav();
	H.page = PAGE_CONTENTS;
	H.topic_i = -1;
	set_title("Contents");
	newline();
	puts_attr("MMBasic Interactive Help", ATTR_HEAD);
	newline();
	puts_attr("Use arrows to move between ", ATTR_TEXT);
	add_link("Index", TGT_INDEX);
	puts_attr(" links. Enter opens a topic.", ATTR_TEXT);
	newline();
	newline();
	emit_source(mmb_help_commands_overview(), -999);
	newline();
	emit_source(mmb_help_basic_overview(), -999);
	newline();
	puts_attr("Commands", ATTR_HEAD);
	newline();
	n = mmb_help_topic_count();
	for (i = 0; i < n; i++)
	{
		if (mmb_help_topic_kind(i) != 1)
			continue;
		if (H.cx + 18 > H.wrap_w && H.cx > 0)
			newline();
		else if (H.cx > 0)
			putc_attr(' ', ATTR_TEXT);
		add_link(mmb_help_topic_name(i), i);
	}
	newline();
	newline();
	puts_attr("Language", ATTR_HEAD);
	newline();
	for (i = 0; i < n; i++)
	{
		if (mmb_help_topic_kind(i) != 2)
			continue;
		if (H.cx + 18 > H.wrap_w && H.cx > 0)
			newline();
		else if (H.cx > 0)
			putc_attr(' ', ATTR_TEXT);
		add_link(mmb_help_topic_name(i), i);
	}
	H.sel = 0;
	H.scroll = 0;
}

static void load_topic(int ti)
{
	const char *name;
	page_reset();
	add_nav();
	H.page = PAGE_TOPIC;
	H.topic_i = ti;
	name = mmb_help_topic_name(ti);
	set_title(name);
	newline();
	emit_source(mmb_help_topic_text(ti), ti);
	H.sel = NAV_N < H.nlinks ? NAV_N : 0;
	H.scroll = 0;
	ensure_visible();
}

static void push_frame(void)
{
	if (H.stack_n >= IH_STACK)
		return;
	H.stack[H.stack_n].page = H.page;
	H.stack[H.stack_n].topic_i = H.topic_i;
	H.stack[H.stack_n].scroll = H.scroll;
	H.stack[H.stack_n].sel = H.sel;
	H.stack_n++;
}

static void restore_page(int page, int topic_i)
{
	if (page == PAGE_CONTENTS)
		load_contents();
	else if (page == PAGE_TOPIC && topic_i >= 0)
		load_topic(topic_i);
	else
		load_index();
}

static void pop_frame(void)
{
	ih_frame f;
	if (H.stack_n <= 0)
		return;
	H.stack_n--;
	f = H.stack[H.stack_n];
	restore_page(f.page, f.topic_i);
	H.scroll = f.scroll;
	H.sel = f.sel;
	if (H.sel < 0)
		H.sel = 0;
	if (H.sel >= H.nlinks)
		H.sel = H.nlinks ? H.nlinks - 1 : 0;
	ensure_visible();
}

static void close_ihelp(void)
{
	H.active = 0;
	H.esc = 0;
	H.stack_n = 0;
	tui_end();
	ser("\r\n");
}

static void do_back(void)
{
	if (H.stack_n > 0)
		pop_frame();
	else if (H.page == PAGE_INDEX)
		close_ihelp();
	else
	{
		load_index();
		set_status("<Esc=Quit>  <Enter=Open>  <PgUp/PgDn=Scroll>");
	}
}

static int find_body_link(int target)
{
	int i;
	for (i = NAV_N; i < H.nlinks; i++)
	{
		if (H.links[i].target == target)
			return i;
	}
	return -1;
}

static void ensure_visible(void)
{
	int ly, bh;
	if (H.sel < 0 || H.sel >= H.nlinks)
		return;
	ly = H.links[H.sel].line;
	if (ly < 0)
		return;
	bh = body_h();
	if (H.scroll < 0)
		H.scroll = 0;
	if (ly < H.scroll)
		H.scroll = ly;
	if (ly >= H.scroll + bh)
		H.scroll = ly - bh + 1;
	if (H.scroll < 0)
		H.scroll = 0;
	if (H.nlines > bh && H.scroll > H.nlines - bh)
		H.scroll = H.nlines - bh;
}

static void select_visible_after_scroll(int from_bottom)
{
	int bh = body_h();
	int y0 = H.scroll;
	int y1 = H.scroll + bh;
	int i, pick = -1;
	for (i = NAV_N; i < H.nlinks; i++)
	{
		int ly = H.links[i].line;
		if (ly < y0 || ly >= y1)
			continue;
		if (pick < 0)
			pick = i;
		if (from_bottom)
			pick = i;
	}
	if (pick >= 0)
		H.sel = pick;
}

static void move_sel(int dir)
{
	if (H.nlinks <= 0)
		return;
	if (dir < 0)
	{
		if (H.sel > 0)
			H.sel--;
	}
	else
	{
		if (H.sel + 1 < H.nlinks)
			H.sel++;
	}
	ensure_visible();
}

static void move_vert(int dir)
{
	int i, cur, cline, ccol, best = -1, best_d = 1 << 30;
	if (H.nlinks <= 0)
		return;
	cur = H.sel;
	if (cur < 0 || cur >= H.nlinks)
		cur = 0;
	cline = H.links[cur].line;
	ccol = H.links[cur].col;
	if (cline < 0)
	{
		if (dir > 0)
		{
			for (i = NAV_N; i < H.nlinks; i++)
			{
				if (H.links[i].line >= H.scroll)
				{
					H.sel = i;
					ensure_visible();
					return;
				}
			}
		}
		move_sel(dir);
		return;
	}
	for (i = 0; i < H.nlinks; i++)
	{
		int ly = H.links[i].line;
		int d, dc;
		if (dir > 0)
		{
			if (ly <= cline)
				continue;
		}
		else
		{
			if (ly < 0 || ly >= cline)
				continue;
		}
		dc = H.links[i].col - ccol;
		if (dc < 0)
			dc = -dc;
		d = (ly - cline);
		if (d < 0)
			d = -d;
		d = d * 1000 + dc;
		if (d < best_d)
		{
			best_d = d;
			best = i;
		}
	}
	if (best >= 0)
		H.sel = best;
	else
		move_sel(dir);
	ensure_visible();
}

static void page_scroll(int dir)
{
	int bh = body_h();
	int maxs = H.nlines > bh ? H.nlines - bh : 0;
	H.scroll += dir * bh;
	if (H.scroll < 0)
		H.scroll = 0;
	if (H.scroll > maxs)
		H.scroll = maxs;
	select_visible_after_scroll(dir < 0);
}

static void jump_letter(char letter)
{
	int i;
	letter = (char)to_upper(letter);
	if (H.page != PAGE_INDEX)
		return;
	for (i = NAV_N; i < H.nlinks; i++)
	{
		if (H.links[i].label[0] && to_upper(H.links[i].label[0]) == letter &&
		    H.links[i].target >= 0)
		{
			H.sel = i;
			ensure_visible();
			return;
		}
	}
}

static void go_target(int tgt)
{
	if (tgt == TGT_BACK)
	{
		do_back();
		return;
	}
	if (tgt == TGT_INDEX)
	{
		if (H.page == PAGE_INDEX)
			return;
		push_frame();
		load_index();
		return;
	}
	if (tgt == TGT_CONTENTS)
	{
		if (H.page == PAGE_CONTENTS)
			return;
		push_frame();
		load_contents();
		return;
	}
	if (tgt >= 0)
	{
		if (H.page == PAGE_TOPIC && H.topic_i == tgt)
			return;
		push_frame();
		load_topic(tgt);
	}
}

static void activate(void)
{
	if (H.sel < 0 || H.sel >= H.nlinks)
		return;
	go_target(H.links[H.sel].target);
}

static void draw_link_span(int x, int y, const char *label, int selected)
{
	int i;
	tui_put(x, y, '<', TUI_BRGREEN, selected ? TUI_BRWHITE : TUI_BLACK);
	for (i = 0; label[i]; i++)
		tui_put(x + 1 + i, y, (unsigned char)label[i],
			selected ? TUI_BLACK : TUI_BRWHITE,
			selected ? TUI_BRWHITE : TUI_BLACK);
	tui_put(x + 1 + i, y, '>', TUI_BRGREEN, selected ? TUI_BRWHITE : TUI_BLACK);
}

static void ih_draw(void)
{
	int x, y, w, h, bh, by, sbx, maxs, thumb;
	int i;
	char st[96];

	if (!H.active)
		return;
	tui_begin();
	w = tui_cols();
	h = tui_rows();
	tui_clear(TUI_WHITE, TUI_BLACK);
	tui_fill(0, 0, w, 1, ' ', TUI_BRWHITE, TUI_BRBLACK);
	{
		int n = (int)strlen(H.title);
		int tx = (w - n) / 2;
		if (tx < 1)
			tx = 1;
		tui_puts(tx, 0, H.title, TUI_BRWHITE, TUI_BRBLACK);
	}
	tui_fill(0, 1, w, 1, ' ', TUI_WHITE, TUI_BLACK);
	for (i = 0; i < NAV_N && i < H.nlinks; i++)
		draw_link_span(H.links[i].col, 1, H.links[i].label, H.sel == i);

	bh = body_h();
	by = body_y0();
	sbx = w - 1;
	for (y = 0; y < bh; y++)
	{
		int ly = H.scroll + y;
		int ry = by + y;
		if (ly < 0 || ly >= H.nlines)
		{
			tui_fill(0, ry, w - 1, 1, ' ', TUI_WHITE, TUI_BLACK);
			continue;
		}
		for (x = 0; x < w - 1; x++)
		{
			char ch = (x < H.wrap_w) ? H.lines[ly][x] : ' ';
			unsigned char a = (x < H.wrap_w) ? H.attr[ly][x] : ATTR_TEXT;
			int fg = TUI_WHITE, bg = TUI_BLACK;
			if (a == ATTR_BRACK)
				fg = TUI_BRGREEN;
			else if (a == ATTR_LINK)
				fg = TUI_BRWHITE;
			else if (a == ATTR_HEAD)
				fg = TUI_BRWHITE;
			else if (a == ATTR_DIM)
				fg = TUI_WHITE;
			tui_put(x, ry, (unsigned char)ch, fg, bg);
		}
	}
	for (i = NAV_N; i < H.nlinks; i++)
	{
		int ly = H.links[i].line;
		int ry;
		if (ly < H.scroll || ly >= H.scroll + bh)
			continue;
		ry = by + (ly - H.scroll);
		draw_link_span(H.links[i].col, ry, H.links[i].label, H.sel == i);
	}

	maxs = H.nlines > bh ? H.nlines - bh : 0;
	tui_put(sbx, by, '^', TUI_BRGREEN, TUI_BLACK);
	for (y = 1; y < bh - 1; y++)
		tui_put(sbx, by + y, '|', TUI_GREEN, TUI_BLACK);
	if (bh > 1)
		tui_put(sbx, by + bh - 1, 'v', TUI_BRGREEN, TUI_BLACK);
	if (bh > 2)
	{
		if (maxs <= 0)
			thumb = 1;
		else
			thumb = 1 + (H.scroll * (bh - 3)) / maxs;
		if (thumb < 1)
			thumb = 1;
		if (thumb > bh - 2)
			thumb = bh - 2;
		tui_put(sbx, by + thumb, '#', TUI_BRGREEN, TUI_BLACK);
	}

	tui_fill(0, h - 1, w, 1, ' ', TUI_BLACK, TUI_CYAN);
	if (H.status[0])
		strncpy(st, H.status, sizeof(st) - 1);
	else
		strncpy(st, "<Esc=Back>  <Enter=Open>  <PgUp/PgDn=Scroll>", sizeof(st) - 1);
	st[sizeof(st) - 1] = 0;
	{
		int p = 1;
		char *s = st;
		while (*s && p < w - 1)
		{
			if (*s == '<')
			{
				tui_put(p++, h - 1, '<', TUI_BRGREEN, TUI_CYAN);
				s++;
				while (*s && *s != '>' && p < w - 1)
				{
					tui_put(p++, h - 1, (unsigned char)*s, TUI_BLACK, TUI_CYAN);
					s++;
				}
				if (*s == '>' && p < w - 1)
				{
					tui_put(p++, h - 1, '>', TUI_BRGREEN, TUI_CYAN);
					s++;
				}
			}
			else
			{
				tui_put(p++, h - 1, (unsigned char)*s, TUI_BLACK, TUI_CYAN);
				s++;
			}
		}
	}
	tui_cursor(0, 0, 0);
	tui_flush();
}

static void apply_open(const char *topic)
{
	int ti;
	H.stack_n = 0;
	H.esc = 0;
	H.csi_n = 0;
	set_status("");
	if (!topic || !topic[0])
	{
		load_index();
		return;
	}
	if (mmb_keyword_eq(topic, "BASIC"))
	{
		load_contents();
		return;
	}
	ti = mmb_help_lookup(topic);
	if (ti < 0)
	{
		char msg[80];
		load_index();
		strncpy(msg, "Unknown topic: ", sizeof(msg) - 1);
		strncat(msg, topic, sizeof(msg) - strlen(msg) - 1);
		set_status(msg);
		return;
	}
	load_index();
	{
		int idx = find_body_link(ti);
		if (idx >= 0)
			H.sel = idx;
		ensure_visible();
	}
	push_frame();
	load_topic(ti);
}

void mmb_ihelp_open(const char *topic)
{
	memset(&H, 0, sizeof(H));
	H.active = 1;
	tui_begin();
	tui_invalidate();
	H.wrap_w = tui_cols() - 1;
	if (H.wrap_w > IH_COLS)
		H.wrap_w = IH_COLS;
	if (H.wrap_w < 40)
		H.wrap_w = 40;
	build_matches();
	apply_open(topic);
	ih_draw();
}

int mmb_in_ihelp(void)
{
	return H.active;
}

static void handle_arrow(int which)
{
	if (which == 0)
		move_vert(-1);
	else if (which == 1)
		move_vert(1);
	else if (which == 2)
		move_sel(1);
	else if (which == 3)
		move_sel(-1);
}

static int handle_esc_char(char c)
{
	if (H.esc == 1)
	{
		if (c == '[' || c == 'O')
		{
			H.esc = (c == '[') ? 2 : 5;
			H.csi_n = 0;
			return 1;
		}
		H.esc = 0;
		do_back();
		return 1;
	}
	if (H.esc == 2)
	{
		if (c == '[')
		{
			H.esc = 3;
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
		else if (c == 'H')
		{
			H.sel = 0;
			H.scroll = 0;
		}
		else if (c == 'F')
		{
			if (H.nlinks)
				H.sel = H.nlinks - 1;
			ensure_visible();
		}
		else if (c >= '0' && c <= '9')
		{
			H.csi_n = c - '0';
			H.esc = 4;
			return 1;
		}
		H.esc = 0;
		return 1;
	}
	if (H.esc == 3)
	{
		H.esc = 0;
		return 1;
	}
	if (H.esc == 4)
	{
		if (c >= '0' && c <= '9')
		{
			H.csi_n = H.csi_n * 10 + (c - '0');
			return 1;
		}
		H.esc = 0;
		if (c == '~')
		{
			if (H.csi_n == 5)
				page_scroll(-1);
			else if (H.csi_n == 6)
				page_scroll(1);
			else if (H.csi_n == 1)
			{
				H.sel = 0;
				H.scroll = 0;
			}
			else if (H.csi_n == 4)
			{
				if (H.nlinks)
					H.sel = H.nlinks - 1;
				ensure_visible();
			}
		}
		return 1;
	}
	if (H.esc == 5)
	{
		H.esc = 0;
		if (c == 'A')
			handle_arrow(0);
		else if (c == 'B')
			handle_arrow(1);
		else if (c == 'C')
			handle_arrow(2);
		else if (c == 'D')
			handle_arrow(3);
		return 1;
	}
	return 0;
}

const char *mmb_ihelp_key(char c)
{
	G.outn = 0;
	G.out[0] = 0;
	if (!H.active)
		return G.out;
	if (H.esc)
	{
		handle_esc_char(c);
		if (H.active)
			ih_draw();
		return G.out;
	}
	if (c == 27)
	{
		H.esc = 1;
		return G.out;
	}
	if (c == 3)
	{
		close_ihelp();
		return G.out;
	}
	if (c == '\r' || c == '\n')
		activate();
	else if ((unsigned char)c == 0x0c)
		tui_invalidate();
	else if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))
		jump_letter(c);
	else
		return G.out;
	if (H.active)
		ih_draw();
	return G.out;
}
