#include "mmb_priv.h"

extern int mmb_parse_var_ref(char *name, int *nidx, int *idx);
extern void mmb_do_assign(const char *name, int type_hint, int nidx, int *idx, mmb_val val);

static void exec_statement(void);
static void run_program(void);
static int line_match(const char *body, const char *kw);
static int find_wend_pc(int from);
static int find_loop_pc(int from);
static int find_endif_pc(int from);
static int find_end_select_pc(int from);
static int find_end_sub_pc(int from);
static int at_end_of_statement(void);
static int process_line_structure(const char *body);
static int sub_find(const char *name);
static void sub_register(const char *name, int pc, int is_func);
static mmb_val read_data_item(void);
static int file_getc(int fn);
static void file_ungetc(int fn, int c);

int mmb_find_line_pc(int num)
{
	int i;
	for (i = 0; i < G.nprog; i++)
		if (G.prog_num[i] == num)
			return i;
	mmb_error("?LINE NOT FOUND");
	return 0;
}

static int line_match(const char *body, const char *kw)
{
	const char *save = G.p;
	int r;
	G.p = body;
	r = mmb_match(kw);
	G.p = save;
	return r;
}

static int find_wend_pc(int from)
{
	int depth = 1, i;
	for (i = from + 1; i < G.nprog; i++)
	{
		if (line_match(G.prog[i], "WHILE"))
			depth++;
		if (line_match(G.prog[i], "WEND"))
		{
			depth--;
			if (depth == 0)
				return i;
		}
	}
	mmb_error("?WEND");
	return G.nprog;
}

static int find_loop_pc(int from)
{
	int depth = 1, i;
	for (i = from + 1; i < G.nprog; i++)
	{
		if (line_match(G.prog[i], "DO"))
			depth++;
		if (line_match(G.prog[i], "LOOP"))
		{
			depth--;
			if (depth == 0)
				return i;
		}
	}
	mmb_error("?LOOP");
	return G.nprog;
}

static int find_endif_pc(int from)
{
	int depth = 1, i;
	for (i = from + 1; i < G.nprog; i++)
	{
		if (line_match(G.prog[i], "IF"))
		{
			const char *save = G.p;
			G.p = G.prog[i];
			mmb_match("IF");
			{
				mmb_val v = mmb_expr();
				(void)v;
				if (mmb_match("THEN") && at_end_of_statement())
					depth++;
			}
			G.p = save;
		}
		if (line_match(G.prog[i], "ENDIF"))
		{
			depth--;
			if (depth == 0)
				return i;
		}
	}
	mmb_error("?ENDIF");
	return G.nprog;
}

static int find_end_select_pc(int from)
{
	int depth = 1, i;
	for (i = from + 1; i < G.nprog; i++)
	{
		const char *save = G.p;
		G.p = G.prog[i];
		if (mmb_match("SELECT"))
			depth++;
		if (mmb_match("END") && mmb_match("SELECT"))
		{
			depth--;
			if (depth == 0)
			{
				G.p = save;
				return i;
			}
		}
		G.p = save;
	}
	mmb_error("?END SELECT");
	return G.nprog;
}

static int find_end_sub_pc(int from)
{
	int i;
	for (i = from + 1; i < G.nprog; i++)
	{
		const char *save = G.p;
		G.p = G.prog[i];
		if (mmb_match("END"))
		{
			if (mmb_match("SUB") || mmb_match("FUNCTION"))
			{
				G.p = save;
				return i;
			}
		}
		G.p = save;
	}
	mmb_error("?END SUB");
	return G.nprog;
}

static int at_end_of_statement(void)
{
	mmb_skip_sp();
	return *G.p == 0 || *G.p == ':' || *G.p == '\'';
}

static int process_line_structure(const char *body)
{
	const char *save = G.p;
	G.p = body;
	mmb_skip_sp();

	if (G.sel_skip)
	{
		if (mmb_match("CASE"))
		{
			G.p = save;
			return 0;
		}
		if (mmb_match("END") && mmb_match("SELECT"))
		{
			G.sel_skip = 0;
			G.sel_active = 0;
			G.p = save;
			return 1;
		}
		G.p = save;
		return 1;
	}

	if (G.if_skip)
	{
		if (mmb_match("ENDIF"))
		{
			G.if_skip = 0;
			G.if_taken = 0;
			G.p = save;
			return 1;
		}
		if (mmb_match("ELSEIF"))
		{
			if (!G.if_taken)
			{
				mmb_val v = mmb_expr();
				if (mmb_as_int(v))
				{
					G.if_skip = 0;
					G.if_taken = 1;
				}
			}
			G.p = save;
			return 1;
		}
		if (mmb_match("ELSE"))
		{
			if (!G.if_taken)
			{
				G.if_skip = 0;
				G.if_taken = 1;
			}
			G.p = save;
			return G.if_skip;
		}
		G.p = save;
		return 1;
	}

	if (G.if_taken)
	{
		if (mmb_match("ELSEIF") || mmb_match("ELSE"))
		{
			G.if_skip = 1;
			G.p = save;
			return 1;
		}
		if (mmb_match("ENDIF"))
		{
			G.if_taken = 0;
			G.p = save;
			return 1;
		}
	}

	G.p = save;
	return 0;
}

static int sub_find(const char *name)
{
	int i;
	char nbuf[MMB_MAX_NAME];
	strncpy(nbuf, name, MMB_MAX_NAME - 1);
	nbuf[MMB_MAX_NAME - 1] = 0;
	mmb_upper(nbuf);
	for (i = 0; i < MMB_MAX_SUBS; i++)
		if (G.subs[i].used && mmb_keyword_eq(G.subs[i].name, nbuf))
			return i;
	return -1;
}

