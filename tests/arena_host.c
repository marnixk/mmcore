/*
 * Host test for the per-console string-temporary arena (#1351).
 *
 * Compiles the real arena implementation from mmbasic/src/util.c and drives
 * two mmb contexts directly, proving each has its own chunk list: allocating,
 * resetting, marking/releasing and freeing on one console never touches the
 * other.  It also proves the per-line reset reuses one chunk (a long loop does
 * not grow the arena).
 */
#include "mmbasic.h"
#include "mmb_priv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* util.c's mmb_error() references this; the arena path never reaches it. */
void mmb_play_stop_owned(void)
{
}

static void *host_alloc(unsigned n)
{
	return malloc(n);
}

static void host_free(void *p)
{
	free(p);
}

static const mmb_platform host_plat = {
	.alloc = host_alloc,
	.free = host_free,
};

#define CHECK(cond) \
	do { \
		if (!(cond)) \
		{ \
			fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond); \
			return 1; \
		} \
	} while (0)

static int chunk_count(const mmb *m)
{
	const mmb_achunk *c;
	int n = 0;

	for (c = m->arena; c; c = c->next)
		n++;
	return n;
}

int main(void)
{
	mmb *a = calloc(1, sizeof(mmb));
	mmb *b = calloc(1, sizeof(mmb));
	char *ap;
	char *bp;
	mmb_achunk *a_chunk;
	mmb_arena_save mark;
	unsigned used;
	int i;

	if (!a || !b)
		return 2;
	a->plat = &host_plat;
	b->plat = &host_plat;

	/* Console A allocates its own first chunk. */
	g_cur = a;
	CHECK(a->arena == 0 && a->arena_cur == 0);
	ap = mmb_tmp_alloc(24);
	CHECK(ap != 0);
	strcpy(ap, "alpha");
	CHECK(chunk_count(a) == 1);
	CHECK(a->arena == a->arena_cur);

	/* Console B starts empty and gets a different chunk. */
	g_cur = b;
	CHECK(b->arena == 0 && b->arena_cur == 0);
	bp = mmb_tmp_alloc(24);
	CHECK(bp != 0);
	strcpy(bp, "bravo");
	CHECK(chunk_count(b) == 1);
	CHECK(a->arena != b->arena);
	CHECK(a->arena_cur != b->arena_cur);

	/* Resetting B must not reclaim or alter A's data, and B reuses its
	 * own chunk on the next allocation. */
	mmb_str_reset();
	CHECK(strcmp(bp, "bravo") == 0);
	CHECK(strcmp(ap, "alpha") == 0);
	CHECK(chunk_count(a) == 1);
	CHECK(mmb_tmp_alloc(16) == bp);

	/* Resetting A reclaims its bump pointer and reuses the same chunk. */
	g_cur = a;
	a_chunk = a->arena;
	mmb_str_reset();
	CHECK(a->arena_cur == a->arena);
	CHECK(a->arena->used == 0);
	CHECK(mmb_tmp_alloc(24) == ap);
	CHECK(a->arena == a_chunk);
	CHECK(chunk_count(a) == 1);

	/* A mark/release only moves A's watermark; B is untouched. */
	used = a->arena_cur->used;
	mmb_arena_mark(&mark);
	(void)mmb_tmp_alloc(64);
	CHECK(a->arena_cur->used > used);
	mmb_arena_release(&mark);
	CHECK(a->arena_cur->used == used);
	CHECK(b->arena_cur->used > 0);

	/* A long per-line reset loop must not grow the arena without bound. */
	for (i = 0; i < 100000; i++)
	{
		mmb_str_reset();
		mmb_tmp_alloc(200)[0] = (char)i;
	}
	CHECK(chunk_count(a) == 1);
	CHECK(a->arena->next == 0);

	/* Freeing one console's arena leaves the other console's chunks alone. */
	g_cur = b;
	CHECK(b->arena != 0);
	mmb_arena_free();
	CHECK(b->arena == 0 && b->arena_cur == 0);
	CHECK(a->arena != 0 && a->arena_cur != 0);

	g_cur = a;
	mmb_arena_free();
	CHECK(a->arena == 0 && a->arena_cur == 0);

	printf("all checks passed\n");
	free(a);
	free(b);
	return 0;
}
