#include "mmb_priv.h"
#include "frontend.h"
#include <stdio.h>
#include <string.h>

extern int mmb_parse_var_ref(char *name, int *nidx, int *idx);
extern void mmb_do_assign(const char *name, int type_hint, int nidx, int *idx, mmb_val val);
extern mmb_val mmb_load_var(mmb_var *v, int off);

static void exec_statement(void);
static void gosub_restore_top(void);
static void exec_line_body(const char *body);
static void exec_line_from(const char *body, const char *at);
static int exec_line_guarded(const char *body, const char *at);
static void run_program(void);
static int line_match(const char *body, const char *kw);
static int find_wend_pc(int from);
static int find_loop_pc(int from);
static const char *find_matching_loop(int pc, const char *p, int *out_pc);
static const char *stmt_after(const char *np);
static int find_endif_pc(int from);
static int find_end_select_pc(int from);
static int find_end_sub_pc(int from);
static int find_next_pc(int from);
static void build_jumps(void);
static void mmb_break_events(void);
static void exec_error_reset(int was_running);

/* mmb_poll() re-entrancy guard (see mmb_poll below). File scope so the
 * interpreter's setjmp error landings can clear it after an unwind. */
static int s_poll_active;

/* Jump tables and run flags are per-console interpreter state; the names
 * below always address the active context's fields (G.jmp_*, G.jmp_ready,
 * G.run_preserve_vars). */
static int at_end_of_statement(void);
static void skip_stmt(const char **pp);
static int line_block_ifs(const char *body);
static int process_line_structure(const char *body);
static int sub_find(const char *name);
static void sub_register(const char *name, int pc, int is_func, int ret_type);
static mmb_val read_data_item(void);
static int file_getc(int fn);
static void file_ungetc(int fn, int c);

/* ---- growable program store (#1303) -------------------------------- */

static void oom_error(const char *file, int line)
{
	char msg[192];
	if (file && file[0] && line > 0)
		sprintf(msg, "?OUT OF MEMORY in %s:%d", file, line);
	else
		strcpy(msg, "?OUT OF MEMORY");
	mmb_error(msg);
}

static void *prog_alloc(unsigned n, const char *file, int line)
{
	void *p = G.plat->alloc(n);
	if (!p)
		oom_error(file, line);
	return p;
}

/* strdup on the platform heap. */
static char *prog_strdup(const char *s, const char *file, int line)
{
	unsigned n = (unsigned)strlen(s) + 1;
	char *p = (char *)prog_alloc(n, file, line);
	memcpy(p, s, n);
	return p;
}

/* Double the prog/prog_num pointer arrays so they hold at least `need`
 * lines. Old lines are carried across (pointers only); on failure the
 * original tables are untouched and ?OUT OF MEMORY is raised. */
static void prog_grow(int need, const char *file, int line)
{
	int cap = G.prog_cap > 0 ? G.prog_cap : MMB_LINES_INIT;
	char **np;
	int *nn;
	while (cap < need)
		cap *= 2;
	np = (char **)G.plat->alloc((unsigned)cap * sizeof(char *));
	nn = (int *)G.plat->alloc((unsigned)cap * sizeof(int));
	if (!np || !nn)
	{
		if (np)
			G.plat->free(np);
		if (nn)
			G.plat->free(nn);
		oom_error(file, line);
	}
	if (G.prog && G.nprog > 0)
	{
		memcpy(np, G.prog, (unsigned)G.nprog * sizeof(char *));
		memcpy(nn, G.prog_num, (unsigned)G.nprog * sizeof(int));
	}
	if (G.prog)
		G.plat->free(G.prog);
	if (G.prog_num)
		G.plat->free(G.prog_num);
	G.prog = np;
	G.prog_num = nn;
	G.prog_cap = cap;
}

/* Grow the seven jump tables to at least `need` lines (lazily, at RUN). */
static void jmp_grow(int need)
{
	int cap = G.jmp_cap > 0 ? G.jmp_cap : MMB_LINES_INIT;
	int *n[7];
	int *o[7];
	int i, k;
	while (cap < need)
		cap *= 2;
	if (G.jmp_cap >= need && G.jmp_wend)
		return;
	memset(n, 0, sizeof(n));
	for (i = 0; i < 7; i++)
	{
		n[i] = (int *)G.plat->alloc((unsigned)cap * sizeof(int));
		if (!n[i])
		{
			for (k = 0; k < i; k++)
				G.plat->free(n[k]);
			mmb_error("?OUT OF MEMORY");
		}
	}
	o[0] = G.jmp_wend; o[1] = G.jmp_loop; o[2] = G.jmp_next;
	o[3] = G.jmp_endsub; o[4] = G.jmp_else; o[5] = G.jmp_endif;
	o[6] = G.jmp_endsel;
	for (i = 0; i < 7; i++)
		if (o[i])
			G.plat->free(o[i]);
	G.jmp_wend = n[0]; G.jmp_loop = n[1]; G.jmp_next = n[2];
	G.jmp_endsub = n[3]; G.jmp_else = n[4]; G.jmp_endif = n[5];
	G.jmp_endsel = n[6];
	G.jmp_cap = cap;
}

/* Free every strdup'd line, the pointer arrays and the jump tables
 * (NEW / console teardown), returning the store to its empty state. */
static void prog_release(void)
{
	int i;
	if (G.prog)
	{
		for (i = 0; i < G.nprog; i++)
			if (G.prog[i])
				G.plat->free(G.prog[i]);
		G.plat->free(G.prog);
		G.prog = 0;
	}
	if (G.prog_num)
	{
		G.plat->free(G.prog_num);
		G.prog_num = 0;
	}
	G.prog_cap = 0;
	G.nprog = 0;
	if (G.jmp_wend)
	{
		G.plat->free(G.jmp_wend);
		G.plat->free(G.jmp_loop);
		G.plat->free(G.jmp_next);
		G.plat->free(G.jmp_endsub);
		G.plat->free(G.jmp_else);
		G.plat->free(G.jmp_endif);
		G.plat->free(G.jmp_endsel);
		G.jmp_wend = G.jmp_loop = G.jmp_next = 0;
		G.jmp_endsub = G.jmp_else = G.jmp_endif = G.jmp_endsel = 0;
	}
	G.jmp_cap = 0;
	G.jmp_ready = 0;
}

/* Binary search over the sorted prog_num table; -1 when absent. */
static int find_line_idx(int num)
{
	int lo = 0, hi = G.nprog - 1;
	while (lo <= hi)
	{
		int mid = (lo + hi) >> 1;
		if (G.prog_num[mid] == num)
			return mid;
		if (G.prog_num[mid] < num)
			lo = mid + 1;
		else
			hi = mid - 1;
	}
	return -1;
}

/* Clear the SUB table for a fresh run while keeping its capacity (#1303). */
static void subs_clear(void)
{
	int i;
	for (i = 0; i < G.subs_cap; i++)
		G.subs[i].used = 0;
	G.nsubs = 0;
}

/* Drop the label table (NEW / teardown); it regrows on the next RUN. */
static void labels_release(void)
{
	if (G.labels)
		G.plat->free(G.labels);
	G.labels = 0;
	G.labels_cap = 0;
	G.nlabels = 0;
}

/* Drop the SUB table (NEW / teardown). */
static void subs_release(void)
{
	if (G.subs)
		G.plat->free(G.subs);
	G.subs = 0;
	G.subs_cap = 0;
	G.nsubs = 0;
}

int mmb_find_line_pc(int num)
{
	int i = find_line_idx(num);
	if (i >= 0)
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
	if (mmb_is_ident_start(*G.p))
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
	for (i = 0; i < G.nlabels; i++)
		if (G.labels[i].used && mmb_keyword_eq(G.labels[i].name, name))
			return G.labels[i].pc;
	mmb_error("?LABEL NOT FOUND");
	return 0;
}