static void sub_register(const char *name, int pc, int is_func)
{
	int i, slot = -1;
	char nbuf[MMB_MAX_NAME];
	strncpy(nbuf, name, MMB_MAX_NAME - 1);
	nbuf[MMB_MAX_NAME - 1] = 0;
	mmb_upper(nbuf);
	if (sub_find(nbuf) >= 0)
		return;
	for (i = 0; i < MMB_MAX_SUBS; i++)
	{
		if (!G.subs[i].used && slot < 0)
			slot = i;
	}
	if (slot < 0)
		mmb_error("?OUT OF MEMORY");
	strncpy(G.subs[slot].name, nbuf, MMB_MAX_NAME - 1);
	G.subs[slot].line_pc = pc;
	G.subs[slot].is_func = is_func;
	G.subs[slot].used = 1;
	G.nsubs++;
}

static int file_getc(int fn)
{
	unsigned char c;
	unsigned got = 0;
	if (fn < 1 || fn > MMB_MAX_FILES || !G.files[fn].open)
		mmb_error("?FILE");
	if (mmb_vfs_read_at(G.files[fn].path, (unsigned)G.files[fn].pos, &c, 1, &got) != 0 || !got)
		return -1;
	G.files[fn].pos++;
	return c;
}

static void file_ungetc(int fn, int c)
{
	if (fn >= 1 && fn <= MMB_MAX_FILES && G.files[fn].open && c >= 0)
	{
		if (G.files[fn].pos > 0)
			G.files[fn].pos--;
	}
}

static mmb_val read_data_item(void)
{
	int i, j;
	for (i = G.data_line; i < G.nprog; i++)
	{
		const char *save = G.p;
		const char *p = G.prog[i];
		G.p = p;
		if (!mmb_match("DATA"))
		{
			G.p = save;
			continue;
		}
		for (j = 0; j < G.data_pos; j++)
		{
			mmb_val skipv;
			mmb_skip_sp();
			skipv = mmb_expr();
			(void)skipv;
			mmb_skip_sp();
			if (*G.p == ',')
				G.p++;
		}
		mmb_skip_sp();
		if (*G.p == 0 || *G.p == ':')
		{
			G.data_line = i + 1;
			G.data_pos = 0;
			G.p = save;
			mmb_error("?OUT OF DATA");
		}
		{
			mmb_val v = mmb_expr();
			mmb_skip_sp();
			if (*G.p == ',')
				G.data_pos++;
			else
			{
				G.data_pos = 0;
				G.data_line = i + 1;
			}
			G.p = save;
			return v;
		}
	}
	mmb_error("?OUT OF DATA");
	return mmb_int_val(0);
}

void mmb_cmd_goto(void)
{
	mmb_val v = mmb_expr();
	G.branch_pc = mmb_find_line_pc((int)mmb_as_int(v));
}

void mmb_cmd_gosub(void)
{
	mmb_val v = mmb_expr();
	if (G.gosub_sp >= MMB_MAX_GOSUB)
		mmb_error("?GOSUB");
	G.gosub_stack[G.gosub_sp++] = G.run_pc + 1;
	G.branch_pc = mmb_find_line_pc((int)mmb_as_int(v));
}

void mmb_cmd_return(void)
{
	if (G.gosub_sp <= 0)
		mmb_error("?RETURN WITHOUT GOSUB");
	G.branch_pc = G.gosub_stack[--G.gosub_sp];
}

void mmb_cmd_while(void)
{
	mmb_val v = mmb_expr();
	if (mmb_as_int(v))
	{
		if (G.ctrl_sp >= MMB_MAX_CTRL)
			mmb_error("?WHILE");
		G.ctrlstack[G.ctrl_sp].type = 1;
		G.ctrlstack[G.ctrl_sp].line_pc = G.run_pc;
		G.ctrl_sp++;
	}
	else
		G.branch_pc = find_wend_pc(G.run_pc) + 1;
}

void mmb_cmd_wend(void)
{
	int pc;
	if (G.ctrl_sp <= 0 || G.ctrlstack[G.ctrl_sp - 1].type != 1)
		mmb_error("?WEND WITHOUT WHILE");
	pc = G.ctrlstack[G.ctrl_sp - 1].line_pc;
	G.branch_pc = pc;
}

void mmb_cmd_do(void)
{
	int cond = 1;
	mmb_skip_sp();
	if (mmb_match("WHILE"))
	{
		mmb_val v = mmb_expr();
		cond = mmb_as_int(v) != 0;
	}
	else if (mmb_match("UNTIL"))
	{
		mmb_val v = mmb_expr();
		cond = mmb_as_int(v) == 0;
	}
	if (G.ctrl_sp >= MMB_MAX_CTRL)
		mmb_error("?DO");
	G.ctrlstack[G.ctrl_sp].type = 2;
	G.ctrlstack[G.ctrl_sp].line_pc = G.run_pc;
	G.ctrlstack[G.ctrl_sp].skip = cond ? 0 : 1;
	G.ctrl_sp++;
	if (!cond)
		G.branch_pc = find_loop_pc(G.run_pc) + 1;
}

void mmb_cmd_loop(void)
{
	int pc, cond = 1;
	if (G.ctrl_sp <= 0 || G.ctrlstack[G.ctrl_sp - 1].type != 2)
		mmb_error("?LOOP WITHOUT DO");
	pc = G.ctrlstack[G.ctrl_sp - 1].line_pc;
	mmb_skip_sp();
	if (mmb_match("WHILE"))
	{
		mmb_val v = mmb_expr();
		cond = mmb_as_int(v) != 0;
	}
	else if (mmb_match("UNTIL"))
	{
		mmb_val v = mmb_expr();
		cond = mmb_as_int(v) == 0;
	}
	else if (*G.p && *G.p != ':' && *G.p != '\'')
		mmb_syntax();
	if (cond)
		G.branch_pc = pc;
	else
		G.ctrl_sp--;
}

