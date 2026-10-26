/* Execution-frame capture and restore (#1347).
 *
 * The interpreter keeps all control-flow and position state in the per-console
 * mmb struct (G).  Nested entries used to save their own subset; mmb_exec_frame
 * is the single description of the state a nested entry must put back.  This
 * stage only adds the type and the primitives: no call site uses them yet.
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
