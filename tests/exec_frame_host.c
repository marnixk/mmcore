/*
 * Host test for mmbasic/src/exec_frame.c (#1347).
 *
 * Proves mmb_frame_capture/mmb_frame_restore are an identity on every covered
 * G field, and that they never restore the run lifecycle (running/end_req).
 * The arena mark is stubbed here: util.c's allocator is not linked, and the
 * frame only needs to carry the mark through unchanged.
 */
#include "mmbasic.h"
#include "mmb_priv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

mmb *g_cur;

static int arena_sentinel;
static int mark_calls;
static int release_calls;
static void *release_chunk;
static unsigned release_used;

void mmb_arena_mark(mmb_arena_save *m)
{
	mark_calls++;
	m->chunk = &arena_sentinel;
	m->used = 99;
}

void mmb_arena_release(mmb_arena_save *m)
{
	release_calls++;
	release_chunk = m->chunk;
	release_used = m->used;
}

#define CHECK(cond) \
	do { \
		if (!(cond)) \
		{ \
			fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond); \
			return 1; \
		} \
	} while (0)

int main(void)
{
	mmb *m = (mmb *)calloc(1, sizeof(mmb));
	mmb_exec_frame f;
	int i;

	if (!m)
	{
		fprintf(stderr, "FAIL: out of memory\n");
		return 2;
	}
	g_cur = m;

	/* Non-default values in every covered field, plus a live run so the
	 * test can prove restore does not touch the lifecycle. */
	G.run_pc = 1234;
	G.branch_pc = 77;
	G.branch_pos = "branch-pos";
	G.p = "inline-p";
	G.if_depth = 7;
	for (i = 0; i < MMB_MAX_IF_DEPTH; i++)
	{
		G.if_stack[i] = (unsigned char)(i * 3 + 1);
		G.if_pc_stack[i] = 500 + i;
	}
	G.if_shadow = 11;
	G.if_inline = 2;
	G.if_armed = 1;
	G.if_inline_keep = 1;
	G.sel_sp = 6;
	G.sel_base = 3;
	G.ctrl_sp = 9;
	G.for_sp = 8;
	G.loop_seq = 42;
	G.dim_local = 13;
	G.running = 1;
	G.end_req = 1;

	mmb_frame_capture(&f);
	CHECK(mark_calls == 1);
	CHECK(release_calls == 0);
	CHECK(f.run_pc == 1234);
	CHECK(f.branch_pc == 77);
	CHECK(f.branch_pos == G.branch_pos);
	CHECK(f.p == G.p);
	CHECK(f.resume == MMB_RESUME_BRANCH_PC);
	CHECK(f.if_depth == 7);
	CHECK(f.if_shadow == 11);
	CHECK(f.if_inline == 2);
	CHECK(f.if_armed == 1);
	CHECK(f.if_inline_keep == 1);
	CHECK(f.sel_sp == 6);
	CHECK(f.sel_base == 3);
	CHECK(f.ctrl_sp == 9);
	CHECK(f.for_sp == 8);
	CHECK(f.loop_seq == 42);
	CHECK(f.dim_local == 13);
	CHECK(f.arena.chunk == &arena_sentinel);
	CHECK(f.arena.used == 99);

	/* Clobber every covered field, and clear the lifecycle so a restore
	 * that wrongly included either value would be visible. */
	G.run_pc = -100;
	G.branch_pc = -1;
	G.branch_pos = 0;
	G.p = 0;
	G.if_depth = 0;
	memset(G.if_stack, 0xAA, sizeof(G.if_stack));
	memset(G.if_pc_stack, 0x5A, sizeof(G.if_pc_stack));
	G.if_shadow = 0;
	G.if_inline = 0;
	G.if_armed = 0;
	G.if_inline_keep = 0;
	G.sel_sp = 0;
	G.sel_base = 0;
	G.ctrl_sp = 0;
	G.for_sp = 0;
	G.loop_seq = 0;
	G.dim_local = 0;
	G.running = 0;
	G.end_req = 0;

	mmb_frame_restore(&f);

	CHECK(G.run_pc == 1234);
	CHECK(G.branch_pc == 77);
	CHECK(G.branch_pos == f.branch_pos);
	CHECK(G.p == f.p);
	CHECK(G.if_depth == 7);
	for (i = 0; i < MMB_MAX_IF_DEPTH; i++)
	{
		CHECK(G.if_stack[i] == (unsigned char)(i * 3 + 1));
		CHECK(G.if_pc_stack[i] == 500 + i);
	}
	CHECK(G.if_shadow == 11);
	CHECK(G.if_inline == 2);
	CHECK(G.if_armed == 1);
	CHECK(G.if_inline_keep == 1);
	CHECK(G.sel_sp == 6);
	CHECK(G.sel_base == 3);
	CHECK(G.ctrl_sp == 9);
	CHECK(G.for_sp == 8);
	CHECK(G.loop_seq == 42);
	CHECK(G.dim_local == 13);
	/* The lifecycle is not part of the frame: restore must leave the
	 * post-capture values alone, not bring back the captured 1/1. */
	CHECK(G.running == 0);
	CHECK(G.end_req == 0);
	CHECK(release_calls == 1);
	CHECK(release_chunk == &arena_sentinel);
	CHECK(release_used == 99);

	/* The captured resume kind distinguishes the three position sources. */
	G.branch_pc = -1;
	G.branch_pos = "mid-line";
	mmb_frame_capture(&f);
	CHECK(f.resume == MMB_RESUME_BRANCH_POS);
	G.branch_pos = 0;
	mmb_frame_capture(&f);
	CHECK(f.resume == MMB_RESUME_P);

	free(m);
	printf("exec_frame: all checks passed\n");
	return 0;
}