void mmb_cmd_exit_do(void)
{
	int i;
	if (G.ctrl_sp <= 0)
		mmb_error("?EXIT DO");
	for (i = G.ctrl_sp - 1; i >= 0; i--)
	{
		if (G.ctrlstack[i].type == 2)
		{
			G.branch_pc = find_loop_pc(G.ctrlstack[i].line_pc) + 1;
			G.ctrl_sp = i;
			return;
		}
	}
	mmb_error("?EXIT DO");
}

void mmb_cmd_select(void)
{
	mmb_skip_sp();
	if (!mmb_match("CASE"))
		mmb_syntax();
	G.sel_val = mmb_expr();
	G.sel_active = 0;
	G.sel_skip = 1;
}

static int case_matches(mmb_val sel)
{
	mmb_val v;
	int matched = 0;
	for (;;)
	{
		mmb_skip_sp();
		if (mmb_match("IS"))
		{
			int op = 0;
			mmb_skip_sp();
			if (G.p[0] == '<' && G.p[1] == '>')
			{
				op = 2;
				G.p += 2;
			}
			else if (G.p[0] == '<' && G.p[1] == '=')
			{
				op = 5;
				G.p += 2;
			}
			else if (G.p[0] == '>' && G.p[1] == '=')
			{
				op = 6;
				G.p += 2;
			}
			else if (*G.p == '=')
			{
				op = 1;
				G.p++;
			}
			else if (*G.p == '<')
			{
				op = 3;
				G.p++;
			}
			else if (*G.p == '>')
			{
				op = 4;
				G.p++;
			}
			else
				mmb_syntax();
			v = mmb_expr();
			{
				double x = mmb_as_float(sel), y = mmb_as_float(v);
				if (op == 1) matched = x == y;
				else if (op == 2) matched = x != y;
				else if (op == 3) matched = x < y;
				else if (op == 4) matched = x > y;
				else if (op == 5) matched = x <= y;
				else matched = x >= y;
			}
			return matched;
		}
		if (*G.p == 0 || *G.p == ':' || *G.p == '\'')
			break;
		v = mmb_expr();
		if (sel.type == T_STR || v.type == T_STR)
		{
			if (sel.type == T_STR && v.type == T_STR && strcmp(sel.s, v.s) == 0)
				matched = 1;
		}
		else if (mmb_as_float(sel) == mmb_as_float(v))
			matched = 1;
		mmb_skip_sp();
		if (*G.p == ',')
		{
			G.p++;
			continue;
		}
		break;
	}
	return matched;
}

void mmb_cmd_case(void)
{
	mmb_skip_sp();
	if (mmb_match("ELSE"))
	{
		if (!G.sel_active)
		{
			G.sel_skip = 0;
			G.sel_active = 1;
		}
		else
			G.sel_skip = 1;
		return;
	}
	if (G.sel_active)
	{
		G.sel_skip = 1;
		return;
	}
	if (case_matches(G.sel_val))
	{
		G.sel_skip = 0;
		G.sel_active = 1;
	}
}

void mmb_cmd_end_select(void)
{
	G.sel_active = 0;
	G.sel_skip = 0;
}

void mmb_cmd_endif(void)
{
	G.if_skip = 0;
	G.if_taken = 0;
}

void mmb_cmd_data(void)
{
	while (*G.p && *G.p != ':')
		G.p++;
}

void mmb_cmd_read(void)
{
	for (;;)
	{
		char name[MMB_MAX_NAME];
		int nidx, idx[MMB_MAX_DIMS], t;
		mmb_val v;
		mmb_skip_sp();
		if (*G.p == 0 || *G.p == ':' || *G.p == '\'')
			break;
		t = mmb_parse_var_ref(name, &nidx, idx);
		v = read_data_item();
		mmb_do_assign(name, t, nidx, idx, v);
		mmb_skip_sp();
		if (*G.p == ',')
		{
			G.p++;
			continue;
		}
		break;
	}
}

void mmb_cmd_restore(void)
{
	mmb_skip_sp();
	if (*G.p && *G.p != ':' && *G.p != '\'')
	{
		mmb_val v = mmb_expr();
		{
			int pc = mmb_find_line_pc((int)mmb_as_int(v));
			int i;
			G.data_line = pc;
			G.data_pos = 0;
			for (i = pc; i < G.nprog; i++)
			{
				if (line_match(G.prog[i], "DATA"))
					return;
			}
			mmb_error("?NO DATA");
		}
	}
	else
	{
		G.data_line = 0;
		G.data_pos = 0;
	}
}

void mmb_cmd_const(void)
{
	char name[MMB_MAX_NAME];
	int t;
	mmb_val v;
	mmb_ident(name, sizeof(name));
	t = mmb_type_suffix(name);
	mmb_skip_sp();
	mmb_expect('=');
	v = mmb_expr();
	mmb_const_define(name, t, v);
}

void mmb_cmd_save(void)
{
	int i;
	char buf[MMB_LINE_LEN + 32];
	mmb_val v = mmb_expr();
	char fname[128];
	unsigned total = 0;
	unsigned char *out;
	unsigned pos = 0;
	if (v.type != T_STR)
		mmb_syntax();
	strncpy(fname, v.s, sizeof(fname) - 1);
	fname[sizeof(fname) - 1] = 0;
	if (!strchr(fname, '.'))
		strncat(fname, ".BAS", sizeof(fname) - strlen(fname) - 1);
	for (i = 0; i < G.nprog; i++)
		total += 16 + (unsigned)strlen(G.prog[i]) + 2;
	out = G.plat->alloc(total + 1);
	if (!out)
		mmb_error("?OUT OF MEMORY");
	for (i = 0; i < G.nprog; i++)
	{
		int n = 0, j;
		char nb[16];
		int64_t num = G.prog_num[i];
		char *p = nb + sizeof(nb) - 1;
		*p = 0;
		if (num == 0)
			*--p = '0';
		while (num)
		{
			*--p = (char)('0' + (num % 10));
			num /= 10;
		}
		while (*p)
			buf[n++] = *p++;
		buf[n++] = ' ';
		for (j = 0; G.prog[i][j]; j++)
			buf[n++] = G.prog[i][j];
		buf[n++] = '\n';
		memcpy(out + pos, buf, (unsigned)n);
		pos += (unsigned)n;
	}
	out[pos] = 0;
	mmb_vfs_write(fname, out, pos, 0);
	G.plat->free(out);
}

