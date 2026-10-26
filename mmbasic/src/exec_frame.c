/* Execution-frame capture, enter, restore and the nested-entry helper
 * (#1347, #1348).
 *
 * The interpreter keeps all control-flow and position state in the per-console
 * mmb struct (G).  Nested entries used to save their own subset; mmb_exec_frame
 * is the single description of the state a nested entry must put back.
 *
 * mmb_run_nested() is the one entry point: it captures the caller into
 * G.nested_frame[], clears the callee's structure state, runs the body, and
 * restores the caller through one pop path.  The frames stay in G rather than
 * on the C stack so the ON ERROR longjmp can unwind them after it has abandoned
 * the C frames (unwind_call_frames() -> mmb_nested_unwind()).
 *
 * The frame is a plain value copied field by field; no heap allocation and no
 * callback, so it is safe to capture/restore on the interrupt path.
 *
 * Deliberate exclusions (see also the struct comment in mmb_priv.h):
 *   - running and end_req: an END inside a nested body stops the program and
 *     must never be undone by a restore (#1343).
 *   - gosub_sp and the gosub/call arrays: owned by the call-frame push/pop,
 *     not by the execution frame.  An event or FUNCTION wrapper restores them
 *     around the body itself.
 *   - ctrlstack/forstack/selstack contents: only the depth/base pointers are
 *     framed; the arrays stay in G, exactly as gosub_save_ctrl/restore_ctrl
 *     treat them today.
 *   - acc_* and ON ERROR jump state: a resolved-member cache and a longjmp
 *     landing site, neither restorable from a plain snapshot.
 */
#include "mmb_priv.h"

void mmb_frame_capture(mmb_exec_frame *f)
{
	f->run_pc = G.run_pc;
	f->branch_pc = G.branch_pc;
	f->branch_pos = G.branch_pos;
	f->p = G.p;
	f->resume = G.branch_pc >= 0 ? MMB_RESUME_BRANCH_PC :
		    G.branch_pos ? MMB_RESUME_BRANCH_POS : MMB_RESUME_P;

	f->if_depth = G.if_depth;
	memcpy(f->if_stack, G.if_stack, sizeof(f->if_stack));
	memcpy(f->if_pc_stack, G.if_pc_stack, sizeof(f->if_pc_stack));
	f->if_shadow = G.if_shadow;

	f->if_inline = G.if_inline;
	f->if_armed = G.if_armed;
	f->if_inline_keep = G.if_inline_keep;

	f->sel_sp = G.sel_sp;
	f->sel_base = G.sel_base;

	f->ctrl_sp = G.ctrl_sp;
	f->for_sp = G.for_sp;
	f->loop_seq = G.loop_seq;

	f->dim_local = G.dim_local;

	mmb_arena_mark(&f->arena);
}

/* Put the callee into the same clean structure state as gosub_save_ctrl():
 * empty block-IF and one-line-IF state, and the caller's SELECT frames hidden
 * behind sel_base.  Loop frames are shared, so their depths are left alone. */
void mmb_frame_enter(void)
{
	G.if_depth = 0;
	G.if_shadow = 0;
	G.if_inline = 0;
	G.if_armed = 0;
	G.if_inline_keep = 0;
	G.sel_base = G.sel_sp;
}

/* Strict identity restore, used by the frame host test and any caller that
 * wants an exact snapshot.  Nested entries use the shrink-only variant below. */
void mmb_frame_restore(mmb_exec_frame *f)
{
	G.run_pc = f->run_pc;
	G.branch_pc = f->branch_pc;
	G.branch_pos = f->branch_pos;
	G.p = f->p;

	G.if_depth = f->if_depth;
	memcpy(G.if_stack, f->if_stack, sizeof(G.if_stack));
	memcpy(G.if_pc_stack, f->if_pc_stack, sizeof(G.if_pc_stack));
	G.if_shadow = f->if_shadow;

	G.if_inline = f->if_inline;
	G.if_armed = f->if_armed;
	G.if_inline_keep = f->if_inline_keep;

	G.sel_sp = f->sel_sp;
	G.sel_base = f->sel_base;

	G.ctrl_sp = f->ctrl_sp;
	G.for_sp = f->for_sp;
	G.loop_seq = f->loop_seq;

	G.dim_local = f->dim_local;

	/* running and end_req are intentionally not written here. */
	mmb_arena_release(&f->arena);
}

