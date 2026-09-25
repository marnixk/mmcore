/*
 * PAINT undo/redo (#637).
 *
 * History is recorded at stroke/shape granularity. A tool calls pt_undo_push()
 * once, before it starts a committed edit (a whole pencil stroke, a shape, a
 * fill, a grab or a clear), so one history entry is one edit -- never a pixel.
 * Undo restores the canvas as it was before that edit; redo puts it back.
 *
 * The two stacks hold full canvas snapshots. Each is bounded to
 * MMB_UNDO_DEPTH (8) entries and the pair is capped by PT_UNDO_BUDGET; when a
 * push would pass either limit the oldest entry is dropped. Because undo moves
 * one snapshot from the undo stack to the redo stack (and redo moves it back),
 * the combined snapshot count never exceeds MMB_UNDO_DEPTH. A new push clears
 * the redo stack, so the future is discarded after a fresh edit.
 */
#include "mmb_priv.h"
#include "paint.h"

/* Combined allocation ceiling for both stacks. Eight full-canvas snapshots
 * (the step bound) are always smaller, so this is a hard safety cap. */
#define PT_UNDO_BUDGET (2u * 1024u * 1024u)

typedef struct {
	unsigned char *pix;
	unsigned bytes;
} pt_undo_snap;

/* One history per virtual console: undo/redo must not cross consoles (#766). */
typedef struct {
	pt_undo_snap undo[MMB_UNDO_DEPTH];
	pt_undo_snap redo[MMB_UNDO_DEPTH];
	int undo_n, redo_n;
	unsigned bytes;		/* bytes held across both stacks */
	int w, h;		/* canvas the history was built for */
	unsigned snap;		/* one canvas, bytes */
} pt_undo_state;

static pt_undo_state s_undo_state[MMB_MAX_CONSOLES];
#define UNDO (s_undo_state[g_console])

static void snap_free(pt_undo_snap *s)
{
	if (s->pix && G.plat && G.plat->free)
		G.plat->free(s->pix);
	s->pix = 0;
	s->bytes = 0;
}

static void stack_clear(pt_undo_snap *st, int *n)
{
	int i;

	for (i = 0; i < *n; i++)
	{
		UNDO.bytes -= st[i].bytes;
		snap_free(&st[i]);
	}
	*n = 0;
}

static void stack_drop_oldest(pt_undo_snap *st, int *n)
{
	int i;

	if (*n <= 0)
		return;
	UNDO.bytes -= st[0].bytes;
	snap_free(&st[0]);
	for (i = 1; i < *n; i++)
		st[i - 1] = st[i];
	(*n)--;
	st[*n].pix = 0;
	st[*n].bytes = 0;
}

/* Append a copy of the current canvas. Returns 0 (and records nothing) when
 * the budget cannot hold another snapshot. */
static int stack_add(pt_undo_snap *st, int *n)
{
	unsigned char *copy;

	if (!G.plat || !G.plat->alloc)
		return 0;
	if (UNDO.bytes + UNDO.snap > PT_UNDO_BUDGET)
		return 0;
	copy = G.plat->alloc(UNDO.snap);
	if (!copy)
		return 0;
	memcpy(copy, PT.canvas, UNDO.snap);
	st[*n].pix = copy;
	st[*n].bytes = UNDO.snap;
	UNDO.bytes += UNDO.snap;
	(*n)++;
	return 1;
}

static void sync_depths(void)
{
	PT.undo_depth = UNDO.undo_n;
	PT.redo_depth = UNDO.redo_n;
}

void pt_undo_clear(void)
{
	stack_clear(UNDO.undo, &UNDO.undo_n);
	stack_clear(UNDO.redo, &UNDO.redo_n);
	UNDO.bytes = 0;
	sync_depths();
}

void pt_undo_init(void)
{
	UNDO.w = PT.width;
	UNDO.h = PT.height;
	UNDO.snap = (unsigned)PT.width * (unsigned)PT.height;
	pt_undo_clear();
}

void pt_undo_push(void)
{
	if (!PT.canvas || PT.width <= 0 || PT.height <= 0)
		return;

	/* A canvas of a different size cannot reuse the snapshots. */
	if (PT.width != UNDO.w || PT.height != UNDO.h)
	{
		stack_clear(UNDO.undo, &UNDO.undo_n);
		stack_clear(UNDO.redo, &UNDO.redo_n);
		UNDO.w = PT.width;
		UNDO.h = PT.height;
		UNDO.snap = (unsigned)PT.width * (unsigned)PT.height;
	}

	/* A fresh edit invalidates any redo future. */
	stack_clear(UNDO.redo, &UNDO.redo_n);

	while (UNDO.undo_n >= MMB_UNDO_DEPTH ||
	       (UNDO.undo_n > 0 && UNDO.bytes + UNDO.snap > PT_UNDO_BUDGET))
		stack_drop_oldest(UNDO.undo, &UNDO.undo_n);

	if (!stack_add(UNDO.undo, &UNDO.undo_n))
		return;
	sync_depths();
}

void pt_undo(void)
{
	pt_undo_snap *top;

	if (UNDO.undo_n <= 0)
	{
		strncpy(PT.status, "Nothing to undo", sizeof(PT.status) - 1);
		return;
	}
	if (!PT.canvas)
		return;

	while (UNDO.redo_n >= MMB_UNDO_DEPTH)
		stack_drop_oldest(UNDO.redo, &UNDO.redo_n);

	/* Remember where we are so redo can come back to it. */
	if (!stack_add(UNDO.redo, &UNDO.redo_n))
	{
		strncpy(PT.status, "?OUT OF MEMORY", sizeof(PT.status) - 1);
		return;
	}

	/* Restore the state recorded before the edit. */
	top = &UNDO.undo[UNDO.undo_n - 1];
	memcpy(PT.canvas, top->pix, UNDO.snap);
	UNDO.bytes -= top->bytes;
	snap_free(top);
	UNDO.undo_n--;

	sync_depths();
	pt_request_redraw();
	strncpy(PT.status, "Undo", sizeof(PT.status) - 1);
}

void pt_redo(void)
{
	pt_undo_snap *top;

	if (UNDO.redo_n <= 0)
	{
		strncpy(PT.status, "Nothing to redo", sizeof(PT.status) - 1);
		return;
	}
	if (!PT.canvas)
		return;

	while (UNDO.undo_n >= MMB_UNDO_DEPTH)
		stack_drop_oldest(UNDO.undo, &UNDO.undo_n);

	if (!stack_add(UNDO.undo, &UNDO.undo_n))
	{
		strncpy(PT.status, "?OUT OF MEMORY", sizeof(PT.status) - 1);
		return;
	}

	top = &UNDO.redo[UNDO.redo_n - 1];
	memcpy(PT.canvas, top->pix, UNDO.snap);
	UNDO.bytes -= top->bytes;
	snap_free(top);
	UNDO.redo_n--;

	sync_depths();
	pt_request_redraw();
	strncpy(PT.status, "Redo", sizeof(PT.status) - 1);
}

/* Introspection for the native test: bytes currently held by both stacks and
 * the hard ceiling they are kept under. */
unsigned pt_undo_mem(void)
{
	return UNDO.bytes;
}

unsigned pt_undo_budget(void)
{
	return PT_UNDO_BUDGET;
}