void mmb_cmd_seek(void)
{
	int fn;
	mmb_val v;
	mmb_skip_sp();
	if (*G.p != '#')
		mmb_syntax();
	G.p++;
	fn = (int)mmb_as_int(mmb_expr());
	mmb_skip_sp();
	if (*G.p == ',')
		G.p++;
	v = mmb_expr();
	if (fn < 1 || fn > MMB_MAX_FILES || !G.files[fn].open)
		mmb_error("?FILE");
	G.files[fn].pos = (int)mmb_as_int(v);
}

static void input_to_var(int fn)
{
	char name[MMB_MAX_NAME];
	int nidx, idx[MMB_MAX_DIMS], t;
	char buf[MMB_MAX_STR + 1];
	int n = 0, c;
	mmb_val v;
	t = mmb_parse_var_ref(name, &nidx, idx);
	if (t == T_STR || name[strlen(name) - 1] == '$')
	{
		for (;;)
		{
			c = file_getc(fn);
			if (c < 0 || c == '\n' || c == '\r' || c == ',')
				break;
			if (n < MMB_MAX_STR)
				buf[n++] = (char)c;
		}
		if (c == '\r')
		{
			int c2 = file_getc(fn);
			if (c2 != '\n')
				file_ungetc(fn, c2);
		}
		buf[n] = 0;
		v = mmb_str_val(buf);
	}
	else
	{
		int neg = 0, dot = 0;
		double f = 0;
		double frac = 0.1;
		for (;;)
		{
			c = file_getc(fn);
			if (c < 0 || c == '\n' || c == '\r' || c == ',')
				break;
			if (c == '-')
				neg = 1;
			else if (c == '.')
				dot = 1;
			else if (c >= '0' && c <= '9')
			{
				if (dot)
				{
					f += (c - '0') * frac;
					frac *= 0.1;
				}
				else
					f = f * 10 + (c - '0');
			}
		}
		if (c == '\r')
		{
			int c2 = file_getc(fn);
			if (c2 != '\n')
				file_ungetc(fn, c2);
		}
		if (neg)
			f = -f;
		v = dot ? mmb_num_val(f) : mmb_int_val((int64_t)f);
	}
	mmb_do_assign(name, t, nidx, idx, v);
}

void mmb_cmd_input(void)
{
	int fn = 0;
	mmb_skip_sp();
	if (*G.p == '#')
	{
		G.p++;
		fn = (int)mmb_as_int(mmb_expr());
		mmb_skip_sp();
		if (*G.p == ',')
			G.p++;
	}
	for (;;)
	{
		mmb_skip_sp();
		if (*G.p == 0 || *G.p == ':' || *G.p == '\'')
			break;
		if (fn)
			input_to_var(fn);
		else
		{
			char name[MMB_MAX_NAME];
			int nidx, idx[MMB_MAX_DIMS], t;
			mmb_val v;
			t = mmb_parse_var_ref(name, &nidx, idx);
			(void)t;
			v = mmb_str_val("");
			mmb_do_assign(name, t, nidx, idx, v);
		}
		mmb_skip_sp();
		if (*G.p == ',')
		{
			G.p++;
			continue;
		}
		break;
	}
}

void mmb_cmd_line_input(void)
{
	int fn;
	char name[MMB_MAX_NAME];
	int nidx, idx[MMB_MAX_DIMS], t;
	char buf[MMB_MAX_STR + 1];
	int n = 0, c;
	mmb_val v;
	mmb_skip_sp();
	if (*G.p != '#')
		mmb_syntax();
	G.p++;
	fn = (int)mmb_as_int(mmb_expr());
	mmb_skip_sp();
	if (*G.p == ',')
		G.p++;
	t = mmb_parse_var_ref(name, &nidx, idx);
	if (t != T_STR && name[strlen(name) - 1] != '$')
		mmb_error("?TYPE MISMATCH");
	for (;;)
	{
		c = file_getc(fn);
		if (c < 0 || c == '\n' || c == '\r')
			break;
		if (n < MMB_MAX_STR)
			buf[n++] = (char)c;
	}
	if (c == '\r')
	{
		int c2 = file_getc(fn);
		if (c2 != '\n')
			file_ungetc(fn, c2);
	}
	buf[n] = 0;
	v = mmb_str_val(buf);
	mmb_do_assign(name, t ? t : T_STR, nidx, idx, v);
}

void mmb_cmd_call(void)
{
	char name[MMB_MAX_NAME];
	int si;
	mmb_ident(name, sizeof(name));
	mmb_type_suffix(name);
	si = sub_find(name);
	if (si < 0)
		mmb_error("?SUB NOT FOUND");
	if (G.gosub_sp >= MMB_MAX_GOSUB)
		mmb_error("?GOSUB");
	G.gosub_stack[G.gosub_sp++] = G.run_pc + 1;
	G.branch_pc = G.subs[si].line_pc;
}

void mmb_cmd_sub(void)
{
	char name[MMB_MAX_NAME];
	int end_pc;
	mmb_ident(name, sizeof(name));
	mmb_type_suffix(name);
	sub_register(name, G.run_pc + 1, 0);
	end_pc = find_end_sub_pc(G.run_pc);
	G.in_sub = 1;
	G.branch_pc = end_pc + 1;
}

