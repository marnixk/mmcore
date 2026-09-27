#include "yield.h"

/*
 * Pure cooperative-yield scheduler (#858). Host-buildable on its own: the
 * table and the rate limiter carry no interpreter state. Apps register a
 * callback plus their owning console; core.c's mmb_poll() calls
 * mmb_yield_run() with the live millisecond clock.
 *
 * The first call after mmb_yield_add() always runs. Later calls wait until
 * min_ms has elapsed since the previous call, so an app can pick its own
 * polling rate without making every host-loop iteration expensive.
 */

#define MMB_YIELD_MAX 16

typedef struct {
	int used;
	int console;
	mmb_yield_fn fn;
	void *ctx;
	unsigned min_ms;
	unsigned last_ms;
	int primed;
} mmb_yield_slot;

static mmb_yield_slot s_slots[MMB_YIELD_MAX];

int mmb_yield_add(int console, mmb_yield_fn fn, void *ctx, unsigned min_ms)
{
	int i;

	if (!fn)
		return -1;
	/* Reuse an existing slot for the same callback/console so re-entry
	 * (e.g. JUKE started again on its owner) does not leak registrations. */
	for (i = 0; i < MMB_YIELD_MAX; i++)
		if (s_slots[i].used && s_slots[i].fn == fn &&
		    s_slots[i].console == console)
			break;
	if (i == MMB_YIELD_MAX)
	{
		for (i = 0; i < MMB_YIELD_MAX; i++)
			if (!s_slots[i].used)
				break;
		if (i == MMB_YIELD_MAX)
			return -1;
	}
	s_slots[i].used = 1;
	s_slots[i].console = console;
	s_slots[i].fn = fn;
	s_slots[i].ctx = ctx;
	s_slots[i].min_ms = min_ms;
	s_slots[i].last_ms = 0;
	s_slots[i].primed = 0;
	return 0;
}

void mmb_yield_remove(mmb_yield_fn fn)
{
	int i;

	for (i = 0; i < MMB_YIELD_MAX; i++)
		if (s_slots[i].used && s_slots[i].fn == fn)
			s_slots[i].used = 0;
}

void mmb_yield_remove_console(int console)
{
	int i;

	for (i = 0; i < MMB_YIELD_MAX; i++)
		if (s_slots[i].used && s_slots[i].console == console)
			s_slots[i].used = 0;
}

void mmb_yield_clear(void)
{
	int i;

	for (i = 0; i < MMB_YIELD_MAX; i++)
		s_slots[i].used = 0;
}

void mmb_yield_run(unsigned now_ms)
{
	int i;

	for (i = 0; i < MMB_YIELD_MAX; i++)
	{
		mmb_yield_slot *s = &s_slots[i];

		if (!s->used)
			continue;
		if (s->primed && (int)(now_ms - s->last_ms) < (int)s->min_ms)
			continue;
		s->last_ms = now_ms;
		s->primed = 1;
		/* The callback may remove itself; re-read nothing afterwards. */
		s->fn(s->console, s->ctx);
	}
}

int mmb_yield_count(void)
{
	int i, n = 0;

	for (i = 0; i < MMB_YIELD_MAX; i++)
		if (s_slots[i].used)
			n++;
	return n;
}
