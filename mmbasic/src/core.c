#include "mmb_priv.h"

extern int mmb_parse_var_ref(char *name, int *nidx, int *idx);
extern void mmb_do_assign(const char *name, int type_hint, int nidx, int *idx, mmb_val val);
extern mmb_val mmb_load_var(mmb_var *v, int off);

static void exec_statement(void);
static void gosub_restore_top(void);
static void exec_line_body(const char *body);
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

static const char *after_label(const char *body)
{
	const char *save = G.p;
	const char *result = body;
	char name[MMB_MAX_NAME];
	G.p = body;
	mmb_skip_sp();
	if ((*G.p >= 'A' && *G.p <= 'Z') || (*G.p >= 'a' && *G.p <= 'z') || *G.p == '_')
	{
		mmb_ident(name, sizeof(name));
		mmb_skip_sp();
		if (*G.p == ':')
		{
			G.p++;
			result = G.p;
		}
	}
	G.p = save;
	return result;
}

static int find_label_pc(const char *name)
{
	int i;
	for (i = 0; i < MMB_MAX_LABELS; i++)
		if (G.labels[i].used && mmb_keyword_eq(G.labels[i].name, name))
			return G.labels[i].pc;
	mmb_error("?LABEL NOT FOUND");
	return 0;
}

static int parse_target(void)
{
	mmb_skip_sp();
	if (mmb_is_ident(*G.p) && !(*G.p >= '0' && *G.p <= '9'))
	{
		char name[MMB_MAX_NAME];
		const char *save = G.p;
		mmb_ident(name, sizeof(name));
		mmb_type_suffix(name);
		mmb_skip_sp();
		if (*G.p == 0 || *G.p == ':' || *G.p == '\'' || *G.p == ',')
			return find_label_pc(name);
		G.p = save;
	}
	{
		mmb_val v = mmb_expr();
		return mmb_find_line_pc((int)mmb_as_int(v));
	}
}

static void scan_labels(void)
{
	int i;
	memset(G.labels, 0, sizeof(G.labels));
	G.nlabels = 0;
	for (i = 0; i < G.nprog && G.nlabels < MMB_MAX_LABELS; i++)
	{
		const char *save = G.p;
		char name[MMB_MAX_NAME];
		G.p = G.prog[i];
		mmb_skip_sp();
		if ((*G.p >= 'A' && *G.p <= 'Z') || (*G.p >= 'a' && *G.p <= 'z') || *G.p == '_')
		{
			mmb_ident(name, sizeof(name));
			mmb_skip_sp();
			if (*G.p == ':')
			{
				mmb_upper(name);
				strncpy(G.labels[G.nlabels].name, name, MMB_MAX_NAME - 1);
				G.labels[G.nlabels].pc = i;
				G.labels[G.nlabels].used = 1;
				G.nlabels++;
			}
		}
		G.p = save;
	}
}

static int line_match(const char *body, const char *kw)
{
	const char *save = G.p;
	int r;
	G.p = after_label(body);
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
			G.p = after_label(G.prog[i]);
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
		G.p = after_label(G.prog[i]);
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
		G.p = after_label(G.prog[i]);
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
	G.p = after_label(body);
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
	G.subs[slot].nargs = 0;
	mmb_skip_sp();
	if (*G.p == '(')
		G.p++;
	while (G.subs[slot].nargs < MMB_MAX_SUB_ARGS)
	{
		mmb_skip_sp();
		if (*G.p == ')' || *G.p == 0 || *G.p == '\'' || *G.p == ':')
			break;
		if (!((G.p[0] >= 'A' && G.p[0] <= 'Z') || (G.p[0] >= 'a' && G.p[0] <= 'z') || G.p[0] == '_'))
			break;
		mmb_ident(G.subs[slot].args[G.subs[slot].nargs], MMB_MAX_NAME);
		G.subs[slot].nargs++;
		mmb_skip_sp();
		if (*G.p == ',')
		{
			G.p++;
			continue;
		}
		break;
	}
	mmb_skip_sp();
	if (*G.p == ')')
		G.p++;
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
	G.branch_pc = parse_target();
}

void mmb_cmd_gosub(void)
{
	if (G.gosub_sp >= MMB_MAX_GOSUB)
		mmb_error("?GOSUB");
	G.gosub_stack[G.gosub_sp] = G.run_pc + 1;
	G.gosub_event[G.gosub_sp] = 0;
	G.gosub_nsave[G.gosub_sp] = 0;
	G.gosub_sp++;
	G.branch_pc = parse_target();
}