void mmb_cmd_function(void)
{
	char name[MMB_MAX_NAME];
	int end_pc;
	mmb_ident(name, sizeof(name));
	mmb_type_suffix(name);
	sub_register(name, G.run_pc + 1, 1);
	end_pc = find_end_sub_pc(G.run_pc);
	G.in_sub = 1;
	G.branch_pc = end_pc + 1;
}

void mmb_cmd_end_sub(void)
{
	if (G.gosub_sp > 0)
		G.branch_pc = G.gosub_stack[--G.gosub_sp];
	else
		G.branch_pc = G.nprog;
	G.in_sub = 0;
}

void mmb_cmd_end_function(void)
{
	mmb_cmd_end_sub();
}
void mmb_option_reset(void)
{
	memset(&G.opt, 0, sizeof(G.opt));
	G.opt.base = 0;
	G.opt.default_type = T_NUM;
	G.opt.tab = 2;
	G.opt.break_key = 3;
	G.opt.colourcode = 1;
	G.opt.console = 3;
	G.opt.console_port = 3;
	G.opt.crlf = 2;
	G.opt.default_mode = 1;
	G.opt.baudrate = 115200;
	G.opt.status = 1;
	G.opt.vcc = 3.3;
	G.opt.repeat_first = 600;
	G.opt.repeat_next = 150;
	G.opt.edit_font = 1;
	G.opt.mouse_sens = 1;
}

static int starts_with_line_number(const char *s, int *num, const char **rest)
{
	int n = 0;
	const char *p = s;
	while (*p == ' ')
		p++;
	if (*p < '0' || *p > '9')
		return 0;
	while (*p >= '0' && *p <= '9')
		n = n * 10 + (*p++ - '0');
	if (*p != ' ' && *p != '\t' && *p != 0)
		return 0;
	*num = n;
	while (*p == ' ' || *p == '\t')
		p++;
	*rest = p;
	return 1;
}

static void store_line(int num, const char *text)
{
	int i, j;
	if (text[0] == 0)
	{
		for (i = 0; i < G.nprog; i++)
			if (G.prog_num[i] == num)
			{
				for (j = i; j < G.nprog - 1; j++)
				{
					memcpy(G.prog[j], G.prog[j + 1], MMB_LINE_LEN);
					G.prog_num[j] = G.prog_num[j + 1];
				}
				G.nprog--;
				return;
			}
		return;
	}
	for (i = 0; i < G.nprog; i++)
		if (G.prog_num[i] == num)
		{
			strncpy(G.prog[i], text, MMB_LINE_LEN - 1);
			G.prog[i][MMB_LINE_LEN - 1] = 0;
			return;
		}
	if (G.nprog >= MMB_MAX_LINES)
		mmb_error("?PROGRAM FULL");
	for (i = 0; i < G.nprog; i++)
		if (G.prog_num[i] > num)
			break;
	for (j = G.nprog; j > i; j--)
	{
		memcpy(G.prog[j], G.prog[j - 1], MMB_LINE_LEN);
		G.prog_num[j] = G.prog_num[j - 1];
	}
	strncpy(G.prog[i], text, MMB_LINE_LEN - 1);
	G.prog[i][MMB_LINE_LEN - 1] = 0;
	G.prog_num[i] = num;
	G.nprog++;
}

void mmb_cmd_print(void)
{
	int first = 1;
	int no_nl = 0;
	mmb_skip_sp();
	if (*G.p == 0 || *G.p == '\'' || *G.p == ':')
	{
		mmb_out("\n");
		return;
	}
	while (*G.p && *G.p != ':' && *G.p != '\'')
	{
		mmb_val v;
		mmb_skip_sp();
		if (*G.p == '#' )
		{
			/* PRINT #fn, ... */
			int fn;
			G.p++;
			v = mmb_expr();
			fn = (int)mmb_as_int(v);
			mmb_skip_sp();
			if (*G.p == ',')
				G.p++;
			{
				char buf[MMB_OUT_LEN];
				int save = G.outn;
				G.outn = 0;
				G.out[0] = 0;
				no_nl = 0;
				while (*G.p && *G.p != ':' && *G.p != '\'')
				{
					v = mmb_expr();
					mmb_print_val(v);
					mmb_skip_sp();
					if (*G.p == ';' )
					{
						G.p++;
						no_nl = 1;
						continue;
					}
					if (*G.p == ',')
					{
						mmb_out(" ");
						G.p++;
						no_nl = 0;
						continue;
					}
					no_nl = 0;
					break;
				}
				if (!no_nl)
					mmb_out("\n");
				strncpy(buf, G.out, sizeof(buf) - 1);
				if (fn >= 1 && fn <= MMB_MAX_FILES && G.files[fn].open)
					mmb_vfs_write(G.files[fn].path, buf, (unsigned)strlen(buf), 1);
				G.outn = save;
				G.out[G.outn] = 0;
				return;
			}
		}
		if (!first)
			/* keep as-is */;
		first = 0;
		v = mmb_expr();
		mmb_print_val(v);
		mmb_skip_sp();
		if (*G.p == ';')
		{
			G.p++;
			no_nl = 1;
			continue;
		}
		if (*G.p == ',')
		{
			mmb_out(" ");
			G.p++;
			no_nl = 0;
			continue;
		}
		no_nl = 0;
		break;
	}
	if (!no_nl)
		mmb_out("\n");
}

static void do_let(void)
{
	char name[MMB_MAX_NAME];
	int nidx = 0, idx[MMB_MAX_DIMS], t;
	mmb_val v;
	t = mmb_parse_var_ref(name, &nidx, idx);
	mmb_skip_sp();
	mmb_expect('=');
	v = mmb_expr();
	mmb_do_assign(name, t, nidx, idx, v);
}

