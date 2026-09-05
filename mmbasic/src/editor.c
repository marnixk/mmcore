#include "mmb_priv.h"

static char killbuf[512];
static int killlen;
static int esc_state;

static void ed_out(const char *s)
{
	G.outn = 0;
	G.out[0] = 0;
	mmb_out(s);
}

static void redraw(void)
{
	char line[200];
	int i, row, col, n;
	ed_out("\x1b[H\x1b[J");
	mmb_out("-- nano MMBasic  ^O write  ^X exit  ^R run --\r\n");
	if (G.ed.path[0])
	{
		mmb_out("File: ");
		mmb_out(G.ed.path);
		if (G.ed.dirty)
			mmb_out(" *");
		mmb_out("\r\n");
	}
	row = 0;
	col = 0;
	n = 0;
	for (i = 0; i <= G.ed.len; i++)
	{
		char c = (i < G.ed.len) ? G.ed.buf[i] : 0;
		if (row >= G.ed.row0 && row < G.ed.row0 + 20)
		{
			if (col < (int)sizeof(line) - 2 && c && c != '\n')
				line[col] = c;
		}
		if (c == '\n' || c == 0)
		{
			if (row >= G.ed.row0 && row < G.ed.row0 + 20)
			{
				line[col] = 0;
				mmb_out(line);
				mmb_out("\r\n");
			}
			row++;
			col = 0;
			if (c == 0)
				break;
		}
		else
			col++;
		(void)n;
	}
	mmb_out("^O write  ^X exit  ^R run\r\n");
}

void mmb_editor_open(const char *path)
{
	unsigned got = 0;
	memset(&G.ed, 0, sizeof(G.ed));
	esc_state = 0;
	G.ed.active = 1;
	if (path && path[0])
	{
		strncpy(G.ed.path, path, sizeof(G.ed.path) - 1);
		if (!strchr(G.ed.path, '.'))
			strncat(G.ed.path, ".BAS", sizeof(G.ed.path) - strlen(G.ed.path) - 1);
		if (mmb_vfs_read(G.ed.path, G.ed.buf, sizeof(G.ed.buf) - 1, &got) == 0)
			G.ed.len = (int)got;
	}
	G.ed.cx = G.ed.len;
	redraw();
}

static void insert_char(char c)
{
	if (G.ed.len >= (int)sizeof(G.ed.buf) - 1)
		return;
	if (G.ed.cx < G.ed.len)
		memmove(G.ed.buf + G.ed.cx + 1, G.ed.buf + G.ed.cx, (unsigned)(G.ed.len - G.ed.cx));
	G.ed.buf[G.ed.cx++] = c;
	G.ed.len++;
	G.ed.buf[G.ed.len] = 0;
	G.ed.dirty = 1;
}

static void backspace(void)
{
	if (G.ed.cx <= 0)
		return;
	memmove(G.ed.buf + G.ed.cx - 1, G.ed.buf + G.ed.cx, (unsigned)(G.ed.len - G.ed.cx + 1));
	G.ed.cx--;
	G.ed.len--;
	G.ed.dirty = 1;
}