void mmb_cmd_return(void)
{
	if (G.gosub_sp <= 0)
		mmb_error("?RETURN WITHOUT GOSUB");
	G.gosub_sp--;
	gosub_restore_top();
	G.branch_pc = G.gosub_stack[G.gosub_sp];
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

static int find_next_pc(int from)
{
	int depth = 1, i;
	for (i = from + 1; i < G.nprog; i++)
	{
		if (line_match(G.prog[i], "FOR"))
			depth++;
		if (line_match(G.prog[i], "NEXT"))
		{
			depth--;
			if (depth == 0)
				return i;
		}
	}
	mmb_error("?NEXT");
	return G.nprog;
}

void mmb_cmd_exit(void)
{
	if (mmb_match("DO"))
	{
		mmb_cmd_exit_do();
		return;
	}
	if (mmb_match("FOR"))
	{
		if (G.for_sp <= 0)
			mmb_error("?EXIT FOR");
		G.branch_pc = find_next_pc(G.forstack[G.for_sp - 1].line - 1) + 1;
		G.for_sp--;
		return;
	}
	if (mmb_match("SUB") || mmb_match("FUNCTION"))
	{
		mmb_cmd_end_sub();
		return;
	}
	/* CMM2: bare EXIT leaves a DO loop. */
	mmb_cmd_exit_do();
}

void mmb_cmd_continue(void)
{
	if (mmb_match("FOR"))
	{
		if (G.for_sp <= 0)
			mmb_error("?CONTINUE FOR");
		G.branch_pc = find_next_pc(G.forstack[G.for_sp - 1].line - 1);
		return;
	}
	if (mmb_match("DO"))
	{
		int i;
		for (i = G.ctrl_sp - 1; i >= 0; i--)
		{
			if (G.ctrlstack[i].type == 2)
			{
				G.branch_pc = find_loop_pc(G.ctrlstack[i].line_pc);
				return;
			}
		}
		mmb_error("?CONTINUE DO");
	}
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
		mmb_skip_sp();
		if (mmb_match("TO"))
		{
			mmb_val v2 = mmb_expr();
			if (sel.type != T_STR && v.type != T_STR && v2.type != T_STR)
			{
				double x = mmb_as_float(sel);
				if (x >= mmb_as_float(v) && x <= mmb_as_float(v2))
					matched = 1;
			}
		}
		else if (sel.type == T_STR || v.type == T_STR)
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
	else
	{
		G.sel_skip = 1;
		while (*G.p)
			G.p++;
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
		int pc = -1, i;
		if ((G.p[0] >= 'A' && G.p[0] <= 'Z') || (G.p[0] >= 'a' && G.p[0] <= 'z') || G.p[0] == '_')
		{
			const char *save = G.p;
			char name[MMB_MAX_NAME];
			mmb_ident(name, sizeof(name));
			mmb_type_suffix(name);
			mmb_skip_sp();
			if (*G.p == 0 || *G.p == ':' || *G.p == '\'')
				pc = find_label_pc(name);
			else
				G.p = save;
		}
		if (pc < 0)
		{
			mmb_val v = mmb_expr();
			pc = mmb_find_line_pc((int)mmb_as_int(v));
		}
		G.data_line = pc;
		G.data_pos = 0;
		for (i = pc; i < G.nprog; i++)
		{
			if (line_match(G.prog[i], "DATA"))
				return;
		}
		mmb_error("?NO DATA");
	}
	else
	{
		G.data_line = 0;
		G.data_pos = 0;
	}
}

void mmb_cmd_const(void)
{
	for (;;)
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
		mmb_skip_sp();
		if (*G.p == ',')
		{
			G.p++;
			continue;
		}
		break;
	}
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

static void input_assign_mem(const char **ps)
{
	char name[MMB_MAX_NAME];
	int nidx, idx[MMB_MAX_DIMS], t;
	char buf[MMB_MAX_STR + 1];
	int n = 0;
	const char *p = *ps;
	mmb_val v;

	t = mmb_parse_var_ref(name, &nidx, idx);
	while (*p == ' ' || *p == '\t')
		p++;
	if (*p == '"')
	{
		p++;
		while (*p && *p != '"')
		{
			if (n < MMB_MAX_STR)
				buf[n++] = *p;
			p++;
		}
		if (*p == '"')
			p++;
	}
	else
	{
		while (*p && *p != ',')
		{
			if (n < MMB_MAX_STR)
				buf[n++] = *p;
			p++;
		}
		while (n > 0 && (buf[n - 1] == ' ' || buf[n - 1] == '\t'))
			n--;
	}
	buf[n] = 0;
	if (*p == ',')
		p++;
	*ps = p;
	if (t == T_STR || name[strlen(name) - 1] == '$')
		v = mmb_str_val(buf);
	else
	{
		int neg = 0, dot = 0, i;
		double f = 0, frac = 0.1;
		for (i = 0; buf[i]; i++)
		{
			if (buf[i] == '-')
				neg = 1;
			else if (buf[i] == '.')
				dot = 1;
			else if (buf[i] >= '0' && buf[i] <= '9')
			{
				if (dot)
				{
					f += (buf[i] - '0') * frac;
					frac *= 0.1;
				}
				else
					f = f * 10 + (buf[i] - '0');
			}
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
	char extra[3];
	char line[MMB_MAX_STR + 1];
	const char *sp;

	mmb_skip_sp();
	if (*G.p == '#')
	{
		G.p++;
		fn = (int)mmb_as_int(mmb_expr());
		mmb_skip_sp();
		if (*G.p == ',')
			G.p++;
		for (;;)
		{
			mmb_skip_sp();
			if (*G.p == 0 || *G.p == ':' || *G.p == '\'')
				break;
			input_to_var(fn);
			mmb_skip_sp();
			if (*G.p == ',')
			{
				G.p++;
				continue;
			}
			break;
		}
		return;
	}

	extra[0] = 0;
	if (*G.p == '"')
	{
		mmb_val pr = mmb_expr();
		if (pr.type == T_STR)
			mmb_console_write(pr.s);
		mmb_skip_sp();
		if (*G.p == ';')
		{
			extra[0] = '?';
			extra[1] = ' ';
			extra[2] = 0;
			G.p++;
		}
		else if (*G.p == ',')
			G.p++;
	}
	else
	{
		extra[0] = '?';
		extra[1] = ' ';
		extra[2] = 0;
	}
	if (extra[0])
		mmb_console_write(extra);

	line[0] = 0;
	if (G.plat && G.plat->read_line)
	{
		int rc = G.plat->read_line(line, sizeof(line), 0);
		if (rc == -2)
			mmb_error("?BREAK");
		if (rc != 0)
			line[0] = 0;
	}
	sp = line;
	for (;;)
	{
		mmb_skip_sp();
		if (*G.p == 0 || *G.p == ':' || *G.p == '\'')
			break;
		input_assign_mem(&sp);
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
	{
		if (*G.p == '"')
		{
			mmb_val pr = mmb_expr();
			if (pr.type == T_STR)
				mmb_console_write(pr.s);
			mmb_skip_sp();
			if (*G.p == ',' || *G.p == ';')
				G.p++;
		}
		t = mmb_parse_var_ref(name, &nidx, idx);
		if (t != T_STR && name[strlen(name) - 1] != '$')
			mmb_error("?TYPE MISMATCH");
		buf[0] = 0;
		if (G.plat && G.plat->read_line)
		{
			if (G.plat->read_line(buf, sizeof(buf), 0) == -2)
				mmb_error("?BREAK");
		}
		v = mmb_str_val(buf);
		mmb_do_assign(name, t ? t : T_STR, nidx, idx, v);
		return;
	}
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
	mmb_ident(name, sizeof(name));
	mmb_type_suffix(name);
	if (!mmb_call_named_sub(name))
		mmb_error("?SUB NOT FOUND");
}

static void gosub_restore_top(void)
{
	int g, i, ex;
	if (G.gosub_sp < 0)
		return;
	g = G.gosub_sp;
	ex = G.opt.explicit;
	G.opt.explicit = 0;
	for (i = 0; i < G.gosub_nsave[g]; i++)
	{
		if (!G.gosub_saven[g][i][0])
			continue;
		if (G.gosub_savev[g][i].type == 0)
			continue;
		mmb_do_assign(G.gosub_saven[g][i], G.gosub_savev[g][i].type, 0, 0,
			      G.gosub_savev[g][i]);
	}
	G.opt.explicit = ex;
	G.gosub_nsave[g] = 0;
}

int mmb_call_named_sub(const char *name)
{
	int si, i, narg = 0, ex, g;
	mmb_val args[MMB_MAX_SUB_ARGS];
	char nbuf[MMB_MAX_NAME];
	strncpy(nbuf, name, MMB_MAX_NAME - 1);
	nbuf[MMB_MAX_NAME - 1] = 0;
	mmb_upper(nbuf);
	si = sub_find(nbuf);
	if (si < 0)
		return 0;
	mmb_skip_sp();
	if (*G.p == '(')
	{
		G.p++;
		mmb_skip_sp();
		if (*G.p != ')')
		{
			while (narg < MMB_MAX_SUB_ARGS && *G.p && *G.p != ')' && *G.p != ':' && *G.p != '\'')
			{
				mmb_skip_sp();
				if (*G.p == ',')
				{
					args[narg++] = mmb_int_val(0);
					G.p++;
					continue;
				}
				args[narg++] = mmb_expr();
				mmb_skip_sp();
				if (*G.p == ',')
				{
					G.p++;
					continue;
				}
				break;
			}
		}
		mmb_skip_sp();
		if (*G.p == ')')
			G.p++;
	}
	else if (*G.p && *G.p != ':' && *G.p != '\'')
	{
		while (narg < MMB_MAX_SUB_ARGS && *G.p && *G.p != ':' && *G.p != '\'')
		{
			mmb_skip_sp();
			if (*G.p == ',')
			{
				args[narg++] = mmb_int_val(0);
				G.p++;
				continue;
			}
			args[narg++] = mmb_expr();
			mmb_skip_sp();
			if (*G.p == ',')
			{
				G.p++;
				continue;
			}
			break;
		}
	}
	if (G.gosub_sp >= MMB_MAX_GOSUB)
		mmb_error("?GOSUB");
	g = G.gosub_sp;
	G.gosub_stack[g] = G.run_pc + 1;
	G.gosub_event[g] = 0;
	G.gosub_nsave[g] = G.subs[si].nargs;
	ex = G.opt.explicit;
	G.opt.explicit = 0;
	for (i = 0; i < G.subs[si].nargs; i++)
	{
		mmb_var *v;
		int idx = 0;
		strncpy(G.gosub_saven[g][i], G.subs[si].args[i], MMB_MAX_NAME - 1);
		G.gosub_saven[g][i][MMB_MAX_NAME - 1] = 0;
		v = mmb_find_var(G.subs[si].args[i], 0, 0, 0, &idx);
		if (v)
			G.gosub_savev[g][i] = mmb_load_var(v, 0);
		else
			memset(&G.gosub_savev[g][i], 0, sizeof(G.gosub_savev[g][i]));
		if (i < narg)
			mmb_do_assign(G.subs[si].args[i], 0, 0, 0, args[i]);
	}
	G.opt.explicit = ex;
	G.gosub_sp++;
	G.branch_pc = G.subs[si].line_pc;
	return 1;
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
	{
		G.gosub_sp--;
		gosub_restore_top();
		G.branch_pc = G.gosub_stack[G.gosub_sp];
	}
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
	G.opt.console = MMB_DEFAULT_CONSOLE;
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
	G.opt.audio_on = 1;
	G.opt.audio_target = 1; /* HDMI */
	G.opt.wifi_debug = 0;
	G.opt.wifi_country[0] = 'U';
	G.opt.wifi_country[1] = 'S';
	G.opt.wifi_country[2] = 0;
	G.opt.prompt = 0; /* BARE ">" */
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
	int start = G.outn;
	mmb_skip_sp();
	if (*G.p == 0 || *G.p == '\'' || *G.p == ':')
		mmb_out("\n");
	else
	{
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
	if (G.running && G.outn > start)
	{
		G.out[G.outn] = 0;
		mmb_console_write(G.out + start);
		G.outn = start;
		G.out[G.outn] = 0;
	}
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
	if (mmb_keyword_eq(name, "TIMER"))
	{
		G.timer_base = (int64_t)mmb_now_ms() - mmb_as_int(v);
		return;
	}
	if (mmb_keyword_eq(name, "DATE") && (t == T_STR || v.type == T_STR))
	{
		if (mmb_clock_set_date(v.s) != 0)
			strncpy(G.date_s, v.s, sizeof(G.date_s) - 1);
		return;
	}
	if (mmb_keyword_eq(name, "TIME") && (t == T_STR || v.type == T_STR))
	{
		if (mmb_clock_set_time(v.s) != 0)
			strncpy(G.time_s, v.s, sizeof(G.time_s) - 1);
		return;
	}
	mmb_do_assign(name, t, nidx, idx, v);
}

static int is_include_line(const char *line, char *inc, int incsz)
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
		return 1; /* other preprocessor: treat as skip */
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
	return n ? 2 : 1;
}

static void join_rel(const char *base, const char *rel, char *out, int outsz)
{
	int i, last = -1;
	if (rel[0] == '/' || (rel[1] == ':' && rel[2] == '/'))
	{
		strncpy(out, rel, outsz - 1);
		out[outsz - 1] = 0;
		return;
	}
	strncpy(out, base, outsz - 1);
	out[outsz - 1] = 0;
	for (i = 0; out[i]; i++)
		if (out[i] == '/' || out[i] == '\\')
			last = i;
	if (last >= 0)
		out[last + 1] = 0;
	else
		out[0] = 0;
	strncat(out, rel, outsz - strlen(out) - 1);
}

static void load_basic_text(const char *path, const char *text, int *auto_n, int depth);

static void load_basic_file(const char *path, int *auto_n, int depth)
{
	int sz;
	unsigned got = 0;
	char *buf;
	if (depth > 8)
		mmb_error("?INCLUDE");
	sz = mmb_vfs_size(path);
	if (sz < 0)
		mmb_error("?FILE NOT FOUND");
	buf = G.plat->alloc((unsigned)sz + 2);
	if (!buf)
		mmb_error("?OUT OF MEMORY");
	if (mmb_vfs_read(path, buf, (unsigned)sz, &got) != 0)
	{
		G.plat->free(buf);
		mmb_error("?FILE NOT FOUND");
	}
	buf[got] = 0;
	load_basic_text(path, buf, auto_n, depth);
	G.plat->free(buf);
}

static void load_basic_text(const char *path, const char *text, int *auto_n, int depth)
{
	const char *s = text;
	while (*s)
	{
		char line[MMB_LINE_LEN];
		char inc[128], full[160];
		int li = 0, num = 0, kind;
		const char *rest;
		while (*s && *s != '\n' && *s != '\r' && li < MMB_LINE_LEN - 1)
			line[li++] = *s++;
		line[li] = 0;
		while (*s == '\n' || *s == '\r')
			s++;
		kind = is_include_line(line, inc, sizeof(inc));
		if (kind == 2)
		{
			join_rel(path, inc, full, sizeof(full));
			load_basic_file(full, auto_n, depth + 1);
			continue;
		}
		if (kind == 1)
			continue;
		if (starts_with_line_number(line, &num, &rest))
			store_line(num, rest);
		else if (line[0])
		{
			store_line(*auto_n, line);
			*auto_n += 10;
		}
	}
}

static void chdir_to_file(const char *path)
{
	char dir[160];
	int i, last = -1;
	strncpy(dir, path, sizeof(dir) - 1);
	dir[sizeof(dir) - 1] = 0;
	for (i = 0; dir[i]; i++)
		if (dir[i] == '/' || dir[i] == '\\')
			last = i;
	if (last <= 0)
		return;
	dir[last] = 0;
	if (dir[0])
		mmb_vfs_chdir(dir);
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
			int auto_n = 10;
			strncpy(G.current_prog, fname, sizeof(G.current_prog) - 1);
			G.nprog = 0;
			chdir_to_file(fname);
			load_basic_file(fname, &auto_n, 0);
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
	{
		if (*G.p >= '0' && *G.p <= '9')
		{
			int num = 0;
			while (*G.p >= '0' && *G.p <= '9')
				num = num * 10 + (*G.p++ - '0');
			G.branch_pc = mmb_find_line_pc(num);
			return;
		}
		if (mmb_is_ident(*G.p))
		{
			char name[MMB_MAX_NAME];
			const char *save = G.p;
			mmb_ident(name, sizeof(name));
			mmb_type_suffix(name);
			mmb_skip_sp();
			if ((*G.p == 0 || *G.p == ':' || *G.p == '\'') &&
			    sub_find(name) < 0 &&
			    !mmb_keyword_eq(name, "RETURN") &&
			    !mmb_keyword_eq(name, "EXIT") &&
			    !mmb_keyword_eq(name, "END") &&
			    !mmb_keyword_eq(name, "STOP") &&
			    !mmb_keyword_eq(name, "CONTINUE") &&
			    !mmb_keyword_eq(name, "GOTO") &&
			    !mmb_keyword_eq(name, "GOSUB") &&
			    !mmb_keyword_eq(name, "NEW") &&
			    !mmb_keyword_eq(name, "RUN"))
			{
				G.branch_pc = find_label_pc(name);
				return;
			}
			G.p = save;
		}
		exec_statement();
	}
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
	mmb_do_assign(name, t, nidx, idx, from);
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
			/* CMM2: extra NEXT varname acts as CONTINUE; when the loop
			 * ends, skip any later NEXT for the same variable. */
			char vname[MMB_MAX_NAME];
			int i;
			strncpy(vname, G.forstack[G.for_sp - 1].var, MMB_MAX_NAME - 1);
			vname[MMB_MAX_NAME - 1] = 0;
			G.for_sp--;
			G.forstack[G.for_sp].stmt = 0;
			for (i = G.run_pc + 1; i < G.nprog; i++)
			{
				const char *save = G.p;
				char nbuf[MMB_MAX_NAME];
				G.p = after_label(G.prog[i]);
				if (mmb_match("FOR"))
				{
					G.p = save;
					break;
				}
				if (mmb_match("NEXT"))
				{
					mmb_skip_sp();
					if (mmb_is_ident(*G.p) && !(*G.p >= '0' && *G.p <= '9'))
					{
						mmb_ident(nbuf, sizeof(nbuf));
						mmb_type_suffix(nbuf);
						if (mmb_keyword_eq(nbuf, vname))
						{
							G.p = save;
							G.branch_pc = i + 1;
							break;
						}
					}
					G.p = save;
					break;
				}
				G.p = save;
			}
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
		{
			mmb_poll();
			mmb_check_break();
		}
	(void)ms;
}

void mmb_cmd_reboot(void)
{
	if (G.plat && G.plat->reboot)
		G.plat->reboot();
	mmb_error("?REBOOT");
}

int mmb_is_running(void)
{
	return G.running;
}

int mmb_break_key(void)
{
	return G.opt.break_key;
}

void mmb_check_break(void)
{
	if (!G.running)
		return;
	mmb_storage_poll();
	mmb_wlan_poll();
	mmb_play_mix();
	if (G.plat && G.plat->poll_input)
		G.plat->poll_input();
	mmb_run_events();
	if (G.plat && G.plat->take_break && G.plat->take_break())
	{
		G.running = 0;
		mmb_error("?BREAK");
	}
}

static void skip_balanced_paren(void)
{
	int depth = 0;
	mmb_skip_sp();
	if (*G.p != '(')
		return;
	while (*G.p)
	{
		if (*G.p == '"')
		{
			G.p++;
			while (*G.p)
			{
				if (*G.p == '"')
				{
					if (G.p[1] == '"')
					{
						G.p += 2;
						continue;
					}
					G.p++;
					break;
				}
				G.p++;
			}
			continue;
		}
		if (*G.p == '(')
			depth++;
		else if (*G.p == ')')
		{
			depth--;
			G.p++;
			if (depth == 0)
				return;
			continue;
		}
		G.p++;
	}
}

static int is_assign_start(void)
{
	const char *save = G.p;
	char name[MMB_MAX_NAME];
	if (!((G.p[0] >= 'A' && G.p[0] <= 'Z') || (G.p[0] >= 'a' && G.p[0] <= 'z') || G.p[0] == '_'))
		return 0;
	/* Scan only — do not evaluate (SUB fnt.draw("hi") is not an array index). */
	mmb_ident(name, sizeof(name));
	(void)name;
	mmb_skip_sp();
	if (*G.p == '(')
		skip_balanced_paren();
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
	if (mmb_match("ELSEIF"))
	{
		/* Block IF already consumed this line; leftover ELSE IF runs as IF. */
		mmb_cmd_if();
		return;
	}
	if (mmb_match("ELSE"))
	{
		mmb_skip_sp();
		if (*G.p && *G.p != ':' && *G.p != '\'')
			exec_statement();
		return;
	}
	if (mmb_match("ENDIF"))
		return;
	if (mmb_match("MID$"))
	{
		mmb_cmd_mid();
		return;
	}
	if (is_assign_start())
	{
		do_let();
		return;
	}
	if (mmb_match("IHELP") || mmb_match("HELP"))
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
	if (mmb_match("LOCAL"))
	{
		mmb_cmd_local();
		return;
	}
	if (mmb_match("STATIC"))
	{
		mmb_cmd_static();
		return;
	}
	if (mmb_match("ERROR"))
	{
		mmb_cmd_error();
		return;
	}
	if (mmb_match("MEMORY"))
	{
		mmb_cmd_memory();
		return;
	}
	if (mmb_match("RANDOMIZE"))
	{
		mmb_cmd_randomize();
		return;
	}
	if (mmb_match("INC"))
	{
		mmb_cmd_inc();
		return;
	}
	if (mmb_match("DEC"))
	{
		mmb_cmd_dec();
		return;
	}
	if (mmb_match("CAT"))
	{
		mmb_cmd_cat();
		return;
	}
	if (mmb_match("SORT"))
	{
		mmb_cmd_sort();
		return;
	}
	if (mmb_match("ON"))
	{
		mmb_cmd_on();
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
		if (mmb_match("FILES"))
			mmb_cmd_files("DIR");
		else
			mmb_cmd_list();
		return;
	}
	if (mmb_match("LS"))
	{
		mmb_cmd_files("DIR");
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
	if (mmb_match("EXIT"))
	{
		mmb_cmd_exit();
		return;
	}
	if (mmb_match("CONTINUE"))
	{
		mmb_cmd_continue();
		return;
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
	if (mmb_match("IMAGE"))
	{
		mmb_cmd_image();
		return;
	}
	if (mmb_match("FRAMEBUFFER"))
	{
		mmb_cmd_framebuffer();
		return;
	}
	if (mmb_match("TURTLE"))
	{
		mmb_cmd_turtle();
		return;
	}
	if (mmb_match("BITMAP"))
	{
		mmb_cmd_bitmap();
		return;
	}
	if (mmb_match("GUI"))
	{
		if (mmb_match("BITMAP"))
			mmb_cmd_bitmap();
		else
			mmb_syntax();
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
	if (mmb_match("KILL") || mmb_match("RM") || mmb_match("DEL"))
	{
		mmb_cmd_kill();
		return;
	}
	if (mmb_match("COPY"))
	{
		mmb_cmd_copy();
		return;
	}
	if (mmb_match("RENAME") || mmb_match("NAME") || mmb_match("MV"))
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
	if (mmb_match("CONNECT"))
	{
		mmb_cmd_connect();
		return;
	}
	if (mmb_match("IPCONFIG"))
	{
		mmb_cmd_ipconfig();
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
	if (mmb_match("REBOOT") || mmb_match("RESTART"))
	{
		mmb_cmd_reboot();
		return;
	}
	if (mmb_match("ERASE"))
	{
		mmb_cmd_clear();
		return;
	}
	if (mmb_match("SPRITE"))
	{
		mmb_cmd_sprite();
		return;
	}
	if (mmb_match("SETTICK"))
	{
		mmb_cmd_settick();
		return;
	}
	if (*G.p == '#')
	{
		while (*G.p)
			G.p++;
		return;
	}
	{
		const char *save = G.p;
		char name[MMB_MAX_NAME];
		if ((G.p[0] >= 'A' && G.p[0] <= 'Z') || (G.p[0] >= 'a' && G.p[0] <= 'z') || G.p[0] == '_')
		{
			mmb_ident(name, sizeof(name));
			mmb_type_suffix(name);
			if (sub_find(name) >= 0)
			{
				if (!mmb_call_named_sub(name))
					mmb_syntax();
				return;
			}
			G.p = save;
		}
	}
	mmb_syntax();
}

static void exec_line_body(const char *body)
{
	if (process_line_structure(body))
		return;
	G.p = after_label(body);
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

void mmb_run_events(void)
{
	unsigned now;
	int i, saved_pc, saved_sp, saved_branch;
	const char *saved_p;

	if (!G.running || G.tick_busy)
		return;
	now = mmb_now_ms();
	for (i = 0; i < MMB_MAX_TICK; i++)
	{
		if (G.tick[i].period <= 0 || !G.tick[i].sub[0])
			continue;
		if (now - G.tick[i].last < (unsigned)G.tick[i].period)
			continue;
		G.tick[i].last = now;
		saved_pc = G.run_pc;
		saved_p = G.p;
		saved_branch = G.branch_pc;
		saved_sp = G.gosub_sp;
		G.tick_busy = 1;
		G.p = "";
		if (mmb_call_named_sub(G.tick[i].sub))
		{
			if (G.gosub_sp > 0)
				G.gosub_stack[G.gosub_sp - 1] = -2;
			G.run_pc = G.branch_pc;
			G.branch_pc = -1;
			while (G.running && G.run_pc >= 0 && G.run_pc < G.nprog)
			{
				G.branch_pc = -1;
				exec_line_body(G.prog[G.run_pc]);
				if (G.branch_pc == -2)
					break;
				if (G.branch_pc >= 0)
					G.run_pc = G.branch_pc;
				else
					G.run_pc++;
			}
		}
		G.run_pc = saved_pc;
		G.p = saved_p;
		G.branch_pc = saved_branch;
		G.gosub_sp = saved_sp;
		G.tick_busy = 0;
	}
	if (G.on_key[0] && G.inkey_n > 0)
	{
		saved_pc = G.run_pc;
		saved_p = G.p;
		saved_branch = G.branch_pc;
		saved_sp = G.gosub_sp;
		G.tick_busy = 1;
		G.p = "";
		if (mmb_call_named_sub(G.on_key))
		{
			if (G.gosub_sp > 0)
				G.gosub_stack[G.gosub_sp - 1] = -2;
			G.run_pc = G.branch_pc;
			G.branch_pc = -1;
			while (G.running && G.run_pc >= 0 && G.run_pc < G.nprog)
			{
				G.branch_pc = -1;
				exec_line_body(G.prog[G.run_pc]);
				if (G.branch_pc == -2)
					break;
				if (G.branch_pc >= 0)
					G.run_pc = G.branch_pc;
				else
					G.run_pc++;
			}
		}
		G.run_pc = saved_pc;
		G.p = saved_p;
		G.branch_pc = saved_branch;
		G.gosub_sp = saved_sp;
		G.tick_busy = 0;
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
	memset(G.tick, 0, sizeof(G.tick));
	G.on_key[0] = 0;
	G.tick_busy = 0;
	G.inkey_n = G.inkey_r = G.inkey_w = 0;
	scan_labels();
	mmb_clear_vars(1);
	G.opt.explicit = 0;
	G.opt.default_type = T_NUM;
	G.opt.base = 0;
	G.opt.angle_degrees = 0;
	if (G.plat && G.plat->take_break)
		G.plat->take_break();
	while (pc < G.nprog && G.running)
	{
		int loop;
		mmb_check_break();
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
	/* Immediate drive / cd shortcuts (CMM2 console). */
	{
		char a, b;
		a = line[0];
		b = line[1];
		if (((a >= 'A' && a <= 'Z') || (a >= 'a' && a <= 'z')) && b == ':')
		{
			const char *p = line + 2;
			while (*p == ' ' || *p == '\t')
				p++;
			if (*p == 0 || *p == '/' || *p == '\\')
			{
				char cmd[8];
				cmd[0] = '"';
				cmd[1] = a;
				cmd[2] = ':';
				cmd[3] = '"';
				cmd[4] = 0;
				G.p = cmd;
				mmb_cmd_chdir();
				return G.out;
			}
		}
		if ((line[0] == 'c' || line[0] == 'C') && (line[1] == 'd' || line[1] == 'D') &&
		    (line[2] == ' ' || line[2] == '\t'))
		{
			const char *p = line + 3;
			char path[128], cmd[140];
			int n = 0;
			while (*p == ' ' || *p == '\t')
				p++;
			if (*p == '"')
			{
				p++;
				while (*p && *p != '"' && n < 120)
					path[n++] = *p++;
			}
			else
			{
				while (*p && *p != ' ' && n < 120)
					path[n++] = *p++;
			}
			path[n] = 0;
			cmd[0] = '"';
			strncpy(cmd + 1, path, sizeof(cmd) - 3);
			strcat(cmd, "\"");
			G.p = cmd;
			mmb_cmd_chdir();
			return G.out;
		}
	}
	if (starts_with_line_number(line, &num, &rest))
	{
		store_line(num, rest);
		return G.out;
	}
	if (line[0] == '*')
	{
		G.p = line + 1;
		mmb_cmd_run();
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
	mmb_audio_apply_options();
	G.timer_base = 0;
	G.rnd_seed = 0x12345678u;
	mmb_clock_init();
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
	static int wifi_boot;
	mmb_storage_poll();
	mmb_wlan_poll();
	if (!wifi_boot)
	{
		wifi_boot = 1;
		if (G.opt.wifi_enabled && G.opt.wifi_ssid[0])
			mmb_wlan_start(G.opt.wifi_ssid, G.opt.wifi_psk);
	}
	mmb_connect_poll();
	mmb_play_mix();
}