void mmb_cmd_new(void)
{
	G.nprog = 0;
	G.current_prog[0] = 0;
	mmb_clear_vars(1);
	mmb_clear_consts();
	memset(G.subs, 0, sizeof(G.subs));
	G.nsubs = 0;
	G.opt.autorun = 0;
}

void mmb_cmd_list(void)
{
	int i;
	char nb[16];
	for (i = 0; i < G.nprog; i++)
	{
		if (i)
			mmb_out("\n");
		if (G.prog_num[i])
		{
			/* print number then space then text */
			int64_t n = G.prog_num[i];
			mmb_outf(0, n);
			mmb_out(" ");
			(void)nb;
		}
		mmb_out(G.prog[i]);
	}
}

void mmb_cmd_end(void)
{
	G.running = 0;
}

void mmb_cmd_run(void)
{
	char fname[128];
	mmb_skip_sp();
	if (*G.p && *G.p != ':' && *G.p != '\'')
	{
		if (*G.p == '"')
		{
			mmb_val v = mmb_expr();
			if (v.type != T_STR)
				mmb_syntax();
			strncpy(fname, v.s, sizeof(fname) - 1);
		}
		else
		{
			int n = 0;
			while (*G.p && *G.p != ' ' && n < 126)
				fname[n++] = *G.p++;
			fname[n] = 0;
		}
		if (!strchr(fname, '.'))
			strncat(fname, ".BAS", sizeof(fname) - strlen(fname) - 1);
		{
			char buf[8192];
			unsigned got = 0;
			if (mmb_vfs_read(fname, buf, sizeof(buf) - 1, &got) != 0)
				mmb_error("?FILE NOT FOUND");
			buf[got] = 0;
			strncpy(G.current_prog, fname, sizeof(G.current_prog) - 1);
			G.nprog = 0;
			{
				char *s = buf;
				int auto_n = 10;
				while (*s)
				{
					char line[MMB_LINE_LEN];
					int li = 0, num = 0;
					const char *rest;
					while (*s && *s != '\n' && *s != '\r' && li < MMB_LINE_LEN - 1)
						line[li++] = *s++;
					line[li] = 0;
					while (*s == '\n' || *s == '\r')
						s++;
					if (starts_with_line_number(line, &num, &rest))
						store_line(num, rest);
					else if (line[0])
						store_line(auto_n, line);
					auto_n += 10;
				}
			}
		}
	}
	run_program();
}

void mmb_cmd_if(void)
{
	mmb_val v = mmb_expr();
	int cond = mmb_as_int(v) != 0;
	if (!mmb_match("THEN"))
		mmb_syntax();
	mmb_skip_sp();
	if (at_end_of_statement())
	{
		G.if_taken = cond;
		G.if_skip = cond ? 0 : 1;
		return;
	}
	if (cond)
		exec_statement();
	else
	{
		while (*G.p && *G.p != ':')
		{
			if (mmb_match("ELSEIF"))
			{
				v = mmb_expr();
				if (!mmb_match("THEN"))
					mmb_syntax();
				mmb_skip_sp();
				if (mmb_as_int(v))
				{
					exec_statement();
					return;
				}
			}
			else if (mmb_match("ELSE"))
			{
				mmb_skip_sp();
				exec_statement();
				return;
			}
			G.p++;
		}
	}
}

void mmb_cmd_for(void)
{
	char name[MMB_MAX_NAME];
	int nidx, idx[MMB_MAX_DIMS], t;
	mmb_val from, to, step;
	t = mmb_parse_var_ref(name, &nidx, idx);
	mmb_expect('=');
	from = mmb_expr();
	if (!mmb_match("TO"))
		mmb_syntax();
	to = mmb_expr();
	step = mmb_int_val(1);
	if (mmb_match("STEP"))
		step = mmb_expr();
	mmb_do_assign(name, t ? t : T_NUM, nidx, idx, from);
	if (G.for_sp >= 16)
		mmb_error("?FOR");
	strncpy(G.forstack[G.for_sp].var, name, MMB_MAX_NAME - 1);
	G.forstack[G.for_sp].to = mmb_as_int(to);
	G.forstack[G.for_sp].step = mmb_as_int(step);
	G.forstack[G.for_sp].line = G.run_pc + 1;
	G.for_sp++;
}

void mmb_cmd_next(void)
{
	char name[MMB_MAX_NAME];
	int off = 0;
	mmb_var *v;
	mmb_skip_sp();
	if (mmb_is_ident(*G.p))
		mmb_ident(name, sizeof(name));
	else if (G.for_sp > 0)
		strncpy(name, G.forstack[G.for_sp - 1].var, sizeof(name) - 1);
	else
		mmb_syntax();
	mmb_type_suffix(name);
	if (G.for_sp <= 0)
		mmb_error("?NEXT WITHOUT FOR");
	v = mmb_find_var(name, 0, 0, 0, &off);
	if (!v)
		mmb_error("?NEXT WITHOUT FOR");
	{
		int64_t cur = v->type == T_INT ? v->data.i[0] : (int64_t)v->data.f[0];
		int64_t step = G.forstack[G.for_sp - 1].step;
		int64_t to = G.forstack[G.for_sp - 1].to;
		cur += step;
		if (v->type == T_INT)
			v->data.i[0] = cur;
		else
			v->data.f[0] = (double)cur;
		if ((step >= 0 && cur <= to) || (step < 0 && cur >= to))
		{
			/* continue loop: signal by leaving for_sp and setting a flag via line=-2 handled in run */
			G.forstack[G.for_sp - 1].stmt = 1; /* still looping */
		}
		else
		{
			G.for_sp--;
			G.forstack[G.for_sp].stmt = 0;
		}
	}
}

