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

#include <setjmp.h>
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

/* SELECT frames are freed through the string pool in the real build.  The
 * tests only truncate frames with no string value, so the stub is enough. */
void mmb_str_free(char *p)
{
	(void)p;
}

void mmb_error(const char *msg)
{
	fprintf(stderr, "unexpected mmb_error: %s\n", msg);
	exit(3);
}

static void nested_clean_body(void *arg)
{
	int *ran = arg;

	*ran = 1;
	G.run_pc = 500;
	G.branch_pc = 3;
	G.branch_pos = "callee-pos";
	G.p = "callee-p";
	G.if_depth = 1;
	G.if_shadow = 9;
	G.if_inline = 2;
	G.if_armed = 1;
	G.if_inline_keep = 1;
	G.sel_sp = 4;
	G.ctrl_sp = 6;
	G.for_sp = 5;
	G.loop_seq = 77;
	G.dim_local = 21;
	G.running = 0;
	G.end_req = 1;
}

static void nested_set_p_body(void *arg)
{
	(void)arg;
	mmb_nested_set_p("after-args");
}

static jmp_buf unwind_jmp;

static void nested_unwind_inner(void *arg)
{
	(void)arg;
	longjmp(unwind_jmp, 1);
}

static void nested_unwind_outer(void *arg)
{
	(void)arg;
	mmb_run_nested(nested_unwind_inner, 0, MMB_NESTED_GOSUB);
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

	/* mmb_frame_enter() clears the callee-visible structure state and hides
	 * the caller's SELECT frames, without touching position or loop depth. */
	G.run_pc = 99;
	G.branch_pc = 5;
	G.branch_pos = "enter-pos";
	G.p = "enter-p";
	G.if_depth = 3;
	G.if_shadow = 5;
	G.if_inline = 2;
	G.if_armed = 1;
	G.if_inline_keep = 1;
	G.sel_sp = 4;
	G.sel_base = 1;
	G.ctrl_sp = 7;
	G.for_sp = 6;
	mmb_frame_enter();
	CHECK(G.if_depth == 0);
	CHECK(G.if_shadow == 0);
	CHECK(G.if_inline == 0);
	CHECK(G.if_armed == 0);
	CHECK(G.if_inline_keep == 0);
	CHECK(G.sel_sp == 4);
	CHECK(G.sel_base == 4);
	CHECK(G.ctrl_sp == 7);
	CHECK(G.for_sp == 6);
	CHECK(G.run_pc == 99);
	CHECK(G.branch_pc == 5);
	CHECK(G.p && strcmp(G.p, "enter-p") == 0);

	/* Nested restore is an exact snapshot for IF/position, truncates SELECT
	 * frames the callee opened, and only shrinks loop depths. */
	G.run_pc = 10;
	G.branch_pc = -1;
	G.branch_pos = 0;
	G.p = "caller-p";
	G.if_depth = 2;
	G.if_stack[0] = 4;
	G.if_stack[1] = 5;
	G.if_pc_stack[0] = 100;
	G.if_pc_stack[1] = 200;
	G.if_shadow = 3;
	G.if_inline = 1;
	G.if_armed = 0;
	G.if_inline_keep = 0;
	G.sel_sp = 2;
	G.sel_base = 0;
	G.ctrl_sp = 4;
	G.for_sp = 3;
	G.loop_seq = 20;
	G.dim_local = 5;
	mmb_frame_capture(&f);
	G.run_pc = 999;
	G.branch_pc = 7;
	G.branch_pos = "callee-pos";
	G.p = "callee-p";
	G.if_depth = 0;
	G.if_shadow = 9;
	G.if_inline = 0;
	G.sel_sp = 4;
	G.sel_base = 2;
	G.ctrl_sp = 6;
	G.for_sp = 5;
	G.loop_seq = 30;
	G.dim_local = 9;
	mmb_frame_restore_nested(&f);
	CHECK(G.run_pc == 10);
	CHECK(G.branch_pc == -1);
	CHECK(G.branch_pos == 0);
	CHECK(G.p && strcmp(G.p, "caller-p") == 0);
	CHECK(G.if_depth == 2);
	CHECK(G.if_shadow == 3);
	CHECK(G.if_stack[0] == 4 && G.if_stack[1] == 5);
	CHECK(G.if_pc_stack[0] == 100 && G.if_pc_stack[1] == 200);
	CHECK(G.if_inline == 1);
	CHECK(G.sel_sp == 2);
	CHECK(G.sel_base == 0);
	CHECK(G.ctrl_sp == 4);
	CHECK(G.for_sp == 3);
	CHECK(G.loop_seq == 20);
	CHECK(G.dim_local == 5);

	/* A callee that popped a caller loop must not get it resurrected. */
	G.ctrl_sp = 2;
	G.for_sp = 1;
	mmb_frame_restore_nested(&f);
	CHECK(G.ctrl_sp == 2);
	CHECK(G.for_sp == 1);

	/* mmb_run_nested: capture, enter, body, one pop, caller invariant. */
	G.run_pc = 10;
	G.branch_pc = -1;
	G.branch_pos = 0;
	G.p = "caller-p";
	G.if_depth = 2;
	G.if_shadow = 3;
	G.if_inline = 1;
	G.if_armed = 0;
	G.if_inline_keep = 0;
	G.sel_sp = 2;
	G.sel_base = 0;
	G.ctrl_sp = 4;
	G.for_sp = 3;
	G.loop_seq = 20;
	G.dim_local = 5;
	G.running = 1;
	G.end_req = 0;
	{
		int ran = 0;
		int marks = mark_calls;
		int releases = release_calls;

		mmb_run_nested(nested_clean_body, &ran, MMB_NESTED_GOSUB);
		CHECK(ran == 1);
		CHECK(G.nested_depth == 0);
		CHECK(mark_calls == marks + 1);
		CHECK(release_calls == releases + 1);
		CHECK(G.run_pc == 10);
		CHECK(G.branch_pc == -1);
		CHECK(G.branch_pos == 0);
		CHECK(G.p && strcmp(G.p, "caller-p") == 0);
		CHECK(G.if_depth == 2);
		CHECK(G.if_shadow == 3);
		CHECK(G.if_inline == 1);
		CHECK(G.if_armed == 0);
		CHECK(G.if_inline_keep == 0);
		CHECK(G.sel_sp == 2);
		CHECK(G.sel_base == 0);
		CHECK(G.ctrl_sp == 4);
		CHECK(G.for_sp == 3);
		CHECK(G.loop_seq == 20);
		CHECK(G.dim_local == 5);
		/* Lifecycle is never part of the frame. */
		CHECK(G.running == 0);
		CHECK(G.end_req == 1);
	}

	/* The expression path can update the captured resume pointer. */
	G.p = "caller-p";
	G.end_req = 0;
	G.running = 1;
	mmb_run_nested(nested_set_p_body, 0, MMB_NESTED_GOSUB);
	CHECK(G.p != 0 && strcmp(G.p, "after-args") == 0);

	/* A longjmp abandons the C frames; mmb_nested_unwind restores every
	 * outstanding entry and releases its arena mark. */
	G.run_pc = 10;
	G.branch_pc = -1;
	G.branch_pos = 0;
	G.p = "caller-p";
	G.if_depth = 2;
	G.if_shadow = 3;
	G.if_inline = 1;
	G.if_armed = 0;
	G.if_inline_keep = 0;
	G.sel_sp = 2;
	G.sel_base = 0;
	G.ctrl_sp = 4;
	G.for_sp = 3;
	G.loop_seq = 20;
	G.dim_local = 5;
	G.nested_depth = 0;
	if (setjmp(unwind_jmp) == 0)
	{
		mmb_run_nested(nested_unwind_outer, 0, MMB_NESTED_GOSUB);
		CHECK(0);
	}
	CHECK(G.nested_depth == 2);
	mmb_nested_unwind();
	CHECK(G.nested_depth == 0);
	CHECK(G.run_pc == 10);
	CHECK(G.branch_pc == -1);
	CHECK(G.branch_pos == 0);
	CHECK(G.p && strcmp(G.p, "caller-p") == 0);
	CHECK(G.if_depth == 2);
	CHECK(G.if_shadow == 3);
	CHECK(G.if_inline == 1);
	CHECK(G.sel_sp == 2);
	CHECK(G.sel_base == 0);
	CHECK(G.ctrl_sp == 4);
	CHECK(G.for_sp == 3);
	CHECK(G.loop_seq == 20);
	CHECK(G.dim_local == 5);

	free(m);
	printf("exec_frame: all checks passed\n");
	return 0;
}
