#include "mmb_priv.h"
#include <string.h>

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
static int find_next_pc(int from);
static void build_jumps(void);

static int jmp_wend[MMB_MAX_LINES];
static int jmp_loop[MMB_MAX_LINES];
static int jmp_next[MMB_MAX_LINES];
static int jmp_endsub[MMB_MAX_LINES];
static int jmp_else[MMB_MAX_LINES];
static int jmp_endif[MMB_MAX_LINES];
static int jmp_endsel[MMB_MAX_LINES];
static int jmp_ready;
static int run_preserve_vars;
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

static int line_match_end(const char *body, const char *kw)
{
	const char *save = G.p;
	int r = 0;
	G.p = after_label(body);
	if (mmb_match("END") && mmb_match(kw))
		r = 1;
	G.p = save;
	return r;
}

static int line_is_block_if(const char *body)
{
	const char *save = G.p;
	G.p = after_label(body);
	if (!mmb_match("IF"))
	{
		G.p = save;
		return 0;
	}
	while (*G.p && *G.p != '\'')
	{
		mmb_skip_sp();
		if (mmb_match("THEN"))
		{
			int r = at_end_of_statement();
			G.p = save;
			return r;
		}
		if (*G.p == ':')
			break;
		if ((unsigned char)*G.p == 0x80)
			G.p += 3;
		else if (mmb_is_ident(*G.p))
		{
			while (mmb_is_ident(*G.p))
				G.p++;
		}
		else if (*G.p)
			G.p++;
		else
			break;
	}
	G.p = save;
	return 0;
}

static void build_jumps(void)
{
	int i, sp;
	int st[MMB_MAX_CTRL];
	jmp_ready = 0;
	for (i = 0; i < G.nprog; i++)
	{
		jmp_wend[i] = jmp_loop[i] = jmp_next[i] = -1;
		jmp_endsub[i] = jmp_else[i] = jmp_endif[i] = jmp_endsel[i] = -1;
	}
	sp = 0;
	for (i = 0; i < G.nprog; i++)
	{
		if (line_match(G.prog[i], "WHILE"))
		{
			if (sp < MMB_MAX_CTRL)
				st[sp++] = i;
		}
		else if (line_match(G.prog[i], "WEND") && sp > 0)
			jmp_wend[st[--sp]] = i;
	}
	sp = 0;
	for (i = 0; i < G.nprog; i++)
	{
		if (line_match(G.prog[i], "DO"))
		{
			if (sp < MMB_MAX_CTRL)
				st[sp++] = i;
		}
		else if (line_match(G.prog[i], "LOOP") && sp > 0)
			jmp_loop[st[--sp]] = i;
	}
	sp = 0;
	for (i = 0; i < G.nprog; i++)
	{
		if (line_match(G.prog[i], "FOR"))
		{
			if (sp < MMB_MAX_CTRL)
				st[sp++] = i;
		}
		else if (line_match(G.prog[i], "NEXT") && sp > 0)
			jmp_next[st[--sp]] = i;
	}
	for (i = 0; i < G.nprog; i++)
	{
		const char *save = G.p;
		G.p = after_label(G.prog[i]);
		if (mmb_match("SUB") || mmb_match("FUNCTION"))
		{
			int j;
			for (j = i + 1; j < G.nprog; j++)
			{
				const char *s2 = G.p;
				G.p = after_label(G.prog[j]);
				if (mmb_match("END") && (mmb_match("SUB") || mmb_match("FUNCTION")))
				{
					jmp_endsub[i] = j;
					G.p = s2;
					break;
				}
				G.p = s2;
			}
		}
		G.p = save;
	}
	sp = 0;
	for (i = 0; i < G.nprog; i++)
	{
		if (line_is_block_if(G.prog[i]))
		{
			if (sp < MMB_MAX_CTRL)
				st[sp++] = i;
		}
		else if (sp > 0 && (line_match(G.prog[i], "ELSE") ||
				     line_match(G.prog[i], "ELSEIF")))
		{
			if (jmp_else[st[sp - 1]] < 0)
				jmp_else[st[sp - 1]] = i;
		}
		else if (sp > 0 && (line_match(G.prog[i], "ENDIF") ||
				     line_match_end(G.prog[i], "IF")))
			jmp_endif[st[--sp]] = i;
	}
	sp = 0;
	for (i = 0; i < G.nprog; i++)
	{
		const char *save = G.p;
		G.p = after_label(G.prog[i]);
		if (mmb_match("SELECT"))
		{
			if (sp < MMB_MAX_CTRL)
				st[sp++] = i;
		}
		else if (mmb_match("END") && mmb_match("SELECT") && sp > 0)
			jmp_endsel[st[--sp]] = i;
		G.p = save;
	}
	jmp_ready = 1;
}

static int find_wend_pc(int from)
{
	if (jmp_ready && from >= 0 && from < G.nprog && jmp_wend[from] >= 0)
		return jmp_wend[from];
	mmb_error("?WEND");
	return G.nprog;
}

static int find_loop_pc(int from)
{
	if (jmp_ready && from >= 0 && from < G.nprog && jmp_loop[from] >= 0)
		return jmp_loop[from];
	mmb_error("?LOOP");
	return G.nprog;
}

static int find_endif_pc(int from)
{
	if (jmp_ready && from >= 0 && from < G.nprog && jmp_endif[from] >= 0)
		return jmp_endif[from];
	mmb_error("?ENDIF");
	return G.nprog;
}

static int find_end_select_pc(int from)
{
	if (jmp_ready && from >= 0 && from < G.nprog && jmp_endsel[from] >= 0)
		return jmp_endsel[from];
	mmb_error("?END SELECT");
	return G.nprog;
}

static int find_end_sub_pc(int from)
{
	if (jmp_ready && from >= 0 && from < G.nprog && jmp_endsub[from] >= 0)
		return jmp_endsub[from];
	mmb_error("?END SUB");
	return G.nprog;
}

static int at_end_of_statement(void)
{
	mmb_skip_sp();
	return *G.p == 0 || *G.p == ':' || *G.p == '\'';
}

static int match_endif(void)
{
	const char *save = G.p;
	if (mmb_match("ENDIF"))
		return 1;
	if (mmb_match("END") && mmb_match("IF"))
		return 1;
	G.p = save;
	return 0;
}