void mmb_cmd_pause(void)
{
	/* busy-wait using platform millis if available */
	mmb_val v = mmb_expr();
	unsigned ms = (unsigned)mmb_as_int(v);
	unsigned start = mmb_now_ms();
	if (G.plat && G.plat->millis)
		while ((mmb_now_ms() - start) < ms)
			mmb_poll();
	(void)ms;
}

static int is_assign_start(void)
{
	const char *save = G.p;
	char name[MMB_MAX_NAME];
	int nidx, idx[MMB_MAX_DIMS];
	if (!((G.p[0] >= 'A' && G.p[0] <= 'Z') || (G.p[0] >= 'a' && G.p[0] <= 'z') || G.p[0] == '_'))
		return 0;
	mmb_parse_var_ref(name, &nidx, idx);
	mmb_skip_sp();
	if (*G.p == '=')
	{
		G.p = save;
		return 1;
	}
	G.p = save;
	return 0;
}

static void exec_statement(void)
{
	mmb_skip_sp();
	if (*G.p == 0 || *G.p == '\'')
		return;
	if (mmb_match("REM"))
	{
		while (*G.p)
			G.p++;
		return;
	}
	if (mmb_match("LET"))
	{
		do_let();
		return;
	}
	if (is_assign_start())
	{
		do_let();
		return;
	}
	if (mmb_match("HELP"))
	{
		mmb_cmd_help();
		return;
	}
	if (mmb_match("PRINT") || mmb_match("?"))
	{
		mmb_cmd_print();
		return;
	}
	if (mmb_match("DIM"))
	{
		mmb_cmd_dim();
		return;
	}
	if (mmb_match("CLEAR"))
	{
		mmb_cmd_clear();
		return;
	}
	if (mmb_match("NEW"))
	{
		mmb_cmd_new();
		return;
	}
	if (mmb_match("LIST"))
	{
		mmb_cmd_list();
		return;
	}
	if (mmb_match("RUN"))
	{
		mmb_cmd_run();
		return;
	}
	if (mmb_match("END"))
	{
		if (mmb_match("IF"))
		{
			mmb_cmd_endif();
			return;
		}
		if (mmb_match("SELECT"))
		{
			mmb_cmd_end_select();
			return;
		}
		if (mmb_match("SUB"))
		{
			mmb_cmd_end_sub();
			return;
		}
		if (mmb_match("FUNCTION"))
		{
			mmb_cmd_end_function();
			return;
		}
		mmb_cmd_end();
		return;
	}
	if (mmb_match("GOTO"))
	{
		mmb_cmd_goto();
		return;
	}
	if (mmb_match("GOSUB"))
	{
		mmb_cmd_gosub();
		return;
	}
	if (mmb_match("RETURN"))
	{
		mmb_cmd_return();
		return;
	}
	if (mmb_match("WHILE"))
	{
		mmb_cmd_while();
		return;
	}
	if (mmb_match("WEND"))
	{
		mmb_cmd_wend();
		return;
	}
	{
		const char *save = G.p;
		if (mmb_match("EXIT") && mmb_match("DO"))
		{
			mmb_cmd_exit_do();
			return;
		}
		G.p = save;
	}
	if (mmb_match("DO"))
	{
		mmb_cmd_do();
		return;
	}
	if (mmb_match("LOOP"))
	{
		mmb_cmd_loop();
		return;
	}
	if (mmb_match("SELECT"))
	{
		mmb_cmd_select();
		return;
	}
	if (mmb_match("CASE"))
	{
		mmb_cmd_case();
		return;
	}
	if (mmb_match("DATA"))
	{
		mmb_cmd_data();
		return;
	}
	if (mmb_match("READ"))
	{
		mmb_cmd_read();
		return;
	}
	if (mmb_match("RESTORE"))
	{
		mmb_cmd_restore();
		return;
	}
	if (mmb_match("CONST"))
	{
		mmb_cmd_const();
		return;
	}
	if (mmb_match("SAVE"))
	{
		mmb_cmd_save();
		return;
	}
	if (mmb_match("SEEK"))
	{
		mmb_cmd_seek();
		return;
	}
	if (mmb_match("INPUT"))
	{
		mmb_cmd_input();
		return;
	}
	if (mmb_match("CALL"))
	{
		mmb_cmd_call();
		return;
	}
	if (mmb_match("SUB"))
	{
		mmb_cmd_sub();
		return;
	}
	if (mmb_match("FUNCTION"))
	{
		mmb_cmd_function();
		return;
	}
	if (mmb_match("IF"))
	{
		mmb_cmd_if();
		return;
	}
	if (mmb_match("FOR"))
	{
		mmb_cmd_for();
		return;
	}
	if (mmb_match("NEXT"))
	{
		mmb_cmd_next();
		return;
	}
	if (mmb_match("OPTION") || mmb_match("OPTIONS"))
	{
		mmb_cmd_option();
		return;
	}
	if (mmb_match("FACTORY_RESET") || (mmb_match("FACTORY") && mmb_match("RESET")))
	{
		mmb_cmd_factory_reset();
		return;
	}
	if (mmb_match("CLS"))
	{
		mmb_cmd_cls();
		return;
	}
	if (mmb_match("PIXEL"))
	{
		mmb_cmd_pixel();
		return;
	}
	if (mmb_match("LINE"))
	{
		if (mmb_match("INPUT"))
			mmb_cmd_line_input();
		else
			mmb_cmd_line();
		return;
	}
	if (mmb_match("BOX"))
	{
		mmb_cmd_box();
		return;
	}
	if (mmb_match("CIRCLE"))
	{
		mmb_cmd_circle();
		return;
	}
	if (mmb_match("RBOX"))
	{
		mmb_cmd_rbox();
		return;
	}
	if (mmb_match("ARC"))
	{
		mmb_cmd_arc();
		return;
	}
	if (mmb_match("TRIANGLE"))
	{
		mmb_cmd_triangle();
		return;
	}
	if (mmb_match("POLYGON"))
	{
		mmb_cmd_polygon();
		return;
	}
	if (mmb_match("TEXT"))
	{
		mmb_cmd_text();
		return;
	}
	if (mmb_match("FONT"))
	{
		mmb_cmd_font();
		return;
	}
	if (mmb_match("COLOUR") || mmb_match("COLOR"))
	{
		mmb_cmd_colour();
		return;
	}
	if (mmb_match("MODE"))
	{
		mmb_cmd_mode();
		return;
	}
	if (mmb_match("PAGE"))
	{
		mmb_cmd_page();
		return;
	}
	if (mmb_match("BLIT"))
	{
		mmb_cmd_blit();
		return;
	}
	if (mmb_match("CHDIR"))
	{
		mmb_cmd_chdir();
		return;
	}
	if (mmb_match("DRIVE"))
	{
		mmb_cmd_drive();
		return;
	}
	if (mmb_match("MKDIR"))
	{
		mmb_cmd_mkdir();
		return;
	}
	if (mmb_match("RMDIR"))
	{
		mmb_cmd_rmdir();
		return;
	}
	if (mmb_match("KILL"))
	{
		mmb_cmd_kill();
		return;
	}
	if (mmb_match("COPY"))
	{
		mmb_cmd_copy();
		return;
	}
	if (mmb_match("RENAME") || mmb_match("NAME"))
	{
		mmb_cmd_name();
		return;
	}
	if (mmb_match("DIR"))
	{
		mmb_cmd_files("DIR");
		return;
	}
	if (mmb_match("FILES"))
	{
		mmb_cmd_files_ui();
		return;
	}
	if (mmb_match("OPEN"))
	{
		mmb_cmd_open();
		return;
	}
	if (mmb_match("CLOSE"))
	{
		mmb_cmd_close();
		return;
	}
	if (mmb_match("PLAY"))
	{
		mmb_cmd_play();
		return;
	}
	if (mmb_match("LOAD"))
	{
		mmb_cmd_load();
		return;
	}
	if (mmb_match("EDIT"))
	{
		mmb_cmd_edit();
		return;
	}
	if (mmb_match("PAUSE"))
	{
		mmb_cmd_pause();
		return;
	}
	if (mmb_match("ERASE"))
	{
		mmb_cmd_clear();
		return;
	}
	mmb_syntax();
}