static int parse_target(void)
{
	mmb_skip_sp();
	if (mmb_is_ident_start(*G.p))
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

/* Double the label table so the next label fits (#1303). The prescan used to
 * silently drop every label after the 64th; now it grows, and only a real
 * allocation failure stops it. */
static void labels_grow(void)
{
	int cap = G.labels_cap > 0 ? G.labels_cap : MMB_MAX_LABELS;
	mmb_label *nl;
	if (G.labels_cap > 0 && G.nlabels < G.labels_cap)
		return;
	nl = (mmb_label *)G.plat->alloc((unsigned)cap * 2 * sizeof(mmb_label));
	if (!nl)
		mmb_error("?OUT OF MEMORY");
	if (G.labels)
	{
		memcpy(nl, G.labels, (unsigned)cap * sizeof(mmb_label));
		G.plat->free(G.labels);
	}
	G.labels = nl;
	G.labels_cap = cap * 2;
}

static void scan_labels(void)
{
	int i;
	G.nlabels = 0;
	for (i = 0; i < G.nprog; i++)
	{
		const char *save = G.p;
		char name[MMB_MAX_NAME];
		G.p = G.prog[i];
		mmb_skip_sp();
		if (mmb_is_ident_start(*G.p))
		{
			mmb_ident(name, sizeof(name));
			mmb_skip_sp();
			if (*G.p == ':')
			{
				labels_grow();
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

/* True when the statement starting at ``at`` is a block IF (its THEN is
 * followed by the end of that statement). */
static int at_is_block_if(const char *at)
{
	const char *save = G.p;
	G.p = at;
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
	/* First half is the IF-line stack, second half the last arm of each. */
	int st[MMB_MAX_CTRL * 2];
	jmp_grow(G.nprog);
	G.jmp_ready = 0;
	for (i = 0; i < G.nprog; i++)
	{
		G.jmp_wend[i] = G.jmp_loop[i] = G.jmp_next[i] = -1;
		G.jmp_endsub[i] = G.jmp_else[i] = G.jmp_endif[i] = G.jmp_endsel[i] = -1;
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
			G.jmp_wend[st[--sp]] = i;
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
			G.jmp_loop[st[--sp]] = i;
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
			G.jmp_next[st[--sp]] = i;
	}
	/* SUB/END SUB with a single stack pass instead of scanning forward for
	 * each definition (#992). Definitions are not lexically nested, but the
	 * stack keeps mismatched END lines from corrupting the table. */
	sp = 0;
	for (i = 0; i < G.nprog; i++)
	{
		const char *save = G.p;
		G.p = after_label(G.prog[i]);
		if (mmb_match("SUB") || mmb_match("FUNCTION"))
		{
			if (sp < MMB_MAX_CTRL)
				st[sp++] = i;
		}
		else if (sp > 0 && mmb_match("END") &&
			 (mmb_match("SUB") || mmb_match("FUNCTION")))
			G.jmp_endsub[st[--sp]] = i;
		G.p = save;
	}
	/* Block IF arms.  jmp_else is an arm chain: an IF links to its first
	 * ELSE/ELSEIF, each arm links to the next one, and the last structural
	 * line links to the matching ENDIF.  jmp_endif points every line of the
	 * block (IF and arms) at that ENDIF, so a skip can tell whether a line
	 * belongs to the block at the top of the IF stack (#1333).  Parse the
	 * tokenized line: raw text treats ``ELSE : ...`` as a label because the
	 * keyword is followed by a colon. */
	sp = 0;
	for (i = 0; i < G.nprog; i++)
	{
		const char *line = mmb_tok_line(i);
		int n, is_arm = 0;
		/* A line may both close an arm and open a new block IF after a
		 * colon, so handle the leading token first and push the rest. */
		if (sp > 0 && (line_match(line, "ENDIF") ||
			       line_match_end(line, "IF")))
		{
			int fi, last, a;
			sp--;
			fi = st[sp];
			last = st[sp + MMB_MAX_CTRL];
			if (last >= 0)
				G.jmp_else[last] = i;
			if (fi >= 0)
			{
				G.jmp_endif[fi] = i;
				for (a = G.jmp_else[fi]; a >= 0 && a != i;
				     a = G.jmp_else[a])
					G.jmp_endif[a] = i;
			}
		}
		else if (sp > 0 && (line_match(line, "ELSE") ||
				    line_match(line, "ELSEIF")))
		{
			int last = st[sp - 1 + MMB_MAX_CTRL];
			if (last >= 0)
				G.jmp_else[last] = i;
			st[sp - 1 + MMB_MAX_CTRL] = i;
			is_arm = 1;
		}
		/* A line may open more than one block IF through colons (#1301).
		 * Only the first can be named by this line; later ones get -1 so
		 * their arms cannot alias the first block's table entries.  An
		 * arm line keeps its jmp_else slot for the arm chain, so a block
		 * IF after its colon is not named either. */
		n = line_block_ifs(line);
		{
			int k;
			for (k = 0; k < n && sp < MMB_MAX_CTRL; k++)
			{
				int pc = k == 0 && !is_arm ? i : -1;
				st[sp] = pc;
				st[sp + MMB_MAX_CTRL] = pc;
				sp++;
			}
		}
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
			G.jmp_endsel[st[--sp]] = i;
		G.p = save;
	}
	G.jmp_ready = 1;
}

static int find_wend_pc(int from)
{
	if (G.jmp_ready && from >= 0 && from < G.nprog && G.jmp_wend[from] >= 0)
		return G.jmp_wend[from];
	mmb_error("?WEND");
	return G.nprog;
}

static int find_loop_pc(int from)
{
	if (G.jmp_ready && from >= 0 && from < G.nprog && G.jmp_loop[from] >= 0)
		return G.jmp_loop[from];
	mmb_error("?LOOP");
	return G.nprog;
}

static int find_endif_pc(int from)
{
	if (G.jmp_ready && from >= 0 && from < G.nprog && G.jmp_endif[from] >= 0)
		return G.jmp_endif[from];
	mmb_error("?ENDIF");
	return G.nprog;
}

static int find_end_select_pc(int from)
{
	if (G.jmp_ready && from >= 0 && from < G.nprog && G.jmp_endsel[from] >= 0)
		return G.jmp_endsel[from];
	mmb_error("?END SELECT");
	return G.nprog;
}

static int find_end_sub_pc(int from)
{
	if (G.jmp_ready && from >= 0 && from < G.nprog && G.jmp_endsub[from] >= 0)
		return G.jmp_endsub[from];
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

/* Block IF frame states (#1273).  IF_BODY runs the selected branch;
 * IF_SEARCH is skipping a false IF looking for ELSE/ELSEIF; IF_DONE is a
 * taken branch whose later ELSE/ELSEIF arms are being skipped.  Skipped
 * nested IFs are counted in G.if_shadow instead (#1301). */
#define IF_BODY   0
#define IF_SEARCH 1
#define IF_DONE   2

/* 1 when the current line is the ELSE token, without consuming it. */
static int at_else_kw(void)
{
	const char *save = G.p;
	int r = mmb_match("ELSE");
	G.p = save;
	return r;
}

static int at_elseif_kw(void)
{
	const char *save = G.p;
	int r = mmb_match("ELSEIF");
	G.p = save;
	return r;
}

static void if_push(unsigned char state, int pc)
{
	if (G.if_depth >= MMB_MAX_IF_DEPTH)
		mmb_error("?IF");
	G.if_stack[G.if_depth] = state;
	G.if_pc_stack[G.if_depth] = pc;
	G.if_depth++;
}

/* The innermost SELECT frame visible to this call, or NULL outside a SELECT.
 * Frames below sel_base belong to a caller and stay hidden until it returns
 * (#1299). */
static mmb_selframe *sel_top(void)
{
	return G.sel_sp > G.sel_base ? &G.selstack[G.sel_sp - 1] : 0;
}

static void sel_pop(void)
{
	if (G.sel_sp <= G.sel_base)
		return;
	G.sel_sp--;
	if (G.selstack[G.sel_sp].str)
	{
		mmb_str_free(G.selstack[G.sel_sp].str);
		G.selstack[G.sel_sp].str = 0;
	}
}

static void sel_truncate(int depth)
{
	mmb_sel_truncate(depth);
}

static void sel_reset_all(void)
{
	G.sel_base = 0;
	sel_truncate(0);
	G.sel_sp = 0;
}

static void sel_push_skipped(void)
{
	mmb_selframe *f;
	if (G.sel_sp >= MMB_MAX_SELECT)
		mmb_error("?SELECT");
	f = &G.selstack[G.sel_sp++];
	memset(f, 0, sizeof(*f));
	f->val = mmb_int_val(0);
	f->skip = 1;
	f->skipped = 1;
}

/* Restore the control state a loop (or branch out of it) must resume with. */
static void loop_restore(int if_depth, int sel_depth)
{
	G.if_depth = if_depth;
	if (G.sel_sp > sel_depth)
		sel_truncate(sel_depth);
}

static void pop_ctrl_top(void)
{
	if (G.ctrl_sp <= 0)
		return;
	G.ctrl_sp--;
	loop_restore(G.ctrlstack[G.ctrl_sp].if_depth,
		     G.ctrlstack[G.ctrl_sp].sel_depth);
}

static void pop_for_top(void)
{
	if (G.for_sp <= 0)
		return;
	G.for_sp--;
	loop_restore(G.forstack[G.for_sp].if_depth,
		     G.forstack[G.for_sp].sel_depth);
}

/* Drop every loop frame created after the frame with sequence ``seq``, of
 * either kind, restoring its saved IF/SELECT state (#1300). */
static void unwind_above_seq(int seq)
{
	while (G.for_sp > 0 && G.forstack[G.for_sp - 1].seq > seq)
		pop_for_top();
	while (G.ctrl_sp > 0 && G.ctrlstack[G.ctrl_sp - 1].seq > seq)
		pop_ctrl_top();
}

/* Number of block IF statements opened on this line, including ones after a
 * colon.  A line may open more than one (#1301), and the jump tables need the
 * same count so each matching ENDIF pairs at the right depth (#1333). */
static int line_block_ifs(const char *body)
{
	const char *p = after_label(body);
	int n = 0;
	for (;;)
	{
		while (*p == ' ' || *p == '\t')
			p++;
		if (*p == 0 || *p == '\'')
			break;
		if (at_is_block_if(p))
			n++;
		skip_stmt(&p);
		if (*p == ':')
		{
			p++;
			continue;
		}
		break;
	}
	return n;
}

static int line_is_select(const char *body)
{
	const char *save = G.p;
	int r;
	G.p = after_label(body);
	r = mmb_match("SELECT");
	G.p = save;
	return r;
}

static int at_case_kw(void)
{
	const char *save = G.p;
	int r = mmb_match("CASE");
	G.p = save;
	return r;
}

static int process_line_structure(const char *body)
{
	const char *save = G.p;
	mmb_selframe *sel;

	G.p = after_label(body);
	mmb_skip_sp();

	/* A frame other than IF_BODY means this line is being skipped.  Only
	 * the innermost IF_SEARCH frame may evaluate ELSEIF: it is the frame
	 * whose false IF started the skip, so no outer frame is skipping. */
	if (G.if_depth > 0 && G.if_stack[G.if_depth - 1] != IF_BODY)
	{
		if (match_endif())
		{
			if (G.if_shadow > 0)
				G.if_shadow--;
			else
				G.if_depth--;
			G.p = save;
			return 1;
		}
		if (match_else_if())
		{
			if (G.if_shadow == 0 &&
			    G.if_stack[G.if_depth - 1] == IF_SEARCH)
			{
				mmb_val v = mmb_expr();
				if (mmb_as_int(v))
					G.if_stack[G.if_depth - 1] = IF_BODY;
			}
			G.p = save;
			return 1;
		}
		if (mmb_match("ELSE"))
		{
			int run = 0;
			if (G.if_shadow == 0 &&
			    G.if_stack[G.if_depth - 1] == IF_SEARCH)
			{
				G.if_stack[G.if_depth - 1] = IF_BODY;
				run = 1;
			}
			G.p = save;
			return !run;
		}
		/* Nested block IFs are counted without evaluating their
		 * conditions, so functions and array indexes in skipped code never
		 * run (#1274, #1301).  One-line IFs open no block. */
		{
			int n = line_block_ifs(body);
			if (n > 0)
				G.if_shadow += n;
		}
		G.p = save;
		return 1;
	}

	/* SELECT CASE that is currently skipping statements.  A nested SELECT
	 * in skipped code is counted (without evaluating it) so its END SELECT
	 * cannot close the outer SELECT (#1299). */
	sel = sel_top();
	if (sel && sel->skip)
	{
		if (line_is_select(body))
		{
			sel_push_skipped();
			G.p = save;
			return 1;
		}
		if (mmb_match("END") && mmb_match("SELECT"))
		{
			sel_pop();
			G.p = save;
			return 1;
		}
		if (!sel->skipped && at_case_kw())
		{
			G.p = save;
			return 0;
		}
		G.p = save;
		return 1;
	}

	if (G.if_depth > 0)
	{
		/* Executing the body of the innermost taken branch. */
		if (match_else_if() || mmb_match("ELSE"))
		{
			G.if_stack[G.if_depth - 1] = IF_DONE;
			G.p = save;
			return 1;
		}
		if (match_endif())
		{
			G.if_depth--;
			G.p = save;
			return 1;
		}
	}

	G.p = save;
	return 0;
}

/* After a structural line has run, return the pc a skipping block IF should
 * resume at, or -1 to keep the line-by-line fallback (#1333).  A search hops
 * arm to arm until one is taken or the matching ENDIF is reached; a taken arm
 * that has ended at ELSE/ELSEIF jumps past the ENDIF.  The jump is only safe
 * when ``pc`` is a structural line of the block at the top of the IF stack: a
 * GOTO into the middle of a skipped block, an immediate-mode IF or a mid-line
 * block IF has no matching end, so those keep walking. */
static int if_skip_pc(int pc)
{
	int top, ifpc, end, t;

	if (!G.jmp_ready || G.if_depth <= 0 || G.if_shadow != 0)
		return -1;
	if (pc < 0 || pc >= G.nprog)
		return -1;
	top = G.if_depth - 1;
	ifpc = G.if_pc_stack[top];
	if (ifpc < 0 || ifpc >= G.nprog)
		return -1;
	end = G.jmp_endif[ifpc];
	if (end < 0 || G.jmp_endif[pc] != end)
		return -1;
	if (G.if_stack[top] == IF_SEARCH)
	{
		t = G.jmp_else[pc];
		if (t < 0)
			return -1;
		if (t == end)
		{
			G.if_depth--;
			return end + 1;
		}
		return t;
	}
	if (G.if_stack[top] == IF_DONE)
	{
		G.if_depth--;
		return end + 1;
	}
	return -1;
}

static int sub_find(const char *name)
{
	int i;
	char nbuf[MMB_MAX_NAME];
	if (G.nsubs <= 0 || !G.subs)
		return -1;
	strncpy(nbuf, name, MMB_MAX_NAME - 1);
	nbuf[MMB_MAX_NAME - 1] = 0;
	mmb_upper(nbuf);
	for (i = 0; i < G.nsubs; i++)
		if (G.subs[i].used && mmb_keyword_eq(G.subs[i].name, nbuf))
			return i;
	return -1;
}

/* Parse an optional ``AS <type>`` clause.  Returns -1 when no AS is present,
 * -2 for a scalar type (recorded in *type_out), and a struct id for a user
 * type (*type_out set to T_STRUCT). */
static int parse_as_sid(int *type_out)
{
	if (type_out)
		*type_out = 0;
	if (!mmb_match("AS"))
		return -1;
	if (mmb_match("STRING"))
	{
		if (type_out)
			*type_out = T_STR;
		return -2;
	}
	if (mmb_match("INTEGER") || mmb_match("INT"))
	{
		if (type_out)
			*type_out = T_INT;
		return -2;
	}
	if (mmb_match("FLOAT"))
	{
		if (type_out)
			*type_out = T_NUM;
		return -2;
	}
	{
		char tn[MMB_MAX_NAME];
		int sid;
		mmb_ident(tn, sizeof(tn));
		mmb_type_suffix(tn);
		sid = mmb_struct_lookup(tn);
		if (sid < 0)
			mmb_error("?UNKNOWN TYPE");
		if (type_out)
			*type_out = T_STRUCT;
		return sid;
	}
}

/* Bind an argument to a variable of its declared scalar type, and make that
 * binding reachable by the bare (unsuffixed) name when the declaration was
 * unsuffixed -- mirroring how FUNCTION return values are bound. */
static void bind_scalar_arg(const char *name, int type)
{
	char last;
	int suffixed, unsuf, idx = 0;
	mmb_var *v;
	if (!type || type == T_STRUCT)
		return;
	last = name[0] ? name[strlen(name) - 1] : 0;
	suffixed = (last == '$' || last == '%' || last == '!');
	v = mmb_find_var(name, type, 1, 0, &idx);
	if (!v)
		return;
	unsuf = !suffixed && type != (G.opt.default_type ? G.opt.default_type : T_NUM);
	if (v->unsuffixed != unsuf)
	{
		v->unsuffixed = unsuf;
		mmb_vars_rehash();
	}
}

/* Double the SUB table when it is full (#1303). The 129th definition used to
 * fail with a misleading ?OUT OF MEMORY; the table now grows like the
 * program store. */
static void subs_grow(void)
{
	int cap = G.subs_cap > 0 ? G.subs_cap : MMB_MAX_SUBS;
	mmb_sub *ns = (mmb_sub *)G.plat->alloc((unsigned)cap * 2 * sizeof(mmb_sub));
	if (!ns)
		mmb_error("?OUT OF MEMORY");
	/* Zero the whole new table: sub_register() picks the first !used slot,
	 * and sub_find() only walks the first nsubs entries, so garbage `used`
	 * bits would scatter definitions beyond its reach (#1303). */
	memset(ns, 0, (unsigned)cap * 2 * sizeof(mmb_sub));
	if (G.subs)
	{
		memcpy(ns, G.subs, (unsigned)cap * sizeof(mmb_sub));
		G.plat->free(G.subs);
	}
	G.subs = ns;
	G.subs_cap = cap * 2;
}

/* #1335: a SUB/FUNCTION parameter whose name matches a global CONST can never
 * be bound - expressions and the argument store resolve the CONST first - and
 * it used to fail only at the call site with a bare ?CONST that named nothing.
 * Report the parameter and its SUB instead. CONSTs are cleared before RUN and
 * scan_subs() prescans before any CONST line executes, so this also runs when
 * the definition line executes (and again at the call) to catch CONSTs that
 * were defined after the prescan. */
static void check_param_consts(int si)
{
	int i;
	char msg[MMB_MAX_NAME * 2 + 48];
	if (G.nconst <= 0)
		return;
	for (i = 0; i < G.subs[si].nargs; i++)
	{
		mmb_val cv;
		if (G.subs[si].arg_sid[i] >= 0)
			continue;
		if (mmb_const_lookup(G.subs[si].args[i], 0, &cv))
		{
			snprintf(msg, sizeof(msg), "?CONST %s used as parameter of %s",
				 G.subs[si].args[i], G.subs[si].name);
			mmb_error(msg);
		}
	}
}

static void sub_register(const char *name, int pc, int is_func, int ret_type)
{
	int i, slot;
	int ex, suffixed, sl;
	char nbuf[MMB_MAX_NAME];
	char msg[MMB_MAX_NAME + 48];
	strncpy(nbuf, name, MMB_MAX_NAME - 1);
	nbuf[MMB_MAX_NAME - 1] = 0;
	mmb_upper(nbuf);
	sl = (int)strlen(nbuf);
	suffixed = sl > 0 &&
		   (nbuf[sl - 1] == '$' || nbuf[sl - 1] == '%' || nbuf[sl - 1] == '!');
	mmb_type_suffix(nbuf);
	/* #1304: a SUB/FUNCTION named like a built-in function (RGB) or colour
	 * constant (GREY) is unreachable - built-ins resolve first. A suffixed
	 * name (e.g. FUNCTION grey$) is exempt: the suffix keeps it unambiguous
	 * when called as grey$(...). */
	if (!suffixed)
		mmb_check_user_name(nbuf);
	ex = sub_find(nbuf);
	if (ex >= 0)
	{
		/* scan_subs() pre-registers every definition, and executing the
		 * SUB line again registers the same definition (same pc) at
		 * runtime; that is a no-op. A second definition of the same name
		 * is a conflict (#1304): the second one would silently never
		 * bind. */
		if (G.subs[ex].line_pc == pc)
		{
			/* A CONST defined at runtime before this line may collide
			 * with a parameter that prescan could not see (#1335). */
			check_param_consts(ex);
			return;
		}
		snprintf(msg, sizeof(msg), "?NAME CONFLICT: %s is already a %s", nbuf,
			 G.subs[ex].is_func ? "FUNCTION" : "SUB");
		mmb_error(msg);
	}
	/* Dense append (#1303): sub_find() walks the first nsubs entries, so a
	 * definition must land at slot nsubs (growing only when full) rather
	 * than a free-slot scan that scatters it out of that range. */
	if (G.nsubs >= G.subs_cap)
		subs_grow();
	slot = G.nsubs;
	strncpy(G.subs[slot].name, nbuf, MMB_MAX_NAME - 1);
	G.subs[slot].line_pc = pc;
	G.subs[slot].is_func = is_func;
	G.subs[slot].used = 1;
	G.subs[slot].nargs = 0;
	G.subs[slot].ret_sid = -1;
	G.subs[slot].ret_type = ret_type;
	for (i = 0; i < MMB_MAX_SUB_ARGS; i++)
	{
		G.subs[slot].arg_sid[i] = -1;
		G.subs[slot].arg_type[i] = 0;
	}
	mmb_skip_sp();
	if (*G.p == '(')
		G.p++;
	while (G.subs[slot].nargs < MMB_MAX_SUB_ARGS)
	{
		int sid, atype = 0;
		mmb_skip_sp();
		if (*G.p == ')' || *G.p == 0 || *G.p == '\'' || *G.p == ':')
			break;
		if (!((G.p[0] >= 'A' && G.p[0] <= 'Z') || (G.p[0] >= 'a' && G.p[0] <= 'z') || G.p[0] == '_' ||
		      (unsigned char)G.p[0] == 0x80))
			break;
		mmb_ident(G.subs[slot].args[G.subs[slot].nargs], MMB_MAX_NAME);
		sid = parse_as_sid(&atype);
		/* #1304: a parameter whose base name collides with a built-in
		 * function or colour constant is rejected at declaration time,
		 * even if the SUB/FUNCTION is never called. Struct-typed
		 * parameters are exempt like struct variables. */
		if (sid < 0)
			mmb_check_user_name(G.subs[slot].args[G.subs[slot].nargs]);
		if (sid >= 0)
			G.subs[slot].arg_sid[G.subs[slot].nargs] = sid;
		G.subs[slot].arg_type[G.subs[slot].nargs] = atype;
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
		if (!G.subs[slot].ret_type)
			G.subs[slot].ret_type = T_NUM;
		if (mmb_match("AS"))
		{
			if (mmb_match("STRING"))
				G.subs[slot].ret_type = T_STR;
			else if (mmb_match("INTEGER") || mmb_match("INT"))
				G.subs[slot].ret_type = T_INT;
			else if (mmb_match("FLOAT"))
				G.subs[slot].ret_type = T_NUM;
			else
			{
				char tn[MMB_MAX_NAME];
				int sid;
				mmb_ident(tn, sizeof(tn));
				mmb_type_suffix(tn);
				sid = mmb_struct_lookup(tn);
				if (sid < 0)
					mmb_error("?UNKNOWN TYPE");
				G.subs[slot].ret_sid = sid;
				G.subs[slot].ret_type = T_STRUCT;
			}
		}
	}
	check_param_consts(slot);
	G.nsubs++;
}

/* Pre-register every SUB/FUNCTION definition before RUN starts executing
 * (#698). Upstream MMBasic scans the whole program up front, so a call may
 * appear before the textual definition; without this the bare name is not
 * yet in G.subs[] and resolves to ?SYNTAX ERROR. sub_register() is
 * idempotent, so reaching the definition line during execution is harmless. */
static void scan_subs(void)
{
	int i;
	int saved_pc = G.run_pc;
	for (i = 0; i < G.nprog; i++)
	{
		const char *save = G.p;
		char name[MMB_MAX_NAME];
		char raw[MMB_MAX_NAME];
		int is_func;
		/* A declaration-time conflict (#1304) must report this line, not
		 * whatever line ran last. */
		G.run_pc = i;
		G.p = after_label(mmb_tok_line(i));
		if (mmb_match("SUB"))
			is_func = 0;
		else if (mmb_match("FUNCTION"))
			is_func = 1;
		else
		{
			G.p = save;
			continue;
		}
		mmb_ident(name, sizeof(name));
		strncpy(raw, name, MMB_MAX_NAME - 1);
		raw[MMB_MAX_NAME - 1] = 0;
		sub_register(raw, i + 1, is_func, mmb_type_suffix(name));
		G.p = save;
	}
	G.run_pc = saved_pc;
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
	int i;
	for (i = G.data_line; i < G.nprog; i++)
	{
		const char *save = G.p;
		const char *line = G.prog[i];
		G.p = line;
		/* On the current DATA line we already know where the next item
		 * begins, so skip straight to it instead of re-evaluating all
		 * earlier items (#990). */
		if (i == G.data_line && G.data_off > 0)
			G.p = line + G.data_off;
		else
		{
			if (!mmb_match("DATA"))
			{
				G.p = save;
				continue;
			}
		}
		mmb_skip_sp();
		if (*G.p == 0 || *G.p == ':')
		{
			G.data_line = i + 1;
			G.data_pos = 0;
			G.data_off = 0;
			G.p = save;
			mmb_error("?OUT OF DATA");
		}
		{
			mmb_val v = mmb_expr();
			mmb_skip_sp();
			if (*G.p == ',')
			{
				G.p++;
				G.data_pos++;
				G.data_line = i;
				G.data_off = (int)(G.p - line);
			}
			else
			{
				G.data_pos = 0;
				G.data_off = 0;
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
	/* Save the block IF stack and the one-line IF owner so the callee starts
	 * with a clean structure state and the caller resumes exactly where it
	 * left off (#1273, #1276). */
	G.gosub_if_depth[g] = G.if_depth;
	memcpy(G.gosub_if_stack[g], G.if_stack,
	       (size_t)G.if_depth * sizeof(G.if_stack[0]));
	memcpy(G.gosub_if_pc_stack[g], G.if_pc_stack,
	       (size_t)G.if_depth * sizeof(G.if_pc_stack[0]));
	G.gosub_if_inline[g] = G.if_inline;
	G.gosub_if_armed[g] = G.if_armed;
	G.gosub_if_inline_keep[g] = G.if_inline_keep;
	G.gosub_if_shadow[g] = G.if_shadow;
	G.gosub_ctrl_sp[g] = G.ctrl_sp;
	G.gosub_for_sp[g] = G.for_sp;
	/* Hide the caller's SELECT frames; the callee pushes above them, so no
	 * copy is needed and they are intact when it returns (#1299). */
	G.gosub_sel_sp[g] = G.sel_sp;
	G.gosub_sel_base[g] = G.sel_base;
	G.sel_base = G.sel_sp;
	G.if_depth = 0;
	G.if_inline = 0;
	G.if_armed = 0;
	G.if_inline_keep = 0;
	G.if_shadow = 0;
}

static void gosub_restore_ctrl(int g)
{
	G.if_depth = G.gosub_if_depth[g];
	memcpy(G.if_stack, G.gosub_if_stack[g],
	       (size_t)G.if_depth * sizeof(G.if_stack[0]));
	memcpy(G.if_pc_stack, G.gosub_if_pc_stack[g],
	       (size_t)G.if_depth * sizeof(G.if_pc_stack[0]));
	G.if_inline = G.gosub_if_inline[g];
	G.if_armed = G.gosub_if_armed[g];
	G.if_inline_keep = G.gosub_if_inline_keep[g];
	/* Restore, do not zero: an event handler may return into the middle of a
	 * skipped arm, whose nested-IF count still owns the pending END IF
	 * (#1342). */
	G.if_shadow = G.gosub_if_shadow[g];
	/* Drop any DO/WHILE or FOR frames the callee left open.  Only shrink:
	 * a callee may already have popped a caller frame (EXIT DO searches the
	 * whole stack), and a return must not resurrect it. */
	if (G.ctrl_sp > G.gosub_ctrl_sp[g])
		G.ctrl_sp = G.gosub_ctrl_sp[g];
	if (G.for_sp > G.gosub_for_sp[g])
		G.for_sp = G.gosub_for_sp[g];
	/* Drop SELECT frames the callee opened above the caller's stack. */
	sel_truncate(G.gosub_sel_sp[g]);
	G.sel_sp = G.gosub_sel_sp[g];
	G.sel_base = G.gosub_sel_base[g];
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
	G.gosub_pos[G.gosub_sp] = 0;
	G.gosub_event[G.gosub_sp] = 0;
	G.gosub_nsave[G.gosub_sp] = 0;
	G.gosub_nlocal[G.gosub_sp] = 0;
	G.gosub_sub[G.gosub_sp] = G.cur_sub;
	gosub_save_ctrl(G.gosub_sp);
	G.gosub_sp++;
	G.branch_pc = parse_target();
}

/* Return to the caller of a SUB/GOSUB.  A statement-level sub call saves a
 * mid-line position so statements after `name(args) :` still run (#1276). */
static void gosub_resume_caller(void)
{
	int mid_line = G.gosub_pos[G.gosub_sp] != 0;

	G.branch_pc = G.gosub_stack[G.gosub_sp];
	if (mid_line)
	{
		G.branch_pos = G.gosub_pos[G.gosub_sp];
		G.gosub_pos[G.gosub_sp] = 0;
	}
	/* Returning mid-line (a statement SUB call) or into an expression (a
	 * FUNCTION) continues the caller's one-line IF arm, so its END line
	 * must not clear the restored state (#1295).  A plain GOSUB/RETURN to
	 * the next line owns no inline arm. */
	G.if_inline_keep =
		(G.if_inline && (mid_line || G.branch_pc == -2)) ? 1 : 0;
}

void mmb_cmd_return(void)
{
	if (G.gosub_sp <= 0)
		mmb_error("?RETURN WITHOUT GOSUB");
	G.gosub_sp--;
	gosub_restore_top();
	gosub_resume_caller();
}

static void ctrl_reenter(int type)
{
	int i;
	for (i = 0; i < G.ctrl_sp; i++)
	{
		if (G.ctrlstack[i].type == type && G.ctrlstack[i].line_pc == G.run_pc)
		{
			int seq = G.ctrlstack[i].seq;
			unwind_above_seq(seq);
			pop_ctrl_top();
			return;
		}
	}
}

/* Index of the innermost (topmost) loop frame of ``type``, or -1. */
static int find_ctrl(int type)
{
	int i;
	for (i = G.ctrl_sp - 1; i >= 0; i--)
		if (G.ctrlstack[i].type == type)
			return i;
	return -1;
}

static void push_ctrl(int type)
{
	if (G.ctrl_sp >= MMB_MAX_CTRL)
		mmb_error(type == 1 ? "?WHILE" : "?DO");
	G.ctrlstack[G.ctrl_sp].type = type;
	G.ctrlstack[G.ctrl_sp].line_pc = G.run_pc;
	G.ctrlstack[G.ctrl_sp].skip = 0;
	G.ctrlstack[G.ctrl_sp].mid_line = 0;
	G.ctrlstack[G.ctrl_sp].pos = 0;
	G.ctrlstack[G.ctrl_sp].loop_pc = -1;
	G.ctrlstack[G.ctrl_sp].loop_pos = 0;
	G.ctrlstack[G.ctrl_sp].after_loop = 0;
	G.ctrlstack[G.ctrl_sp].if_depth = G.if_depth;
	G.ctrlstack[G.ctrl_sp].sel_depth = G.sel_sp;
	G.ctrlstack[G.ctrl_sp].seq = G.loop_seq++;
	G.ctrl_sp++;
}

void mmb_cmd_while(void)
{
	mmb_val v = mmb_expr();
	ctrl_reenter(1);
	if (mmb_as_int(v))
		push_ctrl(1);
	else
		G.branch_pc = find_wend_pc(G.run_pc) + 1;
}

void mmb_cmd_wend(void)
{
	int i = find_ctrl(1);
	if (i < 0)
		mmb_error("?WEND WITHOUT WHILE");
	unwind_above_seq(G.ctrlstack[i].seq);
	i = G.ctrl_sp - 1;
	G.branch_pc = G.ctrlstack[i].line_pc;
	/* Drop any block IF/SELECT left open in this iteration (#1273, #1299). */
	loop_restore(G.ctrlstack[i].if_depth, G.ctrlstack[i].sel_depth);
}

void mmb_cmd_do(void)
{
	int cond = 1;
	int mid_line = 0;
	const char *pos = 0;
	int loop_pc = -1;
	const char *loop_pos = 0, *after_loop = 0;
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
	/* A DO that does not start its line must not restart the line on
	 * loop-back; resume at the DO statement itself instead (#1329). Find the
	 * matching LOOP at statement boundaries too, for EXIT/CONTINUE DO and a
	 * false DO WHILE. Only trust a pointer inside the tokenized program line
	 * (EXECUTE strings run from a local buffer). */
	{
		const char *base = (G.run_pc >= 0 && G.run_pc < G.nprog)
					   ? mmb_tok_line(G.run_pc)
					   : 0;
		if (base && G.stmt_start >= base &&
		    G.stmt_start < base + MMB_LINE_LEN &&
		    !line_match(G.prog[G.run_pc], "DO"))
		{
			int i;
			mid_line = 1;
			pos = G.stmt_start;
			/* Re-entering the DO on loop-back reuses the location
			 * found on the first pass instead of rescanning. */
			for (i = G.ctrl_sp - 1; i >= 0; i--)
				if (G.ctrlstack[i].type == 2 &&
				    G.ctrlstack[i].line_pc == G.run_pc &&
				    G.ctrlstack[i].mid_line)
				{
					loop_pc = G.ctrlstack[i].loop_pc;
					loop_pos = G.ctrlstack[i].loop_pos;
					after_loop = G.ctrlstack[i].after_loop;
					break;
				}
			if (loop_pc < 0)
			{
				const char *lp = find_matching_loop(G.run_pc, G.p,
								    &loop_pc);
				if (lp)
				{
					loop_pos = lp;
					after_loop = stmt_after(lp);
				}
			}
		}
	}
	ctrl_reenter(2);
	if (!cond)
	{
		if (mid_line && loop_pc >= 0)
		{
			if (after_loop)
			{
				G.branch_pc = loop_pc;
				G.branch_pos = after_loop;
			}
			else
				G.branch_pc = loop_pc + 1;
		}
		else
			G.branch_pc = find_loop_pc(G.run_pc) + 1;
		return;
	}
	push_ctrl(2);
	if (mid_line)
	{
		G.ctrlstack[G.ctrl_sp - 1].mid_line = 1;
		G.ctrlstack[G.ctrl_sp - 1].pos = pos;
		G.ctrlstack[G.ctrl_sp - 1].loop_pc = loop_pc;
		G.ctrlstack[G.ctrl_sp - 1].loop_pos = loop_pos;
		G.ctrlstack[G.ctrl_sp - 1].after_loop = after_loop;
	}
}

void mmb_cmd_loop(void)
{
	int i, pc, cond = 1;
	i = find_ctrl(2);
	if (i < 0)
		mmb_error("?LOOP WITHOUT DO");
	unwind_above_seq(G.ctrlstack[i].seq);
	i = G.ctrl_sp - 1;
	pc = G.ctrlstack[i].line_pc;
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
	/* Drop any block IF/SELECT left open in this iteration (#1273). */
	loop_restore(G.ctrlstack[i].if_depth, G.ctrlstack[i].sel_depth);
	if (cond)
	{
		G.branch_pc = pc;
		/* A mid-line DO resumes at its own statement, not the line start,
		 * so statements before it do not run again (#1329). */
		if (G.ctrlstack[i].mid_line)
			G.branch_pos = G.ctrlstack[i].pos;
	}
	else
		pop_ctrl_top();
}

void mmb_cmd_exit_do(void)
{
	int i = find_ctrl(2);
	if (i < 0)
		mmb_error("?EXIT DO");
	unwind_above_seq(G.ctrlstack[i].seq);
	i = G.ctrl_sp - 1;
	if (G.ctrlstack[i].mid_line && G.ctrlstack[i].loop_pc >= 0)
	{
		/* Resume after the matching LOOP, which may share the DO's line
		 * and have statements after it (#1329). */
		if (G.ctrlstack[i].after_loop)
		{
			G.branch_pc = G.ctrlstack[i].loop_pc;
			G.branch_pos = G.ctrlstack[i].after_loop;
		}
		else
			G.branch_pc = G.ctrlstack[i].loop_pc + 1;
	}
	else
		G.branch_pc = find_loop_pc(G.ctrlstack[i].line_pc) + 1;
	pop_ctrl_top();
}

static int find_next_pc(int from)
{
	if (G.jmp_ready && from >= 0 && from < G.nprog && G.jmp_next[from] >= 0)
		return G.jmp_next[from];
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
		int f = G.for_sp - 1;
		if (f < 0)
			mmb_error("?EXIT FOR");
		/* Leaving through any loops of another kind drops their frames
		 * too, restoring the IF/SELECT state they saved (#1300). */
		unwind_above_seq(G.forstack[f].seq);
		f = G.for_sp - 1;
		if (G.forstack[f].next_pos)
		{
			/* Same-line loop: resume after the matching NEXT statement,
			 * falling to the next line when nothing follows it (#1004). */
			G.branch_pc = G.forstack[f].after_pos
					      ? G.forstack[f].next_pc
					      : G.forstack[f].next_pc + 1;
			G.branch_pos = G.forstack[f].after_pos;
		}
		else
			G.branch_pc = find_next_pc(G.forstack[f].for_pc) + 1;
		/* Leaving the loop discards IF/SELECT blocks opened inside it. */
		pop_for_top();
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
		int f = G.for_sp - 1;
		if (f < 0)
			mmb_error("?CONTINUE FOR");
		/* Skip inner loops of another kind; the target FOR stays open. */
		unwind_above_seq(G.forstack[f].seq);
		f = G.for_sp - 1;
		if (G.forstack[f].next_pos)
		{
			/* Same-line loop: resume at the matching NEXT statement, which
			 * advances the loop when run (#1004). */
			G.branch_pc = G.forstack[f].next_pc;
			G.branch_pos = G.forstack[f].next_pos;
		}
		else
			G.branch_pc = find_next_pc(G.forstack[f].for_pc);
		loop_restore(G.forstack[f].if_depth, G.forstack[f].sel_depth);
		return;
	}
	if (mmb_match("DO"))
	{
		int i = find_ctrl(2);
		if (i < 0)
			mmb_error("?CONTINUE DO");
		unwind_above_seq(G.ctrlstack[i].seq);
		i = G.ctrl_sp - 1;
		if (G.ctrlstack[i].mid_line && G.ctrlstack[i].loop_pc >= 0)
		{
			/* Jump to the LOOP statement itself, wherever it is on its
			 * line (#1329). */
			G.branch_pc = G.ctrlstack[i].loop_pc;
			G.branch_pos = G.ctrlstack[i].loop_pos;
		}
		else
			G.branch_pc = find_loop_pc(G.ctrlstack[i].line_pc);
		loop_restore(G.ctrlstack[i].if_depth,
			     G.ctrlstack[i].sel_depth);
		return;
	}
}

void mmb_cmd_select(void)
{
	mmb_selframe *f;
	mmb_skip_sp();
	if (!mmb_match("CASE"))
		mmb_syntax();
	if (G.sel_sp >= MMB_MAX_SELECT)
		mmb_error("?SELECT");
	f = &G.selstack[G.sel_sp++];
	memset(f, 0, sizeof(*f));
	f->val = mmb_expr();
	mmb_val_own(&f->val, &f->str, 0, "SELECT");
	f->active = 0;
	f->skip = 1;
	f->skipped = 0;
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
	mmb_selframe *f = sel_top();
	mmb_skip_sp();
	if (!f || f->skipped)
	{
		while (*G.p)
			G.p++;
		return;
	}
	if (mmb_match("ELSE"))
	{
		if (!f->active)
		{
			f->skip = 0;
			f->active = 1;
		}
		else
		{
			f->skip = 1;
			while (*G.p)
				G.p++;
		}
		return;
	}
	if (f->active)
	{
		f->skip = 1;
		while (*G.p)
			G.p++;
		return;
	}
	if (case_matches(f->val))
	{
		f->skip = 0;
		f->active = 1;
	}
	else
	{
		f->skip = 1;
		while (*G.p)
			G.p++;
	}
}

void mmb_cmd_end_select(void)
{
	sel_pop();
}

void mmb_cmd_endif(void)
{
	if (G.if_depth > 0)
		G.if_depth--;
}

void mmb_cmd_data(void)
{
	while (*G.p && *G.p != ':')
	{
		/* Same 3-byte keyword tokens as skip_balanced_paren() (#1310): a token
		 * payload byte can be ':' or 0 and must not end the DATA statement. */
		if ((unsigned char)*G.p == 0x80)
		{
			if (!G.p[1] && !G.p[2])
			{
				G.p++;
				continue;
			}
			G.p += 3;
			continue;
		}
		if (*G.p == '"')
		{
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
			continue;
		}
		if (*G.p == '\'')
			break;
		G.p++;
	}
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
		G.data_off = 0;
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
		G.data_off = 0;
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
		/* #1304: CONST grey = ... declares a value nothing can ever read -
		 * the named colour resolves first in expressions. */
		mmb_check_user_name(name);
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

/* A SUB called as a statement returns to the next line by default, which
 * silently drops `name(args) : stmt` (#1276).  When the call came from a
 * program line, remember where on that same line execution resumes. */
static void stmt_call_resume(void)
{
	const char *base, *q;
	int g;
	if (G.branch_pc < 0 || G.gosub_sp <= 0)
		return;
	g = G.gosub_sp - 1;
	base = (G.run_pc >= 0 && G.run_pc < G.nprog)
		       ? mmb_tok_line(G.run_pc)
		       : 0;
	q = G.p;
	while (*q == ' ' || *q == '\t')
		q++;
	if (!base || G.p < base || G.p >= base + MMB_LINE_LEN ||
	    *q == 0 || *q == '\'')
	{
		/* Nothing follows on this line (or it is not a stable program
		 * line): no mid-line resume, and no stale inline-IF owner. */
		G.gosub_if_inline[g] = 0;
		return;
	}
	G.gosub_stack[g] = G.run_pc;
	G.gosub_pos[g] = (*q == ':') ? q + 1 : q;
}

void mmb_cmd_call(void)
{
	char name[MMB_MAX_NAME];
	mmb_ident(name, sizeof(name));
	if (!mmb_call_named_sub(name))
		mmb_error("?SUB NOT FOUND");
	stmt_call_resume();
}

static void gosub_restore_top(void)
{
	int g, i, ex;
	if (G.gosub_sp < 0)
		return;
	g = G.gosub_sp;
	ex = G.opt.explicit;
	G.opt.explicit = 0;
	/* LOCAL bindings must come back before the argument values: a local may
	 * shadow an argument name, and its saved binding holds the argument's
	 * value, which the argument restore below then overwrites. */
	mmb_local_restore(g);
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
	G.cur_sub = (g > 0) ? G.gosub_sub[g - 1] : -1;
	gosub_restore_ctrl(g);
}

/* A trapped runtime error abandons the call stack: run_program jumps to the
 * ON ERROR handler, which runs at the outer level. Restore every pending
 * frame's saved arguments and LOCAL bindings first, or the handler (and any
 * later RESUME) would still see the failed callee's values. */
static void unwind_call_frames(void)
{
	while (G.gosub_sp > 0)
	{
		G.gosub_sp--;
		gosub_restore_top();
	}
	G.in_sub = 0;
	/* The longjmp abandoned any nested run_gosub_body C frame.  Its
	 * mmb_run_nested entry is still recorded in G, so pop it here after the
	 * gosub saves have been restored (the reverse order would let a late
	 * gosub_restore_ctrl overwrite the caller state with the callee's). */
	mmb_nested_unwind();
}

int mmb_call_named_sub(const char *name)
{
	int si, i, narg = 0, ex, g;
	mmb_val args[MMB_MAX_SUB_ARGS];
	char nbuf[MMB_MAX_NAME];
	char base[MMB_MAX_NAME];
	strncpy(nbuf, name, MMB_MAX_NAME - 1);
	nbuf[MMB_MAX_NAME - 1] = 0;
	mmb_upper(nbuf);
	/* The name may carry a $/%/! suffix; sub_register() stores the base
	 * name, but a FUNCTION's return variable must keep the suffix. */
	strncpy(base, nbuf, MMB_MAX_NAME - 1);
	base[MMB_MAX_NAME - 1] = 0;
	mmb_type_suffix(base);
	si = sub_find(base);
	if (si < 0)
		return 0;
	/* A CONST defined after the prescan (or after the definition line) still
	 * collides when the call binds the parameter; name it here instead of
	 * letting mmb_do_assign raise a bare ?CONST at the caller (#1335). */
	check_param_consts(si);
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
			/* `if c then hi else ...`: the zero-arg call ends at the
			 * one-line IF's ELSE, not at an argument (#1295). */
			if (G.if_inline == 1 && (at_else_kw() || at_elseif_kw()))
				break;
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
	G.gosub_pos[g] = 0;
	G.gosub_event[g] = 0;
	G.gosub_nsave[g] = G.subs[si].nargs;
	G.gosub_nlocal[g] = 0;
	G.gosub_sub[g] = si;
	ex = G.opt.explicit;
	G.opt.explicit = 0;
	for (i = 0; i < G.subs[si].nargs; i++)
	{
		mmb_var *v;
		int idx = 0;
		int atype = G.subs[si].arg_sid[i] >= 0 ? T_STRUCT : G.subs[si].arg_type[i];
		strncpy(G.gosub_saven[g][i], G.subs[si].args[i], MMB_MAX_NAME - 1);
		G.gosub_saven[g][i][MMB_MAX_NAME - 1] = 0;
		/* Look the caller's binding up with the declared type so a value
		 * stored under a suffix (e.g. A$ for an ``AS STRING`` argument) is
		 * saved and restored rather than shadowed by a fresh slot. */
		v = mmb_find_var(G.subs[si].args[i], atype, 0, 0, &idx);
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
		else
			bind_scalar_arg(G.subs[si].args[i], G.subs[si].arg_type[i]);
		if (i < narg)
			mmb_do_assign(G.subs[si].args[i], atype, 0, 0, args[i]);
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
		else
		{
			int rtype = G.subs[si].ret_type ? G.subs[si].ret_type : T_NUM;
			char last = nbuf[0] ? nbuf[strlen(nbuf) - 1] : 0;
			int suffixed = (last == '$' || last == '%' || last == '!');
			mmb_var *rv = mmb_find_var(nbuf, rtype, 1, 0, &idx);
			if (rv)
			{
				/* An explicit return type on an unsuffixed name (e.g.
				 * ``AS STRING``) still has to be reachable by the bare
				 * ``F = ...`` assignment. Plain numeric functions keep
				 * the historical unsuffixed=0 binding. */
				int unsuf = !suffixed && rtype != (G.opt.default_type ? G.opt.default_type : T_NUM);
				if (rv->unsuffixed != unsuf)
				{
					rv->unsuffixed = unsuf;
					mmb_vars_rehash();
				}
				if (rtype == T_STR)
					mmb_do_assign(nbuf, T_STR, 0, 0, mmb_str_val(""));
				else
					mmb_do_assign(nbuf, rtype, 0, 0, mmb_num_val(0));
			}
		}
	}
	G.opt.explicit = ex;
	gosub_save_ctrl(g);
	G.cur_sub = si;
	G.gosub_sp++;
	G.branch_pc = G.subs[si].line_pc;
	return 1;
}

void mmb_cmd_sub(void)
{
	char name[MMB_MAX_NAME];
	int end_pc;
	mmb_ident(name, sizeof(name));
	/* sub_register() strips the suffix itself; pass the raw name so a
	 * suffixed SUB stays exempt from the built-in name check (#1304). */
	sub_register(name, G.run_pc + 1, 0, 0);
	end_pc = find_end_sub_pc(G.run_pc);
	G.in_sub = 1;
	G.branch_pc = end_pc + 1;
}

void mmb_cmd_function(void)
{
	char name[MMB_MAX_NAME];
	char raw[MMB_MAX_NAME];
	int end_pc, ret_type;
	mmb_ident(name, sizeof(name));
	strncpy(raw, name, MMB_MAX_NAME - 1);
	raw[MMB_MAX_NAME - 1] = 0;
	ret_type = mmb_type_suffix(name);
	sub_register(raw, G.run_pc + 1, 1, ret_type);
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
		gosub_resume_caller();
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
	G.cur_sub = -1;
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
	G.opt.theme = MMB_OPT_DEFAULT_THEME;
	G.opt.edit_theme = MMB_OPT_EDIT_THEME_SYSTEM;
	G.opt.edit_jump_break = 0;
	G.opt.autosave = 1;
	G.opt.mouse_sens = 1;
	G.opt.audio_on = 1;
	G.opt.audio_target = 1; /* HDMI */
	G.opt.brightness = 100;
	G.opt.volume_l = 100;
	G.opt.volume_r = 100;
	G.opt.wifi_debug = 0;
	G.opt.term_log = 0;
	G.opt.term_scrollback = 200;
	G.opt.term_autolog = 0;
	G.opt.wordpad_view = 0;
	G.opt.wifi_country[0] = 'U';
	G.opt.wifi_country[1] = 'S';
	G.opt.wifi_country[2] = 0;
	G.opt.prompt = MMB_OPT_DEFAULT_PROMPT;
	strncpy(G.opt.app_path, MMB_APP_PATH_DEFAULT, sizeof(G.opt.app_path) - 1);
	G.opt.app_path[sizeof(G.opt.app_path) - 1] = 0;
	G.opt.boot_mode = 0;
	G.opt.boot_app[0] = 0;
	G.opt.ntp_enabled = MMB_NTP_AUTO;
	strncpy(G.opt.ntp_server, MMB_NTP_DEFAULT_SERVER, sizeof(G.opt.ntp_server) - 1);
	G.opt.ntp_server[sizeof(G.opt.ntp_server) - 1] = 0;
	strncpy(G.opt.timezone, "UTC", sizeof(G.opt.timezone) - 1);
	G.opt.timezone[sizeof(G.opt.timezone) - 1] = 0;
	G.opt.tz_offset_min = 0;
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

/* Store one program line. Lines are individually heap-allocated and the
 * prog_num table stays sorted, so replace/delete/insert move pointers only.
 * An append (num greater than the last line) takes the fast path; inserts
 * binary-search for the position. `srcfile`/`srcline` name the source for an
 * ?OUT OF MEMORY report (0/0 = interactive input). */
static void store_line(int num, const char *text, const char *srcfile, int srcline)
{
	int i, j, n;
	char buf[MMB_LINE_LEN];
	char *dup;

	G.prog_dirty = 1; /* token/jump tables are stale until the next RUN (#992) */
	/* Editing a line rewrites its token text in place, so a resolve-cache
	 * entry keyed on a token address may now name a different variable. */
	mmb_resolve_invalidate();

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
		i = find_line_idx(num);
		if (i < 0)
			return;
		G.plat->free(G.prog[i]);
		for (j = i; j < G.nprog - 1; j++)
		{
			G.prog[j] = G.prog[j + 1];
			G.prog_num[j] = G.prog_num[j + 1];
		}
		G.prog[G.nprog - 1] = 0;
		G.nprog--;
		return;
	}
	i = find_line_idx(num);
	if (i >= 0)
	{
		dup = prog_strdup(text, srcfile, srcline);
		G.plat->free(G.prog[i]);
		G.prog[i] = dup;
		return;
	}
	/* Insert position: first line number greater than num. */
	if (G.nprog == 0 || num > G.prog_num[G.nprog - 1])
		i = G.nprog;
	else
	{
		int lo = 0, hi = G.nprog - 1;
		while (lo <= hi)
		{
			int mid = (lo + hi) >> 1;
			if (G.prog_num[mid] > num)
				hi = mid - 1;
			else
				lo = mid + 1;
		}
		i = lo;
	}
	if (G.nprog >= G.prog_cap)
		prog_grow(G.nprog + 1, srcfile, srcline);
	dup = prog_strdup(text, srcfile, srcline);
	for (j = G.nprog; j > i; j--)
	{
		G.prog[j] = G.prog[j - 1];
		G.prog_num[j] = G.prog_num[j - 1];
	}
	G.prog[i] = dup;
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

/* G.p points just past '='. If the right-hand side begins with the same
 * scalar string reference followed by '+', consume `<name>+` and return 1
 * so the caller can append in place instead of rebuilding the string. */
static int self_string_append_ref(const char *name)
{
	const char *save = G.p;
	char nm[MMB_MAX_NAME];
	int n = 0, is_str = 0;
	mmb_skip_sp();
	if (!mmb_is_ident_start(*G.p))
	{
		G.p = save;
		return 0;
	}
	while (((*G.p >= 'A' && *G.p <= 'Z') || (*G.p >= 'a' && *G.p <= 'z') ||
		(*G.p >= '0' && *G.p <= '9') || *G.p == '_') &&
	       n < MMB_MAX_NAME - 2)
	{
		char c = *G.p++;
		if (c >= 'a' && c <= 'z')
			c = (char)(c - 32);
		nm[n++] = c;
	}
	if (*G.p == '$')
	{
		is_str = 1;
		G.p++;
	}
	else if (*G.p == '%' || *G.p == '!')
		G.p++;
	nm[n] = 0;
	mmb_skip_sp();
	if (is_str && mmb_keyword_eq(nm, name) && *G.p == '+')
	{
		G.p++;
		return 1;
	}
	G.p = save;
	return 0;
}

static void do_let(void)
{
	char name[MMB_MAX_NAME];
	int nidx = 0, idx[MMB_MAX_DIMS], t;
	mmb_val v;
	const char *lhs_ref;
	if (mmb_tcache_try_let())
		return;
	lhs_ref = G.p;
	t = mmb_parse_var_ref(name, &nidx, idx);
	mmb_skip_sp();
	mmb_expect('=');
	/* `a$ = a$ + ...` appends to the existing pool block in place, so the
	 * prefix is not copied twice per iteration (#994). Scalar strings only:
	 * array subscripts would need re-evaluating. */
	if (t == T_STR && nidx == 0 && !mmb_keyword_eq(name, "DATE") &&
	    !mmb_keyword_eq(name, "TIME"))
	{
		const char *rhs0 = G.p;
		if (self_string_append_ref(name))
		{
			mmb_val dummy;
			mmb_val rhs;
			mmb_var *sv;
			if (mmb_const_lookup(name, T_STR, &dummy))
				mmb_error("?CONST");
			rhs = mmb_expr();
			if (rhs.type != T_STR)
				mmb_error("?TYPE MISMATCH");
			sv = mmb_find_var_ref(lhs_ref, name, T_STR, 1, 0, 0);
			if (sv->type != T_STR)
				mmb_error("?TYPE MISMATCH");
			mmb_str_append(&sv->data.s[0], rhs.s ? rhs.s : "",
				       rhs.s ? (int)strlen(rhs.s) : 0,
				       sv->maxlen, sv->name);
			return;
		}
		G.p = rhs0;
	}
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
	if (mmb_keyword_eq(name, "MM.BRIGHTNESS"))
	{
		int pct = (int)mmb_as_int(v);
		if (pct < 0)
			pct = 0;
		if (pct > 100)
			pct = 100;
		G.opt.brightness = pct;
#if defined(MMB_PLATFORM_POSIX)
		/* Set the panel backlight percentage; a system with no controllable
		 * backlight ignores the assignment (read still returns 100). */
		(void)mmb_brightness_set(pct);
#endif
		mmb_settings_save();
		return;
	}
	mmb_do_assign_ref(lhs_ref, name, t, nidx, idx, v);
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
	int lineno = 0;
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
		lineno++;
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
			store_line(num, rest, path, lineno);
		else if (line[0])
		{
			store_line(*auto_n, line, path, lineno);
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
	prog_release(); /* drops every line plus the store and jump tables (#1303) */
	labels_release();
	subs_release();
	G.prog_dirty = 1;
	G.prog_ready = 0;
	G.current_prog[0] = 0;
	mmb_pkg_unmount();
	mmb_clear_vars(1);
	mmb_struct_clear();
	mmb_clear_consts();
	mmb_close_tcp_files();
	G.on_key[0] = 0;
	G.on_keydown[0] = 0;
	G.on_keydown_gen = G.keydown_gen;
	memset(G.on_key_ascii, 0, sizeof(G.on_key_ascii));
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
		/* G.out is 4 KB; a long program would otherwise be cut off
		 * mid-LIST (#1303). Flush per line so the whole program prints. */
		mmb_out_flush();
	}
}

void mmb_cmd_end(void)
{
	G.running = 0;
	/* A FUNCTION/SUB body saves and restores G.running around its call
	 * (#1343), so record the stop separately and let run_gosub_body leave
	 * the interpreter stopped instead of undoing it. */
	G.end_req = 1;
	mmb_play_stop_owned();
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
	prog_release(); /* drop the previous program's lines before loading (#1303) */
	G.prog_dirty = 1;
	G.prog_ready = 0;
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
	G.run_preserve_vars = 1;
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

/* True when this IF sits in the body of a same-line FOR ... NEXT.  That body
 * owns the rest of the line, so a one-line IF there keeps its historical
 * single-statement arm (the NEXT and trailing statements must still run). */
static int if_in_same_line_for_body(void)
{
	if (G.for_sp <= 0)
		return 0;
	if (!G.forstack[G.for_sp - 1].next_pos)
		return 0;
	return G.p >= G.forstack[G.for_sp - 1].pos &&
	       G.p < G.forstack[G.for_sp - 1].next_pos;
}

void mmb_cmd_if(void)
{
	mmb_val v;
	int cond, legacy;
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
		if_push(cond ? IF_BODY : IF_SEARCH, G.run_pc);
		return;
	}
	legacy = if_in_same_line_for_body();
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
		if (mmb_is_ident_start(*G.p))
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
		/* The THEN arm owns every statement up to ELSE or the end of the
		 * line; the statement loop runs them and stops at ELSE (#1276). */
		if (legacy)
		{
			exec_statement();
			if_skip_to_stmt_end();
			return;
		}
		G.if_inline = 1;
		G.if_armed = 1;
		return;
	}
	else_id = mmb_kw_id("ELSE");
	elseif_id = mmb_kw_id("ELSEIF");
	then0 = G.p;
	while (*G.p && !(legacy && *G.p == ':'))
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
				if (legacy)
				{
					exec_statement();
					if_skip_to_stmt_end();
					return;
				}
				G.if_inline = 1;
				G.if_armed = 1;
				return;
			}
			then0 = G.p;
			continue;
		}
		if (which == 1)
		{
			if (legacy)
			{
				mmb_skip_sp();
				exec_statement();
				if_skip_to_stmt_end();
				return;
			}
			/* The ELSE arm owns the rest of the line. */
			G.if_inline = 2;
			G.if_armed = 1;
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

/* Advance *pp to the ':' that terminates the current statement (or to the
 * NUL / comment that ends the line). Quoted strings and comments are skipped
 * so a ':' or ' inside them does not end the statement. */
static void skip_stmt(const char **pp)
{
	const char *p = *pp;
	while (*p)
	{
		/* Keyword tokens are 3-byte escapes; their payload bytes must not
		 * be mistaken for ':' or a quote. */
		if ((unsigned char)*p == 0x80)
		{
			p += 3;
			continue;
		}
		if (*p == '"')
		{
			p++;
			while (*p)
			{
				if (*p != '"')
				{
					p++;
					continue;
				}
				p++;
				if (*p == '"')
				{
					p++;
					continue;
				}
				break;
			}
			continue;
		}
		if (*p == '\'' || *p == ':')
			break;
		p++;
	}
	*pp = p;
}

/* Locate the NEXT statement matching the FOR at pc whose body starts at p
 * (#1004). FOR/NEXT are matched at statement boundaries, walking across lines,
 * so a single line may hold both and still be matched. Returns the NEXT
 * statement pointer, or NULL when the FOR has no matching NEXT. */
static const char *find_matching_next(int pc, const char *p, int *out_pc)
{
	int depth = 1;
	while (pc < G.nprog)
	{
		while (*p)
		{
			const char *save = G.p;
			int is_for = 0, is_next = 0;
			while (*p == ' ' || *p == '\t')
				p++;
			if (*p == 0 || *p == '\'')
				break;
			G.p = p;
			if (mmb_match("FOR"))
				is_for = 1;
			else if (mmb_match("NEXT"))
				is_next = 1;
			G.p = save;
			if (is_for)
				depth++;
			else if (is_next && --depth == 0)
			{
				*out_pc = pc;
				return p;
			}
			skip_stmt(&p);
			if (*p == ':')
				p++;
		}
		pc++;
		if (pc < G.nprog)
			p = after_label(mmb_tok_line(pc));
	}
	return 0;
}

/* Locate the LOOP statement matching the DO at pc whose body starts at p
 * (#1329). Like find_matching_next, matching happens at statement boundaries
 * and walks across lines, so a mid-line DO and its LOOP are found even though
 * build_jumps only indexes whole lines. */
static const char *find_matching_loop(int pc, const char *p, int *out_pc)
{
	int depth = 1;
	while (pc < G.nprog)
	{
		while (*p)
		{
			const char *save = G.p;
			int is_do = 0, is_loop = 0;
			while (*p == ' ' || *p == '\t')
				p++;
			if (*p == 0 || *p == '\'')
				break;
			G.p = p;
			if (mmb_match("DO"))
				is_do = 1;
			else if (mmb_match("LOOP"))
				is_loop = 1;
			G.p = save;
			if (is_do)
				depth++;
			else if (is_loop && --depth == 0)
			{
				*out_pc = pc;
				return p;
			}
			skip_stmt(&p);
			if (*p == ':')
				p++;
		}
		pc++;
		if (pc < G.nprog)
			p = after_label(mmb_tok_line(pc));
	}
	return 0;
}

/* First statement after the NEXT at np, or NULL when it is the last statement
 * on its line (#1004). */
static const char *stmt_after(const char *np)
{
	const char *q = np;
	skip_stmt(&q);
	if (*q == ':')
	{
		q++;
		while (*q == ' ' || *q == '\t')
			q++;
		if (*q && *q != '\'')
			return q;
	}
	return 0;
}

/* True when the FOR range is already exhausted before the first pass:
 * start > end for a positive step, start < end for a negative step. The
 * test uses the evaluated values, not just literals, so a variable or
 * expression bound behaves like CMM2 (#1277). */
static int for_range_empty(mmb_val from, mmb_val to, mmb_val step)
{
	if (from.type == T_INT && to.type == T_INT && step.type == T_INT)
		return (step.i >= 0 && from.i > to.i) ||
		       (step.i < 0 && from.i < to.i);
	{
		double f = mmb_as_float(from);
		double t = mmb_as_float(to);
		double s = mmb_as_float(step);
		return (s >= 0 && f > t) || (s < 0 && f < t);
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
		int off = 0, same_line = 0;
		int next_pc = -1;
		const char *base, *body = 0;
		const char *next_pos = 0, *after_pos = 0;
		mmb_var *v = mmb_find_var(name, t, 0, nidx, idx);
		if (!v)
			mmb_error("?FOR");
		off = mmb_var_offset(v, nidx, idx);
		/* A body after `:` on the FOR line must resume mid-line on loop-back,
		 * not at the next line (#997). Only trust the pointer when it lies
		 * inside the tokenized program line: EXECUTE strings run from a local
		 * buffer whose pointer would dangle once the statement returns. */
		mmb_skip_sp();
		base = (G.run_pc >= 0 && G.run_pc < G.nprog) ? mmb_tok_line(G.run_pc) : 0;
		if (*G.p == ':' && base && G.p >= base && G.p < base + MMB_LINE_LEN)
		{
			same_line = 1;
			body = G.p + 1;
		}
		/* Same-line FOR/NEXT is not in the line-granular jump table, so
		 * scan for the matching NEXT at statement level (#1004). This lets
		 * EXIT/CONTINUE FOR resume mid-line instead of reporting ?NEXT. */
		if (same_line)
		{
			int npc = -1;
			const char *np = find_matching_next(G.run_pc, body, &npc);
			if (np)
			{
				next_pc = npc;
				next_pos = np;
				after_pos = stmt_after(np);
			}
		}
		/* An empty range is tested before the first pass, so the body must
		 * not run at all (#1277). A same-line body resumes after its NEXT;
		 * a line body jumps past the matching NEXT line. Immediate mode has
		 * no stored body to skip, so it keeps line-at-a-time behaviour. */
		if (for_range_empty(from, to, step))
		{
			if (same_line && next_pos)
			{
				G.branch_pc = after_pos ? next_pc : next_pc + 1;
				G.branch_pos = after_pos;
			}
			else if (!same_line && G.jmp_ready && G.run_pc >= 0 &&
				 G.run_pc < G.nprog && G.jmp_next[G.run_pc] >= 0)
			{
				G.branch_pc = G.jmp_next[G.run_pc] + 1;
				G.branch_pos = 0;
			}
			return;
		}
		strncpy(G.forstack[G.for_sp].var, name, MMB_MAX_NAME - 1);
		G.forstack[G.for_sp].var[MMB_MAX_NAME - 1] = 0;
		G.forstack[G.for_sp].vp = v;
		G.forstack[G.for_sp].off = off;
		G.forstack[G.for_sp].to = mmb_as_int(to);
		G.forstack[G.for_sp].step = mmb_as_int(step);
		G.forstack[G.for_sp].to_f = mmb_as_float(to);
		G.forstack[G.for_sp].step_f = mmb_as_float(step);
		G.forstack[G.for_sp].cur_f = mmb_as_float(from);
		/* Fractional or mixed bounds must advance in floating point, while
		 * an all-integer range stays exact (#1280). */
		G.forstack[G.for_sp].float_loop =
			!(from.type == T_INT && to.type == T_INT && step.type == T_INT);
		G.forstack[G.for_sp].for_pc = G.run_pc;
		G.forstack[G.for_sp].line = same_line ? G.run_pc : G.run_pc + 1;
		G.forstack[G.for_sp].pos = same_line ? body : 0;
		G.forstack[G.for_sp].next_pc = next_pc;
		G.forstack[G.for_sp].next_pos = next_pos;
		G.forstack[G.for_sp].after_pos = after_pos;
		G.forstack[G.for_sp].if_depth = G.if_depth;
		G.forstack[G.for_sp].sel_depth = G.sel_sp;
		G.forstack[G.for_sp].seq = G.loop_seq++;
		G.forstack[G.for_sp].end_line =
			(G.jmp_ready && G.run_pc >= 0 && G.run_pc < G.nprog)
				? G.jmp_next[G.run_pc]
				: -1;
		G.for_sp++;
	}
}

void mmb_cmd_next(void)
{
	for (;;)
	{
		char name[MMB_MAX_NAME];
		mmb_var *v;
		int named = 0;
		int off, target, frame, cont;

		mmb_skip_sp();
		if (mmb_is_ident_start(*G.p))
		{
			mmb_ident(name, sizeof(name));
			mmb_type_suffix(name);
			named = 1;
		}
		if (G.for_sp <= 0)
			mmb_error("?NEXT WITHOUT FOR");
		target = G.for_sp - 1;
		if (named)
		{
			int j;
			target = -1;
			for (j = G.for_sp - 1; j >= 0; j--)
				if (mmb_keyword_eq(name, G.forstack[j].var))
				{
					target = j;
					break;
				}
			if (target < 0)
				mmb_error("?NEXT WITHOUT FOR");
		}
		/* Find this loop's own frame, dropping abandoned inner loops of
		 * either kind (#1300). */
		while (G.for_sp - 1 > target)
			pop_for_top();
		unwind_above_seq(G.forstack[target].seq);
		frame = G.for_sp - 1;

		v = G.forstack[frame].vp;
		off = G.forstack[frame].off;
		if (!v)
			mmb_error("?NEXT WITHOUT FOR");
		if (G.forstack[frame].float_loop)
		{
			double step = G.forstack[frame].step_f;
			double to = G.forstack[frame].to_f;
			/* A float variable reflects body edits; an integer variable
			 * cannot hold a fractional accumulator, so keep it here. */
			double cur = v->type == T_INT ? G.forstack[frame].cur_f
						      : v->data.f[off];
			cur += step;
			G.forstack[frame].cur_f = cur;
			if (v->type == T_INT)
				v->data.i[off] = (int64_t)cur;
			else
				v->data.f[off] = cur;
			cont = (step >= 0 && cur <= to) || (step < 0 && cur >= to);
		}
		else
		{
			int64_t cur = v->type == T_INT ? v->data.i[off]
						       : (int64_t)v->data.f[off];
			int64_t step = G.forstack[frame].step;
			int64_t to = G.forstack[frame].to;
			cur += step;
			if (v->type == T_INT)
				v->data.i[off] = cur;
			else
				v->data.f[off] = (double)cur;
			cont = (step >= 0 && cur <= to) || (step < 0 && cur >= to);
		}
		if (cont)
		{
			G.forstack[frame].stmt = 1;
			/* Next iteration: discard IF/SELECT blocks opened in this one. */
			loop_restore(G.forstack[frame].if_depth,
				     G.forstack[frame].sel_depth);
			return;
		}
		else
		{
			char vname[MMB_MAX_NAME];
			const char *pos;
			int end_line;
			int i;
			strncpy(vname, G.forstack[frame].var, MMB_MAX_NAME - 1);
			vname[MMB_MAX_NAME - 1] = 0;
			pos = G.forstack[frame].pos;
			end_line = G.forstack[frame].end_line;
			pop_for_top();
			G.forstack[frame].stmt = 0;
			/* NEXT a, b: after closing one loop, advance the next in the
			 * list before deciding where the statement ends (#1301). */
			mmb_skip_sp();
			if (*G.p == ',')
			{
				G.p++;
				continue;
			}
			/* The matching NEXT pc was recorded at FOR (#992). When the
			 * NEXT we are executing is exactly that line, exiting the
			 * loop just falls through to the next line, so skip the
			 * forward scan the old code did. The scan is only needed for
			 * unusual control flow (duplicate or out-of-order NEXTs). */
			if (G.jmp_ready && end_line == G.run_pc)
				return;
			/* A same-line loop's matching NEXT is the one just executed, so
			 * exiting falls through to the rest of the line (#997). */
			if (pos)
				return;
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
					if (mmb_is_ident_start(*G.p))
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
			return;
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
		/* One device/app poll per spin (mmb_poll), plus the cheap break
		 * and event checks — previously both poll stacks ran (#988). */
		while ((mmb_now_ms() - start) < ms)
		{
			mmb_poll();
			mmb_break_events();
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
			mmb_break_events();
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
	{
		mmb_vfs_rbuf_free(&G.files[i].rbuf);
		G.files[i].open = 0;
	}
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

void mmb_shutdown(void)
{
	int i;

	G.running = 0;
	mmb_play_stop();
	mmb_close_tcp_files();
	for (i = 1; i <= MMB_MAX_FILES; i++)
	{
		mmb_vfs_rbuf_free(&G.files[i].rbuf);
		G.files[i].open = 0;
	}
	mmb_settings_save();
	mmb_storage_unmount();
	mmb_console_write("Shutting down...\r\n");
	if (G.plat && G.plat->shutdown)
		G.plat->shutdown();
	mmb_error("?SHUTDOWN");
}

void mmb_cmd_shutdown(void)
{
	mmb_shutdown();
}

/* QUIT: ask the host application to end. At the prompt this leaves the
 * interpreter; inside a program it also stops RUN. A platform with no host
 * loop to end (a bare Pi) just stops the program, like END. */
void mmb_cmd_quit(void)
{
	G.running = 0;
	mmb_play_stop_owned();
	if (G.plat && G.plat->can_quit)
		G.quit_requested = 1;
}

int mmb_quit_requested(void)
{
	return G.quit_requested;
}

int mmb_take_quit(void)
{
	int v = G.quit_requested;

	G.quit_requested = 0;
	return v;
}

int mmb_is_running(void)
{
	return G.running;
}

int mmb_program_suspended(void)
{
	return G.run_suspended;
}

/* Continue a program suspended by a virtual-console switch.
 *
 * mmb_exec_line()'s setjmp(G.errjmp) landing has already returned by the time
 * a switch suspends RUN, so calling run_program() here directly left
 * mmb_error() longjmping into a dead frame (#1014). Establish a fresh landing
 * for the resumed run and report any break/error to this console, then prompt
 * (the REPL submit() path that normally does that is not on this call path). */
void mmb_resume_program(void)
{
	if (!G.run_suspended)
		return;
	if (setjmp(G.errjmp))
	{
		exec_error_reset(G.running);
		G.run_suspended = 0;
		mmb_out("\r\n");
		mmb_out(G.err[0] ? G.err : "?SYNTAX ERROR");
		mmb_out("\r\n");
		mmb_out_flush();
		mmb_front_prompt();
		return;
	}
	run_program();
	/* A resumed program that finishes (rather than suspending again) would
	 * otherwise return to the host loop without a prompt. Errors took the
	 * landing above. */
	if (!G.run_suspended)
	{
		mmb_out_flush();
		mmb_front_prompt();
	}
}

int mmb_break_key(void)
{
	return G.opt.break_key;
}

/* CTRL-C / events without the FS/network/audio poll stack, for callers that
 * have already run mmb_poll() this spin. Read pending keyboard bytes first so
 * ON KEY sees a key that arrived during PAUSE (or on the previous statement)
 * at this boundary instead of one statement later; take_break() then still
 * catches a BREAK byte read here before any handler runs. */
static void mmb_break_events(void)
{
	if (G.plat && G.plat->poll_input)
		G.plat->poll_input();
	if (G.plat && G.plat->take_break && G.plat->take_break())
	{
		mmb_play_stop_owned();
		mmb_error("?BREAK");
	}
	mmb_run_events();
}

void mmb_check_break(void)
{
	unsigned now;

	if (!G.running)
		return;
	if (G.opt.profiling)
		G.prof.check_break++;
	/* Ctrl-C / BREAK and ON KEY must be seen promptly, so check them on
	 * every line. mmb_break_events() also reads pending keyboard bytes. */
	mmb_break_events();
	/* The rest is a device/FS/network/audio poll stack. A loop body pays it
	 * once per line, so rate-limit it to ~1 ms (#988) using the same
	 * primitive the yield registry uses. */
	now = mmb_now_ms();
	if (G.break_primed && (int)(now - G.break_ms) < 1)
		return;
	G.break_primed = 1;
	G.break_ms = now;
	mmb_storage_poll();
	mmb_wlan_poll();
	mmb_net_yield();
	mmb_play_mix();
}

static void skip_balanced_paren(void)
{
	int depth = 0;
	mmb_skip_sp();
	if (*G.p != '(')
		return;
	while (*G.p)
	{
		/* Keyword tokens are 3-byte escapes (0x80, id lo, id hi). Their
		 * payload bytes can be 0 (ids < 256), '"', '(' or ')', so scan them
		 * as one unit instead of character by character (#1310). */
		if ((unsigned char)*G.p == 0x80)
		{
			if (!G.p[1] && !G.p[2])
			{
				G.p++;
				continue;
			}
			G.p += 3;
			continue;
		}
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
		tab[mmb_kw_id("MOUSE")] = mmb_cmd_mouse;
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
		tab[mmb_kw_id("TDF")] = mmb_cmd_tdf;
		tab[mmb_kw_id("BLIT")] = mmb_cmd_blit;
		tab[mmb_kw_id("IMAGE")] = mmb_cmd_image;
		tab[mmb_kw_id("FRAMEBUFFER")] = mmb_cmd_framebuffer;
		tab[mmb_kw_id("TURTLE")] = mmb_cmd_turtle;
		tab[mmb_kw_id("BITMAP")] = mmb_cmd_bitmap;
		tab[mmb_kw_id("SCREENSHOT")] = mmb_cmd_screenshot;
		tab[mmb_kw_id("GUI")] = tok_cmd_gui;
		tab[mmb_kw_id("PACKAGE")] = mmb_cmd_package;
		tab[mmb_kw_id("UNPACK")] = mmb_cmd_unpack;
		tab[mmb_kw_id("APPS")] = mmb_cmd_apps;
		tab[mmb_kw_id("CHDIR")] = mmb_cmd_chdir;
		tab[mmb_kw_id("DRIVE")] = mmb_cmd_drive;
		tab[mmb_kw_id("EJECT")] = mmb_cmd_eject;
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
		tab[mmb_kw_id("NTP")] = mmb_cmd_ntp;
		tab[mmb_kw_id("OPEN")] = mmb_cmd_open;
		tab[mmb_kw_id("CLOSE")] = mmb_cmd_close;
		tab[mmb_kw_id("PLAY")] = mmb_cmd_play;
		tab[mmb_kw_id("JUKE")] = mmb_cmd_juke;
		tab[mmb_kw_id("BEEP")] = mmb_cmd_beep;
		tab[mmb_kw_id("LOAD")] = mmb_cmd_load;
		tab[mmb_kw_id("EDIT")] = mmb_cmd_edit;
		tab[mmb_kw_id("WORDPAD")] = mmb_cmd_wordpad;
		tab[mmb_kw_id("PAINT")] = mmb_cmd_paint;
		tab[mmb_kw_id("CREDITS")] = mmb_cmd_credits;
		tab[mmb_kw_id("AFK")] = mmb_cmd_afk;
		tab[mmb_kw_id("PAUSE")] = mmb_cmd_pause;
		tab[mmb_kw_id("VSYNC_WAIT")] = mmb_cmd_vsync_wait;
		tab[mmb_kw_id("REBOOT")] = mmb_cmd_reboot;
		tab[mmb_kw_id("RESTART")] = mmb_cmd_reboot;
		tab[mmb_kw_id("SHUTDOWN")] = mmb_cmd_shutdown;
		tab[mmb_kw_id("QUIT")] = mmb_cmd_quit;
		tab[mmb_kw_id("ERASE")] = mmb_cmd_clear;
		tab[mmb_kw_id("MATH")] = mmb_cmd_math;
		tab[mmb_kw_id("SPRITE")] = mmb_cmd_sprite;
		tab[mmb_kw_id("SETTINGS")] = mmb_cmd_settings;
		tab[mmb_kw_id("SETTICK")] = mmb_cmd_settick;
		inited = 1;
	}
	id = (unsigned char)G.p[1] | ((unsigned char)G.p[2] << 8);
	if (id && id < 512 && tab[id])
	{
		/* #1337: `quit = 0` is assignment-shaped, but the token is
		 * dispatched as the command before assignment parsing, which
		 * silently ends RUN. Reject the assignment instead of running
		 * the command. Multi-word forms such as `END IF` and `END SUB`
		 * are followed by another token, not `=`, so they are safe. */
		const char *save = G.p;
		int assignment_shaped;
		G.p += 3;
		mmb_skip_sp();
		assignment_shaped = (*G.p == '=');
		G.p = save;
		if (assignment_shaped && mmb_command_hijacks_assignment(mmb_kw_name(id)))
		{
			char msg[MMB_MAX_NAME + 64];
			snprintf(msg, sizeof(msg),
				 "?NAME CONFLICT: %s is a reserved command",
				 mmb_kw_name(id));
			mmb_error(msg);
		}
		G.p += 3;
		tab[id]();
		return 1;
	}
	return 0;
}

static void exec_statement(void)
{
	mmb_skip_sp();
	G.stmt_start = G.p;
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
	if (mmb_match("SCREENSHOT"))
	{
		mmb_cmd_screenshot();
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
	if (mmb_match("UNPACK"))
	{
		mmb_cmd_unpack();
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
	if (mmb_match("NTP"))
	{
		mmb_cmd_ntp();
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
	if (mmb_match("SHUTDOWN"))
	{
		mmb_cmd_shutdown();
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
		char raw[MMB_MAX_NAME];
		if ((G.p[0] >= 'A' && G.p[0] <= 'Z') || (G.p[0] >= 'a' && G.p[0] <= 'z') || G.p[0] == '_')
		{
			mmb_ident(name, sizeof(name));
			strncpy(raw, name, MMB_MAX_NAME - 1);
			raw[MMB_MAX_NAME - 1] = 0;
			mmb_type_suffix(name);
			if (sub_find(name) >= 0)
			{
				if (!mmb_call_named_sub(raw))
					mmb_syntax();
				stmt_call_resume();
				return;
			}
			G.p = save;
		}
	}
	/* Bare token at the prompt: run a PATH .APP if one matches (#520).
	 * Builtins and user SUBs/functions have already won above, and this
	 * only applies to the interactive prompt (never inside RUN). */
	if (!G.running)
	{
		const char *save = G.p;
		char name[MMB_MAX_NAME], app[200];
		if ((G.p[0] >= 'A' && G.p[0] <= 'Z') || (G.p[0] >= 'a' && G.p[0] <= 'z') || G.p[0] == '_')
		{
			mmb_ident(name, sizeof(name));
			mmb_type_suffix(name);
			mmb_skip_sp();
			if (*G.p == 0 || *G.p == '\'')
			{
				if (mmb_app_resolve(name, app, sizeof(app)))
				{
					char cmd[220];
					int n = 0, i = 0;
					cmd[n++] = '"';
					while (app[i] && n < (int)sizeof(cmd) - 3)
						cmd[n++] = app[i++];
					cmd[n++] = '"';
					cmd[n] = 0;
					G.p = cmd;
					mmb_cmd_run();
					return;
				}
			}
			G.p = save;
		}
	}
	mmb_syntax();
}

/* 1 when the rest of a one-line IF's selected arm must be skipped: the
 * THEN arm owns statements up to ELSE, so a top-level ELSE/ELSEIF ends it. */
static int if_inline_stop(void)
{
	if (G.if_inline != 1)
		return 0;
	if (!at_else_kw() && !at_elseif_kw())
		return 0;
	while (*G.p)
		G.p++;
	G.if_inline = 0;
	return 1;
}

/* One statement under an ON ERROR SKIP/IGNORE landing. The landing keeps the
 * call stack (unlike ON ERROR GOTO) and resumes at the statement after the
 * one that failed. The previous landing is saved because nested SUB and
 * FUNCTION bodies install their own. */
static void exec_statement_guarded(void)
{
	const char *next = G.p;
	jmp_buf saved;
	int had = G.skip_stmt_valid;

	skip_stmt(&next);
	if (had)
		memcpy(saved, G.stmt_errjmp, sizeof(jmp_buf));
	G.skip_stmt_valid = 1;
	if (setjmp(G.stmt_errjmp))
	{
		s_poll_active = 0; /* longjmp unwound past mmb_poll() */
		G.p = next;
	}
	else
		exec_statement();
	G.skip_stmt_valid = had;
	if (had)
		memcpy(G.stmt_errjmp, saved, sizeof(jmp_buf));
}

/* A whole line under the line-level landing, so an error outside a statement
 * (for example while evaluating a block IF condition) is tolerated too.
 * Returns non-zero when the trapped error abandoned the rest of the line. */
static int exec_line_guarded(const char *body, const char *at)
{
	int had = G.skip_line_valid;

	if (G.error_skip != 0)
	{
		G.skip_line_valid = 1;
		if (setjmp(G.skip_errjmp))
		{
			s_poll_active = 0;
			G.branch_pc = -1;
			G.branch_pos = 0;
			G.skip_line_valid = had;
			if (G.error_skip > 0)
				G.error_skip--;
			return 1;
		}
	}
	if (at)
		exec_line_from(body, at);
	else
		exec_line_body(body);
	G.skip_line_valid = had;
	return 0;
}

static void exec_stmt_loop(void)
{
	for (;;)
	{
		if (if_inline_stop())
			break;
		if (G.error_skip != 0)
			exec_statement_guarded();
		else
			exec_statement();
		/* SKIP counts the ON ERROR statement itself, so SKIP 1 tolerates
		 * an error in the next statement only (CMM2/MMBasic semantics). */
		if (G.error_skip > 0)
			G.error_skip--;
		if (G.if_armed)
		{
			/* The IF selected a one-line arm; run its statements next. */
			G.if_armed = 0;
			continue;
		}
		if (G.branch_pc >= 0)
		{
			/* A sub call keeps its caller's one-line IF state in the
			 * gosub frame and restores it; any other branch abandons the
			 * rest of this line.  A return that restored the caller's
			 * inline arm must not clear it here (#1295). */
			if (G.if_inline_keep)
				G.if_inline_keep = 0;
			else
				G.if_inline = 0;
			G.if_armed = 0;
			return;
		}
		/* A NEXT that advanced requested a loop-back; stop the line so the
		 * caller can resume the body (same-line FOR/NEXT, #997). */
		if (G.for_sp > 0 && G.forstack[G.for_sp - 1].stmt == 1)
			return;
		mmb_skip_sp();
		if (if_inline_stop())
			break;
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
	if (G.if_inline_keep)
		G.if_inline_keep = 0;
	else
		G.if_inline = 0;
}

static void exec_line_body(const char *body)
{
	if (process_line_structure(body))
		return;
	G.p = after_label(body);
	exec_stmt_loop();
}

/* Resume a line part-way through, for a same-line FOR loop body (#997). The
 * line's label and block-structure prefix were already processed, so start at
 * the saved statement boundary. */
static void exec_line_from(const char *body, const char *at)
{
	(void)body;
	G.p = at;
	exec_stmt_loop();
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

/* Entry parameters for one nested GOSUB body.  The call itself runs inside
 * the frame so the snapshot is the true caller state, before gosub_save_ctrl
 * clears it for the callee. */
struct gosub_body_call {
	const char *name;
	const char *entry_p;
	int expr_result;
	int called;
};

/* Run the callee's statements until RETURN / END SUB / END FUNCTION / END or
 * an error leaves the loop.  Called inside mmb_run_nested, so it never saves
 * or restores caller control flow itself. */
static void run_gosub_loop(void)
{
	mmb_arena_save *arena_mark = mmb_nested_arena();

	if (G.gosub_sp > 0)
		G.gosub_stack[G.gosub_sp - 1] = -2;
	G.run_pc = G.branch_pc;
	G.branch_pc = -1;
	G.branch_pos = 0;
	/* Reclaim per-statement expression temporaries here too, or a
	 * string-producing loop inside FUNCTION/SUB/ON TICK/ON KEY grows the
	 * arena for the whole call (#989). Persistent values (function return,
	 * arguments, variables) are pooled already. Release only back to this
	 * body's entry mark, never a full reset: the caller may still hold
	 * temporaries earlier in the statement being evaluated (#1275). */
	while (G.running && G.run_pc >= 0 && G.run_pc < G.nprog)
	{
		int loop;
		int skipped = 0;
		const char *resume = 0;
		mmb_check_break();
		do
		{
			loop = 0;
			mmb_arena_release(arena_mark);
			G.branch_pc = -1;
			if (resume)
			{
				const char *at = resume;
				resume = 0;
				G.branch_pos = 0;
				skipped = exec_line_guarded(
					mmb_tok_line(G.run_pc), at);
			}
			else if (G.branch_pos)
			{
				/* Clear the position before running the rest of the
				 * line: EXIT SUB / END SUB / RETURN in those statements
				 * sets a new one, which must survive (#1332). */
				const char *at = G.branch_pos;
				G.branch_pos = 0;
				skipped = exec_line_guarded(
					mmb_tok_line(G.run_pc), at);
			}
			else
				skipped = exec_line_guarded(
					mmb_tok_line(G.run_pc), 0);
			if (skipped)
				break;
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
				resume = G.forstack[G.for_sp - 1].pos;
				loop = 1;
			}
		} while (loop && G.running);
		if (skipped)
		{
			int jump = if_skip_pc(G.run_pc);
			if (jump >= 0)
			{
				G.run_pc = jump;
				continue;
			}
			G.run_pc++;
			continue;
		}
		if (G.branch_pc == -2)
			break;
		if (G.branch_pc >= 0)
			continue;
		{
			int jump = if_skip_pc(G.run_pc);
			if (jump >= 0)
			{
				G.run_pc = jump;
				continue;
			}
		}
		G.run_pc++;
	}
}

static void run_gosub_entry(void *arg)
{
	struct gosub_body_call *call = arg;

	G.p = call->entry_p;
	if (!mmb_call_named_sub(call->name))
		return;
	call->called = 1;
	if (call->expr_result)
		/* The expression resumes after the argument list, not at the
		 * opening paren captured before the call parsed it. */
		mmb_nested_set_p(G.p);
	run_gosub_loop();
}

/* One nested GOSUB / SUB / FUNCTION / event entry.  The frame owns the caller
 * snapshot, the clean callee structure state and the single pop path; the skip
 * landing and the running flag are lifecycle, not control flow, so they stay
 * here.  Returns non-zero when the named callee was found and entered. */
static int run_gosub_body(const char *name, const char *entry_p, int expr_result)
{
	struct gosub_body_call call;
	int saved_running = G.running;
	int saved_skip_valid = G.skip_stmt_valid;
	int saved_line_valid = G.skip_line_valid;
	int had_skip = (G.error_skip != 0);
	jmp_buf saved_skip_jmp, saved_stmt_jmp;

	if (G.end_req)
		/* END already ran (for example in an earlier event body): do not
		 * enter another callee and resurrect the stopped program (#1343). */
		return 0;
	if (had_skip)
	{
		memcpy(saved_skip_jmp, G.skip_errjmp, sizeof(jmp_buf));
		memcpy(saved_stmt_jmp, G.stmt_errjmp, sizeof(jmp_buf));
	}
	call.name = name;
	call.entry_p = entry_p;
	call.expr_result = expr_result;
	call.called = 0;
	G.running = 1;
	mmb_run_nested(run_gosub_entry, &call, MMB_NESTED_GOSUB);
	/* An END inside this body is a program stop, not a per-call state
	 * change, so do not restore the running flag it cleared (#1343). */
	G.running = G.end_req ? 0 : saved_running;
	G.skip_stmt_valid = saved_skip_valid;
	G.skip_line_valid = saved_line_valid;
	if (had_skip)
	{
		memcpy(G.skip_errjmp, saved_skip_jmp, sizeof(jmp_buf));
		memcpy(G.stmt_errjmp, saved_stmt_jmp, sizeof(jmp_buf));
	}
	return call.called;
}

int mmb_try_user_function(mmb_val *out)
{
	const char *save = G.p;
	char name[MMB_MAX_NAME];
	char raw[MMB_MAX_NAME];
	int si;

	mmb_skip_sp();
	if (!mmb_is_ident_start(*G.p))
		return 0;
	mmb_ident(name, sizeof(name));
	strncpy(raw, name, MMB_MAX_NAME - 1);
	raw[MMB_MAX_NAME - 1] = 0;
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
	memset(&G.func_ret, 0, sizeof(G.func_ret));
	G.func_ret.type = G.subs[si].ret_type ? G.subs[si].ret_type : T_NUM;
	/* The nested helper calls the FUNCTION inside the frame, so the caller's
	 * run_pc/branch_pc/G.p and the call frame are restored on its one pop
	 * path (and by unwind_call_frames after a trapped error). */
	if (!run_gosub_body(raw, G.p, 1))
	{
		G.p = save;
		return 0;
	}
	/* G.func_ret/G.func_ret_s is a single shared slot: a later nested call
	 * (the next argument, or the other side of a concatenation) would
	 * overwrite it while the caller still holds this value. Detach the
	 * result into the shared arena; nothing resets it before the enclosing
	 * statement finishes (#1275). */
	*out = G.func_ret;
	if (out->type == T_STR)
		*out = mmb_str_val(G.func_ret.s ? G.func_ret.s : "");
	else if (out->type == T_STRUCT && out->blob &&
		 out->struct_idx >= 0 && out->struct_idx < MMB_MAX_STRUCT_TYPES)
	{
		int sz = G.sdef[out->struct_idx].total;
		char *p = mmb_tmp_alloc(sz);
		memcpy(p, out->blob, (size_t)sz);
		out->blob = p;
	}
	return 1;
}

/* Append a signed decimal integer to buf (no stdio in this file). */
static void append_int(char *buf, int *n, int v)
{
	char tmp[12];
	int i = 0;
	unsigned u = v < 0 ? (unsigned)(-(long)v) : (unsigned)v;
	if (v < 0)
		buf[(*n)++] = '-';
	if (u == 0)
		tmp[i++] = '0';
	while (u)
	{
		tmp[i++] = (char)('0' + (u % 10));
		u /= 10;
	}
	while (i)
		buf[(*n)++] = tmp[--i];
}

static int on_key_ascii_active(void)
{
	int i;

	for (i = 0; i < MMB_MAX_ONKEY; i++)
		if (G.on_key_ascii[i].sub[0])
			return 1;
	return 0;
}

static const char *on_key_ascii_handler(int code)
{
	int i;

	for (i = 0; i < MMB_MAX_ONKEY; i++)
		if (G.on_key_ascii[i].sub[0] && G.on_key_ascii[i].code == code)
			return G.on_key_ascii[i].sub;
	return 0;
}

/* Run one registered handler through the single nested-entry path.  The frame
 * restores the interrupted program's position and control state; gosub_sp is
 * call-frame state, so a handler that left without a normal RETURN (END) must
 * not leak its frame into the next dispatch. */
static void run_event_handler(const char *name, const char *args)
{
	int saved_sp = G.gosub_sp;

	G.tick_busy = 1;
	run_gosub_body(name, args, 0);
	G.gosub_sp = saved_sp;
	G.tick_busy = 0;
}

void mmb_run_events(void)
{
	unsigned now;
	int i;

	if (!G.running || G.tick_busy)
		return;
	/* Skip the clock read entirely when nothing is registered (#988). */
	if (!G.on_key[0] && !G.on_keydown[0] && !on_key_ascii_active() &&
	    !G.on_mouseclick[0] && !G.on_mousemove[0] && !G.on_mouseup[0])
	{
		int any = 0;
		for (i = 0; i < MMB_MAX_TICK; i++)
			if (G.tick[i].period > 0 && G.tick[i].sub[0])
			{
				any = 1;
				break;
			}
		if (!any)
			return;
	}
	now = mmb_now_ms();
	for (i = 0; i < MMB_MAX_TICK; i++)
	{
		if (G.tick[i].period <= 0 || !G.tick[i].sub[0])
			continue;
		if (now - G.tick[i].last < (unsigned)G.tick[i].period)
			continue;
		G.tick[i].last = now;
		run_event_handler(G.tick[i].sub, "");
		if (G.end_req)
			return;
	}
	if (on_key_ascii_active() && G.inkey_n > 0)
	{
		int guard;

		for (guard = 0; guard < MMB_INKEY; guard++)
		{
			int len = 0, code;
			const char *sub;

			code = mmb_inkey_peek(&len);
			if (code < 0 || len <= 0)
				break;
			sub = on_key_ascii_handler(code);
			if (!sub)
				break;
			mmb_inkey_consume(len);
			run_event_handler(sub, "");
			if (G.end_req)
				return;
		}
	}
	/* Fire once per queue change (#1293). The general key stays in the
	 * buffer for INKEY$, so a level check here re-ran the handler on every
	 * spin of PAUSE/VSYNC while that key remained unread. */
	if (G.on_key[0] && G.inkey_n > 0 && G.inkey_gen != G.on_key_gen)
	{
		G.on_key_gen = G.inkey_gen;
		run_event_handler(G.on_key, "");
		if (G.end_req)
			return;
	}
	/* ON KEYDOWN fires once per real held-set change: a press or a release,
	 * never an OS auto-repeat (an unchanged set does not bump keydown_gen).
	 * It does not touch the INKEY$ queue, so menus that read INKEY$ are not
	 * affected. The handler reads KEYDOWN() for the new set. */
	if (G.on_keydown[0] && G.keydown_gen != G.on_keydown_gen)
	{
		G.on_keydown_gen = G.keydown_gen;
		run_event_handler(G.on_keydown, "");
		if (G.end_req)
			return;
	}
	{
		int mx, my, mb, ev;
		char abuf[64];
		while ((ev = mmb_mouse_take_event(&mx, &my, &mb)) != 0)
		{
			const char *sub = ev == 1 ? G.on_mousemove :
					  ev == 3 ? G.on_mouseup :
						    G.on_mouseclick;
			int n = 0;
			if (!sub[0])
				continue;
			abuf[n++] = '(';
			append_int(abuf, &n, mx);
			abuf[n++] = ',';
			append_int(abuf, &n, my);
			if (ev != 1)
			{
				const char *btn = mb == 1 ? "left" :
						  mb == 2 ? "right" : "middle";
				int i;
				abuf[n++] = ',';
				abuf[n++] = '"';
				for (i = 0; btn[i]; i++)
					abuf[n++] = btn[i];
				abuf[n++] = '"';
			}
			abuf[n++] = ')';
			abuf[n] = 0;
			run_event_handler(sub, abuf);
			if (G.end_req)
				return;
		}
	}
}

static void run_program(void)
{
	int pc = 0;
	int resume = G.run_suspended;

	G.running = 1;
	G.end_req = 0;
	G.run_suspended = 0;
	if (!resume)
	{
		if (G.opt.profiling)
			mmb_prof_reset();
		mmb_str_reset();
		G.for_sp = 0;
		G.gosub_sp = 0;
		G.nested_depth = 0;
		G.ctrl_sp = 0;
		G.branch_pc = -1;
		G.branch_pos = 0;
		G.if_depth = 0;
		G.if_inline = 0;
		G.if_armed = 0;
		G.if_inline_keep = 0;
		G.if_shadow = 0;
		sel_reset_all();
		G.loop_seq = 0;
		G.in_sub = 0;
		G.cur_sub = -1;
		G.data_line = 0;
		G.data_pos = 0;
		G.data_off = 0;
		subs_clear();
		memset(G.tick, 0, sizeof(G.tick));
		G.on_key[0] = 0;
		G.on_keydown[0] = 0;
		G.on_keydown_gen = G.keydown_gen;
		memset(G.on_key_ascii, 0, sizeof(G.on_key_ascii));
		G.on_mouseclick[0] = 0;
		G.on_mousemove[0] = 0;
		G.on_mouseup[0] = 0;
		G.tick_busy = 0;
		G.inkey_n = G.inkey_r = G.inkey_w = 0;
		G.inkey_gen = G.on_key_gen = 0;
		/* Token/label/jump tables depend only on the program text, so a
		 * re-RUN of an unchanged program can reuse them (#992). */
		if (G.prog_dirty || !G.prog_ready)
		{
			scan_labels();
			mmb_tokenize_program();
			build_jumps();
			G.prog_dirty = 0;
			G.prog_ready = 1;
		}
		/* A fresh run may execute re-tokenized text at addresses the cache
		 * saw last run, so drop every parsed binding (#1002). */
		mmb_resolve_invalidate();
		if (!G.run_preserve_vars)
		{
			mmb_clear_vars(1);
			mmb_clear_consts();
		}
		G.run_preserve_vars = 0;
		mmb_struct_prepare();
		G.on_error_pc = -1;
		G.error_active = 0;
		G.error_skip = 0;
		G.skip_stmt_valid = 0;
		G.skip_line_valid = 0;
		G.mm_errno = 0;
		G.mm_errmsg[0] = 0;
		scan_subs();
		G.opt.explicit = 0;
		G.opt.default_type = T_NUM;
		G.opt.base = 0;
		G.opt.angle_degrees = 0;
		if (G.plat && G.plat->take_break)
			G.plat->take_break();
	}
	else
		pc = G.run_pc;
	while (pc < G.nprog && G.running)
	{
		int loop;
		int trapped = 0;
		int skipped = 0;
		const char *resume = 0;
		mmb_check_break();
		if (mmb_console_switch_pending())
		{
			/* A virtual-console switch asked us to stop at a line
			 * boundary. Keep the context runnable and return to the
			 * host loop, which performs the switch. */
			G.run_pc = pc;
			G.run_suspended = 1;
			G.running = 0;
			return;
		}
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
					s_poll_active = 0;	/* unwound past mmb_poll() */
					trapped = 1;
					break;
				}
			}
			if (resume)
			{
				const char *at = resume;
				resume = 0;
				G.branch_pos = 0;
				skipped = exec_line_guarded(
					mmb_tok_line(pc), at);
			}
			else if (G.branch_pos)
			{
				/* EXIT/CONTINUE FOR requested a mid-line resume (#1004).
				 * Clear it before executing: EXIT SUB / END SUB / RETURN
				 * inside the resumed statements sets a new position that
				 * must not be wiped (#1332). */
				const char *at = G.branch_pos;
				G.branch_pos = 0;
				skipped = exec_line_guarded(
					mmb_tok_line(pc), at);
			}
			else
				skipped = exec_line_guarded(
					mmb_tok_line(pc), 0);
			if (skipped)
				break;
			if (G.branch_pc >= 0)
			{
				pc = G.branch_pc;
				break;
			}
			if (G.for_sp > 0 && G.forstack[G.for_sp - 1].stmt == 1)
			{
				G.forstack[G.for_sp - 1].stmt = 0;
				pc = G.forstack[G.for_sp - 1].line;
				resume = G.forstack[G.for_sp - 1].pos;
				loop = 1;
			}
		} while (loop && G.running);
		if (skipped)
		{
			int jump = if_skip_pc(pc);
			if (jump >= 0)
			{
				pc = jump;
				continue;
			}
			pc++;
			continue;
		}
		if (trapped)
		{
			unwind_call_frames();
			G.branch_pos = 0;
			pc = G.on_error_pc;
			continue;
		}
		if (G.branch_pc >= 0)
			continue;
		{
			int jump = if_skip_pc(pc);
			if (jump >= 0)
			{
				pc = jump;
				continue;
			}
		}
		pc++;
	}
	if (G.opt.profiling)
		mmb_prof_report();
	G.running = 0;
	mmb_play_stop_owned();
	mmb_gfx_reset_console(0);
}

/* True when s contains a comma outside a quoted string. The language CAT
 * statement always has one (CAT dest$, text), so its absence marks the
 * prompt-only "cat file" shortcut. */
static int has_unquoted_comma(const char *s)
{
	while (*s)
	{
		if (*s == '"')
		{
			s++;
			while (*s)
			{
				if (*s == '"')
				{
					if (s[1] == '"')
					{
						s += 2;
						continue;
					}
					s++;
					break;
				}
				s++;
			}
			continue;
		}
		if (*s == ',')
			return 1;
		s++;
	}
	return 0;
}

static void clear_exec_flags(void)
{
	G.running = 0;
	G.end_req = 0;
	G.if_depth = 0;
	G.if_inline = 0;
	G.if_armed = 0;
	G.if_inline_keep = 0;
	G.if_shadow = 0;
	sel_reset_all();
	G.branch_pc = -1;
	G.branch_pos = 0;
}

/* Unwind state common to every setjmp(G.errjmp) landing: clear the poll
 * re-entrancy guard the longjmp skipped past, drop execution flags, and
 * restore the graphics state a stray error left behind. `was_running` is
 * read before clear_exec_flags() resets G.running. G.out is left empty for
 * the caller to fill with the error message. */
static void exec_error_reset(int was_running)
{
	int pages_off = G.gfx.write_page || G.gfx.display_page ||
			G.gfx.write_fb;
	s_poll_active = 0;	/* longjmp unwound past mmb_poll() */
	G.outn = 0;
	G.out[0] = 0;
	/* An unhandled error can also abandon a nested run_gosub_body C frame.
	 * Drop its recorded frame before clearing the execution flags. */
	mmb_nested_unwind();
	clear_exec_flags();
	mmb_pkg_unmount();
	if (was_running || pages_off)
		mmb_gfx_reset_console(1);
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
		exec_error_reset(G.running);
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
		if ((line[0] == 'c' || line[0] == 'C') && (line[1] == 'a' || line[1] == 'A') &&
		    (line[2] == 't' || line[2] == 'T') && (line[3] == ' ' || line[3] == '\t'))
		{
			const char *arg = line + 3;
			while (*arg == ' ' || *arg == '\t')
				arg++;
			if (!has_unquoted_comma(arg))
			{
				mmb_cmd_cat_file(arg);
				return G.out;
			}
		}
	}
	if (starts_with_line_number(line, &num, &rest))
	{
		store_line(num, rest, 0, 0);
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
	if (!g_mmb[0] && plat && plat->alloc)
		g_mmb[0] = (mmb *)plat->alloc(sizeof(mmb));
	g_cur = g_mmb[0];
	if (!g_cur)
		return;
	memset(&G, 0, sizeof(G));
	G.plat = plat;
	/* The variable name hash is file-scope heap state; release any tables a
	 * previous session left behind before the first variable is created
	 * (#998, #1303). */
	mmb_clear_vars(0);
	mmb_option_reset();
	mmb_vfs_init();
	mmb_gfx_init();
	mmb_ramdisk_seed();
	/* Mark the player gain unset until the persisted PLAY VOLUME is applied;
	 * settings_finish() below stamps a real 0..100 value (#1163). */
	g_audio.vol_l = g_audio.vol_r = MMB_AUDIO_VOL_UNSET;
	mmb_settings_load();
	mmb_gfx_apply_default_mode();
	mmb_audio_apply_options();
	mmb_console_apply_colour();
	G.timer_base = 0;
	G.rnd_seed = mmb_rng_seed();
	mmb_clock_init();
}

void mmb_reset(void)
{
	int i;
	mmb_pkg_unmount();
	mmb_clear_vars(0);
	/* All pool-owned strings (variables, constants, saved call frames) are
	 * released above or belong to a context that is about to be discarded,
	 * so return the pooled slabs too (#989). mmb_strpool_reset had no
	 * callers and every warm reset leaked them until the host freed them. */
	mmb_strpool_reset();
	mmb_option_reset();
	mmb_play_stop();
	/* Release the growable program store and its tables (#1303): the fixed
	 * 512 KB prog array was inline in this struct, so its heap replacement
	 * must be freed explicitly before the context is discarded or reused. */
	prog_release();
	labels_release();
	subs_release();
	mmb_clear_consts();
	/* EDIT tabs own growable document buffers; the editor session statics
	 * (kill buffer, find scratch) are dropped by mmb_editor_reset_all. */
	for (i = 0; i < MMB_ED_TABS; i++)
	{
		if (G.ed.tab[i].buf)
			G.plat->free(G.ed.tab[i].buf);
		memset(&G.ed.tab[i], 0, sizeof(G.ed.tab[i]));
	}
	G.prog_dirty = 1;
	G.prog_ready = 0;
	mmb_tok_release();
	s_poll_active = 0;
	mmb_gfx_init();
}

/* mmb_poll() is not re-entrant: it drives the app polls and the one-shot
 * boot-time network bring-up. A storage yield (or an app poll) can call back
 * into it while it is still on the stack - a WLAN config write goes through
 * the FS layer, whose per-chunk yield called mmb_poll() again, which re-ran
 * the network bring-up and recursed. Guard the whole body so a nested call is
 * a no-op instead of re-entering the poll stack. mmb_error()'s longjmp can
 * leave the guard set, so the setjmp error landings clear it (see run_program
 * and mmb_exec_line). */
void mmb_poll(void)
{
	static int net_boot;
	if (s_poll_active)
		return;
	s_poll_active = 1;
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
	mmb_ntp_poll();
	mmb_connect_poll();
	/* Fill the audio queue before TERM paint/present, then again after.
	 * term_draw also mixes while it is on the screen (#936). */
	mmb_play_mix();
	mmb_term_poll();
	mmb_net_tcp_debug_poll();
	mmb_play_mix();
	mmb_editor_poll();
	mmb_ihelp_poll();
	mmb_package_poll();
	mmb_apptui_poll();
	mmb_settings_poll();
	/* The FTP server is one machine-global resource owned by whichever FILES
	 * screen started it, so poll it from the host loop and not only while
	 * that screen is the active console (#812). */
	mmb_ftp_poll();
	mmb_files_poll();
	mmb_wordpad_poll();
	mmb_paint_poll();
	mmb_afk_poll();
	mmb_juke_poll();
	/* #858: run the cooperative background callbacks registered by apps on
	 * non-active consoles (JUKE queue, TERM/CONNECT socket drain). Rate
	 * limits live in the registry, so this is cheap on every yield. */
	if (mmb_yield_count() > 0)
		mmb_yield_run(mmb_now_ms());
	mmb_front_poll();
	s_poll_active = 0;
}
