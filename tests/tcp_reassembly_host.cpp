/* Host test for the patched Circle TCP reassembly queue
   (patches/circle-tcp-robust.patch). Built by tests/test_circle_patches.py
   against a pristine Circle copy with both patches applied and the shim
   headers in tests/circle_shim. Byte at sequence number s is (u8)s, so the
   delivered stream can be checked for trimming errors. */

#include <circle/net/reassemblyqueue.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail;

#define CHECK(cond)                                                          \
	do                                                                   \
	{                                                                    \
		if (!(cond))                                                 \
		{                                                            \
			printf ("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			g_fail++;                                            \
		}                                                            \
	} while (0)

static CNetBuffer *seg (u32 nSeq, size_t ulLen)
{
	u8 tmp[FRAME_BUFFER_SIZE];
	for (size_t i = 0; i < ulLen; i++)
		tmp[i] = (u8) (nSeq + i);
	return new CNetBuffer (CNetBuffer::Receive, ulLen, tmp);
}

/* Enqueue like CTCPConnection does: a rejected segment is deleted. */
static boolean enq (CReassemblyQueue &q, u32 nSeq, size_t ulLen)
{
	CNetBuffer *p = seg (nSeq, ulLen);
	if (q.Enqueue (nSeq, p))
		return TRUE;
	delete p;
	return FALSE;
}

/* Drain the RX queue and verify it is the contiguous byte stream
   [nFrom, nTo). */
static void expect_stream (CNetBufferQueue &rx, u32 nFrom, u32 nTo)
{
	u32 nSeq = nFrom;
	CNetBuffer *p;
	while ((p = rx.Dequeue ()) != nullptr)
	{
		const u8 *d = (const u8 *) p->GetPtr ();
		for (size_t i = 0; i < p->GetLength (); i++)
		{
			if (d[i] != (u8) (nSeq + i))
			{
				printf ("FAIL stream byte at seq %u: got %u\n",
					(unsigned) (nSeq + i), d[i]);
				g_fail++;
				delete p;
				return;
			}
		}
		nSeq += p->GetLength ();
		delete p;
	}
	CHECK (nSeq == nTo);
}

static void test_out_of_order_basic (void)
{
	CNetBufferQueue rx;
	CReassemblyQueue q (&rx, 65536);

	CHECK (enq (q, 200, 100));
	CHECK (q.GetBytesQueued () == 100);
	CHECK (enq (q, 100, 100));
	CHECK (q.GetBytesQueued () == 200);
	CHECK (q.Dequeue (100) == 300);
	CHECK (q.GetBytesQueued () == 0);
	expect_stream (rx, 100, 300);
}

static void test_trim_coalesced_retransmission (void)
{
	/* RCV.NXT is 150; peer retransmits [100,200) as one segment. */
	CNetBuffer *p = seg (100, 100);
	u32 nSeq = 100;
	CHECK (CReassemblyQueue::TrimSegment (&nSeq, p, 150));
	CHECK (nSeq == 150);
	CHECK (p->GetLength () == 50);
	CHECK (((const u8 *) p->GetPtr ())[0] == (u8) 150);
	CHECK (((const u8 *) p->GetPtr ())[49] == (u8) 199);
	delete p;
}

static void test_trim_fully_stale (void)
{
	CNetBuffer *p = seg (100, 100);
	u32 nSeq = 100;
	CHECK (CReassemblyQueue::TrimSegment (&nSeq, p, 200));
	CHECK (nSeq == 200);
	CHECK (p->GetLength () == 0);
	delete p;

	/* far behind RCV.NXT: skip is clamped to the segment length */
	p = seg (100, 100);
	nSeq = 100;
	CHECK (CReassemblyQueue::TrimSegment (&nSeq, p, 5000));
	CHECK (nSeq == 5000);
	CHECK (p->GetLength () == 0);
	delete p;
}

static void test_trim_leaves_future_segment (void)
{
	CNetBuffer *p = seg (150, 10);
	u32 nSeq = 150;
	CHECK (!CReassemblyQueue::TrimSegment (&nSeq, p, 100));
	CHECK (nSeq == 150);
	CHECK (p->GetLength () == 10);
	nSeq = 150;
	CHECK (!CReassemblyQueue::TrimSegment (&nSeq, p, 150));
	CHECK (p->GetLength () == 10);
	delete p;
}

static void test_overlap_in_queue_is_trimmed_not_disabled (void)
{
	CNetBufferQueue rx;
	CReassemblyQueue q (&rx, 65536);

	CHECK (enq (q, 200, 100));
	CHECK (enq (q, 250, 150));	/* overlaps the queued one */
	CHECK (q.Dequeue (200) == 400);
	expect_stream (rx, 200, 400);

	/* queue keeps working afterwards */
	CHECK (enq (q, 500, 10));
	CHECK (q.Dequeue (500) == 510);
	expect_stream (rx, 500, 510);
}

static void test_overlap_before_queued_segment (void)
{
	CNetBufferQueue rx;
	CReassemblyQueue q (&rx, 65536);

	CHECK (enq (q, 250, 150));
	CHECK (enq (q, 200, 100));	/* new one starts first */
	CHECK (q.Dequeue (200) == 400);
	expect_stream (rx, 200, 400);
}

static void test_same_seq_longer_replaces_shorter (void)
{
	CNetBufferQueue rx;
	CReassemblyQueue q (&rx, 65536);

	CHECK (enq (q, 200, 50));
	CHECK (enq (q, 200, 100));	/* covers the queued one */
	CHECK (q.Dequeue (200) == 300);
	expect_stream (rx, 200, 300);

	CHECK (enq (q, 400, 100));
	CHECK (!enq (q, 400, 50));			/* covered: dropped */
	CHECK (!enq (q, 420, 20));		/* covered: dropped */
	CHECK (q.Dequeue (400) == 500);
	expect_stream (rx, 400, 500);
}

static void test_stale_entries_are_dropped (void)
{
	CNetBufferQueue rx;
	CReassemblyQueue q (&rx, 65536);

	CHECK (enq (q, 300, 100));
	CHECK (enq (q, 600, 100));
	/* in-order data arrived meanwhile up to 450: [300,400) is stale */
	CHECK (q.Dequeue (450) == 450);
	CHECK (rx.IsEmpty ());
	/* ... then up to 600 */
	CHECK (q.Dequeue (600) == 700);
	expect_stream (rx, 600, 700);
}

static void test_partial_stale_entry_is_trimmed (void)
{
	CNetBufferQueue rx;
	CReassemblyQueue q (&rx, 65536);

	CHECK (enq (q, 300, 100));
	CHECK (q.Dequeue (350) == 400);
	expect_stream (rx, 350, 400);
}

static void test_sequence_wrap (void)
{
	CNetBufferQueue rx;
	CReassemblyQueue q (&rx, 65536);
	u32 nBase = 0xFFFFFFF0u;

	CHECK (enq (q, nBase + 40, 20));
	CHECK (enq (q, nBase + 10, 40));	/* overlaps, wraps */
	CHECK (q.Dequeue (nBase) == nBase);
	CHECK (rx.IsEmpty ());
	CHECK (q.Dequeue (nBase + 10) == nBase + 60);
	expect_stream (rx, nBase + 10, nBase + 60);
}

static void test_byte_cap (void)
{
	CNetBufferQueue rx;
	CReassemblyQueue q (&rx, 150);

	CHECK (enq (q, 200, 100));
	CHECK (q.GetBytesQueued () == 100);
	CHECK (!enq (q, 400, 100));	/* would exceed cap */
	CHECK (q.GetBytesQueued () == 100);
	CHECK (enq (q, 400, 50));
	CHECK (q.GetBytesQueued () == 150);
	CHECK (!enq (q, 500, 1));
	CHECK (q.Dequeue (200) == 300);
	expect_stream (rx, 200, 300);
	CHECK (enq (q, 600, 100));	/* space released */
	q.Flush ();
}

static void test_zero_length_rejected (void)
{
	CNetBufferQueue rx;
	CReassemblyQueue q (&rx, 65536);
	CNetBuffer *p = seg (100, 0);
	CHECK (!q.Enqueue (100, p));
	delete p;
}

int main (void)
{
	test_out_of_order_basic ();
	test_trim_coalesced_retransmission ();
	test_trim_fully_stale ();
	test_trim_leaves_future_segment ();
	test_overlap_in_queue_is_trimmed_not_disabled ();
	test_overlap_before_queued_segment ();
	test_same_seq_longer_replaces_shorter ();
	test_stale_entries_are_dropped ();
	test_partial_stale_entry_is_trimmed ();
	test_sequence_wrap ();
	test_byte_cap ();
	test_zero_length_rejected ();

	if (g_fail)
	{
		printf ("%d check(s) failed\n", g_fail);
		return 1;
	}
	printf ("all checks passed\n");
	return 0;
}