static void exec_line_body(const char *body)
{
	if (process_line_structure(body))
		return;
	if (G.if_skip || G.sel_skip)
		return;
	G.p = body;
	for (;;)
	{
		exec_statement();
		if (G.branch_pc >= 0)
			return;
		mmb_skip_sp();
		if (*G.p == ':')
		{
			G.p++;
			continue;
		}
		if (*G.p == '\'' || *G.p == 0)
			break;
		if (*G.p)
			mmb_syntax();
		break;
	}
}

static void run_program(void)
{
	int pc = 0;
	G.running = 1;
	G.for_sp = 0;
	G.gosub_sp = 0;
	G.ctrl_sp = 0;
	G.branch_pc = -1;
	G.if_skip = 0;
	G.if_taken = 0;
	G.sel_active = 0;
	G.sel_skip = 0;
	G.in_sub = 0;
	G.data_line = 0;
	G.data_pos = 0;
	memset(G.subs, 0, sizeof(G.subs));
	G.nsubs = 0;
	mmb_clear_vars(1);
	G.opt.explicit = 0;
	G.opt.default_type = T_NUM;
	G.opt.base = 0;
	G.opt.angle_degrees = 0;
	while (pc < G.nprog && G.running)
	{
		int loop;
		do
		{
			loop = 0;
			G.run_pc = pc;
			G.branch_pc = -1;
			exec_line_body(G.prog[pc]);
			if (G.branch_pc >= 0)
			{
				pc = G.branch_pc;
				break;
			}
			if (G.for_sp > 0 && G.forstack[G.for_sp - 1].stmt == 1)
			{
				G.forstack[G.for_sp - 1].stmt = 0;
				pc = G.forstack[G.for_sp - 1].line;
				loop = 1;
			}
		} while (loop && G.running);
		if (G.branch_pc >= 0)
			continue;
		pc++;
	}
	G.running = 0;
}

const char *mmb_exec_line(const char *line)
{
	int num;
	const char *rest;
	G.outn = 0;
	G.out[0] = 0;
	G.err[0] = 0;
	if (setjmp(G.errjmp))
	{
		G.outn = 0;
		G.out[0] = 0;
		mmb_out(G.err[0] ? G.err : "?SYNTAX ERROR");
		return G.out;
	}
	if (!line)
		return G.out;
	while (*line == ' ' || *line == '\t')
		line++;
	if (*line == 0)
		return G.out;
	if (starts_with_line_number(line, &num, &rest))
	{
		store_line(num, rest);
		return G.out;
	}
	exec_line_body(line);
	return G.out;
}

int mmb_in_editor(void)
{
	return G.ed.active;
}

int mmb_take_home_prompt(void)
{
	int v = G.home_prompt;
	G.home_prompt = 0;
	return v;
}

const char *mmb_editor_key(char c)
{
	return mmb_editor_feed(c);
}

void mmb_init(const mmb_platform *plat)
{
	memset(&G, 0, sizeof(G));
	G.plat = plat;
	mmb_option_reset();
	mmb_vfs_init();
	mmb_gfx_init();
	mmb_assets_seed();
	mmb_settings_load();
}

void mmb_reset(void)
{
	mmb_clear_vars(0);
	mmb_option_reset();
	mmb_play_stop();
	G.nprog = 0;
	mmb_gfx_init();
}

void mmb_poll(void)
{
	mmb_storage_poll();
}