/* Restore after a nested entry.  Control-flow fields the callee might have
 * legitimately changed are handled like gosub_restore_ctrl():
 *   - IF and one-line-IF state is an exact snapshot;
 *   - SELECT frames the callee opened above the caller's stack are dropped;
 *   - DO/WHILE and FOR depths only SHRINK, so a callee that popped a caller
 *     frame with EXIT DO does not get it resurrected on return. */
void mmb_frame_restore_nested(mmb_exec_frame *f)
{
	G.run_pc = f->run_pc;
	G.branch_pc = f->branch_pc;
	G.branch_pos = f->branch_pos;
	G.p = f->p;

	G.if_depth = f->if_depth;
	memcpy(G.if_stack, f->if_stack, sizeof(G.if_stack));
	memcpy(G.if_pc_stack, f->if_pc_stack, sizeof(G.if_pc_stack));
	G.if_shadow = f->if_shadow;

	G.if_inline = f->if_inline;
	G.if_armed = f->if_armed;
	G.if_inline_keep = f->if_inline_keep;

	mmb_sel_truncate(f->sel_sp);
	G.sel_sp = f->sel_sp;
	G.sel_base = f->sel_base;

	if (G.ctrl_sp > f->ctrl_sp)
		G.ctrl_sp = f->ctrl_sp;
	if (G.for_sp > f->for_sp)
		G.for_sp = f->for_sp;
	G.loop_seq = f->loop_seq;

	G.dim_local = f->dim_local;

	/* running and end_req are intentionally not written here. */
	mmb_arena_release(&f->arena);
}

/* Drop SELECT frames above depth (and above the visible base).  Shared with
 * core.c's sel_truncate so the nested pop frees the same string values. */
void mmb_sel_truncate(int depth)
{
	while (G.sel_sp > depth && G.sel_sp > G.sel_base)
	{
		G.sel_sp--;
		if (G.selstack[G.sel_sp].str)
		{
			mmb_str_free(G.selstack[G.sel_sp].str);
			G.selstack[G.sel_sp].str = 0;
		}
	}
}

/* The only nested pop.  Normal return, END SUB/FUNCTION and the error unwind
 * all come through here. */
static void nested_pop(void)
{
	mmb_exec_frame *f;

	if (G.nested_depth <= 0)
		return;
	G.nested_depth--;
	f = &G.nested_frame[G.nested_depth];
	mmb_frame_restore_nested(f);
}

void mmb_nested_unwind(void)
{
	while (G.nested_depth > 0)
		nested_pop();
}

/* The expression path resumes after the argument list, not at the opening
 * paren captured before mmb_call_named_sub() parsed it. */
void mmb_nested_set_p(const char *p)
{
	if (G.nested_depth > 0)
		G.nested_frame[G.nested_depth - 1].p = p;
}

/* Per-line arena reclaim inside a nested interpreter loop.  The mark belongs
 * to this entry's frame, so the loop and the final pop release to the same
 * watermark without a second hand-rolled mark in the body. */
mmb_arena_save *mmb_nested_arena(void)
{
	if (G.nested_depth <= 0)
		return 0;
	return &G.nested_frame[G.nested_depth - 1].arena;
}

void mmb_run_nested(void (*body)(void *), void *arg, enum mmb_nested_kind kind)
{
	int depth = G.nested_depth;

	(void)kind;
	if (depth >= MMB_MAX_NESTED)
		mmb_error("?NESTING");
	mmb_frame_capture(&G.nested_frame[depth]);
	G.nested_depth = depth + 1;
	mmb_frame_enter();
	body(arg);
	nested_pop();
}