static int match_else_if(void)
{
	const char *save = G.p;
	if (mmb_match("ELSEIF"))
		return 1;
	if (mmb_match("ELSE") && mmb_match("IF"))
		return 1;
	G.p = save;
	return 0;
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
		if (match_endif())
		{
			G.if_skip--;
			if (!G.if_skip)
				G.if_taken = 0;
			G.p = save;
			return 1;
		}
		if (match_else_if())
		{
			if (G.if_skip == 1 && !G.if_taken)
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
			if (G.if_skip == 1 && !G.if_taken)
			{
				G.if_skip = 0;
				G.if_taken = 1;
			}
			G.p = save;
			return G.if_skip != 0;
		}
		if (mmb_match("IF"))
		{
			mmb_val v = mmb_expr();
			(void)v;
			if (mmb_match("THEN") && at_end_of_statement())
				G.if_skip++;
			G.p = save;
			return 1;
		}
		G.p = save;
		return 1;
	}

	if (G.if_taken)
	{
		if (match_else_if() || mmb_match("ELSE"))
		{
			G.if_skip = 1;
			G.p = save;
			return 1;
		}
		if (match_endif())
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

static int parse_as_sid(void)
{
	if (!mmb_match("AS"))
		return -1;
	if (mmb_match("INTEGER") || mmb_match("INT") || mmb_match("FLOAT") || mmb_match("STRING"))
		return -2;
	{
		char tn[MMB_MAX_NAME];
		int sid;
		mmb_ident(tn, sizeof(tn));
		mmb_type_suffix(tn);
		sid = mmb_struct_lookup(tn);
		if (sid < 0)
			mmb_error("?UNKNOWN TYPE");
		return sid;
	}
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
	G.subs[slot].ret_sid = -1;
	for (i = 0; i < MMB_MAX_SUB_ARGS; i++)
		G.subs[slot].arg_sid[i] = -1;
	mmb_skip_sp();
	if (*G.p == '(')
		G.p++;
	while (G.subs[slot].nargs < MMB_MAX_SUB_ARGS)
	{
		int sid;
		mmb_skip_sp();
		if (*G.p == ')' || *G.p == 0 || *G.p == '\'' || *G.p == ':')
			break;
		if (!((G.p[0] >= 'A' && G.p[0] <= 'Z') || (G.p[0] >= 'a' && G.p[0] <= 'z') || G.p[0] == '_' ||
		      (unsigned char)G.p[0] == 0x80))
			break;
		mmb_ident(G.subs[slot].args[G.subs[slot].nargs], MMB_MAX_NAME);
		sid = parse_as_sid();
		if (sid >= 0)
			G.subs[slot].arg_sid[G.subs[slot].nargs] = sid;
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
	if (is_func)
	{
		int sid = parse_as_sid();
		if (sid >= 0)
			G.subs[slot].ret_sid = sid;
	}
	G.nsubs++;
}

static int file_getc(int fn)
{
	unsigned char c;
	if (mmb_file_read(fn, (char *)&c, 1) != 1)
		return -1;
	return c;
}

static void file_ungetc(int fn, int c)
{
	if (fn >= 1 && fn <= MMB_MAX_FILES && G.files[fn].open && c >= 0)
	{
		if (G.files[fn].kind == MMB_FK_TCP)
			G.files[fn].ungot = c;
		else if (G.files[fn].pos > 0)
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

static void gosub_save_ctrl(int g)
{
	G.gosub_sel_skip[g] = G.sel_skip;
	G.gosub_sel_active[g] = G.sel_active;
	G.gosub_if_skip[g] = G.if_skip;
	G.gosub_if_taken[g] = G.if_taken;
	G.gosub_sel_val[g] = G.sel_val;
	if (G.sel_val.type == T_STR)
	{
		mmb_str_set(&G.gosub_sel_str[g], G.sel_val.s, -1, 0, "SELECT");
		G.gosub_sel_val[g].s = G.gosub_sel_str[g];
	}
	G.sel_skip = 0;
	G.sel_active = 0;
	G.if_skip = 0;
	G.if_taken = 0;
}

static void gosub_restore_ctrl(int g)
{
	G.sel_skip = G.gosub_sel_skip[g];
	G.sel_active = G.gosub_sel_active[g];
	G.if_skip = G.gosub_if_skip[g];
	G.if_taken = G.gosub_if_taken[g];
	G.sel_val = G.gosub_sel_val[g];
	if (G.sel_val.type == T_STR)
	{
		mmb_str_free(G.sel_str);
		G.sel_str = G.gosub_sel_str[g];
		G.gosub_sel_str[g] = 0;
		G.sel_val.s = G.sel_str ? G.sel_str : mmb_str_empty();
	}
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
	gosub_save_ctrl(G.gosub_sp);
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

static void ctrl_reenter(int type)
{
	int i;
	for (i = 0; i < G.ctrl_sp; i++)
	{
		if (G.ctrlstack[i].type == type && G.ctrlstack[i].line_pc == G.run_pc)
		{
			G.ctrl_sp = i;
			return;
		}
	}
}

void mmb_cmd_while(void)
{
	mmb_val v = mmb_expr();
	ctrl_reenter(1);
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
	ctrl_reenter(2);
	if (!cond)
	{
		G.branch_pc = find_loop_pc(G.run_pc) + 1;
		return;
	}
	if (G.ctrl_sp >= MMB_MAX_CTRL)
		mmb_error("?DO");
	G.ctrlstack[G.ctrl_sp].type = 2;
	G.ctrlstack[G.ctrl_sp].line_pc = G.run_pc;
	G.ctrlstack[G.ctrl_sp].skip = 0;
	G.ctrl_sp++;
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
	if (jmp_ready && from >= 0 && from < G.nprog && jmp_next[from] >= 0)
		return jmp_next[from];
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
	mmb_val_own(&G.sel_val, &G.sel_str, 0, "SELECT");
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
		{
			G.sel_skip = 1;
			while (*G.p)
				G.p++;
		}
		return;
	}
	if (G.sel_active)
	{
		G.sel_skip = 1;
		while (*G.p)
			G.p++;
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
	strncpy(G.current_prog, fname, sizeof(G.current_prog) - 1);
	G.current_prog[sizeof(G.current_prog) - 1] = 0;
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
	if (G.files[fn].kind == MMB_FK_TCP)
		mmb_error("?FILE");
	G.files[fn].pos = (int)mmb_as_int(v);
}

static void input_to_var(int fn)
{
	char name[MMB_MAX_NAME];
	int nidx, idx[MMB_MAX_DIMS], t;
	char *buf;
	int cap = 256, n = 0, c;
	mmb_val v;
	buf = mmb_tmp_alloc(cap);
	t = mmb_parse_var_ref(name, &nidx, idx);
	if (t == T_STR || name[strlen(name) - 1] == '$')
	{
		for (;;)
		{
			c = file_getc(fn);
			if (c < 0 || c == '\n' || c == '\r' || c == ',')
				break;
			if (n + 1 >= cap)
			{
				char *nb = mmb_tmp_alloc(cap * 2);
				memcpy(nb, buf, (size_t)n);
				buf = nb;
				cap *= 2;
			}
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
	char *buf;
	int n = 0;
	const char *p = *ps;
	mmb_val v;

	t = mmb_parse_var_ref(name, &nidx, idx);
	while (*p == ' ' || *p == '\t')
		p++;
	if (*p == '"')
	{
		const char *start;
		p++;
		start = p;
		while (*p && *p != '"')
			p++;
		n = (int)(p - start);
		buf = mmb_tmp_alloc(n + 1);
		if (n)
			memcpy(buf, start, (size_t)n);
		buf[n] = 0;
		if (*p == '"')
			p++;
	}
	else
	{
		const char *start = p;
		while (*p && *p != ',')
			p++;
		n = (int)(p - start);
		while (n > 0 && (start[n - 1] == ' ' || start[n - 1] == '\t'))
			n--;
		buf = mmb_tmp_alloc(n + 1);
		if (n)
			memcpy(buf, start, (size_t)n);
		buf[n] = 0;
	}
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
	char *line;
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

	mmb_out_flush();
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

	line = mmb_read_line(0);
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
	char *buf;
	int cap = 256, n = 0, c;
	mmb_val v;
	mmb_skip_sp();
	if (*G.p != '#')
	{
		char *line;
		mmb_out_flush();
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
		line = mmb_read_line(0);
		v = mmb_str_val(line);
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
	buf = mmb_tmp_alloc(cap);
	for (;;)
	{
		c = file_getc(fn);
		if (c < 0 || c == '\n' || c == '\r')
			break;
		if (n + 1 >= cap)
		{
			char *nb = mmb_tmp_alloc(cap * 2);
			memcpy(nb, buf, (size_t)n);
			buf = nb;
			cap *= 2;
		}
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
	gosub_restore_ctrl(g);
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
		{
			G.gosub_savev[g][i] = mmb_load_var(v, 0);
			mmb_val_own(&G.gosub_savev[g][i], &G.gosub_ss[g][i], 0,
				    G.gosub_saven[g][i]);
		}
		else
			memset(&G.gosub_savev[g][i], 0, sizeof(G.gosub_savev[g][i]));
		if (G.subs[si].arg_sid[i] >= 0)
			mmb_bind_struct_var(G.subs[si].args[i], G.subs[si].arg_sid[i]);
		if (i < narg)
			mmb_do_assign(G.subs[si].args[i],
				      G.subs[si].arg_sid[i] >= 0 ? T_STRUCT : 0, 0, 0, args[i]);
	}
	if (G.subs[si].is_func)
	{
		mmb_var *v;
		int idx = 0;
		int slot = G.gosub_nsave[g];
		G.gosub_event[g] = 2;
		if (slot >= MMB_MAX_SUB_ARGS)
			mmb_error("?OUT OF MEMORY");
		strncpy(G.gosub_saven[g][slot], nbuf, MMB_MAX_NAME - 1);
		G.gosub_saven[g][slot][MMB_MAX_NAME - 1] = 0;
		v = mmb_find_var(nbuf, 0, 0, 0, &idx);
		if (v)
		{
			G.gosub_savev[g][slot] = mmb_load_var(v, 0);
			mmb_val_own(&G.gosub_savev[g][slot], &G.gosub_ss[g][slot], 0,
				    G.gosub_saven[g][slot]);
		}
		else
			memset(&G.gosub_savev[g][slot], 0, sizeof(G.gosub_savev[g][slot]));
		G.gosub_nsave[g] = slot + 1;
		if (G.subs[si].ret_sid >= 0)
			mmb_bind_struct_var(nbuf, G.subs[si].ret_sid);
		else if (nbuf[0] && nbuf[strlen(nbuf) - 1] == '$')
			mmb_do_assign(nbuf, T_STR, 0, 0, mmb_str_val(""));
		else
			mmb_do_assign(nbuf, T_NUM, 0, 0, mmb_num_val(0));
	}
	G.opt.explicit = ex;
	gosub_save_ctrl(g);
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
	int g;
	if (G.gosub_sp > 0)
	{
		g = G.gosub_sp - 1;
		if (G.gosub_event[g] == 2 && G.gosub_nsave[g] > 0)
		{
			char *fname = G.gosub_saven[g][G.gosub_nsave[g] - 1];
			mmb_var *v;
			int idx = 0;
			v = mmb_find_var(fname, 0, 0, 0, &idx);
			if (!v)
				v = mmb_find_var(fname, T_STRUCT, 0, 0, &idx);
			if (v)
			{
				G.func_ret = mmb_load_var(v, 0);
				if (G.func_ret.type == T_STRUCT && G.func_ret.blob)
				{
					int sz = G.sdef[G.func_ret.struct_idx].total;
					if (sz > MMB_STRUCT_RET_MAX)
						mmb_error("?OVERFLOW");
					memcpy(G.func_ret_blob, G.func_ret.blob, (unsigned)sz);
					G.func_ret.blob = G.func_ret_blob;
				}
				else
					mmb_val_own(&G.func_ret, &G.func_ret_s, 0, fname);
			}
			else
			{
				memset(&G.func_ret, 0, sizeof(G.func_ret));
				G.func_ret.type = T_NUM;
			}
		}
	}
	mmb_cmd_end_sub();
}
void mmb_option_reset(void)
{
	memset(&G.opt, 0, sizeof(G.opt));
	G.opt.base = 0;
	G.opt.default_type = T_NUM;
	G.opt.tab = 2;
	G.opt.break_key = 3;
	G.opt.profiling = 0;
	G.opt.tracecache = 1;
	G.opt.colourcode = 1;
	G.opt.console = MMB_DEFAULT_CONSOLE;
	G.opt.console_port = 3;
	G.opt.crlf = 2;
	G.opt.default_mode = MMB_OPT_DEFAULT_MODE;
	G.opt.baudrate = 115200;
	G.opt.status = 1;
	G.opt.vcc = 3.3;
	G.opt.repeat_first = MMB_REPEAT_FIRST_DEFAULT;
	G.opt.repeat_next = MMB_REPEAT_NEXT_DEFAULT;
	G.opt.edit_font = 1;
	G.opt.edit_theme = MMB_OPT_DEFAULT_EDIT_THEME;
	G.opt.edit_jump_break = 0;
	G.opt.mouse_sens = 1;
	G.opt.audio_on = 1;
	G.opt.audio_target = 1; /* HDMI */
	G.opt.wifi_debug = 0;
	G.opt.term_log = 0;
	G.opt.wifi_country[0] = 'U';
	G.opt.wifi_country[1] = 'S';
	G.opt.wifi_country[2] = 0;
	G.opt.prompt = MMB_OPT_DEFAULT_PROMPT;
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
	int i, j, n;
	char buf[MMB_LINE_LEN];

	while (*text == ' ' || *text == '\t')
		text++;
	n = 0;
	while (text[n] && n < MMB_LINE_LEN - 1)
	{
		buf[n] = text[n];
		n++;
	}
	buf[n] = 0;
	while (n > 0 && (buf[n - 1] == ' ' || buf[n - 1] == '\t'))
		buf[--n] = 0;
	text = buf;
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

static int locate_opt_int(int *out)
{
	mmb_skip_sp();
	if (!*G.p || *G.p == ',' || *G.p == ':' || *G.p == '\'')
		return 0;
	*out = (int)mmb_as_int(mmb_expr());
	return 1;
}

void mmb_cmd_locate(void)
{
	int y = 0, x = 0, cur = 0;
	int have_y, have_x = 0, have_cur = 0;

	have_y = locate_opt_int(&y);
	mmb_skip_sp();
	if (*G.p == ',')
	{
		G.p++;
		have_x = locate_opt_int(&x);
		mmb_skip_sp();
		if (*G.p == ',')
		{
			G.p++;
			have_cur = locate_opt_int(&cur);
		}
	}
	if (have_y || have_x)
	{
		int px = G.print_x;
		int py = G.print_y;

		if (have_x)
			px = x * mmb_print_font_w();
		if (have_y)
			py = y * mmb_print_font_h();
		mmb_print_cursor_goto(px, py);
		G.print_locate = 1;
		mmb_print_locate_pending();
	}
	if (have_cur)
		mmb_hw_cursor(cur != 0);
}

static void mmb_print_track_new(int from)
{
	if (G.outn > from)
		mmb_print_track(G.out + from, (unsigned)(G.outn - from));
}

void mmb_cmd_print(void)
{
	int first = 1;
	int no_nl = 0;
	int start = G.outn;
	int track_from = G.outn;
	mmb_skip_sp();
	if (*G.p == 0 || *G.p == '\'' || *G.p == ':')
	{
		mmb_out("\n");
		mmb_print_track_new(track_from);
	}
	else
	{
		mmb_print_locate_pending();
		while (*G.p && *G.p != ':' && *G.p != '\'')
		{
		mmb_val v;
		mmb_skip_sp();
		if (mmb_print_try_at())
			continue;
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
				{
					int n = G.outn;
					if (n < 0)
						n = 0;
					mmb_file_write(fn, G.out, (unsigned)n);
				}
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
		mmb_print_track_new(track_from);
		track_from = G.outn;
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
			mmb_print_track_new(track_from);
			track_from = G.outn;
			G.p++;
			no_nl = 0;
			continue;
		}
		no_nl = 0;
		break;
		}
		if (!no_nl)
		{
			mmb_out("\n");
			mmb_print_track_new(track_from);
		}
	}
	if (G.outn > start)
	{
		G.out[G.outn] = 0;
		if (G.running)
		{
			mmb_console_write(G.out + start);
			G.outn = start;
			G.out[G.outn] = 0;
		}
	}
}

static void do_let(void)
{
	char name[MMB_MAX_NAME];
	int nidx = 0, idx[MMB_MAX_DIMS], t;
	mmb_val v;
	if (mmb_tcache_try_let())
		return;
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
	mmb_pkg_unmount();
	mmb_clear_vars(1);
	mmb_struct_clear();
	mmb_clear_consts();
	mmb_close_tcp_files();
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
	mmb_play_stop();
}

static void load_prog_from_disk(const char *name)
{
	char fname[128];
	int auto_n = 10;
	strncpy(fname, name, sizeof(fname) - 1);
	fname[sizeof(fname) - 1] = 0;
	if (!strchr(fname, '.'))
		strncat(fname, ".BAS", sizeof(fname) - strlen(fname) - 1);
	strncpy(G.current_prog, fname, sizeof(G.current_prog) - 1);
	G.current_prog[sizeof(G.current_prog) - 1] = 0;
	G.nprog = 0;
	chdir_to_file(fname);
	load_basic_file(fname, &auto_n, 0);
}

void mmb_cmd_run(void)
{
	char fname[128];
	int from_disk = 0;
	int mounted_pkg = 0;
	mmb_skip_sp();
	if (*G.p && *G.p != ':' && *G.p != '\'')
	{
		if (*G.p == '"')
		{
			mmb_val v = mmb_expr();
			if (v.type != T_STR)
				mmb_syntax();
			strncpy(fname, v.s, sizeof(fname) - 1);
			fname[sizeof(fname) - 1] = 0;
		}
		else
		{
			int n = 0;
			while (*G.p && *G.p != ' ' && n < 126)
				fname[n++] = *G.p++;
			fname[n] = 0;
		}
		from_disk = 1;
	}
	else if (G.current_prog[0] && !mmb_pkg_is_name(G.current_prog) &&
		 !(G.current_prog[0] == 'B' && G.current_prog[1] == ':'))
	{
		strncpy(fname, G.current_prog, sizeof(fname) - 1);
		fname[sizeof(fname) - 1] = 0;
		from_disk = 1;
	}
	if (from_disk)
	{
		mmb_pkg_unmount();
		if (mmb_pkg_is_name(fname))
		{
			mmb_pkg_mount(fname);
			load_prog_from_disk("B:/MAIN.BAS");
			mounted_pkg = 1;
		}
		else
			load_prog_from_disk(fname);
	}
	run_program();
	if (mounted_pkg)
	{
		mmb_pkg_unmount();
		G.current_prog[0] = 0;
	}
}

int mmb_parse_target(void)
{
	return parse_target();
}

void mmb_cmd_chain(void)
{
	char fname[128];
	int mounted_pkg = 0;
	mmb_skip_sp();
	if (*G.p == '"')
	{
		mmb_val v = mmb_expr();
		if (v.type != T_STR)
			mmb_syntax();
		strncpy(fname, v.s, sizeof(fname) - 1);
		fname[sizeof(fname) - 1] = 0;
	}
	else if (*G.p && *G.p != ':' && *G.p != '\'')
	{
		int n = 0;
		while (*G.p && *G.p != ' ' && n < 126)
			fname[n++] = *G.p++;
		fname[n] = 0;
	}
	else
		mmb_syntax();
	mmb_pkg_unmount();
	if (mmb_pkg_is_name(fname))
	{
		mmb_pkg_mount(fname);
		load_prog_from_disk("B:/MAIN.BAS");
		mounted_pkg = 1;
	}
	else
		load_prog_from_disk(fname);
	run_preserve_vars = 1;
	run_program();
	if (mounted_pkg)
	{
		mmb_pkg_unmount();
		G.current_prog[0] = 0;
	}
}

static int if_tok_id(const char *p)
{
	if ((unsigned char)*p != 0x80)
		return 0;
	return (unsigned char)p[1] | ((unsigned char)p[2] << 8);
}

static int if_ascii_kw(const char *kw)
{
	const char *p = G.p;
	const char *k = kw;
	while (*k)
	{
		char a = *p, b = *k;
		if (a >= 'a' && a <= 'z')
			a = (char)(a - 32);
		if (b >= 'a' && b <= 'z')
			b = (char)(b - 32);
		if (a != b)
			return 0;
		p++;
		k++;
	}
	if (mmb_is_ident(*p))
		return 0;
	G.p = p;
	return 1;
}

static int if_at_ident_start(const char *then0)
{
	if (G.p == then0)
		return 1;
	return !mmb_is_ident(G.p[-1]);
}

static int if_skip_string(void)
{
	if (*G.p != '"')
		return 0;
	G.p++;
	while (*G.p)
	{
		if (*G.p != '"')
		{
			G.p++;
			continue;
		}
		G.p++;
		if (*G.p == '"')
		{
			G.p++;
			continue;
		}
		break;
	}
	return 1;
}

static int if_take_else_kw(const char *then0, int else_id, int elseif_id)
{
	int id;
	if ((unsigned char)*G.p == 0x80)
	{
		id = if_tok_id(G.p);
		if (id == elseif_id)
		{
			G.p += 3;
			return 2;
		}
		if (id == else_id)
		{
			G.p += 3;
			return 1;
		}
		return 0;
	}
	if (!if_at_ident_start(then0))
		return 0;
	if (if_ascii_kw("ELSEIF"))
		return 2;
	if (if_ascii_kw("ELSE"))
		return 1;
	return 0;
}

static void if_skip_to_stmt_end(void)
{
	while (*G.p && *G.p != ':')
	{
		if (*G.p == '\'')
			break;
		if (*G.p == ' ' || *G.p == '\t')
		{
			G.p++;
			continue;
		}
		if (if_skip_string())
			continue;
		if ((unsigned char)*G.p == 0x80)
		{
			G.p += 3;
			continue;
		}
		G.p++;
	}
}

void mmb_cmd_if(void)
{
	mmb_val v;
	int cond;
	int else_id, elseif_id;
	const char *then0;
	if (mmb_tcache_try_if())
		return;
	v = mmb_expr();
	cond = mmb_as_int(v) != 0;
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
		if_skip_to_stmt_end();
		return;
	}
	else_id = mmb_kw_id("ELSE");
	elseif_id = mmb_kw_id("ELSEIF");
	then0 = G.p;
	while (*G.p && *G.p != ':')
	{
		int which;
		if (*G.p == '\'')
			break;
		if (*G.p == ' ' || *G.p == '\t')
		{
			G.p++;
			continue;
		}
		if (if_skip_string())
			continue;
		which = if_take_else_kw(then0, else_id, elseif_id);
		if (which == 2)
		{
			v = mmb_expr();
			if (!mmb_match("THEN"))
				mmb_syntax();
			mmb_skip_sp();
			if (mmb_as_int(v))
			{
				exec_statement();
				if_skip_to_stmt_end();
				return;
			}
			then0 = G.p;
			continue;
		}
		if (which == 1)
		{
			mmb_skip_sp();
			exec_statement();
			if_skip_to_stmt_end();
			return;
		}
		if ((unsigned char)*G.p == 0x80)
		{
			G.p += 3;
			continue;
		}
		G.p++;
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
	{
		int off = 0;
		mmb_var *v = mmb_find_var(name, t, 0, nidx, idx);
		if (!v)
			mmb_error("?FOR");
		off = mmb_var_offset(v, nidx, idx);
		strncpy(G.forstack[G.for_sp].var, name, MMB_MAX_NAME - 1);
		G.forstack[G.for_sp].var[MMB_MAX_NAME - 1] = 0;
		G.forstack[G.for_sp].vp = v;
		G.forstack[G.for_sp].off = off;
		G.forstack[G.for_sp].to = mmb_as_int(to);
		G.forstack[G.for_sp].step = mmb_as_int(step);
		G.forstack[G.for_sp].line = G.run_pc + 1;
		G.for_sp++;
	}
}

void mmb_cmd_next(void)
{
	char name[MMB_MAX_NAME];
	mmb_var *v;
	int named = 0;
	int off;
	mmb_skip_sp();
	if (mmb_is_ident(*G.p))
	{
		mmb_ident(name, sizeof(name));
		mmb_type_suffix(name);
		named = 1;
	}
	if (G.for_sp <= 0)
		mmb_error("?NEXT WITHOUT FOR");
	if (named && !mmb_keyword_eq(name, G.forstack[G.for_sp - 1].var))
		mmb_error("?NEXT WITHOUT FOR");
	v = G.forstack[G.for_sp - 1].vp;
	off = G.forstack[G.for_sp - 1].off;
	if (!v)
		mmb_error("?NEXT WITHOUT FOR");
	{
		int64_t cur = v->type == T_INT ? v->data.i[off] : (int64_t)v->data.f[off];
		int64_t step = G.forstack[G.for_sp - 1].step;
		int64_t to = G.forstack[G.for_sp - 1].to;
		cur += step;
		if (v->type == T_INT)
			v->data.i[off] = cur;
		else
			v->data.f[off] = (double)cur;
		if ((step >= 0 && cur <= to) || (step < 0 && cur >= to))
		{
			G.forstack[G.for_sp - 1].stmt = 1;
		}
		else
		{
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

void mmb_cmd_vsync_wait(void)
{
	static unsigned last;
	unsigned now;

	if (G.plat && G.plat->wait_vsync)
		G.plat->wait_vsync();
	now = mmb_now_ms();
	if (last != 0 && (now - last) < 50)
	{
		while ((mmb_now_ms() - last) < 16)
		{
			mmb_poll();
			mmb_check_break();
		}
	}
	last = mmb_now_ms();
}

void mmb_reboot(void)
{
	int i;
	G.running = 0;
	mmb_play_stop();
	mmb_close_tcp_files();
	for (i = 1; i <= MMB_MAX_FILES; i++)
		G.files[i].open = 0;
	mmb_settings_save();
	mmb_storage_unmount();
	mmb_console_write("Rebooting...\r\n");
	if (G.plat && G.plat->reboot)
		G.plat->reboot();
	mmb_error("?REBOOT");
}

void mmb_cmd_reboot(void)
{
	mmb_reboot();
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
	if (G.opt.profiling)
		G.prof.check_break++;
	mmb_storage_poll();
	mmb_wlan_poll();
	mmb_net_yield();
	mmb_play_mix();
	if (G.plat && G.plat->poll_input)
		G.plat->poll_input();
	mmb_run_events();
	if (G.plat && G.plat->take_break && G.plat->take_break())
	{
		mmb_play_stop();
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
	if ((unsigned char)G.p[0] != 0x80 &&
	    !((G.p[0] >= 'A' && G.p[0] <= 'Z') || (G.p[0] >= 'a' && G.p[0] <= 'z') || G.p[0] == '_'))
		return 0;
	/* Scan only — do not evaluate (SUB fnt.draw("hi") is not an array index). */
	mmb_ident(name, sizeof(name));
	(void)name;
	mmb_skip_sp();
	if (*G.p == '(')
		skip_balanced_paren();
	mmb_skip_sp();
	while (*G.p == '.')
	{
		G.p++;
		if (!((G.p[0] >= 'A' && G.p[0] <= 'Z') || (G.p[0] >= 'a' && G.p[0] <= 'z') || G.p[0] == '_' ||
		      (unsigned char)G.p[0] == 0x80))
			break;
		mmb_ident(name, sizeof(name));
		mmb_skip_sp();
		if (*G.p == '(')
			skip_balanced_paren();
		mmb_skip_sp();
	}
	if (*G.p == '=')
	{
		G.p = save;
		return 1;
	}
	G.p = save;
	return 0;
}

static void tok_cmd_end(void)
{
	if (mmb_match("IF"))
		mmb_cmd_endif();
	else if (mmb_match("SELECT"))
		mmb_cmd_end_select();
	else if (mmb_match("SUB"))
		mmb_cmd_end_sub();
	else if (mmb_match("FUNCTION"))
		mmb_cmd_end_function();
	else if (mmb_match("TYPE"))
		mmb_cmd_end_type();
	else
		mmb_cmd_end();
}

static void tok_cmd_list(void)
{
	if (mmb_match("FILES"))
		mmb_cmd_files("DIR");
	else if (mmb_match("TYPE"))
		mmb_cmd_list_type();
	else
		mmb_cmd_list();
}

static void tok_cmd_line(void)
{
	if (mmb_match("INPUT"))
		mmb_cmd_line_input();
	else
		mmb_cmd_line();
}

static void tok_cmd_else(void)
{
	mmb_skip_sp();
	if (*G.p && *G.p != ':' && *G.p != '\'')
		exec_statement();
}

static void tok_cmd_elseif(void)
{
	mmb_cmd_if();
}

static void tok_cmd_endif(void)
{
}

static void tok_cmd_rem(void)
{
	while (*G.p)
		G.p++;
}

static void tok_cmd_let(void)
{
	do_let();
}

static void tok_cmd_factory(void)
{
	mmb_match("RESET");
	mmb_cmd_factory_reset();
}

static void tok_cmd_gui(void)
{
	if (mmb_match("BITMAP"))
		mmb_cmd_bitmap();
	else
		mmb_syntax();
}

static void tok_cmd_help(void)
{
	mmb_cmd_help();
}

static void tok_cmd_dir(void)
{
	mmb_cmd_files("DIR");
}

static int try_tok_cmd(void)
{
	static void (*tab[512])(void);
	static int inited;
	int id;
	if ((unsigned char)*G.p != 0x80)
		return 0;
	if (!inited)
	{
		tab[mmb_kw_id("REM")] = tok_cmd_rem;
		tab[mmb_kw_id("LET")] = tok_cmd_let;
		tab[mmb_kw_id("ELSEIF")] = tok_cmd_elseif;
		tab[mmb_kw_id("ELSE")] = tok_cmd_else;
		tab[mmb_kw_id("ENDIF")] = tok_cmd_endif;
		tab[mmb_kw_id("MID$")] = mmb_cmd_mid;
		tab[mmb_kw_id("LSET")] = mmb_cmd_lset;
		tab[mmb_kw_id("RSET")] = mmb_cmd_rset;
		tab[mmb_kw_id("IHELP")] = tok_cmd_help;
		tab[mmb_kw_id("HELP")] = tok_cmd_help;
		tab[mmb_kw_id("PRINT")] = mmb_cmd_print;
		tab[mmb_kw_id("DIM")] = mmb_cmd_dim;
		tab[mmb_kw_id("REDIM")] = mmb_cmd_redim;
		tab[mmb_kw_id("COMMON")] = mmb_cmd_common;
		tab[mmb_kw_id("SWAP")] = mmb_cmd_swap;
		tab[mmb_kw_id("BIT")] = mmb_cmd_bit;
		tab[mmb_kw_id("BYTE")] = mmb_cmd_byte;
		tab[mmb_kw_id("EXECUTE")] = mmb_cmd_execute;
		tab[mmb_kw_id("CHAIN")] = mmb_cmd_chain;
		tab[mmb_kw_id("RESUME")] = mmb_cmd_resume;
		tab[mmb_kw_id("STOP")] = mmb_cmd_end;
		tab[mmb_kw_id("ARRAY")] = mmb_cmd_array;
		tab[mmb_kw_id("TYPE")] = mmb_cmd_type;
		tab[mmb_kw_id("STRUCT")] = mmb_cmd_struct;
		tab[mmb_kw_id("JSON_PARSE")] = mmb_cmd_json_parse;
		tab[mmb_kw_id("LOCAL")] = mmb_cmd_local;
		tab[mmb_kw_id("STATIC")] = mmb_cmd_static;
		tab[mmb_kw_id("ERROR")] = mmb_cmd_error;
		tab[mmb_kw_id("MEMORY")] = mmb_cmd_memory;
		tab[mmb_kw_id("RANDOMIZE")] = mmb_cmd_randomize;
		tab[mmb_kw_id("INC")] = mmb_cmd_inc;
		tab[mmb_kw_id("DEC")] = mmb_cmd_dec;
		tab[mmb_kw_id("CAT")] = mmb_cmd_cat;
		tab[mmb_kw_id("SORT")] = mmb_cmd_sort;
		tab[mmb_kw_id("ON")] = mmb_cmd_on;
		tab[mmb_kw_id("CLEAR")] = mmb_cmd_clear;
		tab[mmb_kw_id("NEW")] = mmb_cmd_new;
		tab[mmb_kw_id("LIST")] = tok_cmd_list;
		tab[mmb_kw_id("LS")] = tok_cmd_dir;
		tab[mmb_kw_id("RUN")] = mmb_cmd_run;
		tab[mmb_kw_id("END")] = tok_cmd_end;
		tab[mmb_kw_id("GOTO")] = mmb_cmd_goto;
		tab[mmb_kw_id("GOSUB")] = mmb_cmd_gosub;
		tab[mmb_kw_id("RETURN")] = mmb_cmd_return;
		tab[mmb_kw_id("WHILE")] = mmb_cmd_while;
		tab[mmb_kw_id("WEND")] = mmb_cmd_wend;
		tab[mmb_kw_id("EXIT")] = mmb_cmd_exit;
		tab[mmb_kw_id("CONTINUE")] = mmb_cmd_continue;
		tab[mmb_kw_id("DO")] = mmb_cmd_do;
		tab[mmb_kw_id("LOOP")] = mmb_cmd_loop;
		tab[mmb_kw_id("SELECT")] = mmb_cmd_select;
		tab[mmb_kw_id("CASE")] = mmb_cmd_case;
		tab[mmb_kw_id("DATA")] = mmb_cmd_data;
		tab[mmb_kw_id("READ")] = mmb_cmd_read;
		tab[mmb_kw_id("RESTORE")] = mmb_cmd_restore;
		tab[mmb_kw_id("CONST")] = mmb_cmd_const;
		tab[mmb_kw_id("SAVE")] = mmb_cmd_save;
		tab[mmb_kw_id("SEEK")] = mmb_cmd_seek;
		tab[mmb_kw_id("INPUT")] = mmb_cmd_input;
		tab[mmb_kw_id("CALL")] = mmb_cmd_call;
		tab[mmb_kw_id("SUB")] = mmb_cmd_sub;
		tab[mmb_kw_id("FUNCTION")] = mmb_cmd_function;
		tab[mmb_kw_id("IF")] = mmb_cmd_if;
		tab[mmb_kw_id("FOR")] = mmb_cmd_for;
		tab[mmb_kw_id("NEXT")] = mmb_cmd_next;
		tab[mmb_kw_id("OPTIONS")] = mmb_cmd_options;
		tab[mmb_kw_id("OPTION")] = mmb_cmd_option;
		tab[mmb_kw_id("FACTORY_RESET")] = mmb_cmd_factory_reset;
		tab[mmb_kw_id("FACTORY")] = tok_cmd_factory;
		tab[mmb_kw_id("CLS")] = mmb_cmd_cls;
		tab[mmb_kw_id("LOCATE")] = mmb_cmd_locate;
		tab[mmb_kw_id("PIXEL")] = mmb_cmd_pixel;
		tab[mmb_kw_id("LINE")] = tok_cmd_line;
		tab[mmb_kw_id("BOX")] = mmb_cmd_box;
		tab[mmb_kw_id("CIRCLE")] = mmb_cmd_circle;
		tab[mmb_kw_id("RBOX")] = mmb_cmd_rbox;
		tab[mmb_kw_id("ARC")] = mmb_cmd_arc;
		tab[mmb_kw_id("TRIANGLE")] = mmb_cmd_triangle;
		tab[mmb_kw_id("POLYGON")] = mmb_cmd_polygon;
		tab[mmb_kw_id("TEXT")] = mmb_cmd_text;
		tab[mmb_kw_id("FONT")] = mmb_cmd_font;
		tab[mmb_kw_id("COLOUR")] = mmb_cmd_colour;
		tab[mmb_kw_id("COLOR")] = mmb_cmd_colour;
		tab[mmb_kw_id("MODE")] = mmb_cmd_mode;
		tab[mmb_kw_id("PAGE")] = mmb_cmd_page;
		tab[mmb_kw_id("BLIT")] = mmb_cmd_blit;
		tab[mmb_kw_id("IMAGE")] = mmb_cmd_image;
		tab[mmb_kw_id("FRAMEBUFFER")] = mmb_cmd_framebuffer;
		tab[mmb_kw_id("TURTLE")] = mmb_cmd_turtle;
		tab[mmb_kw_id("BITMAP")] = mmb_cmd_bitmap;
		tab[mmb_kw_id("GUI")] = tok_cmd_gui;
		tab[mmb_kw_id("PACKAGE")] = mmb_cmd_package;
		tab[mmb_kw_id("CHDIR")] = mmb_cmd_chdir;
		tab[mmb_kw_id("DRIVE")] = mmb_cmd_drive;
		tab[mmb_kw_id("MKDIR")] = mmb_cmd_mkdir;
		tab[mmb_kw_id("RMDIR")] = mmb_cmd_rmdir;
		tab[mmb_kw_id("KILL")] = mmb_cmd_kill;
		tab[mmb_kw_id("RM")] = mmb_cmd_kill;
		tab[mmb_kw_id("DEL")] = mmb_cmd_kill;
		tab[mmb_kw_id("COPY")] = mmb_cmd_copy;
		tab[mmb_kw_id("XFER")] = mmb_cmd_xfer;
		tab[mmb_kw_id("RENAME")] = mmb_cmd_name;
		tab[mmb_kw_id("NAME")] = mmb_cmd_name;
		tab[mmb_kw_id("MV")] = mmb_cmd_name;
		tab[mmb_kw_id("DIR")] = tok_cmd_dir;
		tab[mmb_kw_id("FILES")] = mmb_cmd_files_ui;
		tab[mmb_kw_id("CONNECT")] = mmb_cmd_connect;
		tab[mmb_kw_id("TERM")] = mmb_cmd_term;
		tab[mmb_kw_id("IPCONFIG")] = mmb_cmd_ipconfig;
		tab[mmb_kw_id("OPEN")] = mmb_cmd_open;
		tab[mmb_kw_id("CLOSE")] = mmb_cmd_close;
		tab[mmb_kw_id("PLAY")] = mmb_cmd_play;
		tab[mmb_kw_id("BEEP")] = mmb_cmd_beep;
		tab[mmb_kw_id("LOAD")] = mmb_cmd_load;
		tab[mmb_kw_id("EDIT")] = mmb_cmd_edit;
		tab[mmb_kw_id("WORDPAD")] = mmb_cmd_wordpad;
		tab[mmb_kw_id("CREDITS")] = mmb_cmd_credits;
		tab[mmb_kw_id("AFK")] = mmb_cmd_afk;
		tab[mmb_kw_id("PAUSE")] = mmb_cmd_pause;
		tab[mmb_kw_id("VSYNC_WAIT")] = mmb_cmd_vsync_wait;
		tab[mmb_kw_id("REBOOT")] = mmb_cmd_reboot;
		tab[mmb_kw_id("RESTART")] = mmb_cmd_reboot;
		tab[mmb_kw_id("ERASE")] = mmb_cmd_clear;
		tab[mmb_kw_id("MATH")] = mmb_cmd_math;
		tab[mmb_kw_id("SPRITE")] = mmb_cmd_sprite;
		tab[mmb_kw_id("SETTICK")] = mmb_cmd_settick;
		inited = 1;
	}
	id = (unsigned char)G.p[1] | ((unsigned char)G.p[2] << 8);
	if (id && id < 512 && tab[id])
	{
		G.p += 3;
		tab[id]();
		return 1;
	}
	return 0;
}

static void exec_statement(void)
{
	mmb_skip_sp();
	if (*G.p == 0 || *G.p == '\'')
		return;
	if (G.opt.profiling && G.running)
		G.prof.stmt++;
	if (try_tok_cmd())
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
	if (mmb_match("LSET"))
	{
		mmb_cmd_lset();
		return;
	}
	if (mmb_match("RSET"))
	{
		mmb_cmd_rset();
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
	if (mmb_match("REDIM"))
	{
		mmb_cmd_redim();
		return;
	}
	if (mmb_match("COMMON"))
	{
		mmb_cmd_common();
		return;
	}
	if (mmb_match("SWAP"))
	{
		mmb_cmd_swap();
		return;
	}
	if (mmb_match("BIT"))
	{
		mmb_cmd_bit();
		return;
	}
	if (mmb_match("BYTE"))
	{
		mmb_cmd_byte();
		return;
	}
	if (mmb_match("EXECUTE"))
	{
		mmb_cmd_execute();
		return;
	}
	if (mmb_match("CHAIN"))
	{
		mmb_cmd_chain();
		return;
	}
	if (mmb_match("RESUME"))
	{
		mmb_cmd_resume();
		return;
	}
	if (mmb_match("STOP"))
	{
		mmb_cmd_end();
		return;
	}
	if (mmb_match("ARRAY"))
	{
		mmb_cmd_array();
		return;
	}
	if (mmb_match("TYPE"))
	{
		mmb_cmd_type();
		return;
	}
	if (mmb_match("STRUCT"))
	{
		mmb_cmd_struct();
		return;
	}
	if (mmb_match("JSON_PARSE"))
	{
		mmb_cmd_json_parse();
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
		else if (mmb_match("TYPE"))
			mmb_cmd_list_type();
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
		if (mmb_match("TYPE"))
		{
			mmb_cmd_end_type();
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
	if (mmb_match("OPTIONS"))
	{
		mmb_cmd_options();
		return;
	}
	if (mmb_match("OPTION"))
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
	if (mmb_match("LOCATE"))
	{
		mmb_cmd_locate();
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
	if (mmb_match("PACKAGE"))
	{
		mmb_cmd_package();
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
	if (mmb_match("XFER"))
	{
		mmb_cmd_xfer();
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
	if (mmb_match("TERM"))
	{
		mmb_cmd_term();
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
	if (mmb_match("BEEP"))
	{
		mmb_cmd_beep();
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
	if (mmb_match("WORDPAD"))
	{
		mmb_cmd_wordpad();
		return;
	}
	if (mmb_match("CREDITS"))
	{
		mmb_cmd_credits();
		return;
	}
	if (mmb_match("AFK"))
	{
		mmb_cmd_afk();
		return;
	}
	if (mmb_match("PAUSE"))
	{
		mmb_cmd_pause();
		return;
	}
	if (mmb_match("VSYNC_WAIT"))
	{
		mmb_cmd_vsync_wait();
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
	if (mmb_match("MATH"))
	{
		mmb_cmd_math();
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

void mmb_cmd_execute(void)
{
	mmb_val v = mmb_expr();
	char body[MMB_LINE_LEN];
	const char *save;
	if (v.type != T_STR)
		mmb_error("?TYPE MISMATCH");
	save = G.p;
	mmb_tokenize_text(v.s ? v.s : "", body, (int)sizeof(body));
	exec_line_body(body);
	G.p = save;
}

static void run_gosub_body(void)
{
	int saved_ctrl = G.ctrl_sp;
	int saved_for = G.for_sp;
	int saved_running = G.running;

	G.running = 1;
	if (G.gosub_sp > 0)
		G.gosub_stack[G.gosub_sp - 1] = -2;
	G.run_pc = G.branch_pc;
	G.branch_pc = -1;
	while (G.running && G.run_pc >= 0 && G.run_pc < G.nprog)
	{
		int loop;
		mmb_check_break();
		do
		{
			loop = 0;
			G.branch_pc = -1;
			exec_line_body(mmb_tok_line(G.run_pc));
			if (G.branch_pc == -2)
				break;
			if (G.branch_pc >= 0)
			{
				G.run_pc = G.branch_pc;
				break;
			}
			if (G.for_sp > 0 && G.forstack[G.for_sp - 1].stmt == 1)
			{
				G.forstack[G.for_sp - 1].stmt = 0;
				G.run_pc = G.forstack[G.for_sp - 1].line;
				loop = 1;
			}
		} while (loop && G.running);
		if (G.branch_pc == -2)
			break;
		if (G.branch_pc >= 0)
			continue;
		G.run_pc++;
	}
	G.ctrl_sp = saved_ctrl;
	G.for_sp = saved_for;
	G.running = saved_running;
}

int mmb_try_user_function(mmb_val *out)
{
	const char *save = G.p;
	char name[MMB_MAX_NAME];
	int saved_pc, saved_sp, saved_branch;
	const char *saved_p;
	int si;

	mmb_skip_sp();
	if (!mmb_is_ident(*G.p))
		return 0;
	mmb_ident(name, sizeof(name));
	mmb_type_suffix(name);
	si = sub_find(name);
	if (si < 0 || !G.subs[si].is_func)
	{
		G.p = save;
		return 0;
	}
	mmb_skip_sp();
	if (*G.p != '(')
	{
		G.p = save;
		return 0;
	}
	saved_pc = G.run_pc;
	saved_branch = G.branch_pc;
	saved_sp = G.gosub_sp;
	memset(&G.func_ret, 0, sizeof(G.func_ret));
	G.func_ret.type = T_NUM;
	if (!mmb_call_named_sub(name))
	{
		G.p = save;
		return 0;
	}
	saved_p = G.p;
	run_gosub_body();
	G.run_pc = saved_pc;
	G.branch_pc = saved_branch;
	G.gosub_sp = saved_sp;
	G.p = saved_p;
	*out = G.func_ret;
	return 1;
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
			run_gosub_body();
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
			run_gosub_body();
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
	if (G.opt.profiling)
		mmb_prof_reset();
	mmb_str_reset();
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
	mmb_tokenize_program();
	build_jumps();
	if (!run_preserve_vars)
	{
		mmb_clear_vars(1);
		mmb_clear_consts();
	}
	run_preserve_vars = 0;
	mmb_struct_prepare();
	G.on_error_pc = -1;
	G.error_active = 0;
	G.opt.explicit = 0;
	G.opt.default_type = T_NUM;
	G.opt.base = 0;
	G.opt.angle_degrees = 0;
	if (G.plat && G.plat->take_break)
		G.plat->take_break();
	while (pc < G.nprog && G.running)
	{
		int loop;
		int trapped = 0;
		mmb_check_break();
		do
		{
			loop = 0;
			mmb_str_reset();
			G.run_pc = pc;
			G.branch_pc = -1;
			if (G.on_error_pc >= 0)
			{
				if (setjmp(G.run_errjmp))
				{
					trapped = 1;
					break;
				}
			}
			exec_line_body(mmb_tok_line(pc));
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
		if (trapped)
		{
			pc = G.on_error_pc;
			continue;
		}
		if (G.branch_pc >= 0)
			continue;
		pc++;
	}
	if (G.opt.profiling)
		mmb_prof_report();
	G.running = 0;
	mmb_play_stop();
	mmb_gfx_reset_console(0);
}

static void clear_exec_flags(void)
{
	G.running = 0;
	G.if_skip = 0;
	G.if_taken = 0;
	G.sel_skip = 0;
	G.sel_active = 0;
	G.branch_pc = -1;
}

const char *mmb_exec_line(const char *line)
{
	int num;
	const char *rest;
	G.outn = 0;
	G.out[0] = 0;
	G.err[0] = 0;
	mmb_str_reset();
	if (setjmp(G.errjmp))
	{
		int was_running = G.running;
		int pages_off = G.gfx.write_page || G.gfx.display_page ||
				G.gfx.write_fb || G.gfx.page1_any ||
				G.gfx.page1_alpha_used;
		G.outn = 0;
		G.out[0] = 0;
		clear_exec_flags();
		mmb_pkg_unmount();
		if (was_running || pages_off)
			mmb_gfx_reset_console(1);
		mmb_out(G.err[0] ? G.err : "?SYNTAX ERROR");
		return G.out;
	}
	clear_exec_flags();
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
	exec_line_body(mmb_tok_immediate(line));
	return G.out;
}

int mmb_in_editor(void)
{
	return G.ed.active || G.ed.wait_continue;
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
	mmb_ramdisk_seed();
	mmb_settings_load();
	mmb_gfx_apply_default_mode();
	mmb_audio_apply_options();
	mmb_console_apply_colour();
	G.timer_base = 0;
	G.rnd_seed = 0x12345678u;
	mmb_clock_init();
}

void mmb_reset(void)
{
	mmb_pkg_unmount();
	mmb_clear_vars(0);
	mmb_option_reset();
	mmb_play_stop();
	G.nprog = 0;
	mmb_gfx_init();
}

void mmb_poll(void)
{
	static int net_boot;
	mmb_storage_poll();
	mmb_wlan_poll();
	mmb_net_yield();
	if (!net_boot && G.opt.ethernet_enabled)
	{
		mmb_eth_start();
		net_boot = 1;
	}
	if (!net_boot && G.opt.wifi_enabled && G.opt.wifi_ssid[0])
	{
		if (mmb_wlan_start(G.opt.wifi_ssid, G.opt.wifi_psk) == 0)
			net_boot = 1;
		else if (!mmb_wlan_radio_pending())
			net_boot = 1;
	}
	mmb_connect_poll();
	mmb_term_poll();
	mmb_net_tcp_debug_poll();
	mmb_play_mix();
	mmb_editor_poll();
	mmb_ihelp_poll();
	mmb_files_poll();
	mmb_wordpad_poll();
	mmb_afk_poll();
}