static void pos_to_rowcol(int pos, int *row, int *col)
{
	int r = 0, c = 0, i;
	for (i = 0; i < pos && i < G.ed.len; i++)
	{
		if (G.ed.buf[i] == '\n')
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
	int r = 0, c = 0, i;
	for (i = 0; i < G.ed.len; i++)
	{
		if (r == row && c == col)
			return i;
		if (G.ed.buf[i] == '\n')
		{
			r++;
			c = 0;
		}
		else
			c++;
	}
	if (r == row && c == col)
		return i;
	return G.ed.len;
}

static int line_end(int pos)
{
	while (pos < G.ed.len && G.ed.buf[pos] != '\n')
		pos++;
	return pos;
}

static void move_left(void)
{
	if (G.ed.cx > 0)
		G.ed.cx--;
}

static void move_right(void)
{
	if (G.ed.cx < G.ed.len)
		G.ed.cx++;
}

static void move_up(void)
{
	int row, col;
	pos_to_rowcol(G.ed.cx, &row, &col);
	if (row > 0)
		G.ed.cx = rowcol_to_pos(row - 1, col);
}

static void move_down(void)
{
	int row, col;
	pos_to_rowcol(G.ed.cx, &row, &col);
	G.ed.cx = rowcol_to_pos(row + 1, col);
}

static void cut_line(void)
{
	int end = line_end(G.ed.cx);
	if (G.ed.buf[end] == '\n')
		end++;
	killlen = 0;
	if (end > G.ed.cx)
	{
		int n = end - G.ed.cx;
		if (n >= (int)sizeof(killbuf))
			n = (int)sizeof(killbuf) - 1;
		memcpy(killbuf, G.ed.buf + G.ed.cx, (unsigned)n);
		killlen = n;
		killbuf[killlen] = 0;
		memmove(G.ed.buf + G.ed.cx, G.ed.buf + end, (unsigned)(G.ed.len - end + 1));
		G.ed.len -= (end - G.ed.cx);
		G.ed.dirty = 1;
	}
}

static void paste_kill(void)
{
	int n;
	if (killlen <= 0)
		return;
	n = killlen;
	if (G.ed.len + n >= (int)sizeof(G.ed.buf) - 1)
		n = (int)sizeof(G.ed.buf) - 1 - G.ed.len;
	if (n <= 0)
		return;
	if (G.ed.cx < G.ed.len)
		memmove(G.ed.buf + G.ed.cx + n, G.ed.buf + G.ed.cx, (unsigned)(G.ed.len - G.ed.cx + 1));
	memcpy(G.ed.buf + G.ed.cx, killbuf, (unsigned)n);
	G.ed.cx += n;
	G.ed.len += n;
	G.ed.buf[G.ed.len] = 0;
	G.ed.dirty = 1;
}

static int handle_escape(char c)
{
	if (esc_state == 1)
	{
		if (c == '[')
		{
			esc_state = 2;
			return 1;
		}
		esc_state = 0;
		return 0;
	}
	if (esc_state == 2)
	{
		esc_state = 0;
		if (c == 'A')
			move_up();
		else if (c == 'B')
			move_down();
		else if (c == 'C')
			move_right();
		else if (c == 'D')
			move_left();
		else
			return 0;
		redraw();
		return 1;
	}
	return 0;
}

static void save(void)
{
	if (!G.ed.path[0])
		strcpy(G.ed.path, "UNTITLED.BAS");
	mmb_vfs_write(G.ed.path, G.ed.buf, (unsigned)G.ed.len, 0);
	G.ed.dirty = 0;
	strncpy(G.current_prog, G.ed.path, sizeof(G.current_prog) - 1);
}

const char *mmb_editor_feed(char c)
{
	G.outn = 0;
	G.out[0] = 0;
	if (esc_state)
	{
		if (handle_escape(c))
			return G.out;
	}
	if (c == 27)
	{
		esc_state = 1;
		return G.out;
	}
	if (c == 15) /* Ctrl+O write */
	{
		save();
		mmb_out("\r\n[Wrote ");
		mmb_out(G.ed.path);
		mmb_out("]\r\n");
		redraw();
		return G.out;
	}
	if (c == 24) /* Ctrl+X exit */
	{
		if (G.ed.dirty)
			save();
		G.ed.active = 0;
		ed_out("\r\n");
		return G.out;
	}
	if (c == 18) /* Ctrl+R save and run */
	{
		save();
		G.ed.active = 0;
		G.ed.run_on_exit = 1;
		{
			char cmd[160];
			strcpy(cmd, "RUN \"");
			strncat(cmd, G.ed.path, sizeof(cmd) - 8);
			strcat(cmd, "\"");
			mmb_exec_line(cmd);
		}
		return G.out;
	}
	if (c == 11) /* Ctrl+K cut line */
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
